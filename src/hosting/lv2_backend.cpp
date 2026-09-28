// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "hosting/lv2_backend.h"

#include <lilv/lilv.h>
#include <dlfcn.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/time/time.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/state/state.h>
#include <lv2/midi/midi.h>
#include <lv2/ui/ui.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cmath>
#include <limits>
#include <cstring>
#include <map>
#include <mutex>
#include <atomic>
#include <semaphore>
#include <string>
#include <string_view>
#include <chrono>
#include <thread>
#include <vector>

#include "core/rt_queue.h"
#include "hosting/common.h"

namespace nirbija {
namespace {

// Ardour's LV2 midnam: a plugin hands the host an XML MIDINameDocument of
// the keys it actually answers to. Black Pearl and the other AVL kits live
// here; CLAP has clap.note-name for the same job.
#define LV2_MIDNAM_URI "http://ardour.org/lv2/midnam"
#define LV2_MIDNAM__interface LV2_MIDNAM_URI "#interface"
#define LV2_MIDNAM__update LV2_MIDNAM_URI "#update"

struct LV2_Midnam {
  void* handle;
  void (*update)(void* handle);
};

struct LV2_Midnam_Interface {
  char* (*midnam)(LV2_Handle instance);
  char* (*model)(LV2_Handle instance);
  void (*free)(char*);
};

// Pad names in the wild carry XML entities ("Ride &amp; Crash"); attribute
// values are otherwise taken verbatim, so decode the five predefined ones.
std::string unescape_xml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '&') {
      if (text.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; continue; }
      if (text.compare(i, 4, "&lt;") == 0) { out += '<'; i += 4; continue; }
      if (text.compare(i, 4, "&gt;") == 0) { out += '>'; i += 4; continue; }
      if (text.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; continue; }
      if (text.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; continue; }
    }
    out += text[i++];
  }
  return out;
}

std::vector<NoteName> parse_midnam_notes(const char* xml) {
  std::vector<NoteName> out;
  if (xml == nullptr) return out;
  const char* cursor = xml;
  while (const char* tag = std::strstr(cursor, "<Note")) {
    const char* close = std::strchr(tag, '>');
    if (close == nullptr) break;
    const std::string_view attrs(tag, static_cast<size_t>(close - tag));
    auto quoted = [&](std::string_view key) -> std::string {
      const std::string needle = std::string(key) + "=\"";
      const auto pos = attrs.find(needle);
      if (pos == std::string_view::npos) return {};
      const auto start = pos + needle.size();
      const auto end = attrs.find('"', start);
      if (end == std::string_view::npos) return {};
      return std::string(attrs.substr(start, end - start));
    };
    const std::string number = quoted("Number");
    const std::string name = unescape_xml(quoted("Name"));
    if (!number.empty() && !name.empty()) {
      char* endp = nullptr;
      const long key = std::strtol(number.c_str(), &endp, 10);
      if (endp != number.c_str() && key >= 0 && key <= 127)
        out.push_back({static_cast<int>(key), name});
    }
    cursor = close + 1;
  }
  return out;
}

// urid:map, shared by every LV2 instance in the process. Plugins call map on
// the UI thread during instantiation; the mutex never reaches the audio thread.
class UridMap {
 public:
  UridMap() {
    map_.handle = this;
    map_.map = &UridMap::map_uri;
    unmap_.handle = this;
    unmap_.unmap = &UridMap::unmap_urid;
  }

  LV2_URID_Map* map_feature() { return &map_; }
  LV2_URID_Unmap* unmap_feature() { return &unmap_; }

  LV2_URID map_string(const char* uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = to_urid_.find(uri);
    if (it != to_urid_.end()) return it->second;
    const LV2_URID id = static_cast<LV2_URID>(to_uri_.size()) + 1;
    to_urid_.emplace(uri, id);
    to_uri_.emplace_back(uri);
    return id;
  }

 private:
  static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char* uri) {
    return static_cast<UridMap*>(handle)->map_string(uri);
  }
  static const char* unmap_urid(LV2_URID_Unmap_Handle handle, LV2_URID urid) {
    auto* self = static_cast<UridMap*>(handle);
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (urid == 0 || urid > self->to_uri_.size()) return nullptr;
    return self->to_uri_[urid - 1].c_str();
  }

  std::mutex mutex_;
  std::map<std::string, LV2_URID> to_urid_;
  std::vector<std::string> to_uri_;
  LV2_URID_Map map_{};
  LV2_URID_Unmap unmap_{};
};

// A worker request or response in flight. Fixed size so neither the audio
// thread nor the worker thread ever allocates to pass one along; plugins that
// need more than this are refused rather than silently truncated.
constexpr size_t kWorkPayloadBytes = 2048;

struct WorkMessage {
  uint32_t size = 0;
  std::array<uint8_t, kWorkPayloadBytes> data{};
};

// Atom traffic between a plugin's DSP and its own editor, crossing threads.
// Analysers live on this: the DSP streams spectrum frames to the UI through an
// atom output port, and the UI asks for them through an atom input port. The
// host has to carry both, or the editor draws an empty grid forever.
struct AtomBridge {
  struct Message {
    uint32_t port = 0;
    uint32_t size = 0;
    std::array<uint8_t, 8192> data{};
  };

  // Slot queues, not RtQueue: a Message is 8 KiB, and push-by-value copied
  // each one twice on the audio thread. Here it is written in place once.
  hosting::SlotQueue<Message, 128> to_ui;
  hosting::SlotQueue<Message, 16> to_plugin;
};

enum class PortKind { Ignored, AudioIn, AudioOut, ControlIn, ControlOut, AtomIn, AtomOut };

struct PortInfo {
  uint32_t index = 0;
  PortKind kind = PortKind::Ignored;
  std::string name;
  std::string symbol;  // LV2 state keys on this, not on the display name
  float min_value = 0.0f;
  float max_value = 1.0f;
  float default_value = 0.0f;
  // Atom ports: rsz:minimumSize, or 0 when the plugin did not say.
  uint32_t minimum_size = 0;
  // Atom ports: atom:supports midi:MidiEvent.
  bool supports_midi = false;
};

// Cached lilv URIs, built once per world.
struct PortClasses {
  explicit PortClasses(LilvWorld* world)
      : audio(lilv_new_uri(world, LV2_CORE__AudioPort)),
        control(lilv_new_uri(world, LV2_CORE__ControlPort)),
        cv(lilv_new_uri(world, LV2_CORE__CVPort)),
        atom(lilv_new_uri(world, LV2_ATOM__AtomPort)),
        input(lilv_new_uri(world, LV2_CORE__InputPort)),
        output(lilv_new_uri(world, LV2_CORE__OutputPort)),
        midi_event(lilv_new_uri(world, LV2_MIDI__MidiEvent)),
        minimum_size(lilv_new_uri(world, LV2_RESIZE_PORT__minimumSize)) {}

  ~PortClasses() {
    for (LilvNode* node : {audio, control, cv, atom, input, output, midi_event,
                           minimum_size})
      lilv_node_free(node);
  }

  LilvNode* audio;
  LilvNode* control;
  LilvNode* cv;
  LilvNode* atom;
  LilvNode* input;
  LilvNode* output;
  LilvNode* midi_event;
  LilvNode* minimum_size;
};

// The lilv world, its cached URIs and the URID map, kept alive by every
// instance created from it. A plugin instance outliving its world would free
// nodes through a dangling pointer, which is exactly what happens when the UI
// tears down its plugin list before its engine.
struct Lv2World {
  Lv2World() : world(lilv_world_new()) {
    lilv_world_load_all(world);
    classes = std::make_unique<PortClasses>(world);
  }

  ~Lv2World() {
    classes.reset();
    lilv_world_free(world);
  }

  LilvWorld* world;
  std::unique_ptr<PortClasses> classes;
  UridMap urids;
};

// An LV2 editor that draws with X11, loaded straight from its own binary. suil
// exists to wrap editors written for a different toolkit than the host's; an
// X11 editor needs no wrapping, so this covers the common case without the
// extra dependency.
class Lv2Instance;

