#include "hosting/lv2_backend.h"

#include <lilv/lilv.h>
#include <dlfcn.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/state/state.h>
#include <lv2/midi/midi.h>
#include <lv2/ui/ui.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <atomic>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include "core/rt_queue.h"

namespace nirbija {
namespace {

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

enum class PortKind { Ignored, AudioIn, AudioOut, ControlIn, ControlOut, AtomIn, AtomOut };

struct PortInfo {
  uint32_t index = 0;
  PortKind kind = PortKind::Ignored;
  std::string name;
  std::string symbol;  // LV2 state keys on this, not on the display name
  float min_value = 0.0f;
  float max_value = 1.0f;
  float default_value = 0.0f;
};

// Cached lilv URIs, built once per world.
struct PortClasses {
  explicit PortClasses(LilvWorld* world)
      : audio(lilv_new_uri(world, LV2_CORE__AudioPort)),
        control(lilv_new_uri(world, LV2_CORE__ControlPort)),
        cv(lilv_new_uri(world, LV2_CORE__CVPort)),
        atom(lilv_new_uri(world, LV2_ATOM__AtomPort)),
        input(lilv_new_uri(world, LV2_CORE__InputPort)),
        output(lilv_new_uri(world, LV2_CORE__OutputPort)) {}

  ~PortClasses() {
    for (LilvNode* node : {audio, control, cv, atom, input, output})
      lilv_node_free(node);
  }

  LilvNode* audio;
  LilvNode* control;
  LilvNode* cv;
  LilvNode* atom;
  LilvNode* input;
  LilvNode* output;
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
class Lv2Gui : public PluginGui {
 public:
  Lv2Gui(std::shared_ptr<Lv2World> world, const LilvPlugin* plugin,
         LilvInstance* instance, std::vector<float>* control_values,
         std::vector<PortInfo> control_ports)
      : world_(std::move(world)),
        plugin_(plugin),
        instance_(instance),
        control_values_(control_values),
        control_ports_(std::move(control_ports)) {}

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

  void idle() override {
    if (idle_iface_ != nullptr && handle_ != nullptr) idle_iface_->idle(handle_);
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

  void instantiate(const char* bundle_path, uintptr_t parent_window) {
    parent_feature_ = {LV2_UI__parent, reinterpret_cast<void*>(parent_window)};
    instance_feature_ = {LV2_INSTANCE_ACCESS_URI,
                         lilv_instance_get_handle(instance_)};
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
  }

  static int request_resize(LV2UI_Feature_Handle handle, int width, int height) {
    auto* self = static_cast<Lv2Gui*>(handle);
    self->requested_width_ = width;
    self->requested_height_ = height;
    return 0;
  }

  // The editor writes a control value back to the host. Only plain float
  // control ports are handled; anything atom-shaped needs the MIDI work first.
  static void write_port(LV2UI_Controller controller, uint32_t port_index,
                         uint32_t buffer_size, uint32_t format, const void* buffer) {
    auto* self = static_cast<Lv2Gui*>(controller);
    if (format != 0 || buffer_size != sizeof(float)) return;

    const float value = *static_cast<const float*>(buffer);
    for (size_t i = 0; i < self->control_ports_.size(); ++i) {
      if (self->control_ports_[i].index != port_index) continue;
      (*self->control_values_)[i] = value;
      return;
    }
  }

  std::shared_ptr<Lv2World> world_;
  const LilvPlugin* plugin_;
  LilvInstance* instance_;
  std::vector<float>* control_values_;
  std::vector<PortInfo> control_ports_;

  void* library_ = nullptr;
  const LV2UI_Descriptor* descriptor_ = nullptr;
  LV2UI_Handle handle_ = nullptr;
  LV2UI_Widget widget_ = nullptr;
  const LV2UI_Idle_Interface* idle_iface_ = nullptr;
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

  ~Lv2Instance() override { deactivate(); }

  bool activate(double sample_rate, uint32_t max_block_frames) override {
    if (instance_ != nullptr) deactivate();
    max_block_frames_ = max_block_frames;

    // Features are handed to the plugin by pointer and must outlive it, so they
    // live in members rather than locals.
    build_features(max_block_frames);

    instance_ = lilv_plugin_instantiate(plugin_, sample_rate, features_.data());
    if (instance_ == nullptr) return false;

    // Audio buffers are per-port and owned here, so a plugin with more ports
    // than the strip is wide still gets a valid buffer for every one.
    audio_buffers_.assign(audio_in_.size() + audio_out_.size(),
                          std::vector<float>(max_block_frames, 0.0f));
    // Atom ports get a small buffer each: inputs an empty sequence, outputs
    // scratch the plugin may fill and we discard until MIDI lands.
    atom_buffers_.assign(atom_in_.size() + atom_out_.size(),
                         std::vector<uint8_t>(kAtomBufferBytes, 0));

    connect_all();

    worker_iface_ = static_cast<const LV2_Worker_Interface*>(
        lilv_instance_get_extension_data(instance_, LV2_WORKER__interface));
    if (worker_iface_ != nullptr) start_worker();

    lilv_instance_activate(instance_);
    return true;
  }

  void deactivate() override {
    if (instance_ == nullptr) return;
    lilv_instance_deactivate(instance_);
    stop_worker();
    worker_iface_ = nullptr;
    lilv_instance_free(instance_);
    instance_ = nullptr;
  }

  void queue_midi(const MidiEvent& event) override {
    if (atom_in_.empty() || event.size == 0) return;
    if (pending_midi_count_ >= pending_midi_.size()) return;  // block overrun
    pending_midi_[pending_midi_count_++] = event;
  }

  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override {
    if (instance_ == nullptr) return;

    // The strip is narrower than the plugin as often as not. Extra plugin inputs
    // get a copy of the last channel we have rather than silence, which is what
    // a mono source into a stereo effect should sound like.
    const int strip_channels = strip_channels_;
    for (size_t i = 0; i < audio_in_.size(); ++i) {
      const int source = std::min(static_cast<int>(i), strip_channels - 1);
      std::copy_n(inputs[source], frames, audio_buffers_[i].data());
    }

    reset_atom_inputs();
    write_pending_midi();
    lilv_instance_run(instance_, frames);
    deliver_worker_responses();

    const size_t out_base = audio_in_.size();
    for (int ch = 0; ch < strip_channels; ++ch) {
      if (audio_out_.empty()) break;
      const size_t source =
          out_base + std::min(static_cast<size_t>(ch), audio_out_.size() - 1);
      std::copy_n(audio_buffers_[source].data(), frames, outputs[ch]);
    }
  }

  void set_channel_layout(int channels) override { strip_channels_ = channels; }

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
    return control_values_[id];
  }

