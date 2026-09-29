// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "hosting/clap_backend.h"

#include "core/rt_queue.h"
#include "hosting/clap_events.h"
#include "hosting/common.h"
#include "hosting/gui_resize.h"

#include <clap/clap.h>
#include <dlfcn.h>

#include <poll.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nirbija {
namespace {

namespace fs = std::filesystem;

std::vector<fs::path> clap_search_paths() {
  std::vector<fs::path> paths{"/usr/lib/clap", "/usr/local/lib/clap"};
  if (const char* home = std::getenv("HOME")) paths.emplace_back(fs::path(home) / ".clap");
  if (const char* extra = std::getenv("CLAP_PATH")) paths.emplace_back(extra);
  return paths;
}

// A dlopen'd .clap module. Instances keep it alive; unloading while a plugin
// from it is still running would pull the code out from under the audio thread.
class ClapModule {
 public:
  static std::shared_ptr<ClapModule> open(const fs::path& path) {
    hosting::module_open_count().fetch_add(1, std::memory_order_relaxed);
    void* handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr) return nullptr;

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (entry == nullptr || !entry->init(path.c_str())) {
      dlclose(handle);
      return nullptr;
    }
    return std::shared_ptr<ClapModule>(new ClapModule(handle, entry));
  }

  ~ClapModule() {
    entry_->deinit();
    dlclose(handle_);
  }

  const clap_plugin_factory_t* factory() const {
    return static_cast<const clap_plugin_factory_t*>(
        entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
  }

 private:
  ClapModule(void* handle, const clap_plugin_entry_t* entry)
      : handle_(handle), entry_(entry) {}

  void* handle_;
  const clap_plugin_entry_t* entry_;
};

// A CLAP editor embedded into a host window. CLAP hands the plugin a parent
// window and lets it draw inside; nothing is reparented behind its back.
class ClapInstance;

class ClapGui : public PluginGui, public hosting::ResizablePluginGui {
 public:
  ClapGui(ClapInstance* owner, const clap_plugin_t* plugin,
          const clap_plugin_gui_t* gui);

  ~ClapGui() override;

  bool attach(uintptr_t parent_window) override {
    if (created_) detach();
    if (!gui_->create(plugin_, CLAP_WINDOW_API_X11, false)) return false;
    created_ = true;
    close_requested_.store(false, std::memory_order_relaxed);
    pending_resize_.store(0, std::memory_order_relaxed);

    // The order the extension prescribes: scale, then size, then parent, then
    // show. A plugin told its parent before its size lays out once at some
    // default and again at the real one, and a few only ever do the first.
    gui_->set_scale(plugin_, 1.0);
    resizable_ = gui_->can_resize(plugin_);

    // The plugin knows what size it wants, but it will not lay itself out
    // until the host confirms one, so ask and then tell. A fixed-size editor
    // is not told: set_size on one is a contract violation some enforce.
    uint32_t width = 0;
    uint32_t height = 0;
    if (gui_->get_size(plugin_, &width, &height) && width > 0 && height > 0 &&
        resizable_)
      gui_->set_size(plugin_, width, height);

    clap_window_t window{};
    window.api = CLAP_WINDOW_API_X11;
    window.x11 = static_cast<unsigned long>(parent_window);
    if (!gui_->set_parent(plugin_, &window)) {
      detach();
      return false;
    }

    gui_->show(plugin_);
    return true;
  }

  void detach() override {
    if (!created_) return;
    gui_->hide(plugin_);
    gui_->destroy(plugin_);
    created_ = false;
  }

  // A CLAP plugin does not run its own event loop on Linux: it hands the host
  // timers and file descriptors and expects to be called back. Without this an
  // editor creates its window and then never paints a single pixel.
  int idle() override;

  bool preferred_size(int* width, int* height) const override {
    uint32_t w = 0;
    uint32_t h = 0;
    if (!created_ || !gui_->get_size(plugin_, &w, &h)) return false;
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    return true;
  }

  // --- hosting::ResizablePluginGui ---
  // The plugin asked, through clap_host_gui.request_resize, for its client
  // area to be this big. Returned once per request; the window that embeds the
  // editor is expected to poll this right after idle(), resize itself, and
  // then call resized() with what it settled on.
  bool take_resize_request(int* width, int* height) override {
    const uint64_t packed = pending_resize_.exchange(0, std::memory_order_acq_rel);
    if (packed == 0) return false;
    *width = static_cast<int>(packed >> 32);
    *height = static_cast<int>(packed & 0xffffffffu);
    return true;
  }

  // The embedding window is now `width` x `height`. Tells a resizable editor
  // so it can lay out; a fixed one already is what it is.
  void resized(int width, int height) override {
    if (!created_ || !resizable_ || width <= 0 || height <= 0) return;
    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    // adjust_size lets the plugin round to a size it can actually draw at -
    // an aspect ratio, a step - before it is committed to.
    gui_->adjust_size(plugin_, &w, &h);
    gui_->set_size(plugin_, w, h);
  }

  bool resizable() const override { return resizable_; }

  // What the plugin would make of a size the user is dragging towards.
  bool constrain_size(int* width, int* height) const override {
    if (!created_ || !resizable_ || *width <= 0 || *height <= 0) return false;
    uint32_t w = static_cast<uint32_t>(*width);
    uint32_t h = static_cast<uint32_t>(*height);
    if (!gui_->adjust_size(plugin_, &w, &h)) return false;
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    return true;
  }

 private:
  friend class ClapInstance;