class Lv2Gui : public PluginGui {
 public:
  Lv2Gui(Lv2Instance* owner, std::shared_ptr<Lv2World> world,
         const LilvPlugin* plugin, std::vector<float>* control_values,
         std::vector<PortInfo> control_ports,
         std::vector<float>* control_out_values,
         std::vector<PortInfo> control_out_ports, AtomBridge* bridge,
         std::atomic<bool>* state_dirty)
      : owner_(owner),
        world_(std::move(world)),
        plugin_(plugin),
        control_values_(control_values),
        control_ports_(std::move(control_ports)),
        control_out_values_(control_out_values),
        control_out_ports_(std::move(control_out_ports)),
        bridge_(bridge),
        state_dirty_(state_dirty),
        event_transfer_urid_(world_->urids.map_string(LV2_ATOM__eventTransfer)) {}

  ~Lv2Gui() override { detach(); }

  // Reports whether this plugin ships an editor we can embed, without loading
  // anything yet.
  static bool available(LilvWorld* world, const LilvPlugin* plugin) {
    LilvNode* x11 = lilv_new_uri(world, LV2_UI__X11UI);
    LilvUIs* uis = lilv_plugin_get_uis(plugin);
    bool found = false;
    if (uis != nullptr) {
      LILV_FOREACH(uis, iter, uis) {
        if (lilv_ui_is_a(lilv_uis_get(uis, iter), x11)) {
          found = true;
          break;
        }
      }
      lilv_uis_free(uis);
    }
    lilv_node_free(x11);
    return found;
  }

  bool attach(uintptr_t parent_window) override {
    if (widget_ != nullptr) detach();

    LilvNode* x11 = lilv_new_uri(world_->world, LV2_UI__X11UI);
    LilvUIs* uis = lilv_plugin_get_uis(plugin_);
    if (uis == nullptr) {
      lilv_node_free(x11);
      return false;
    }

    bool ok = false;
    LILV_FOREACH(uis, iter, uis) {
      const LilvUI* ui = lilv_uis_get(uis, iter);
      if (!lilv_ui_is_a(ui, x11)) continue;
      ok = load(ui, parent_window);
      if (ok) break;
    }

    lilv_uis_free(uis);
    lilv_node_free(x11);
    return ok;
  }

  void detach() override {
    if (descriptor_ != nullptr && handle_ != nullptr) descriptor_->cleanup(handle_);
    handle_ = nullptr;
    widget_ = nullptr;
    descriptor_ = nullptr;
    idle_iface_ = nullptr;
    if (library_ != nullptr) {
      dlclose(library_);
      library_ = nullptr;
    }
  }

  int idle() override {
    if (handle_ == nullptr) return 0;

    // The editor was given instance-access to a DSP instance that no longer
    // exists (the sample rate changed and the plugin was re-instantiated). Its
    // pointer is dead; the only safe thing is to have it closed. Non-zero is
    // "the editor asked to close"; the user reopens it against the new one.
    if (instance_stale()) return 1;

    push_changed_ports();

    // Whatever the DSP produced for its editor since the last tick.
    if (bridge_ != nullptr && descriptor_ != nullptr &&
        descriptor_->port_event != nullptr) {
      while (const AtomBridge::Message* message = bridge_->to_ui.peek()) {
        descriptor_->port_event(handle_, message->port, message->size,
                                event_transfer_urid_, message->data.data());
        bridge_->to_ui.pop();
      }
    }

    if (idle_iface_ != nullptr) return idle_iface_->idle(handle_);
    return 0;
  }

  // Only known once the editor has asked for a size through ui:resize.
  bool preferred_size(int* width, int* height) const override {
    if (requested_width_ <= 0 || requested_height_ <= 0) return false;
    *width = requested_width_;
    *height = requested_height_;
    return true;
  }

 private:
  bool load(const LilvUI* ui, uintptr_t parent_window) {
    const LilvNode* binary_node = lilv_ui_get_binary_uri(ui);
    const LilvNode* bundle_node = lilv_ui_get_bundle_uri(ui);
    if (binary_node == nullptr || bundle_node == nullptr) return false;

    char* binary_path = lilv_file_uri_parse(lilv_node_as_uri(binary_node), nullptr);
    char* bundle_path = lilv_file_uri_parse(lilv_node_as_uri(bundle_node), nullptr);
    if (binary_path == nullptr || bundle_path == nullptr) {
      lilv_free(binary_path);
      lilv_free(bundle_path);
      return false;
    }

    const bool debug = std::getenv("NIRBIJA_DEBUG_EMBED") != nullptr;
    // Same .so as the DSP, already loaded by lilv. Reusing that handle keeps
    // instance-access pointing at the object the UI's C++ methods expect —
    // a second RTLD_LOCAL copy would be a second vtable and a dead editor.
    library_ = dlopen(binary_path, RTLD_NOLOAD | RTLD_LOCAL | RTLD_NOW);
    if (library_ == nullptr)
      library_ = dlopen(binary_path, RTLD_LOCAL | RTLD_NOW);
    if (debug && library_ == nullptr)
      std::fprintf(stderr, "lv2 gui: dlopen failed: %s\n", dlerror());
    if (library_ != nullptr) {
      auto entry = reinterpret_cast<LV2UI_DescriptorFunction>(
          dlsym(library_, "lv2ui_descriptor"));
      const char* wanted = lilv_node_as_uri(lilv_ui_get_uri(ui));
      for (uint32_t i = 0; entry != nullptr; ++i) {
        const LV2UI_Descriptor* candidate = entry(i);
        if (candidate == nullptr) break;
        if (std::strcmp(candidate->URI, wanted) == 0) {
          descriptor_ = candidate;
          break;
        }
      }
    }

    if (debug && descriptor_ == nullptr)
      std::fprintf(stderr, "lv2 gui: no descriptor matching %s in %s\n",
                   lilv_node_as_uri(lilv_ui_get_uri(ui)), binary_path);

    if (descriptor_ != nullptr)
      instantiate(bundle_path, parent_window);

    if (descriptor_ != nullptr && handle_ == nullptr) {
      // Most LV2 editors draw with OpenGL and refuse to instantiate when GLX
      // cannot give them a context, which is a property of the display rather
      // than of the plugin. Worth saying out loud, since the plugin itself
      // reports nothing.
      std::fprintf(stderr,
                   "lv2 gui: %s refused to instantiate. If other editors fail "
                   "too, check GLX on this display; forcing "
                   "__GLX_VENDOR_LIBRARY_NAME=mesa fixes it on some setups.\n",
                   lilv_node_as_uri(lilv_ui_get_uri(ui)));
    }

    lilv_free(binary_path);
    lilv_free(bundle_path);

    if (handle_ == nullptr) {
      detach();
      return false;
    }
    return true;
  }

  // Defined after Lv2Instance: the DSP instance the editor is given access to,
  // and whether it has since been replaced.
  LV2_Handle current_handle() const;
  bool instance_stale() const;

  void instantiate(const char* bundle_path, uintptr_t parent_window) {
    parent_feature_ = {LV2_UI__parent, reinterpret_cast<void*>(parent_window)};
    instance_feature_ = {LV2_INSTANCE_ACCESS_URI, current_handle()};
    idle_feature_ = {LV2_UI__idleInterface, nullptr};

    // Editors that lay themselves out ask the host for a size, and several
    // refuse to instantiate at all without somewhere to ask.
    resize_.handle = this;
    resize_.ui_resize = &Lv2Gui::request_resize;
    resize_feature_ = {LV2_UI__resize, &resize_};
    map_feature_ = {LV2_URID__map, world_->urids.map_feature()};
    unmap_feature_ = {LV2_URID__unmap, world_->urids.unmap_feature()};

    const LV2_Feature* features[] = {&parent_feature_, &instance_feature_,
                                     &idle_feature_,   &resize_feature_,
                                     &map_feature_,    &unmap_feature_,
                                     nullptr};

    handle_ = descriptor_->instantiate(descriptor_, lilv_node_as_uri(
                                           lilv_plugin_get_uri(plugin_)),
                                       bundle_path, &Lv2Gui::write_port, this,
                                       &widget_, features);
    if (handle_ == nullptr) return;

    if (descriptor_->extension_data != nullptr) {
      idle_iface_ = static_cast<const LV2UI_Idle_Interface*>(
          descriptor_->extension_data(LV2_UI__idleInterface));
    }

    // An editor opens showing its own defaults until the host tells it what the
    // values actually are. Without this a session restored from disk plays the
    // right thing while its editor shows something else entirely.
    last_sent_.assign(control_values_->size(),
                      std::numeric_limits<float>::quiet_NaN());
    last_sent_out_.assign(control_out_values_->size(),
                          std::numeric_limits<float>::quiet_NaN());
    push_changed_ports();
  }