  void set_parameter(uint32_t id, double value) override {
    if (id >= control_values_.size()) return;
    const PortInfo& port = control_in_[id];
    control_values_[id] = std::clamp(static_cast<float>(value), port.min_value,
                                     port.max_value);
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
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, nullptr);
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

    lilv_state_restore(state, instance_, &Lv2Instance::set_port_value, this, 0,
                       nullptr);
    lilv_state_free(state);
    return true;
  }

  const PluginDescriptor& descriptor() const override { return desc_; }

  std::unique_ptr<PluginGui> create_gui() override {
    if (instance_ == nullptr) return nullptr;
    if (!Lv2Gui::available(world_->world, plugin_)) return nullptr;
    return std::make_unique<Lv2Gui>(world_, plugin_, instance_, &control_values_,
                                    control_in_);
  }

 private:
  static constexpr size_t kAtomBufferBytes = 4096;

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
      self->control_values_[i] = *static_cast<const float*>(value);
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
        (is_input ? atom_in_ : atom_out_).push_back(info);
      } else if (lilv_port_is_a(plugin_, port, classes.cv)) {
        // CV is not routed yet; the port still needs a buffer so the plugin does
        // not write through a null pointer.
        (is_input ? audio_in_ : audio_out_).push_back(info);
      }
    }
    control_outputs_scratch_.assign(control_out_.size(), 0.0f);
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