  ClapInstance* owner_;
  const clap_plugin_t* plugin_;
  const clap_plugin_gui_t* gui_;
  bool created_ = false;
  bool resizable_ = false;
  // width << 32 | height, zero for none. Written from whatever thread the
  // plugin calls request_resize on, read on the main thread.
  std::atomic<uint64_t> pending_resize_{0};
  std::atomic<bool> close_requested_{false};
};

class ClapInstance : public PluginInstance {
 public:
  ClapInstance(PluginDescriptor desc, std::shared_ptr<ClapModule> module)
      : desc_(std::move(desc)), module_(std::move(module)) {
    host_.clap_version = CLAP_VERSION;
    host_.host_data = this;
    host_.name = "Nirbija";
    host_.vendor = "Nirbija";
    host_.url = "";
    host_.version = "0.1.0";
    host_.get_extension = &ClapInstance::host_get_extension;

    timer_support_.register_timer = &ClapInstance::host_register_timer;
    timer_support_.unregister_timer = &ClapInstance::host_unregister_timer;
    fd_support_.register_fd = &ClapInstance::host_register_fd;
    fd_support_.modify_fd = &ClapInstance::host_modify_fd;
    fd_support_.unregister_fd = &ClapInstance::host_unregister_fd;
    host_.request_restart = &ClapInstance::host_request_restart;
    host_.request_process = &ClapInstance::host_request_process;
    host_.request_callback = &ClapInstance::host_request_callback;

    // How a CLAP plugin says "my state moved, ask me for it again". Without
    // answering this, a patch loaded from the plugin's own editor never
    // reached the session file.
    state_support_.mark_dirty = &ClapInstance::host_mark_dirty;
    note_name_host_.changed = &ClapInstance::host_note_name_changed;

    latency_host_.changed = &ClapInstance::host_latency_changed;
    params_host_.rescan = &ClapInstance::host_params_rescan;
    params_host_.clear = &ClapInstance::host_params_clear;
    params_host_.request_flush = &ClapInstance::host_params_request_flush;
    gui_host_.resize_hints_changed = &ClapInstance::host_gui_resize_hints_changed;
    gui_host_.request_resize = &ClapInstance::host_gui_request_resize;
    gui_host_.request_show = &ClapInstance::host_gui_request_show;
    gui_host_.request_hide = &ClapInstance::host_gui_request_hide;
    gui_host_.closed = &ClapInstance::host_gui_closed;
    audio_ports_host_.is_rescan_flag_supported =
        &ClapInstance::host_audio_ports_is_rescan_flag_supported;
    audio_ports_host_.rescan = &ClapInstance::host_audio_ports_rescan;

    discard_out_events_.ctx = nullptr;
    discard_out_events_.try_push = &ClapInstance::discard_event;
  }

  ~ClapInstance() override { destroy(); }