  // Sends the editor every control value that has moved since it was last
  // told. Also covers changes made from outside the editor — a restored
  // session, or a parameter set from the host. Control outputs too: meters
  // and latency readouts are output ports, and an editor with a meter draws
  // it from what the host forwards, or draws nothing.
  void push_changed_ports() {
    if (descriptor_ == nullptr || descriptor_->port_event == nullptr) return;
    if (handle_ == nullptr) return;

    for (size_t i = 0; i < control_ports_.size() && i < control_values_->size();
         ++i) {
      const float value =
          std::atomic_ref<float>((*control_values_)[i]).load(std::memory_order_relaxed);
      if (last_sent_[i] == value) continue;
      last_sent_[i] = value;
      descriptor_->port_event(handle_, control_ports_[i].index, sizeof(float), 0,
                              &value);
    }
    for (size_t i = 0;
         i < control_out_ports_.size() && i < control_out_values_->size(); ++i) {
      // The audio thread writes these through the plugin, plainly; the
      // atomic read at least cannot tear.
      const float value = std::atomic_ref<float>((*control_out_values_)[i])
                              .load(std::memory_order_relaxed);
      if (last_sent_out_[i] == value) continue;
      last_sent_out_[i] = value;
      descriptor_->port_event(handle_, control_out_ports_[i].index, sizeof(float),
                              0, &value);
    }
  }

  static int request_resize(LV2UI_Feature_Handle handle, int width, int height) {
    auto* self = static_cast<Lv2Gui*>(handle);
    self->requested_width_ = width;
    self->requested_height_ = height;
    return 0;
  }

  // The editor writes back to the host: plain floats for control ports, and
  // atom events for everything an analyser or sequencer UI needs to tell its
  // DSP — including the "I am visible, start streaming" handshake.
  static void write_port(LV2UI_Controller controller, uint32_t port_index,
                         uint32_t buffer_size, uint32_t format, const void* buffer) {
    auto* self = static_cast<Lv2Gui*>(controller);

    // The editor speaking to its DSP is the only thing LV2 lets the host see.
    // It covers a knob turned in the plugin's window; it does not cover a
    // sampler told to load a kit, which the UI and the DSP arrange between
    // themselves through instance-access. Closing the editor stands in there.
    if (self->state_dirty_ != nullptr)
      self->state_dirty_->store(true, std::memory_order_release);

    if (format == self->event_transfer_urid_) {
      if (self->bridge_ == nullptr) return;
      AtomBridge::Message* message = self->bridge_->to_plugin.begin_push();
      if (message == nullptr) return;  // ring full: the editor repeats itself
      if (buffer_size > message->data.size()) return;
      message->port = port_index;
      message->size = buffer_size;
      std::memcpy(message->data.data(), buffer, buffer_size);
      self->bridge_->to_plugin.commit_push();
      return;
    }

    if (format != 0 || buffer_size != sizeof(float)) return;

    const float value = *static_cast<const float*>(buffer);
    for (size_t i = 0; i < self->control_ports_.size(); ++i) {
      if (self->control_ports_[i].index != port_index) continue;
      // The audio thread reads this float through the port the plugin is
      // connected to; the store is atomic so it never sees half of it.
      std::atomic_ref<float>((*self->control_values_)[i])
          .store(value, std::memory_order_relaxed);
      return;
    }
  }

  Lv2Instance* owner_;
  std::shared_ptr<Lv2World> world_;
  const LilvPlugin* plugin_;
  // Which DSP instance the editor was built against, see instance_stale().
  mutable uint64_t attached_generation_ = 0;
  std::vector<float>* control_values_;
  std::vector<PortInfo> control_ports_;
  std::vector<float>* control_out_values_;
  std::vector<PortInfo> control_out_ports_;

  void* library_ = nullptr;
  const LV2UI_Descriptor* descriptor_ = nullptr;
  LV2UI_Handle handle_ = nullptr;
  LV2UI_Widget widget_ = nullptr;
  const LV2UI_Idle_Interface* idle_iface_ = nullptr;
  // What the editor has already been told, so idle only sends what moved.
  std::vector<float> last_sent_;
  std::vector<float> last_sent_out_;
  AtomBridge* bridge_;
  std::atomic<bool>* state_dirty_;
  LV2_URID event_transfer_urid_;
  LV2_Feature parent_feature_{}, instance_feature_{}, idle_feature_{};
  LV2_Feature resize_feature_{}, map_feature_{}, unmap_feature_{};
  LV2UI_Resize resize_{};
  int requested_width_ = 0;
  int requested_height_ = 0;
};

class Lv2Instance : public PluginInstance {
 public:
  Lv2Instance(PluginDescriptor desc, const LilvPlugin* plugin,
              std::shared_ptr<Lv2World> world)
      : desc_(std::move(desc)), plugin_(plugin), world_(std::move(world)),
        urids_(world_->urids) {
    scan_ports(*world_->classes);
  }

  ~Lv2Instance() override {
    deactivate();
    destroy_instance();
  }

  bool activate(double sample_rate, uint32_t max_block_frames) override {
    // LV2 fixes the sample rate at instantiation: there is no way to tell a
    // running instance the rate moved, so a plugin kept across a rate change
    // would play everything transposed. The state is carried across a fresh
    // instantiation instead. The editor, if open, holds instance-access to
    // the old instance and is told to close (see Lv2Gui::idle).
    if (instance_ != nullptr && sample_rate != sample_rate_) {
      const std::vector<uint8_t> carried = save_state();
      deactivate();
      destroy_instance();
      if (!activate(sample_rate, max_block_frames)) return false;
      if (!carried.empty()) load_state(carried);
      return true;
    }

    // Same instance, brought back (or grown): a period change must not free
    // the instance - a live editor holds instance-access to this handle. Grow
    // buffers, reconnect, and re-activate instead.
    if (instance_ != nullptr) {
      // Nothing to change: same block, already running. The park below costs
      // a real wait per plugin, and prepare() calls this for every insert.
      if (activated_ && max_block_frames <= max_block_frames_ &&
          static_cast<int32_t>(max_block_frames) == nominal_block_length_)
        return true;

      // Reallocating buffers instance_ is connected to races the audio
      // thread the same way a state restore does, so borrow that guard:
      // silence process() and wait for it to be seen before touching them.
      quiet_.store(true, std::memory_order_release);
      if (!wait_until_parked()) {
        // Never confirmed the audio thread left lilv_instance_run; leave the
        // instance exactly as it is rather than reallocate under it.
        quiet_.store(false, std::memory_order_release);
        return activated_;
      }
      if (max_block_frames > max_block_frames_) {
        max_block_frames_ = max_block_frames;
        for (auto& buffer : audio_buffers_)
          buffer.assign(max_block_frames_, 0.0f);
      }
      connect_all();
      update_options(max_block_frames);
      if (!activated_) {
        if (worker_iface_ != nullptr) start_worker();
        lilv_instance_activate(instance_);
        activated_ = true;
      }
      quiet_.store(false, std::memory_order_release);
      return true;
    }

    sample_rate_ = sample_rate;
    max_block_frames_ = max_block_frames;

    // Features are handed to the plugin by pointer and must outlive it, so they
    // live in members rather than locals.
    build_features(max_block_frames);
    lv2_atom_forge_init(&forge_, world_->urids.map_feature());
    forges_.assign(atom_in_.size(), forge_);
    forge_frames_.assign(atom_in_.size(), LV2_Atom_Forge_Frame{});

    instance_ = lilv_plugin_instantiate(plugin_, sample_rate, features_.data());
    if (instance_ == nullptr) return false;
    instance_generation_.fetch_add(1, std::memory_order_acq_rel);

    // Audio buffers are per-port and owned here, so a plugin with more ports
    // than the strip is wide still gets a valid buffer for every one.
    audio_buffers_.assign(audio_in_.size() + audio_out_.size(),
                          std::vector<float>(max_block_frames, 0.0f));
    // Atom ports get a buffer each, as big as the plugin asked for and never
    // smaller than the default: inputs an empty sequence, outputs scratch the
    // plugin may fill.
    atom_buffers_.clear();
    for (const PortInfo& port : atom_in_)
      atom_buffers_.emplace_back(atom_buffer_bytes(port), 0);
    for (const PortInfo& port : atom_out_)
      atom_buffers_.emplace_back(atom_buffer_bytes(port), 0);

    connect_all();

    worker_iface_ = static_cast<const LV2_Worker_Interface*>(
        lilv_instance_get_extension_data(instance_, LV2_WORKER__interface));
    options_iface_ = static_cast<const LV2_Options_Interface*>(
        lilv_instance_get_extension_data(instance_, LV2_OPTIONS__interface));
    if (worker_iface_ != nullptr) start_worker();

    midnam_iface_ = static_cast<const LV2_Midnam_Interface*>(
        lilv_instance_get_extension_data(instance_, LV2_MIDNAM__interface));
    note_names_.invalidate();

    lilv_instance_activate(instance_);
    activated_ = true;
    quiet_.store(false, std::memory_order_release);
    return true;
  }