  void build_features(uint32_t max_block_frames) {
    map_feature_ = {LV2_URID__map, urids_.map_feature()};
    unmap_feature_ = {LV2_URID__unmap, urids_.unmap_feature()};

    const LV2_URID int_urid = urids_.map_string(LV2_ATOM__Int);
    block_length_ = static_cast<int32_t>(max_block_frames);
    options_ = {
        {LV2_OPTIONS_INSTANCE, 0, urids_.map_string(LV2_BUF_SIZE__maxBlockLength),
         sizeof(int32_t), int_urid, &block_length_},
        {LV2_OPTIONS_INSTANCE, 0, urids_.map_string(LV2_BUF_SIZE__minBlockLength),
         sizeof(int32_t), int_urid, &block_length_},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    options_feature_ = {LV2_OPTIONS__options, options_.data()};
    bounded_feature_ = {LV2_BUF_SIZE__boundedBlockLength, nullptr};

    schedule_ = {this, &Lv2Instance::schedule_work};
    worker_feature_ = {LV2_WORKER__schedule, &schedule_};

    features_ = {&map_feature_,  &unmap_feature_,  &options_feature_,
                 &bounded_feature_, &worker_feature_, nullptr};
  }

  void connect_all() {
    for (size_t i = 0; i < audio_in_.size(); ++i)
      lilv_instance_connect_port(instance_, audio_in_[i].index,
                                 audio_buffers_[i].data());
    for (size_t i = 0; i < audio_out_.size(); ++i)
      lilv_instance_connect_port(instance_, audio_out_[i].index,
                                 audio_buffers_[audio_in_.size() + i].data());

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

  // Appends this block's MIDI to the first atom input port, which is where a
  // synth listens. Events are already in order because the engine reads them
  // from JACK in order.
  void write_pending_midi() {
    if (atom_in_.empty() || pending_midi_count_ == 0) return;

    auto* sequence = reinterpret_cast<LV2_Atom_Sequence*>(atom_buffers_[0].data());
    uint8_t* const base = atom_buffers_[0].data();
    const size_t capacity = kAtomBufferBytes;

    for (size_t i = 0; i < pending_midi_count_; ++i) {
      const MidiEvent& event = pending_midi_[i];
      const size_t offset = sizeof(LV2_Atom) + sequence->atom.size;
      const size_t needed = sizeof(LV2_Atom_Event) + lv2_atom_pad_size(event.size);
      if (offset + needed > capacity) break;

      auto* atom_event = reinterpret_cast<LV2_Atom_Event*>(base + offset);
      atom_event->time.frames = event.frame;
      atom_event->body.size = event.size;
      atom_event->body.type = midi_event_urid_;
      std::memcpy(atom_event + 1, event.data, event.size);

      sequence->atom.size += static_cast<uint32_t>(needed);
    }

    pending_midi_count_ = 0;
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
      auto* sequence =
          reinterpret_cast<LV2_Atom_Sequence*>(atom_buffers_[atom_in_.size() + i].data());
      sequence->atom.size = kAtomBufferBytes - sizeof(LV2_Atom);
      sequence->atom.type = sequence_urid;
    }
  }

  PluginDescriptor desc_;
  const LilvPlugin* plugin_;
  std::shared_ptr<Lv2World> world_;
  UridMap& urids_;
  LilvInstance* instance_ = nullptr;

  int strip_channels_ = 2;
  uint32_t max_block_frames_ = 0;

  std::vector<PortInfo> audio_in_, audio_out_, control_in_, control_out_;
  std::vector<PortInfo> atom_in_, atom_out_;
  std::vector<float> control_values_;
  std::vector<float> control_outputs_scratch_;
  std::vector<std::vector<float>> audio_buffers_;
  std::vector<std::vector<uint8_t>> atom_buffers_;

  LV2_URID sequence_urid_ = urids_.map_string(LV2_ATOM__Sequence);
  LV2_URID midi_event_urid_ = urids_.map_string(LV2_MIDI__MidiEvent);
  LV2_URID float_urid_ = urids_.map_string(LV2_ATOM__Float);

  // Fixed so queueing never allocates on the audio thread. A block carrying
  // more than this is a chord nobody plays.
  std::array<MidiEvent, 64> pending_midi_{};
  size_t pending_midi_count_ = 0;
  int32_t block_length_ = 0;
  LV2_Feature map_feature_{}, unmap_feature_{}, options_feature_{}, bounded_feature_{};
  LV2_Feature worker_feature_{};
  LV2_Worker_Schedule schedule_{};
  std::vector<LV2_Options_Option> options_;
  std::vector<const LV2_Feature*> features_;

  const LV2_Worker_Interface* worker_iface_ = nullptr;
  std::thread worker_thread_;
  std::atomic<bool> worker_running_{false};
  std::counting_semaphore<> work_signal_{0};
  RtQueue<WorkMessage, 32> work_requests_;
  RtQueue<WorkMessage, 32> work_responses_;
};

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
    return desc;
  }

  std::shared_ptr<Lv2World> world_;
};

}  // namespace

std::unique_ptr<PluginBackend> make_lv2_backend() {
  return std::make_unique<Lv2Backend>();
}

}  // namespace nirbija