  bool create() {
    const clap_plugin_factory_t* factory = module_->factory();
    if (factory == nullptr) return false;
    plugin_ = factory->create_plugin(factory, &host_, desc_.uid.c_str());
    if (plugin_ == nullptr) return false;
    if (!plugin_->init(plugin_)) {
      plugin_->destroy(plugin_);
      plugin_ = nullptr;
      return false;
    }

    params_ = static_cast<const clap_plugin_params_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    state_ = static_cast<const clap_plugin_state_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    audio_ports_ = static_cast<const clap_plugin_audio_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
    gui_ = static_cast<const clap_plugin_gui_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_GUI));
    plugin_timers_ = static_cast<const clap_plugin_timer_support_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_TIMER_SUPPORT));
    plugin_fds_ = static_cast<const clap_plugin_posix_fd_support_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_POSIX_FD_SUPPORT));
    plugin_latency_ = static_cast<const clap_plugin_latency_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));

    read_port_counts();
    // Whether it takes notes comes from its note ports, once there is an
    // instance to ask. The scan guessed from the feature list; this is the
    // plugin's own word - and so is the dialect: a synth that only speaks
    // CLAP notes hears nothing in a MIDI event, and would stay silent.
    if (const auto* note_ports = static_cast<const clap_plugin_note_ports_t*>(
            plugin_->get_extension(plugin_, CLAP_EXT_NOTE_PORTS))) {
      desc_.has_midi_input = note_ports->count(plugin_, true) > 0;
      clap_note_port_info_t info{};
      if (desc_.has_midi_input && note_ports->get(plugin_, 0, true, &info))
        dialect_ = hosting::ClapNoteDialect::from_port(info.supported_dialects,
                                                       info.preferred_dialect);
    }
    return true;
  }

  void set_channel_layout(int channels) override { strip_channels_ = channels; }

  bool activate(double sample_rate, uint32_t max_block_frames) override {
    if (plugin_ == nullptr) return false;
    if (active_) deactivate();

    if (!plugin_->activate(plugin_, sample_rate, 1, max_block_frames)) return false;
    active_ = true;
    sample_rate_ = sample_rate;
    max_block_ = max_block_frames;

    input_channels_.assign(std::max(desc_.audio_inputs, 1),
                           std::vector<float>(max_block_frames, 0.0f));
    output_channels_.assign(std::max(desc_.audio_outputs, 1),
                            std::vector<float>(max_block_frames, 0.0f));
    input_ptrs_.clear();
    output_ptrs_.clear();
    for (auto& channel : input_channels_) input_ptrs_.push_back(channel.data());
    for (auto& channel : output_channels_) output_ptrs_.push_back(channel.data());
    // latency.get is allowed while active or being activated, which this is;
    // it is not allowed once deactivated, so this is the moment to ask.
    refresh_latency();
    latency_dirty_.store(false, std::memory_order_relaxed);

    want_processing_.store(true, std::memory_order_release);
    return true;
  }

  void deactivate() override {
    if (plugin_ == nullptr || !active_) return;
    want_processing_.store(false, std::memory_order_release);

    // Wait for process() to service the stop request (and thus call
    // stop_processing on the audio thread) before calling plugin_->deactivate,
    // which CLAP requires to happen only once processing has stopped. If
    // nothing is calling process() at all, there is nothing to wait for.
    hosting::wait_for_audio_thread(process_generation_, [this](uint64_t) {
      return !processing_.load(std::memory_order_acquire);
    });

    plugin_->deactivate(plugin_);
    active_ = false;
  }

  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override {
    process_generation_.fetch_add(1, std::memory_order_release);

    const bool want = want_processing_.load(std::memory_order_acquire);
    bool proc = processing_.load(std::memory_order_relaxed);
    if (want && !proc) {
      proc = plugin_->start_processing(plugin_);
      processing_.store(proc, std::memory_order_release);
      // Don't hammer a plugin that refuses to start on every single block.
      if (!proc) want_processing_.store(false, std::memory_order_release);
    } else if (!want && proc) {
      plugin_->stop_processing(plugin_);
      proc = false;
      processing_.store(false, std::memory_order_release);
    }
    if (!proc) return;

    hosting::copy_strip_inputs(inputs, strip_channels_, input_ptrs_.data(),
                               input_ptrs_.size(), frames);

    clap_audio_buffer_t in_bus{};
    in_bus.data32 = input_ptrs_.data();
    in_bus.channel_count = static_cast<uint32_t>(input_ptrs_.size());

    clap_audio_buffer_t out_bus{};
    out_bus.data32 = output_ptrs_.data();
    out_bus.channel_count = static_cast<uint32_t>(output_ptrs_.size());

    clap_process_t process{};
    process.steady_time = steady_time_;
    process.frames_count = frames;
    process.audio_inputs = desc_.audio_inputs > 0 ? &in_bus : nullptr;
    process.audio_inputs_count = desc_.audio_inputs > 0 ? 1 : 0;
    process.audio_outputs = desc_.audio_outputs > 0 ? &out_bus : nullptr;
    process.audio_outputs_count = desc_.audio_outputs > 0 ? 1 : 0;
    process.transport = has_transport_ ? &transport_event_ : nullptr;
    process.in_events = &in_events_.list;
    process.out_events = &out_events_;

    PendingParam pending[kMaxBlockMidi];
    size_t pending_n = 0;
    PendingParam incoming;
    while (pending_n < kMaxBlockMidi && param_queue_.pop(incoming))
      pending[pending_n++] = incoming;
    in_events_.rebuild(pending, pending_n, pending_midi_.data(),
                       pending_midi_count_, dialect_);
    pending_midi_count_ = 0;
    produced_midi_count_ = 0;

    plugin_->process(plugin_, &process);
    steady_time_ += frames;

    hosting::copy_strip_outputs(output_ptrs_.data(), output_ptrs_.size(), outputs,
                                strip_channels_, frames);
  }

  std::vector<ParameterInfo> parameters() const override {
    if (params_ == nullptr) return {};
    if (params_rescan_.exchange(false, std::memory_order_acq_rel))
      params_cache_valid_ = false;
    if (params_cache_valid_) return params_cache_;

    params_cache_.clear();
    const uint32_t count = params_->count(plugin_);
    params_cache_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      clap_param_info_t info{};
      if (!params_->get_info(plugin_, i, &info)) continue;
      params_cache_.push_back({static_cast<uint32_t>(info.id), info.name,
                               info.min_value, info.max_value,
                               info.default_value});
    }
    params_cache_valid_ = true;
    return params_cache_;
  }

  double parameter_value(uint32_t id) const override {
    if (params_ == nullptr) return 0.0;
    double value = 0.0;
    if (!params_->get_value(plugin_, id, &value)) return 0.0;
    return value;
  }

  size_t take_midi_output(MidiEvent* out, size_t capacity) override {
    const size_t count = std::min(capacity, produced_midi_count_);
    std::copy_n(produced_midi_.begin(), count, out);
    produced_midi_count_ = 0;
    return count;
  }

  void set_transport(const TransportInfo& transport) override {
    transport_event_ = {};
    transport_event_.header.size = sizeof(transport_event_);
    transport_event_.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    transport_event_.header.type = CLAP_EVENT_TRANSPORT;

    transport_event_.flags = CLAP_TRANSPORT_HAS_TEMPO |
                             CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                             CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
                             CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    if (transport.playing) transport_event_.flags |= CLAP_TRANSPORT_IS_PLAYING;

    transport_event_.tempo = transport.tempo_bpm;
    transport_event_.tsig_num = static_cast<uint16_t>(transport.numerator);
    transport_event_.tsig_denom = static_cast<uint16_t>(transport.denominator);

    // CLAP counts beats and seconds in fixed point, not doubles.
    transport_event_.song_pos_beats =
        static_cast<clap_beattime>(transport.beats * CLAP_BEATTIME_FACTOR);
    transport_event_.song_pos_seconds =
        static_cast<clap_sectime>(transport.seconds * CLAP_SECTIME_FACTOR);

    const double bars = transport.numerator > 0
                            ? transport.beats / transport.numerator
                            : transport.beats;
    transport_event_.bar_number = static_cast<int32_t>(bars);
    transport_event_.bar_start = static_cast<clap_beattime>(
        static_cast<int32_t>(bars) * transport.numerator * CLAP_BEATTIME_FACTOR);
    has_transport_ = true;
  }

  void queue_midi(const MidiEvent& event) override {
    if (event.size == 0) return;
    if (!midi_queue_admits(pending_midi_count_, kMaxBlockMidi, event)) return;
    pending_midi_[pending_midi_count_++] = event;
  }

  void set_parameter(uint32_t id, double value) override {
    // Parameter changes reach the plugin as events on the next process call,
    // which is the only way CLAP allows them to be sampled in time - while it
    // is active. Deactivated, there is no next process call, and the extension
    // says to hand them over through flush() on the main thread instead.
    // Without that, every knob turned before the engine started was lost.
    if (!active_) {
      const PendingParam single{id, value};
      flush_params_now(&single, 1);
      return;
    }
    if (!param_queue_.push({id, value}))
      param_drops_.fetch_add(1, std::memory_order_relaxed);
  }

  bool take_state_dirty() override {
    const bool dirty = state_dirty_.exchange(false, std::memory_order_acq_rel);
    if (dirty) note_names_.invalidate();
    return dirty;
  }

  std::vector<NoteName> note_names() const override {
    if (note_names_.stale() || state_dirty_.load(std::memory_order_acquire))
      refresh_note_names();
    return note_names_.names();
  }

  void host_idle() override {
    pump_main_thread();
    if (plugin_ == nullptr) return;

    // A plugin that changed its ports, its latency or anything else it may
    // only change deactivated asks for this and waits. Same instance, same
    // state; only the activation is redone.
    if (restart_requested_.exchange(false, std::memory_order_acq_rel) && active_) {
      deactivate();
      activate(sample_rate_, max_block_);
    }

    // latency.get is [main-thread & (being-activated | active)]: the cached
    // copy the audio thread reads is refreshed here, only while that holds.
    if (active_ && latency_dirty_.exchange(false, std::memory_order_acq_rel))
      refresh_latency();

    // params.flush is the main thread's job only while inactive; active, the
    // plugin's own process() is where its pending changes get flushed.
    if (flush_requested_.exchange(false, std::memory_order_acq_rel) && !active_)
      flush_params_now(nullptr, 0);

    // Active, but nothing is calling process(): the engine has no audio
    // server, or this strip is out of the graph. Edits would sit in the queue
    // until it overflowed. If a whole idle interval passed with the block
    // counter still, no audio thread is consuming the queue and the main
    // thread may drain it itself - the graph is only ever changed from this
    // same thread, so process() cannot start on us in between.
    const uint64_t generation = process_generation_.load(std::memory_order_acquire);
    if (active_ && !processing_.load(std::memory_order_acquire) &&
        generation == idle_seen_generation_ && param_queue_.size() > 0) {
      PendingParam pending[kMaxBlockMidi];
      size_t pending_n = 0;
      PendingParam incoming;
      while (pending_n < kMaxBlockMidi && param_queue_.pop(incoming))
        pending[pending_n++] = incoming;
      flush_params_now(pending, pending_n);
    }
    idle_seen_generation_ = generation;

    const uint32_t drops = param_drops_.load(std::memory_order_relaxed);
    if (drops > 0 && !param_drops_logged_) {
      param_drops_logged_ = true;
      std::fprintf(stderr,
                   "clap: %s dropped %u parameter change(s): the queue to the "
                   "audio thread was full\n",
                   desc_.name.c_str(), drops);
    }
  }

  std::vector<uint8_t> save_state() const override {
    std::vector<uint8_t> blob;
    if (state_ == nullptr) return blob;
    OutStream stream{&blob};
    state_->save(plugin_, &stream.stream);
    return blob;
  }

  bool load_state(const std::vector<uint8_t>& blob) override {
    if (state_ == nullptr || blob.empty()) return false;
    InStream stream{&blob};
    const bool ok = state_->load(plugin_, &stream.stream);
    note_names_.invalidate();
    return ok;
  }

  const PluginDescriptor& descriptor() const override { return desc_; }

  // Cached, never asked for here: clap_plugin_latency.get is [main-thread] and
  // this is called from the audio thread, twice per strip per block.
  uint32_t latency_samples() const override {
    return latency_.load(std::memory_order_relaxed);
  }

  int extra_output_pairs() const override {
    return hosting::extra_output_pairs(output_ptrs_.size(), strip_channels_);
  }

  void copy_extra_output(int pair, float* left, float* right,
                         uint32_t frames) override {
    hosting::copy_extra_output(output_ptrs_.data(), output_ptrs_.size(),
                               strip_channels_, pair, left, right, frames);
  }

  std::unique_ptr<PluginGui> create_gui() override {
    if (gui_ == nullptr) return nullptr;
    // A plugin that cannot draw on X11 is no use here, and asking it to create
    // an editor anyway tends to end in a crash rather than a clean refusal.
    if (!gui_->is_api_supported(plugin_, CLAP_WINDOW_API_X11, false)) return nullptr;
    return std::make_unique<ClapGui>(this, plugin_, gui_);
  }

  // Runs everything the plugin asked the host to run on the main thread: its
  // due timers, its ready file descriptors, and any callback it requested.
  void pump_main_thread() {
    if (plugin_ == nullptr) return;

    if (callback_requested_.exchange(false, std::memory_order_acquire))
      plugin_->on_main_thread(plugin_);

    const auto now = std::chrono::steady_clock::now();
    if (plugin_timers_ != nullptr) {
      // Snapshot: on_timer may register/unregister and invalidate the vector.
      const std::vector<Timer> snapshot = timers_;
      for (const Timer& timer : snapshot) {
        if (now - timer.last_fired < std::chrono::milliseconds(timer.period_ms))
          continue;
        for (Timer& live : timers_) {
          if (live.id != timer.id) continue;
          live.last_fired = now;
          break;
        }
        plugin_timers_->on_timer(plugin_, timer.id);
      }
    }

    if (plugin_fds_ != nullptr && !fds_.empty()) {
      poll_set_.clear();
      poll_set_.reserve(fds_.size());
      for (const RegisteredFd& registered : fds_) {
        pollfd entry{};
        entry.fd = registered.fd;
        entry.events = 0;
        if (registered.flags & CLAP_POSIX_FD_READ) entry.events |= POLLIN;
        if (registered.flags & CLAP_POSIX_FD_WRITE) entry.events |= POLLOUT;
        poll_set_.push_back(entry);
      }

      // Zero timeout: this is a poll of what is ready now, not a wait.
      if (poll(poll_set_.data(), poll_set_.size(), 0) > 0) {
        for (const pollfd& entry : poll_set_) {
          clap_posix_fd_flags_t flags = 0;
          if (entry.revents & POLLIN) flags |= CLAP_POSIX_FD_READ;
          if (entry.revents & POLLOUT) flags |= CLAP_POSIX_FD_WRITE;
          if (entry.revents & (POLLERR | POLLHUP)) flags |= CLAP_POSIX_FD_ERROR;
          if (flags != 0) plugin_fds_->on_fd(plugin_, entry.fd, flags);
        }
      }
    }
  }

 private:
  friend class ClapGui;

  struct Timer {
    clap_id id;
    uint32_t period_ms;
    std::chrono::steady_clock::time_point last_fired;
  };

  struct RegisteredFd {
    int fd;
    clap_posix_fd_flags_t flags;
  };

  struct PendingParam {
    uint32_t id;
    double value;
  };

  // An input event list backed by a vector the audio thread only reads. The
  // events are built before process() runs, so nothing allocates mid-block.
  struct InEventList {
    InEventList() {
      list.ctx = this;
      list.size = &InEventList::size_fn;
      list.get = &InEventList::get_fn;
      // Reserved once, here, for the most either side can deliver in a block.
      // rebuild() runs on the audio thread, where growing this vector was an
      // allocation in the middle of process().
      events.reserve(2 * kMaxBlockMidi);
    }

    void rebuild(const PendingParam* pending, size_t pending_count,
                 const MidiEvent* midi, size_t midi_count,
                 hosting::ClapNoteDialect dialect) {
      events.clear();

      // Both kinds share one list, and CLAP wants it sorted by time. Parameter
      // changes all land at frame 0, so putting them first keeps that true.
      for (size_t i = 0; i < pending_count; ++i) {
        const PendingParam& param = pending[i];
        clap_event_param_value_t event{};
        event.header.size = sizeof(event);
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.header.flags = 0;
        event.param_id = param.id;
        event.cookie = nullptr;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = param.value;
        events.push_back(hosting::ClapInEvent{event});
      }

      for (size_t i = 0; i < midi_count; ++i) {
        hosting::ClapInEvent event;
        if (hosting::clap_event_from_midi(midi[i], dialect, &event))
          events.push_back(event);
      }
    }

    static uint32_t size_fn(const clap_input_events_t* list) {
      return static_cast<uint32_t>(
          static_cast<const InEventList*>(list->ctx)->events.size());
    }
    static const clap_event_header_t* get_fn(const clap_input_events_t* list,
                                             uint32_t index) {
      const auto* self = static_cast<const InEventList*>(list->ctx);
      if (index >= self->events.size()) return nullptr;
      return &self->events[index].header;
    }

    clap_input_events_t list{};
    std::vector<hosting::ClapInEvent> events;
  };

  // Plugins push their own events here during process: parameter gestures,
  // latency changes, and MIDI from anything that generates notes. Only the MIDI
  // is kept, since that is what the insert chain below can use.
  static bool out_event_push(const clap_output_events_t* list,
                             const clap_event_header_t* header) {
    auto* self = static_cast<ClapInstance*>(list->ctx);
    if (self == nullptr || header == nullptr) return true;
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
    if (self->produced_midi_count_ >= kMaxBlockMidi) return false;

    MidiEvent event;
    event.frame = header->time;

    switch (header->type) {
      case CLAP_EVENT_MIDI: {
        const auto* midi = reinterpret_cast<const clap_event_midi_t*>(header);
        event.size = 3;
        std::copy_n(midi->data, 3, event.data);
        break;
      }
      // CLAP's own note events are the native way to say note on and off, and a
      // plugin may use them instead of raw MIDI. They are turned into MIDI here
      // so the rest of the host only ever deals with one representation.
      case CLAP_EVENT_NOTE_ON:
      case CLAP_EVENT_NOTE_OFF: {
        const auto* note = reinterpret_cast<const clap_event_note_t*>(header);
        if (note->key < 0) return true;
        event = hosting::make_note_event(header->type == CLAP_EVENT_NOTE_ON,
                                         note->channel < 0 ? 0 : note->channel,
                                         note->key, note->velocity, header->time);
        break;
      }
      default:
        return true;
    }

    self->produced_midi_[self->produced_midi_count_++] = event;
    return true;
  }

  // The output side of a main-thread flush. Whatever the plugin says back -
  // parameter values it clamped, gestures - has nowhere to go here, and the
  // audio thread's MIDI collector must not be touched from this thread.
  static bool discard_event(const clap_output_events_t*, const clap_event_header_t*) {
    return true;
  }

  // Hands `items` to the plugin through params.flush, on the main thread, for
  // when there is no process() to carry them. Also what a plugin gets when it
  // asked for a flush itself and nothing needs saying (items null).
  void flush_params_now(const PendingParam* items, size_t count) {
    if (plugin_ == nullptr || params_ == nullptr || params_->flush == nullptr) return;
    flush_events_.rebuild(items, count, nullptr, 0, dialect_);
    params_->flush(plugin_, &flush_events_.list, &discard_out_events_);
  }

  struct OutStream {
    explicit OutStream(std::vector<uint8_t>* target) : buffer(target) {
      stream.ctx = this;
      stream.write = &OutStream::write_fn;
    }
    static int64_t write_fn(const clap_ostream_t* stream, const void* data,
                            uint64_t size) {
      auto* self = static_cast<OutStream*>(stream->ctx);
      const auto* bytes = static_cast<const uint8_t*>(data);
      self->buffer->insert(self->buffer->end(), bytes, bytes + size);
      return static_cast<int64_t>(size);
    }
    clap_ostream_t stream{};
    std::vector<uint8_t>* buffer;
  };

  struct InStream {
    explicit InStream(const std::vector<uint8_t>* source) : buffer(source) {
      stream.ctx = this;
      stream.read = &InStream::read_fn;
    }
    static int64_t read_fn(const clap_istream_t* stream, void* data, uint64_t size) {
      auto* self = static_cast<InStream*>(stream->ctx);
      const uint64_t remaining = self->buffer->size() - self->offset;
      const uint64_t taken = std::min(size, remaining);
      std::memcpy(data, self->buffer->data() + self->offset, taken);
      self->offset += taken;
      return static_cast<int64_t>(taken);
    }
    clap_istream_t stream{};
    const std::vector<uint8_t>* buffer;
    uint64_t offset = 0;
  };

  void read_port_counts() {
    if (audio_ports_ == nullptr) return;
    clap_audio_port_info_t info{};
    if (audio_ports_->count(plugin_, true) > 0 &&
        audio_ports_->get(plugin_, 0, true, &info))
      desc_.audio_inputs = static_cast<int>(info.channel_count);
    if (audio_ports_->count(plugin_, false) > 0 &&
        audio_ports_->get(plugin_, 0, false, &info))
      desc_.audio_outputs = static_cast<int>(info.channel_count);
  }

  void refresh_latency() {
    if (plugin_latency_ == nullptr || plugin_ == nullptr) return;
    latency_.store(plugin_latency_->get(plugin_), std::memory_order_relaxed);
  }

  void destroy() {
    deactivate();
    if (plugin_ != nullptr) {
      plugin_->destroy(plugin_);
      plugin_ = nullptr;
    }
  }

  static ClapInstance* self_of(const clap_host_t* host) {
    return static_cast<ClapInstance*>(host->host_data);
  }

  // The extensions this host answers to. Anything else a plugin asks for is
  // better left unanswered than half-implemented.
  static const void* host_get_extension(const clap_host_t* host, const char* id) {
    ClapInstance* self = self_of(host);
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &self->timer_support_;
    if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &self->fd_support_;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &self->state_support_;
    if (std::strcmp(id, CLAP_EXT_NOTE_NAME) == 0) return &self->note_name_host_;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &self->latency_host_;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &self->params_host_;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &self->gui_host_;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &self->audio_ports_host_;
    return nullptr;
  }

  static void host_note_name_changed(const clap_host_t* host) {
    self_of(host)->note_names_.invalidate();
  }

  void refresh_note_names() const {
    note_names_.refresh([this](std::vector<NoteName>& out) {
      if (plugin_ == nullptr) return;
      const auto* ext = static_cast<const clap_plugin_note_name_t*>(
          plugin_->get_extension(plugin_, CLAP_EXT_NOTE_NAME));
      if (ext == nullptr || ext->count == nullptr || ext->get == nullptr) return;
      const uint32_t n = ext->count(plugin_);
      out.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        clap_note_name_t item{};
        if (!ext->get(plugin_, i, &item)) continue;
        if (item.key < 0 || item.key > 127) continue;
        const size_t len = strnlen(item.name, CLAP_NAME_SIZE);
        if (len == 0) continue;
        NoteName named;
        named.key = item.key;
        named.name.assign(item.name, len);
        out.push_back(std::move(named));
      }
    });
  }

  // Callable from any thread per the extension, so the flag is atomic and the
  // UI picks it up on its next poll rather than saving from under the plugin.
  static void host_mark_dirty(const clap_host_t* host) {
    ClapInstance* self = self_of(host);
    self->state_dirty_.store(true, std::memory_order_release);
    self->note_names_.invalidate();
  }

  // [thread-safe] Serviced on the next host_idle: deactivate and activate
  // again with the same sample rate and block size.
  static void host_request_restart(const clap_host_t* host) {
    self_of(host)->restart_requested_.store(true, std::memory_order_release);
  }

  // The plugin wants process() called even with no audio to run. The graph
  // calls it every block for every active insert anyway, so there is nothing
  // to arrange; an insert that is out of the graph is deactivated.
  static void host_request_process(const clap_host_t*) {}

  // Called from any thread, serviced on the next main-thread pump.
  static void host_request_callback(const clap_host_t* host) {
    self_of(host)->callback_requested_.store(true, std::memory_order_release);
  }

  // The plugin's latency moved. Only legal while deactivated (or it must ask
  // for a restart); the cached value is re-read once it is active again.
  static void host_latency_changed(const clap_host_t* host) {
    self_of(host)->latency_dirty_.store(true, std::memory_order_release);
  }

  // The parameter list itself changed: names, ranges, or which exist. The
  // cached list is dropped and the state counts as changed, since a plugin
  // that grew a parameter usually did so by loading something.
  static void host_params_rescan(const clap_host_t* host, clap_param_rescan_flags flags) {
    ClapInstance* self = self_of(host);
    self->params_rescan_.store(true, std::memory_order_release);
    if (flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL))
      self->state_dirty_.store(true, std::memory_order_release);
  }

  // Nothing of the host's is keyed on a parameter id that would need clearing:
  // no automation, no modulation.
  static void host_params_clear(const clap_host_t*, clap_id, clap_param_clear_flags) {}

  static void host_params_request_flush(const clap_host_t* host) {
    self_of(host)->flush_requested_.store(true, std::memory_order_release);
  }

  static void host_gui_resize_hints_changed(const clap_host_t*) {}

  static bool host_gui_request_resize(const clap_host_t* host, uint32_t width,
                                      uint32_t height) {
    ClapInstance* self = self_of(host);
    ClapGui* gui = self->gui_object_.load(std::memory_order_acquire);
    if (gui == nullptr || width == 0 || height == 0) return false;
    gui->pending_resize_.store((static_cast<uint64_t>(width) << 32) | height,
                               std::memory_order_release);
    // True here means the request is acknowledged and will be processed; the
    // window follows when it polls take_resize_request.
    return true;
  }

  // The window's visibility is the host's; a plugin cannot show or hide it.
  static bool host_gui_request_show(const clap_host_t*) { return false; }
  static bool host_gui_request_hide(const clap_host_t*) { return false; }

  static void host_gui_closed(const clap_host_t* host, bool) {
    ClapInstance* self = self_of(host);
    if (ClapGui* gui = self->gui_object_.load(std::memory_order_acquire))
      gui->close_requested_.store(true, std::memory_order_release);
  }

  static bool host_audio_ports_is_rescan_flag_supported(const clap_host_t*, uint32_t) {
    return true;
  }

  // Ports changed. The plugin may only say so about anything but names while
  // deactivated, so the counts are read back then; the next activate sizes
  // its buffers from them.
  static void host_audio_ports_rescan(const clap_host_t* host, uint32_t flags) {
    ClapInstance* self = self_of(host);
    if (flags == CLAP_AUDIO_PORTS_RESCAN_NAMES) return;
    if (!self->active_) self->read_port_counts();
  }

  static bool host_register_timer(const clap_host_t* host, uint32_t period_ms,
                                  clap_id* timer_id) {
    ClapInstance* self = self_of(host);
    const clap_id id = self->next_timer_id_++;
    // A plugin asking for a 0 ms timer means "as often as you can"; clamp it to
    // the pump rate so it cannot spin the main thread.
    self->timers_.push_back({id, std::max<uint32_t>(period_ms, 8),
                             std::chrono::steady_clock::now()});
    *timer_id = id;
    return true;
  }

  static bool host_unregister_timer(const clap_host_t* host, clap_id timer_id) {
    ClapInstance* self = self_of(host);
    const auto it = std::find_if(self->timers_.begin(), self->timers_.end(),
                                 [timer_id](const Timer& timer) {
                                   return timer.id == timer_id;
                                 });
    if (it == self->timers_.end()) return false;
    self->timers_.erase(it);
    return true;
  }

  static bool host_register_fd(const clap_host_t* host, int fd,
                               clap_posix_fd_flags_t flags) {
    self_of(host)->fds_.push_back({fd, flags});
    return true;
  }

  static bool host_modify_fd(const clap_host_t* host, int fd,
                             clap_posix_fd_flags_t flags) {
    ClapInstance* self = self_of(host);
    for (RegisteredFd& registered : self->fds_) {
      if (registered.fd != fd) continue;
      registered.flags = flags;
      return true;
    }
    return false;
  }

  static bool host_unregister_fd(const clap_host_t* host, int fd) {
    ClapInstance* self = self_of(host);
    const auto it = std::find_if(
        self->fds_.begin(), self->fds_.end(),
        [fd](const RegisteredFd& registered) { return registered.fd == fd; });
    if (it == self->fds_.end()) return false;
    self->fds_.erase(it);
    return true;
  }

  PluginDescriptor desc_;
  std::shared_ptr<ClapModule> module_;
  clap_host_t host_{};
  const clap_plugin_t* plugin_ = nullptr;
  const clap_plugin_params_t* params_ = nullptr;
  const clap_plugin_state_t* state_ = nullptr;
  const clap_plugin_audio_ports_t* audio_ports_ = nullptr;
  const clap_plugin_gui_t* gui_ = nullptr;
  const clap_plugin_latency_t* plugin_latency_ = nullptr;
  const clap_plugin_timer_support_t* plugin_timers_ = nullptr;
  const clap_plugin_posix_fd_support_t* plugin_fds_ = nullptr;

  clap_host_timer_support_t timer_support_{};
  clap_host_posix_fd_support_t fd_support_{};
  clap_host_state_t state_support_{};
  clap_host_note_name_t note_name_host_{};
  clap_host_latency_t latency_host_{};
  clap_host_params_t params_host_{};
  clap_host_gui_t gui_host_{};
  clap_host_audio_ports_t audio_ports_host_{};
  std::atomic<bool> state_dirty_{false};
  hosting::NoteNameCache note_names_;
  std::vector<Timer> timers_;
  std::vector<RegisteredFd> fds_;
  std::vector<pollfd> poll_set_;
  clap_id next_timer_id_ = 1;
  std::atomic<bool> callback_requested_{false};
  std::atomic<bool> restart_requested_{false};
  std::atomic<bool> latency_dirty_{false};
  std::atomic<bool> flush_requested_{false};
  mutable std::atomic<bool> params_rescan_{false};
  mutable std::vector<ParameterInfo> params_cache_;
  mutable bool params_cache_valid_ = false;
  // The one editor open on this instance, for the gui callbacks to reach.
  std::atomic<ClapGui*> gui_object_{nullptr};
  hosting::ClapNoteDialect dialect_;

  bool active_ = false;
  double sample_rate_ = 48000.0;
  uint32_t max_block_ = 0;
  // start_processing/stop_processing are [audio-thread] per the CLAP spec,
  // so activate()/deactivate() (called from the UI/control thread) only
  // request the state; process() (the audio thread) is what actually calls
  // them, servicing the request at the top of the next block.
  std::atomic<bool> want_processing_{false};
  std::atomic<bool> processing_{false};
  std::atomic<uint64_t> process_generation_{0};
  uint64_t idle_seen_generation_ = 0;
  int strip_channels_ = 2;
  int64_t steady_time_ = 0;
  std::atomic<uint32_t> latency_{0};
  std::atomic<uint32_t> param_drops_{0};
  bool param_drops_logged_ = false;

  std::vector<std::vector<float>> input_channels_, output_channels_;
  std::vector<float*> input_ptrs_, output_ptrs_;
  static constexpr size_t kMaxBlockMidi = 1024;
  RtQueue<PendingParam, 256> param_queue_;
  std::array<MidiEvent, kMaxBlockMidi> pending_midi_{};
  size_t pending_midi_count_ = 0;
  std::array<MidiEvent, kMaxBlockMidi> produced_midi_{};
  size_t produced_midi_count_ = 0;
  clap_event_transport_t transport_event_{};
  bool has_transport_ = false;
  InEventList in_events_;
  // Main-thread only, for flush(): the audio thread's list must not be rebuilt
  // under it.
  InEventList flush_events_;
  clap_output_events_t out_events_{this, &ClapInstance::out_event_push};
  clap_output_events_t discard_out_events_{};
};