  // Stops the instance without freeing it: its state, its editor's
  // instance-access and its buffers all survive, and activate() brings it
  // back. Only the destructor and a sample-rate change free it.
  void deactivate() override {
    if (instance_ == nullptr || !activated_) return;
    // process() must be out of lilv_instance_run before deactivate, and must
    // stay out until activate: quiet_ stays set in between.
    quiet_.store(true, std::memory_order_release);
    wait_until_parked();
    stop_worker();
    lilv_instance_deactivate(instance_);
    activated_ = false;
  }

  void set_transport(const TransportInfo& transport) override {
    transport_ = transport;
    has_transport_ = true;
  }

  void queue_midi(const MidiEvent& event) override {
    if (midi_in_ < 0 || event.size == 0) return;
    if (!midi_queue_admits(pending_midi_count_, pending_midi_.size(), event))
      return;  // block overrun; note-offs get the last of the room
    pending_midi_[pending_midi_count_++] = event;
  }

  // Reads MIDI the plugin wrote to its MIDI atom output port. A step
  // sequencer's entire output lives here.
  size_t take_midi_output(MidiEvent* out, size_t capacity) override {
    if (midi_out_ < 0 || instance_ == nullptr) return 0;

    const auto* sequence = reinterpret_cast<const LV2_Atom_Sequence*>(
        atom_buffers_[atom_in_.size() + static_cast<size_t>(midi_out_)].data());
    if (sequence->atom.type != sequence_urid_) return 0;

    size_t written = 0;
    LV2_ATOM_SEQUENCE_FOREACH(sequence, event) {
      if (written >= capacity) break;
      if (event->body.type != midi_event_urid_) continue;
      if (event->body.size == 0 || event->body.size > 3) continue;

      const auto* data = reinterpret_cast<const uint8_t*>(event + 1);
      MidiEvent& target = out[written++];
      target.frame = static_cast<uint32_t>(event->time.frames);
      target.size = static_cast<uint8_t>(event->body.size);
      std::copy_n(data, event->body.size, target.data);
    }
    return written;
  }

  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override {
    if (instance_ == nullptr) return;

    // The UI thread has the instance: a restore, a deactivate, a buffer
    // change. This block is silence and nothing else, and the counter says
    // we saw it.
    if (quiet_.load(std::memory_order_acquire)) {
      for (int ch = 0; ch < strip_channels_; ++ch)
        std::fill_n(outputs[ch], frames, 0.0f);
      processed_generation_.fetch_add(1, std::memory_order_release);
      return;
    }

    hosting::copy_strip_inputs(inputs, strip_channels_, audio_in_ptrs_.data(),
                               audio_in_ptrs_.size(), frames);

    reset_atom_inputs();
    write_input_events();
    lilv_instance_run(instance_, frames);
    deliver_worker_responses();
    forward_atoms_to_ui();

    hosting::copy_strip_outputs(audio_out_ptrs_.data(), audio_out_ptrs_.size(),
                                outputs, strip_channels_, frames);
  }

  void set_channel_layout(int channels) override { strip_channels_ = channels; }

  // The port is found once in scan_ports, not by comparing strings on the
  // audio thread: this runs twice per strip per block.
  uint32_t latency_samples() const override {
    if (latency_port_ < 0) return 0;
    return static_cast<uint32_t>(
        std::max(0.0f, control_outputs_scratch_[latency_port_]));
  }

  int extra_output_pairs() const override {
    return hosting::extra_output_pairs(audio_out_.size(), strip_channels_);
  }

  void copy_extra_output(int pair, float* left, float* right,
                         uint32_t frames) override {
    hosting::copy_extra_output(audio_out_ptrs_.data(), audio_out_ptrs_.size(),
                               strip_channels_, pair, left, right, frames);
  }

  std::vector<ParameterInfo> parameters() const override {
    std::vector<ParameterInfo> out;
    out.reserve(control_in_.size());
    for (size_t i = 0; i < control_in_.size(); ++i) {
      const PortInfo& port = control_in_[i];
      out.push_back({static_cast<uint32_t>(i), port.name, port.min_value,
                     port.max_value, port.default_value});
    }
    return out;
  }

  double parameter_value(uint32_t id) const override {
    if (id >= control_values_.size()) return 0.0;
    return std::atomic_ref<const float>(control_values_[id])
        .load(std::memory_order_relaxed);
  }

  void set_parameter(uint32_t id, double value) override {
    if (id >= control_values_.size()) return;
    const PortInfo& port = control_in_[id];
    // The plugin reads this float straight from the connected port on the
    // audio thread; an atomic store is the one way to never hand it a torn
    // value.
    std::atomic_ref<float>(control_values_[id])
        .store(std::clamp(static_cast<float>(value), port.min_value, port.max_value),
               std::memory_order_relaxed);
  }

  // Real LV2 state, not just the control ports: a sampler's loaded file or a
  // synth's patch lives in the plugin's own state, and control values alone
  // would restore an empty instrument.
  std::vector<uint8_t> save_state() const override {
    if (instance_ == nullptr) return {};

    LilvState* state = lilv_state_new_from_instance(
        plugin_, instance_, world_->urids.map_feature(),
        nullptr, nullptr, nullptr, nullptr,
        &Lv2Instance::get_port_value, const_cast<Lv2Instance*>(this),
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, map_path_features());
    if (state == nullptr) return {};

    char* text = lilv_state_to_string(world_->world, world_->urids.map_feature(),
                                      world_->urids.unmap_feature(), state,
                                      kStateUri, nullptr);
    lilv_state_free(state);
    if (text == nullptr) return {};

    const std::vector<uint8_t> blob(text, text + std::strlen(text));
    lilv_free(text);
    return blob;
  }

  bool load_state(const std::vector<uint8_t>& blob) override {
    if (instance_ == nullptr || blob.empty()) return false;

    const std::string text(blob.begin(), blob.end());
    LilvState* state = lilv_state_new_from_string(
        world_->world, world_->urids.map_feature(), text.c_str());
    if (state == nullptr) return false;

    // A plugin that does not declare state:threadSafeRestore must not have its
    // state restored while it runs — DrumGizmo quietly shelves the restored
    // kit config in that case and never loads the kit. So the audio thread is
    // parked first: a flag makes process() emit silence, two observed blocks
    // prove it is out of lilv_instance_run, and only then is the instance
    // deactivated, restored with the full feature set, and brought back.
    const bool was_activated = activated_;
    quiet_.store(true, std::memory_order_release);
    const bool parked = wait_until_parked();
    if (!parked) {
      // Never confirmed the audio thread left lilv_instance_run: restoring
      // now would race it, so back out instead of risking a use-after-free.
      quiet_.store(!was_activated, std::memory_order_release);
      lilv_state_free(state);
      return false;
    }

    if (was_activated) lilv_instance_deactivate(instance_);
    lilv_state_restore(state, instance_, &Lv2Instance::set_port_value, this, 0,
                       map_path_features());
    note_names_.invalidate();
    if (was_activated) {
      lilv_instance_activate(instance_);
      quiet_.store(false, std::memory_order_release);
    }

    lilv_state_free(state);
    return true;
  }

  const PluginDescriptor& descriptor() const override { return desc_; }

