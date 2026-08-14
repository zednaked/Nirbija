#include "hosting/clap_backend.h"

#include <clap/clap.h>
#include <dlfcn.h>

#include <poll.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
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

class ClapGui : public PluginGui {
 public:
  ClapGui(ClapInstance* owner, const clap_plugin_t* plugin,
          const clap_plugin_gui_t* gui)
      : owner_(owner), plugin_(plugin), gui_(gui) {}

  ~ClapGui() override { detach(); }

  bool attach(uintptr_t parent_window) override {
    if (created_) detach();
    if (!gui_->create(plugin_, CLAP_WINDOW_API_X11, false)) return false;
    created_ = true;

    clap_window_t window{};
    window.api = CLAP_WINDOW_API_X11;
    window.x11 = static_cast<unsigned long>(parent_window);
    if (!gui_->set_parent(plugin_, &window)) {
      detach();
      return false;
    }

    // The plugin knows what size it wants, but it will not lay itself out
    // until the host confirms one, so ask and then tell.
    uint32_t width = 0;
    uint32_t height = 0;
    if (gui_->get_size(plugin_, &width, &height) && width > 0 && height > 0)
      gui_->set_size(plugin_, width, height);

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
  void idle() override;

  bool preferred_size(int* width, int* height) const override {
    uint32_t w = 0;
    uint32_t h = 0;
    if (!created_ || !gui_->get_size(plugin_, &w, &h)) return false;
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    return true;
  }

 private:
  ClapInstance* owner_;
  const clap_plugin_t* plugin_;
  const clap_plugin_gui_t* gui_;
  bool created_ = false;
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

    read_port_counts();
    return true;
  }

  void set_channel_layout(int channels) override { strip_channels_ = channels; }

  bool activate(double sample_rate, uint32_t max_block_frames) override {
    if (plugin_ == nullptr) return false;
    if (active_) deactivate();

    if (!plugin_->activate(plugin_, sample_rate, 1, max_block_frames)) return false;
    active_ = true;

    input_channels_.assign(std::max(desc_.audio_inputs, 1),
                           std::vector<float>(max_block_frames, 0.0f));
    output_channels_.assign(std::max(desc_.audio_outputs, 1),
                            std::vector<float>(max_block_frames, 0.0f));
    input_ptrs_.clear();
    output_ptrs_.clear();
    for (auto& channel : input_channels_) input_ptrs_.push_back(channel.data());
    for (auto& channel : output_channels_) output_ptrs_.push_back(channel.data());

    if (!plugin_->start_processing(plugin_)) {
      plugin_->deactivate(plugin_);
      active_ = false;
      return false;
    }
    processing_ = true;
    return true;
  }

  void deactivate() override {
    if (plugin_ == nullptr || !active_) return;
    if (processing_) {
      plugin_->stop_processing(plugin_);
      processing_ = false;
    }
    plugin_->deactivate(plugin_);
    active_ = false;
  }

  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override {
    if (!processing_) return;

    // Feed every plugin input, duplicating the last strip channel when the
    // plugin is wider than the strip.
    for (size_t i = 0; i < input_ptrs_.size(); ++i) {
      const int source = std::min(static_cast<int>(i), strip_channels_ - 1);
      std::copy_n(inputs[source], frames, input_ptrs_[i]);
    }

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
    process.in_events = &in_events_.list;
    process.out_events = &out_events_;

    in_events_.rebuild(pending_params_, pending_midi_);
    pending_params_.clear();
    pending_midi_.clear();

    plugin_->process(plugin_, &process);
    steady_time_ += frames;

    for (int ch = 0; ch < strip_channels_; ++ch) {
      if (output_ptrs_.empty()) break;
      const size_t source =
          std::min(static_cast<size_t>(ch), output_ptrs_.size() - 1);
      std::copy_n(output_ptrs_[source], frames, outputs[ch]);
    }
  }