ClapGui::ClapGui(ClapInstance* owner, const clap_plugin_t* plugin,
                 const clap_plugin_gui_t* gui)
    : owner_(owner), plugin_(plugin), gui_(gui) {
  owner_->gui_object_.store(this, std::memory_order_release);
}

ClapGui::~ClapGui() {
  detach();
  owner_->gui_object_.store(nullptr, std::memory_order_release);
}

int ClapGui::idle() {
  owner_->pump_main_thread();
  // Non-zero is "the editor asked to close", which is what clap_host_gui.closed
  // means; the window that embeds it tears down through detach() as usual.
  return close_requested_.load(std::memory_order_acquire) ? 1 : 0;
}

class ClapBackend : public PluginBackend {
 public:
  PluginFormat format() const override { return PluginFormat::Clap; }

  std::vector<PluginDescriptor> scan() override {
    std::vector<PluginDescriptor> found;
    hosting::ScanCache cache("clap", PluginFormat::Clap);
    for (const fs::path& dir : clap_search_paths()) {
      std::error_code ec;
      if (!fs::is_directory(dir, ec)) continue;
      for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".clap") continue;
        std::vector<PluginDescriptor> here;
        if (!cache.lookup(entry.path().string(), entry.path(), &here)) {
          scan_module(entry.path(), here);
          cache.store(entry.path().string(), entry.path(), here);
        }
        found.insert(found.end(), here.begin(), here.end());
      }
    }
    cache.save();
    return found;
  }

  std::unique_ptr<PluginInstance> instantiate(const PluginDescriptor& desc) override {
    std::shared_ptr<ClapModule> module = module_for(desc.path, true);
    if (module == nullptr) return nullptr;

    auto instance = std::make_unique<ClapInstance>(desc, std::move(module));
    if (!instance->create()) return nullptr;
    return instance;
  }

 private:
  std::shared_ptr<ClapModule> module_for(const std::string& path, bool pin) {
    return modules_.get(path, [&path] { return ClapModule::open(path); }, pin);
  }

  // CLAP declares itself as a null-terminated list of feature strings. The
  // first few are the ones that decide the bucket; the rest are descriptive
  // ("reverb", "granular") and go to the picker as written. Port counts cannot
  // help here - a CLAP has to be instantiated before it will say how many it
  // has - so an unhelpful feature list stays Unknown rather than guessing.
  static void read_features(const clap_plugin_descriptor_t* d,
                            PluginDescriptor& desc) {
    if (d->features == nullptr) return;
    std::string words;
    for (const char* const* f = d->features; *f != nullptr; ++f) {
      const std::string_view feature(*f);
      if (feature == CLAP_PLUGIN_FEATURE_INSTRUMENT) {
        desc.kind = PluginKind::Instrument;
        desc.has_midi_input = true;
      } else if (feature == CLAP_PLUGIN_FEATURE_NOTE_EFFECT ||
                 feature == CLAP_PLUGIN_FEATURE_NOTE_DETECTOR) {
        desc.kind = PluginKind::MidiEffect;
        desc.has_midi_input = true;
      }
      else if (feature == CLAP_PLUGIN_FEATURE_ANALYZER)
        desc.kind = PluginKind::Analyzer;
      else if (feature == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT &&
               desc.kind == PluginKind::Unknown)
        desc.kind = PluginKind::Effect;
      else {
        if (!words.empty()) words += ' ';
        words += feature;
      }
    }
    desc.category = std::move(words);
  }

  void scan_module(const fs::path& path, std::vector<PluginDescriptor>& out) {
    std::shared_ptr<ClapModule> module = module_for(path.string(), false);
    if (module == nullptr) return;

    const clap_plugin_factory_t* factory = module->factory();
    if (factory == nullptr) return;

    const uint32_t count = factory->get_plugin_count(factory);
    for (uint32_t i = 0; i < count; ++i) {
      const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory, i);
      if (d == nullptr) continue;
      PluginDescriptor desc;
      desc.format = PluginFormat::Clap;
      desc.uid = d->id != nullptr ? d->id : "";
      desc.name = d->name != nullptr ? d->name : "";
      desc.vendor = d->vendor != nullptr ? d->vendor : "";
      desc.path = path.string();
      read_features(d, desc);
      // Port counts need a live instance, so they stay zero until the plugin is
      // actually loaded and read_port_counts fills them in.
      out.push_back(std::move(desc));
    }
  }

  hosting::ModuleCache<ClapModule> modules_;
};

}  // namespace

std::unique_ptr<PluginBackend> make_clap_backend() {
  return std::make_unique<ClapBackend>();
}

}  // namespace nirbija