  std::unique_ptr<PluginGui> create_gui() override {
    if (instance_ == nullptr) return nullptr;
    if (!Lv2Gui::available(world_->world, plugin_)) return nullptr;

    // The bridge outlives the editor: closing and reopening the window reuses
    // it, and the audio thread may be mid-block with it either way.
    if (bridge_ == nullptr) {
      bridge_ = std::make_unique<AtomBridge>();
      bridge_live_.store(bridge_.get(), std::memory_order_release);
    }
    return std::make_unique<Lv2Gui>(this, world_, plugin_, &control_values_,
                                    control_in_, &control_outputs_scratch_,
                                    control_out_, bridge_.get(), &state_dirty_);
  }

  bool take_state_dirty() override {
    const bool dirty = state_dirty_.exchange(false, std::memory_order_acq_rel);
    if (dirty) note_names_.invalidate();
    return dirty;
  }

  std::vector<NoteName> note_names() const override {
    if (note_names_.stale()) refresh_note_names();
    return note_names_.names();
  }

  // For the editor: the instance it is given access to, and a count that moves
  // every time that instance is replaced.
  LV2_Handle instance_handle() const {
    return instance_ != nullptr ? lilv_instance_get_handle(instance_) : nullptr;
  }
  uint64_t instance_generation() const {
    return instance_generation_.load(std::memory_order_acquire);
  }

 private:
  // Waits for two blocks that observed the quiet_ flag, which is what proves
  // the audio thread is out of lilv_instance_run. The counter only moves
  // inside that branch, so no movement at all means process() is not being
  // called — the host graph is already parked, or there is no audio thread.
  // Returns whether the audio thread was confirmed parked (or was never
  // running process() at all). False means the wait timed out with the audio
  // thread still moving, and the caller must not touch the instance.
  bool wait_until_parked() {
    return hosting::wait_for_audio_thread(
        processed_generation_, [this](uint64_t seen) {
          return processed_generation_.load(std::memory_order_acquire) >= seen + 2;
        });
  }

  void destroy_instance() {
    if (instance_ == nullptr) return;
    worker_iface_ = nullptr;
    options_iface_ = nullptr;
    midnam_iface_ = nullptr;
    note_names_.clear();
    lilv_instance_free(instance_);
    instance_ = nullptr;
    // A generation the editor never saw: whichever it was attached to is gone.
    instance_generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  // Room for a full pending_midi_ of three-byte events, each an atom of its
  // own with a frame time: 24 bytes apiece, plus the transport. A plugin may
  // ask for more through rsz:minimumSize, never for less.
  static constexpr size_t kAtomBufferBytes = 32768;

  static size_t atom_buffer_bytes(const PortInfo& port) {
    return std::max<size_t>(kAtomBufferBytes, port.minimum_size);
  }

  // State is kept in memory rather than in a bundle on disk, so the subject URI
  // only has to be stable, not resolvable.
  static constexpr const char* kStateUri = "urn:nirbija:state";

  // lilv asks for and hands back port values by symbol during save and restore.
  static const void* get_port_value(const char* port_symbol, void* user_data,
                                    uint32_t* size, uint32_t* type) {
    auto* self = static_cast<Lv2Instance*>(user_data);
    for (size_t i = 0; i < self->control_in_.size(); ++i) {
      if (self->control_in_[i].symbol != port_symbol) continue;
      *size = sizeof(float);
      *type = self->float_urid_;
      return &self->control_values_[i];
    }
    *size = 0;
    *type = 0;
    return nullptr;
  }

  static void set_port_value(const char* port_symbol, void* user_data,
                             const void* value, uint32_t size, uint32_t type) {
    auto* self = static_cast<Lv2Instance*>(user_data);
    if (size != sizeof(float) || type != self->float_urid_) return;
    for (size_t i = 0; i < self->control_in_.size(); ++i) {
      if (self->control_in_[i].symbol != port_symbol) continue;
      std::atomic_ref<float>(self->control_values_[i])
          .store(*static_cast<const float*>(value), std::memory_order_relaxed);
      return;
    }
  }

  void scan_ports(const PortClasses& classes) {
    const uint32_t count = lilv_plugin_get_num_ports(plugin_);
    std::vector<float> mins(count), maxes(count), defaults(count);
    lilv_plugin_get_port_ranges_float(plugin_, mins.data(), maxes.data(),
                                      defaults.data());

    for (uint32_t i = 0; i < count; ++i) {
      const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
      const bool is_input = lilv_port_is_a(plugin_, port, classes.input);

      PortInfo info;
      info.index = i;
      info.min_value = mins[i];
      info.max_value = maxes[i];
      info.default_value = defaults[i];
      if (LilvNode* name = lilv_port_get_name(plugin_, port)) {
        info.name = lilv_node_as_string(name);
        lilv_node_free(name);
      }
      if (const LilvNode* symbol = lilv_port_get_symbol(plugin_, port))
        info.symbol = lilv_node_as_string(symbol);

      if (lilv_port_is_a(plugin_, port, classes.audio)) {
        (is_input ? audio_in_ : audio_out_).push_back(info);
      } else if (lilv_port_is_a(plugin_, port, classes.control)) {
        if (is_input) {
          control_in_.push_back(info);
          control_values_.push_back(defaults[i]);
        } else {
          control_out_.push_back(info);
        }
      } else if (lilv_port_is_a(plugin_, port, classes.atom)) {
        info.supports_midi = lilv_port_supports_event(plugin_, port, classes.midi_event);
        if (LilvNode* minimum = lilv_port_get(plugin_, port, classes.minimum_size)) {
          if (lilv_node_is_int(minimum) && lilv_node_as_int(minimum) > 0)
            info.minimum_size = static_cast<uint32_t>(lilv_node_as_int(minimum));
          lilv_node_free(minimum);
        }
        (is_input ? atom_in_ : atom_out_).push_back(info);
      } else if (lilv_port_is_a(plugin_, port, classes.cv)) {
        // CV is not routed yet; the port still needs a buffer so the plugin does
        // not write through a null pointer.
        (is_input ? audio_in_ : audio_out_).push_back(info);
      }
    }
    control_outputs_scratch_.assign(control_out_.size(), 0.0f);

    // MIDI goes to the atom port that says it takes MIDI, not to whichever
    // atom port comes first: a plugin with a control atom port ahead of its
    // MIDI port would otherwise never hear a note. A plugin that says nothing
    // gets the first one, which is what it always got.
    auto pick_midi = [](const std::vector<PortInfo>& ports) {
      for (size_t i = 0; i < ports.size(); ++i)
        if (ports[i].supports_midi) return static_cast<int>(i);
      return ports.empty() ? -1 : 0;
    };
    midi_in_ = pick_midi(atom_in_);
    midi_out_ = pick_midi(atom_out_);

    // Latency is the control output the plugin marked lv2:reportsLatency (or
    // designated lv2:latency); lilv resolves both. The symbol "latency" is
    // the fallback for plugins that only named it.
    if (lilv_plugin_has_latency(plugin_)) {
      const uint32_t index = lilv_plugin_get_latency_port_index(plugin_);
      for (size_t i = 0; i < control_out_.size(); ++i) {
        if (control_out_[i].index != index) continue;
        latency_port_ = static_cast<int>(i);
        break;
      }
    }
    if (latency_port_ < 0) {
      for (size_t i = 0; i < control_out_.size(); ++i) {
        if (control_out_[i].symbol == "latency") {
          latency_port_ = static_cast<int>(i);
          break;
        }
      }
    }
  }

  // --- worker -------------------------------------------------------------
  // Plugins that load impulse responses or resize buffers do it here instead of
  // on the audio thread. Requests cross by value through a lock-free queue.

  static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle handle,
                                         uint32_t size, const void* data) {
    auto* self = static_cast<Lv2Instance*>(handle);
    if (size > kWorkPayloadBytes) return LV2_WORKER_ERR_NO_SPACE;
    WorkMessage message;
    message.size = size;
    std::memcpy(message.data.data(), data, size);
    if (!self->work_requests_.push(message)) return LV2_WORKER_ERR_NO_SPACE;
    self->work_signal_.release();
    return LV2_WORKER_SUCCESS;
  }