  std::vector<ParameterInfo> parameters() const override {
    std::vector<ParameterInfo> out;
    if (params_ == nullptr) return out;
    const uint32_t count = params_->count(plugin_);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      clap_param_info_t info{};
      if (!params_->get_info(plugin_, i, &info)) continue;
      out.push_back({static_cast<uint32_t>(info.id), info.name, info.min_value,
                     info.max_value, info.default_value});
    }
    return out;
  }

  double parameter_value(uint32_t id) const override {
    if (params_ == nullptr) return 0.0;
    double value = 0.0;
    if (!params_->get_value(plugin_, id, &value)) return 0.0;
    return value;
  }

  void queue_midi(const MidiEvent& event) override {
    if (event.size == 0 || pending_midi_.size() >= kMaxBlockMidi) return;
    pending_midi_.push_back(event);
  }

  void set_parameter(uint32_t id, double value) override {
    // Parameter changes reach the plugin as events on the next process call,
    // which is the only way CLAP allows them to be sampled in time.
    pending_params_.push_back({id, value});
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
    return state_->load(plugin_, &stream.stream);
  }

  const PluginDescriptor& descriptor() const override { return desc_; }

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
      for (Timer& timer : timers_) {
        if (now - timer.last_fired < std::chrono::milliseconds(timer.period_ms))
          continue;
        timer.last_fired = now;
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
    }

    void rebuild(const std::vector<PendingParam>& pending,
                 const std::vector<MidiEvent>& midi) {
      events.clear();
      events.reserve(pending.size() + midi.size());

      // Both kinds share one list, and CLAP wants it sorted by time. Parameter
      // changes all land at frame 0, so putting them first keeps that true.
      for (const PendingParam& param : pending) {
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
        events.push_back(Event{event});
      }

      for (const MidiEvent& source : midi) {
        clap_event_midi_t event{};
        event.header.size = sizeof(event);
        event.header.time = source.frame;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_MIDI;
        event.header.flags = 0;
        event.port_index = 0;
        std::memcpy(event.data, source.data, sizeof(event.data));
        events.push_back(Event{event});
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

    // Parameter changes and MIDI travel in the same list, so the storage has to
    // hold either one. Both start with a clap_event_header_t.
    union Event {
      Event() : header{} {}
      explicit Event(const clap_event_param_value_t& value) : param(value) {}
      explicit Event(const clap_event_midi_t& value) : midi(value) {}

      clap_event_header_t header;
      clap_event_param_value_t param;
      clap_event_midi_t midi;
    };

    clap_input_events_t list{};
    std::vector<Event> events;
  };

  // Plugins emit parameter gestures and latency changes here. Swallowed until
  // the UI exists to receive them.
  static bool out_event_push(const clap_output_events_t*, const clap_event_header_t*) {
    return true;
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

  // Only the two event-loop extensions are offered. Anything else a plugin asks
  // for is better left unanswered than half-implemented.
  static const void* host_get_extension(const clap_host_t* host, const char* id) {
    ClapInstance* self = self_of(host);
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &self->timer_support_;
    if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &self->fd_support_;
    return nullptr;
  }

  static void host_request_restart(const clap_host_t*) {}
  static void host_request_process(const clap_host_t*) {}

  // Called from any thread, serviced on the next main-thread pump.
  static void host_request_callback(const clap_host_t* host) {
    self_of(host)->callback_requested_.store(true, std::memory_order_release);
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
  const clap_plugin_timer_support_t* plugin_timers_ = nullptr;
  const clap_plugin_posix_fd_support_t* plugin_fds_ = nullptr;

  clap_host_timer_support_t timer_support_{};
  clap_host_posix_fd_support_t fd_support_{};
  std::vector<Timer> timers_;
  std::vector<RegisteredFd> fds_;
  std::vector<pollfd> poll_set_;
  clap_id next_timer_id_ = 1;
  std::atomic<bool> callback_requested_{false};

  bool active_ = false;
  bool processing_ = false;
  int strip_channels_ = 2;
  int64_t steady_time_ = 0;

  std::vector<std::vector<float>> input_channels_, output_channels_;
  std::vector<float*> input_ptrs_, output_ptrs_;
  static constexpr size_t kMaxBlockMidi = 64;
  std::vector<PendingParam> pending_params_;
  std::vector<MidiEvent> pending_midi_;
  InEventList in_events_;
  clap_output_events_t out_events_{nullptr, &ClapInstance::out_event_push};
};

void ClapGui::idle() { owner_->pump_main_thread(); }

class ClapBackend : public PluginBackend {
 public:
  PluginFormat format() const override { return PluginFormat::Clap; }

  std::vector<PluginDescriptor> scan() override {
    std::vector<PluginDescriptor> found;
    for (const fs::path& dir : clap_search_paths()) {
      std::error_code ec;
      if (!fs::is_directory(dir, ec)) continue;
      for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".clap") continue;
        scan_module(entry.path(), found);
      }
    }
    return found;
  }

  std::unique_ptr<PluginInstance> instantiate(const PluginDescriptor& desc) override {
    std::shared_ptr<ClapModule> module = module_for(desc.path);
    if (module == nullptr) return nullptr;

    auto instance = std::make_unique<ClapInstance>(desc, std::move(module));
    if (!instance->create()) return nullptr;
    return instance;
  }

 private:
  // Modules are cached so loading two plugins from one bundle does not dlopen it
  // twice, and so a module stays resident while any of its plugins is alive.
  std::shared_ptr<ClapModule> module_for(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(path);
    if (it != modules_.end())
      if (std::shared_ptr<ClapModule> alive = it->second.lock()) return alive;

    std::shared_ptr<ClapModule> module = ClapModule::open(path);
    if (module != nullptr) modules_[path] = module;
    return module;
  }

  void scan_module(const fs::path& path, std::vector<PluginDescriptor>& out) {
    std::shared_ptr<ClapModule> module = module_for(path.string());
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
      // Port counts need a live instance, so they stay zero until the plugin is
      // actually loaded and read_port_counts fills them in.
      out.push_back(std::move(desc));
    }
  }

  std::mutex mutex_;
  std::map<std::string, std::weak_ptr<ClapModule>> modules_;
};

}  // namespace

std::unique_ptr<PluginBackend> make_clap_backend() {
  return std::make_unique<ClapBackend>();
}

}  // namespace nirbija