  static LV2_Worker_Status respond(LV2_Worker_Respond_Handle handle, uint32_t size,
                                   const void* data) {
    auto* self = static_cast<Lv2Instance*>(handle);
    if (size > kWorkPayloadBytes) return LV2_WORKER_ERR_NO_SPACE;
    WorkMessage message;
    message.size = size;
    std::memcpy(message.data.data(), data, size);
    return self->work_responses_.push(message) ? LV2_WORKER_SUCCESS
                                               : LV2_WORKER_ERR_NO_SPACE;
  }

  void start_worker() {
    worker_running_ = true;
    worker_thread_ = std::thread([this] {
      while (true) {
        work_signal_.acquire();
        if (!worker_running_) return;
        WorkMessage message;
        while (work_requests_.pop(message)) {
          worker_iface_->work(lilv_instance_get_handle(instance_), &Lv2Instance::respond,
                              this, message.size, message.data.data());
        }
      }
    });
  }

  void stop_worker() {
    if (!worker_thread_.joinable()) return;
    worker_running_ = false;
    work_signal_.release();
    worker_thread_.join();
  }

  void deliver_worker_responses() {
    if (worker_iface_ == nullptr) return;
    WorkMessage message;
    while (work_responses_.pop(message)) {
      worker_iface_->work_response(lilv_instance_get_handle(instance_), message.size,
                                   message.data.data());
    }
    if (worker_iface_->end_run != nullptr)
      worker_iface_->end_run(lilv_instance_get_handle(instance_));
  }

  // --- options ------------------------------------------------------------

  // The values the option list points at. Members: the plugin keeps the
  // pointers for as long as it lives.
  void set_option_values(uint32_t nominal_block) {
    max_block_length_ = static_cast<int32_t>(max_block_frames_);
    nominal_block_length_ = static_cast<int32_t>(nominal_block);
    min_block_length_ = 1;
    sample_rate_option_ = static_cast<float>(sample_rate_);
    size_t sequence = kAtomBufferBytes;
    for (const PortInfo& port : atom_in_)
      sequence = std::max(sequence, atom_buffer_bytes(port));
    sequence_size_ = static_cast<int32_t>(sequence);
  }

  void build_features(uint32_t max_block_frames) {
    map_feature_ = {LV2_URID__map, urids_.map_feature()};
    unmap_feature_ = {LV2_URID__unmap, urids_.unmap_feature()};

    const LV2_URID int_urid = urids_.map_string(LV2_ATOM__Int);
    const LV2_URID float_urid = urids_.map_string(LV2_ATOM__Float);
    set_option_values(max_block_frames);
    // min is 1, not the block size: JACK hands a shorter block at a period
    // change and on the last cycle before a stop, and a plugin told the
    // minimum was 256 is entitled to assume it. Nominal is what it usually
    // gets, max what it must survive.
    options_ = {
        {LV2_OPTIONS_INSTANCE, 0, urids_.map_string(LV2_BUF_SIZE__maxBlockLength),
         sizeof(int32_t), int_urid, &max_block_length_},
        {LV2_OPTIONS_INSTANCE, 0, urids_.map_string(LV2_BUF_SIZE__minBlockLength),
         sizeof(int32_t), int_urid, &min_block_length_},
        {LV2_OPTIONS_INSTANCE, 0, urids_.map_string(LV2_BUF_SIZE__nominalBlockLength),
         sizeof(int32_t), int_urid, &nominal_block_length_},
        {LV2_OPTIONS_INSTANCE, 0, urids_.map_string(LV2_BUF_SIZE__sequenceSize),
         sizeof(int32_t), int_urid, &sequence_size_},
        {LV2_OPTIONS_INSTANCE, 0, urids_.map_string(LV2_PARAMETERS__sampleRate),
         sizeof(float), float_urid, &sample_rate_option_},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    options_feature_ = {LV2_OPTIONS__options, options_.data()};
    bounded_feature_ = {LV2_BUF_SIZE__boundedBlockLength, nullptr};

    schedule_ = {this, &Lv2Instance::schedule_work};
    worker_feature_ = {LV2_WORKER__schedule, &schedule_};

    map_path_.handle = this;
    map_path_.abstract_path = &Lv2Instance::abstract_path;
    map_path_.absolute_path = &Lv2Instance::absolute_path;
    map_path_feature_ = {LV2_STATE__mapPath, &map_path_};

    midnam_host_.handle = this;
    midnam_host_.update = &Lv2Instance::midnam_changed;
    midnam_feature_ = {LV2_MIDNAM__update, &midnam_host_};

    features_ = {&map_feature_,     &unmap_feature_,  &options_feature_,
                 &bounded_feature_, &worker_feature_, &map_path_feature_,
                 &midnam_feature_,  nullptr};
  }

  // The block sizes moved after instantiation. A plugin with the options
  // interface is told; one without keeps what it was told at instantiation,
  // which is why maxBlockLength only ever grows. Called with the audio thread
  // parked: set() is in the instantiation class and must not overlap run().
  void update_options(uint32_t nominal_block) {
    const int32_t old_max = max_block_length_;
    const int32_t old_nominal = nominal_block_length_;
    set_option_values(nominal_block);
    if (options_iface_ == nullptr || options_iface_->set == nullptr) return;
    if (old_max == max_block_length_ && old_nominal == nominal_block_length_) return;
    const LV2_Options_Option changed[] = {options_[0], options_[2], options_[5]};
    options_iface_->set(lilv_instance_get_handle(instance_), changed);
  }

  static void midnam_changed(void* handle) {
    static_cast<Lv2Instance*>(handle)->note_names_.invalidate();
  }

  void refresh_note_names() const {
    note_names_.refresh([this](std::vector<NoteName>& out) {
      if (instance_ == nullptr || midnam_iface_ == nullptr ||
          midnam_iface_->midnam == nullptr)
        return;
      char* xml = midnam_iface_->midnam(lilv_instance_get_handle(instance_));
      if (xml == nullptr) return;
      out = parse_midnam_notes(xml);
      if (midnam_iface_->free != nullptr) midnam_iface_->free(xml);
    });
  }

  static char* abstract_path(LV2_State_Map_Path_Handle, const char* path) {
    return path != nullptr ? strdup(path) : nullptr;
  }
  static char* absolute_path(LV2_State_Map_Path_Handle, const char* path) {
    return path != nullptr ? strdup(path) : nullptr;
  }

  const LV2_Feature* const* map_path_features() const { return features_.data(); }

  void connect_all() {
    audio_in_ptrs_.clear();
    audio_out_ptrs_.clear();
    for (size_t i = 0; i < audio_in_.size(); ++i) {
      audio_in_ptrs_.push_back(audio_buffers_[i].data());
      lilv_instance_connect_port(instance_, audio_in_[i].index,
                                 audio_buffers_[i].data());
    }
    for (size_t i = 0; i < audio_out_.size(); ++i) {
      audio_out_ptrs_.push_back(audio_buffers_[audio_in_.size() + i].data());
      lilv_instance_connect_port(instance_, audio_out_[i].index,
                                 audio_buffers_[audio_in_.size() + i].data());
    }

    for (size_t i = 0; i < control_in_.size(); ++i)
      lilv_instance_connect_port(instance_, control_in_[i].index,
                                 &control_values_[i]);
    for (size_t i = 0; i < control_out_.size(); ++i)
      lilv_instance_connect_port(instance_, control_out_[i].index,
                                 &control_outputs_scratch_[i]);

    for (size_t i = 0; i < atom_in_.size(); ++i)
      lilv_instance_connect_port(instance_, atom_in_[i].index,
                                 atom_buffers_[i].data());
    for (size_t i = 0; i < atom_out_.size(); ++i)
      lilv_instance_connect_port(instance_, atom_out_[i].index,
                                 atom_buffers_[atom_in_.size() + i].data());
  }

  // Whether `forge` can take `bytes` more. The forge refuses an atom that does
  // not fit, but only after the frame time before it went in - and a time
  // stamp with no atom behind it is a corrupt sequence. So the room is checked
  // for the whole event before either half is written.
  static bool forge_has_room(const LV2_Atom_Forge& forge, size_t bytes) {
    return static_cast<size_t>(forge.offset) + bytes <= forge.size;
  }

  // Builds this block's input sequences: on the MIDI port the transport first,
  // then the MIDI; on every atom input whatever its editor asked to reach the
  // DSP. Everything goes through the forge, because an atom object is not
  // something to hand-assemble.
  //
  // MIDI goes in before the editor's messages: when the block is too full for
  // both it is the editor's message that is dropped, never a note, and never
  // a note-off.
  void write_input_events() {
    if (atom_in_.empty()) return;

    for (size_t k = 0; k < atom_in_.size(); ++k) {
      lv2_atom_forge_set_buffer(&forges_[k], atom_buffers_[k].data(),
                                atom_buffers_[k].size());
      lv2_atom_forge_sequence_head(&forges_[k], &forge_frames_[k], 0);
    }

    if (midi_in_ >= 0) {
      LV2_Atom_Forge& forge = forges_[static_cast<size_t>(midi_in_)];
      if (has_transport_) write_transport(forge);

      for (size_t i = 0; i < pending_midi_count_; ++i) {
        const MidiEvent& event = pending_midi_[i];
        const size_t needed = sizeof(LV2_Atom_Event) + lv2_atom_pad_size(event.size);
        if (!forge_has_room(forge, needed)) break;
        if (lv2_atom_forge_frame_time(&forge, event.frame) == 0) break;
        if (lv2_atom_forge_atom(&forge, event.size, midi_event_urid_) == 0) break;
        if (lv2_atom_forge_write(&forge, event.data, event.size) == 0) break;
      }
    }
    pending_midi_count_ = 0;

    // Anything the editor asked to reach the DSP, injected as events at the
    // start of the block, on the port it named.
    if (AtomBridge* bridge = bridge_live_.load(std::memory_order_acquire)) {
      while (const AtomBridge::Message* message = bridge->to_plugin.peek()) {
        // Popped whatever happens to it: a message that cannot be delivered
        // must not clog the ring for the ones behind it.
        struct Pop {
          AtomBridge* bridge;
          ~Pop() { bridge->to_plugin.pop(); }
        } pop{bridge};

        size_t k = 0;
        while (k < atom_in_.size() && atom_in_[k].index != message->port) ++k;
        if (k == atom_in_.size()) continue;
        if (message->size < sizeof(LV2_Atom)) continue;
        const auto* atom = reinterpret_cast<const LV2_Atom*>(message->data.data());
        // The header's own length field is the editor's word, not ours: an
        // atom claiming more than arrived would read off the end of the ring.
        const size_t total = sizeof(LV2_Atom) + atom->size;
        if (total > message->size) continue;
        LV2_Atom_Forge& forge = forges_[k];
        if (!forge_has_room(forge, sizeof(int64_t) + lv2_atom_pad_size(total)))
          continue;
        if (lv2_atom_forge_frame_time(&forge, 0) == 0) continue;
        lv2_atom_forge_raw(&forge, atom, total);
        lv2_atom_forge_pad(&forge, total);
      }
    }

    for (size_t k = 0; k < atom_in_.size(); ++k)
      lv2_atom_forge_pop(&forges_[k], &forge_frames_[k]);

    if (debug_transport_ && midi_in_ >= 0) {
      const auto* written = reinterpret_cast<const LV2_Atom_Sequence*>(
          atom_buffers_[static_cast<size_t>(midi_in_)].data());
      uint32_t events = 0;
      LV2_ATOM_SEQUENCE_FOREACH(written, event) { (void)event; ++events; }
      std::fprintf(stderr,
                   "lv2 transport: seq type=%u size=%u events=%u speed=%.1f "
                   "beats=%.3f\n",
                   written->atom.type, written->atom.size, events,
                   transport_.playing ? 1.0 : 0.0, transport_.beats);
    }
  }

  // A time:Position object at the start of the block. Without it a sequencer
  // has no clock and simply never advances.
  void write_transport(LV2_Atom_Forge& forge) {
    // Seven properties of a few bytes each; well under this, and a buffer
    // that cannot take it is too small for the MIDI behind it anyway.
    if (!forge_has_room(forge, 256)) return;

    const double beats_per_bar = static_cast<double>(transport_.numerator);
    const double bars = beats_per_bar > 0.0 ? transport_.beats / beats_per_bar : 0.0;
    const double bar = std::floor(bars);

    LV2_Atom_Forge_Frame object;
    lv2_atom_forge_frame_time(&forge, 0);
    lv2_atom_forge_object(&forge, &object, 0, time_position_urid_);

    lv2_atom_forge_key(&forge, time_frame_urid_);
    lv2_atom_forge_long(&forge, static_cast<int64_t>(transport_.frame));
    lv2_atom_forge_key(&forge, time_speed_urid_);
    lv2_atom_forge_float(&forge, transport_.playing ? 1.0f : 0.0f);
    lv2_atom_forge_key(&forge, time_bar_urid_);
    lv2_atom_forge_long(&forge, static_cast<int64_t>(bar));
    lv2_atom_forge_key(&forge, time_bar_beat_urid_);
    lv2_atom_forge_float(&forge,
                         static_cast<float>((bars - bar) * beats_per_bar));
    lv2_atom_forge_key(&forge, time_beats_per_bar_urid_);
    lv2_atom_forge_float(&forge, static_cast<float>(beats_per_bar));
    lv2_atom_forge_key(&forge, time_beat_unit_urid_);
    lv2_atom_forge_int(&forge, transport_.denominator);
    lv2_atom_forge_key(&forge, time_bpm_urid_);
    lv2_atom_forge_float(&forge, static_cast<float>(transport_.tempo_bpm));

    lv2_atom_forge_pop(&forge, &object);
  }

  // Copies what the DSP wrote to its atom outputs into the ring the editor
  // drains, keeping the port index so the UI knows which port spoke.
  void forward_atoms_to_ui() {
    AtomBridge* bridge = bridge_live_.load(std::memory_order_acquire);
    if (bridge == nullptr) return;

    for (size_t out = 0; out < atom_out_.size(); ++out) {
      const auto* sequence = reinterpret_cast<const LV2_Atom_Sequence*>(
          atom_buffers_[atom_in_.size() + out].data());
      if (sequence->atom.type != sequence_urid_) continue;

      LV2_ATOM_SEQUENCE_FOREACH(sequence, event) {
        const uint32_t total = sizeof(LV2_Atom) + event->body.size;
        AtomBridge::Message* message = bridge->to_ui.begin_push();
        if (message == nullptr) return;  // ring full, UI will catch up
        if (total > message->data.size()) continue;  // oversized frame, skip
        message->port = atom_out_[out].index;
        message->size = total;
        std::memcpy(message->data.data(), &event->body, total);
        bridge->to_ui.commit_push();
      }
    }
  }

  // An atom input port must present a valid, empty sequence every block, or the
  // plugin reads whatever the last block left behind.
  void reset_atom_inputs() {
    const LV2_URID sequence_urid = sequence_urid_;
    for (size_t i = 0; i < atom_in_.size(); ++i) {
      auto* sequence = reinterpret_cast<LV2_Atom_Sequence*>(atom_buffers_[i].data());
      sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
      sequence->atom.type = sequence_urid;
      sequence->body.unit = 0;
      sequence->body.pad = 0;
    }
    for (size_t i = 0; i < atom_out_.size(); ++i) {
      auto& buffer = atom_buffers_[atom_in_.size() + i];
      auto* sequence = reinterpret_cast<LV2_Atom_Sequence*>(buffer.data());
      sequence->atom.size = static_cast<uint32_t>(buffer.size() - sizeof(LV2_Atom));
      sequence->atom.type = sequence_urid;
    }
  }

  PluginDescriptor desc_;
  const LilvPlugin* plugin_;
  std::shared_ptr<Lv2World> world_;
  UridMap& urids_;
  LilvInstance* instance_ = nullptr;
  bool activated_ = false;
  // Moves whenever instance_ is created or freed; the editor compares.
  std::atomic<uint64_t> instance_generation_{0};

  int strip_channels_ = 2;
  double sample_rate_ = 0.0;
  uint32_t max_block_frames_ = 0;
  // Index into control_out_ of the latency port, or -1 for none.
  int latency_port_ = -1;
  // Index into atom_in_/atom_out_ of the port MIDI travels on, or -1.
  int midi_in_ = -1;
  int midi_out_ = -1;
  // Read once at construction, not per block: getenv walks the environment.
  const bool debug_transport_ = std::getenv("NIRBIJA_DEBUG_TRANSPORT") != nullptr;

  std::vector<PortInfo> audio_in_, audio_out_, control_in_, control_out_;
  std::vector<PortInfo> atom_in_, atom_out_;
  std::vector<float> control_values_;
  std::vector<float> control_outputs_scratch_;
  std::vector<std::vector<float>> audio_buffers_;
  std::vector<float*> audio_in_ptrs_, audio_out_ptrs_;
  std::vector<std::vector<uint8_t>> atom_buffers_;

  LV2_URID sequence_urid_ = urids_.map_string(LV2_ATOM__Sequence);
  LV2_URID midi_event_urid_ = urids_.map_string(LV2_MIDI__MidiEvent);
  LV2_URID float_urid_ = urids_.map_string(LV2_ATOM__Float);
  LV2_URID time_position_urid_ = urids_.map_string(LV2_TIME__Position);
  LV2_URID time_frame_urid_ = urids_.map_string(LV2_TIME__frame);
  LV2_URID time_speed_urid_ = urids_.map_string(LV2_TIME__speed);
  LV2_URID time_bar_urid_ = urids_.map_string(LV2_TIME__bar);
  LV2_URID time_bar_beat_urid_ = urids_.map_string(LV2_TIME__barBeat);
  LV2_URID time_beats_per_bar_urid_ = urids_.map_string(LV2_TIME__beatsPerBar);
  LV2_URID time_beat_unit_urid_ = urids_.map_string(LV2_TIME__beatUnit);
  LV2_URID time_bpm_urid_ = urids_.map_string(LV2_TIME__beatsPerMinute);

  // One forge per atom input, so a block's events can be spread over several
  // ports in one pass.
  LV2_Atom_Forge forge_{};
  std::vector<LV2_Atom_Forge> forges_;
  std::vector<LV2_Atom_Forge_Frame> forge_frames_;
  TransportInfo transport_;
  bool has_transport_ = false;

  // process() emits silence and counts the block while this is set: the UI
  // thread has the instance (restore, deactivate, buffer change).
  std::atomic<bool> quiet_{true};
  std::atomic<bool> state_dirty_{false};
  std::atomic<uint64_t> processed_generation_{0};

  // Fixed so queueing never allocates on the audio thread. A block carrying
  // more than this is a chord nobody plays.
  std::array<MidiEvent, 1024> pending_midi_{};
  size_t pending_midi_count_ = 0;
  int32_t max_block_length_ = 0;
  int32_t min_block_length_ = 1;
  int32_t nominal_block_length_ = 0;
  int32_t sequence_size_ = 0;
  float sample_rate_option_ = 0.0f;
  LV2_Feature map_feature_{}, unmap_feature_{}, options_feature_{}, bounded_feature_{};
  LV2_Feature worker_feature_{}, map_path_feature_{}, midnam_feature_{};
  LV2_Midnam midnam_host_{};
  const LV2_Midnam_Interface* midnam_iface_ = nullptr;
  const LV2_Options_Interface* options_iface_ = nullptr;
  hosting::NoteNameCache note_names_;
  LV2_State_Map_Path map_path_{};
  LV2_Worker_Schedule schedule_{};
  std::vector<LV2_Options_Option> options_;
  std::vector<const LV2_Feature*> features_;

  const LV2_Worker_Interface* worker_iface_ = nullptr;
  std::unique_ptr<AtomBridge> bridge_;
  std::atomic<AtomBridge*> bridge_live_{nullptr};
  std::thread worker_thread_;
  std::atomic<bool> worker_running_{false};
  std::counting_semaphore<> work_signal_{0};
  RtQueue<WorkMessage, 32> work_requests_;
  RtQueue<WorkMessage, 32> work_responses_;
};

LV2_Handle Lv2Gui::current_handle() const {
  attached_generation_ = owner_->instance_generation();
  return owner_->instance_handle();
}

bool Lv2Gui::instance_stale() const {
  return owner_->instance_generation() != attached_generation_;
}

class Lv2Backend : public PluginBackend {
 public:
  Lv2Backend() : world_(std::make_shared<Lv2World>()) {}

  PluginFormat format() const override { return PluginFormat::Lv2; }

  std::vector<PluginDescriptor> scan() override {
    std::vector<PluginDescriptor> found;
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world_->world);
    LILV_FOREACH(plugins, iter, plugins) {
      found.push_back(describe(lilv_plugins_get(plugins, iter)));
    }
    return found;
  }

  std::unique_ptr<PluginInstance> instantiate(const PluginDescriptor& desc) override {
    LilvNode* uri = lilv_new_uri(world_->world, desc.uid.c_str());
    if (uri == nullptr) return nullptr;
    const LilvPlugin* plugin =
        lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_->world), uri);
    lilv_node_free(uri);
    if (plugin == nullptr) return nullptr;

    return std::make_unique<Lv2Instance>(describe(plugin), plugin, world_);
  }

 private:
  PluginDescriptor describe(const LilvPlugin* plugin) {
    PluginDescriptor desc;
    desc.format = PluginFormat::Lv2;
    desc.uid = lilv_node_as_uri(lilv_plugin_get_uri(plugin));

    if (LilvNode* name = lilv_plugin_get_name(plugin)) {
      desc.name = lilv_node_as_string(name);
      lilv_node_free(name);
    }
    if (LilvNode* author = lilv_plugin_get_author_name(plugin)) {
      desc.vendor = lilv_node_as_string(author);
      lilv_node_free(author);
    }
    if (const LilvNode* bundle = lilv_plugin_get_bundle_uri(plugin))
      desc.path = lilv_node_as_uri(bundle);

    desc.audio_inputs = static_cast<int>(lilv_plugin_get_num_ports_of_class(
        plugin, world_->classes->input, world_->classes->audio, nullptr));
    desc.audio_outputs = static_cast<int>(lilv_plugin_get_num_ports_of_class(
        plugin, world_->classes->output, world_->classes->audio, nullptr));
    desc.has_midi_input = lilv_plugin_get_num_ports_of_class(
                              plugin, world_->classes->input, world_->classes->atom, nullptr) > 0;

    // LV2 states its class as a URI in a hierarchy - lv2:ReverbPlugin is under
    // lv2:DelayPlugin is under lv2:Plugin - and carries a human label beside
    // it. The label is what the picker shows; the URI is what it classifies by,
    // since a label is free text and a URI is not.
    if (const LilvPluginClass* klass = lilv_plugin_get_class(plugin)) {
      if (const LilvNode* label = lilv_plugin_class_get_label(klass))
        desc.category = lilv_node_as_string(label);
      if (const LilvNode* uri = lilv_plugin_class_get_uri(klass))
        desc.kind = kind_from_class_uri(lilv_node_as_uri(uri));
    }
    // Ports outrank the label. A plugin with no audio ports at all cannot be
    // an audio effect whatever its class says, and the LV2 class for MIDI has
    // no constant in the core header to compare against - the x42 MIDI suite
    // calls itself "MIDI", the one before it "Utility Plugin", and both only
    // move notes around.
    if (desc.audio_inputs == 0 && desc.audio_outputs == 0 && desc.has_midi_input)
      desc.kind = PluginKind::MidiEffect;
    else if (desc.kind == PluginKind::Unknown)
      desc.kind = kind_from_ports(desc.audio_inputs, desc.audio_outputs,
                                  desc.has_midi_input);
    return desc;
  }

  // Only the handful of classes that change which bucket a plugin lands in.
  // Everything under lv2:Plugin that is not called out here is an effect,
  // which is true of the great majority of them.
  static PluginKind kind_from_class_uri(const char* uri) {
    if (uri == nullptr) return PluginKind::Unknown;
    const std::string_view u(uri);
    if (u == LV2_CORE__InstrumentPlugin) return PluginKind::Instrument;
    if (u == LV2_CORE__AnalyserPlugin) return PluginKind::Analyzer;
    if (u == LV2_CORE__UtilityPlugin || u == LV2_CORE__MixerPlugin ||
        u == LV2_CORE__GeneratorPlugin)
      return PluginKind::Utility;
    // lv2:Plugin itself is the root: saying only "I am a plugin" is saying
    // nothing, so let the port counts decide instead.
    if (u == LV2_CORE__Plugin) return PluginKind::Unknown;
    return PluginKind::Effect;
  }

  std::shared_ptr<Lv2World> world_;
};

}  // namespace

std::unique_ptr<PluginBackend> make_lv2_backend() {
  return std::make_unique<Lv2Backend>();
}

}  // namespace nirbija
