#include "hosting/vst3_backend.h"

#include <dlfcn.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/rt_queue.h"

// Only the raw interface headers, never the SDK sources: a host needs the
// contracts, not Steinberg's convenience classes. Without INIT_CLASS_IID the
// headers define per-file TUID constants (Name_iid) and leave the FUID members
// undefined, so everything here works in TUIDs and links clean.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

namespace nirbija {
namespace {

namespace fs = std::filesystem;
using namespace Steinberg;

bool same_iid(const TUID a, const TUID b) { return std::memcmp(a, b, 16) == 0; }

std::string utf16_to_utf8(const Vst::TChar* text) {
  std::string out;
  // Bounds first: a title using all 128 units carries no terminator, and the
  // old order dereferenced one element past the array before checking.
  for (int i = 0; i < 128 && text[i] != 0; ++i) {
    uint32_t code = static_cast<char16_t>(text[i]);
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < 128) {
      const uint32_t low = static_cast<char16_t>(text[i + 1]);
      if (low >= 0xdc00 && low <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        ++i;
      }
    }
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
  }
  return out;
}

// Boilerplate every micro-implementation below shares. The interfaces a class
// answers to are listed in its own queryInterface.
#define NIRBIJA_REFCOUNT()                                                    \
  std::atomic<int32> refs_{1};                                                \
  uint32 PLUGIN_API addRef() override { return ++refs_; }                     \
  uint32 PLUGIN_API release() override {                                      \
    const int32 left = --refs_;                                               \
    if (left == 0) delete this;                                               \
    return left;                                                              \
  }

// --- streams ----------------------------------------------------------------

// State travels through this, in both directions.
struct MemStream : IBStream {
  explicit MemStream(std::vector<uint8_t>* backing) : data(backing) {}
  std::vector<uint8_t>* data;
  int64 cursor = 0;

  NIRBIJA_REFCOUNT()
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, IBStream_iid)) {
      addRef();
      *obj = this;
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  tresult PLUGIN_API read(void* buffer, int32 bytes, int32* read_out) override {
    const int64 available = static_cast<int64>(data->size()) - cursor;
    const int32 taken = static_cast<int32>(std::max<int64>(
        0, std::min<int64>(bytes, available)));
    std::memcpy(buffer, data->data() + cursor, taken);
    cursor += taken;
    if (read_out != nullptr) *read_out = taken;
    return kResultOk;
  }

  tresult PLUGIN_API write(void* buffer, int32 bytes, int32* written) override {
    const auto* src = static_cast<const uint8_t*>(buffer);
    if (cursor + bytes > static_cast<int64>(data->size()))
      data->resize(cursor + bytes);
    std::copy_n(src, bytes, data->data() + cursor);
    cursor += bytes;
    if (written != nullptr) *written = bytes;
    return kResultOk;
  }

  tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
    switch (mode) {
      case kIBSeekSet: cursor = pos; break;
      case kIBSeekCur: cursor += pos; break;
      case kIBSeekEnd: cursor = static_cast<int64>(data->size()) + pos; break;
      default: return kInvalidArgument;
    }
    cursor = std::max<int64>(0, cursor);
    if (result != nullptr) *result = cursor;
    return kResultOk;
  }

  tresult PLUGIN_API tell(int64* pos) override {
    if (pos != nullptr) *pos = cursor;
    return kResultOk;
  }
};

// --- host context ------------------------------------------------------------

struct HostAttributes;

// The one object plugins interrogate about who is hosting them.
struct HostApplication : Vst::IHostApplication {
  NIRBIJA_REFCOUNT()
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IHostApplication_iid)) {
      addRef();
      *obj = static_cast<Vst::IHostApplication*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  tresult PLUGIN_API getName(Vst::String128 name) override {
    static const char16_t text[] = u"Nirbija";
    std::copy_n(text, sizeof(text) / sizeof(char16_t), name);
    return kResultOk;
  }

  tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override;
};

// Attribute lists and messages exist so a component and its controller can talk
// to each other; several plugins refuse to run without them.
struct HostAttributes : Vst::IAttributeList {
  struct Value {
    int64 integer = 0;
    double real = 0.0;
    std::u16string text;
    std::vector<uint8_t> blob;
  };
  std::map<std::string, Value> values;

  NIRBIJA_REFCOUNT()
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IAttributeList_iid)) {
      addRef();
      *obj = this;
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  tresult PLUGIN_API setInt(AttrID id, int64 value) override {
    values[id].integer = value;
    return kResultOk;
  }
  tresult PLUGIN_API getInt(AttrID id, int64& value) override {
    auto it = values.find(id);
    if (it == values.end()) return kResultFalse;
    value = it->second.integer;
    return kResultOk;
  }
  tresult PLUGIN_API setFloat(AttrID id, double value) override {
    values[id].real = value;
    return kResultOk;
  }
  tresult PLUGIN_API getFloat(AttrID id, double& value) override {
    auto it = values.find(id);
    if (it == values.end()) return kResultFalse;
    value = it->second.real;
    return kResultOk;
  }
  tresult PLUGIN_API setString(AttrID id, const Vst::TChar* string) override {
    std::u16string text;
    for (int i = 0; string[i] != 0; ++i) text.push_back(string[i]);
    values[id].text = std::move(text);
    return kResultOk;
  }
  tresult PLUGIN_API getString(AttrID id, Vst::TChar* string,
                               uint32 bytes) override {
    auto it = values.find(id);
    if (it == values.end()) return kResultFalse;
    // A probing call with a tiny buffer must not underflow the bound into
    // four billion.
    if (bytes < sizeof(Vst::TChar)) return kResultFalse;
    const uint32 count = std::min<uint32>(bytes / sizeof(Vst::TChar) - 1,
                                          it->second.text.size());
    std::copy_n(it->second.text.data(), count, string);
    string[count] = 0;
    return kResultOk;
  }
  tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 bytes) override {
    const auto* src = static_cast<const uint8_t*>(data);
    values[id].blob.assign(src, src + bytes);
    return kResultOk;
  }
  tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& bytes) override {
    auto it = values.find(id);
    if (it == values.end()) return kResultFalse;
    data = it->second.blob.data();
    bytes = static_cast<uint32>(it->second.blob.size());
    return kResultOk;
  }
};

struct HostMessage : Vst::IMessage {
  std::string id;
  HostAttributes* attributes = new HostAttributes();

  ~HostMessage() { attributes->release(); }

  NIRBIJA_REFCOUNT()
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IMessage_iid)) {
      addRef();
      *obj = this;
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  FIDString PLUGIN_API getMessageID() override { return id.c_str(); }
  void PLUGIN_API setMessageID(FIDString text) override {
    id = text != nullptr ? text : "";
  }
  Vst::IAttributeList* PLUGIN_API getAttributes() override { return attributes; }
};

tresult PLUGIN_API HostApplication::createInstance(TUID cid, TUID iid, void** obj) {
  if (same_iid(cid, Vst::IMessage_iid) && same_iid(iid, Vst::IMessage_iid)) {
    *obj = new HostMessage();
    return kResultOk;
  }
  if (same_iid(cid, Vst::IAttributeList_iid) &&
      same_iid(iid, Vst::IAttributeList_iid)) {
    *obj = new HostAttributes();
    return kResultOk;
  }
  *obj = nullptr;
  return kNoInterface;
}

// --- parameter changes -------------------------------------------------------

// One block's worth of parameter edits, rebuilt before each process call from
// what the UI queued. Every entry is its own queue object: a plugin may hold
// several queue pointers at once, so a single view that switches identity under
// them would report the wrong parameter.
struct ParamChanges : Vst::IParameterChanges {
  static constexpr int32 kMaxParams = 64;

  struct Queue : Vst::IParamValueQueue {
    Vst::ParamID id = 0;
    Vst::ParamValue value = 0.0;

    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
      if (same_iid(iid, FUnknown_iid) ||
          same_iid(iid, Vst::IParamValueQueue_iid)) {
        *obj = this;
        return kResultOk;
      }
      *obj = nullptr;
      return kNoInterface;
    }

    Vst::ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return 1; }
    tresult PLUGIN_API getPoint(int32, int32& offset,
                                Vst::ParamValue& out) override {
      offset = 0;
      out = value;
      return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, Vst::ParamValue, int32&) override {
      return kResultFalse;
    }
  };

  Queue queues[kMaxParams];
  int32 count = 0;

  // Repeated edits to one parameter coalesce to the last value: the interface
  // promises at most one queue per parameter.
  bool add(Vst::ParamID id, Vst::ParamValue value) {
    for (int32 i = 0; i < count; ++i) {
      if (queues[i].id == id) {
        queues[i].value = value;
        return true;
      }
    }
    if (count >= kMaxParams) return false;
    queues[count].id = id;
    queues[count].value = value;
    ++count;
    return true;
  }

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IParameterChanges_iid)) {
      *obj = static_cast<Vst::IParameterChanges*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  int32 PLUGIN_API getParameterCount() override { return count; }
  Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
    if (index < 0 || index >= count) return nullptr;
    return &queues[index];
  }
  Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID&,
                                                     int32&) override {
    return nullptr;  // input side only
  }
};

// Collects the note events a plugin emits during process — a VST3 sequencer's
// whole output arrives here, and dropping it would leave the instrument below
// it silent.
struct OutEventList : Vst::IEventList {
  static constexpr int32 kMaxEvents = 64;
  Vst::Event events[kMaxEvents];
  int32 count = 0;

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IEventList_iid)) {
      *obj = this;
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  int32 PLUGIN_API getEventCount() override { return count; }
  tresult PLUGIN_API getEvent(int32 index, Vst::Event& event) override {
    if (index < 0 || index >= count) return kResultFalse;
    event = events[index];
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Vst::Event& event) override {
    if (count >= kMaxEvents) return kResultFalse;
    events[count++] = event;
    return kResultOk;
  }
};

// Where the plugin writes its own parameter moves during process. The host
// does not consume them yet, but a null pointer here is something plugins are
// entitled to reject.
struct OutParamChanges : Vst::IParameterChanges, Vst::IParamValueQueue {
  Vst::ParamID last_id = 0;

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IParameterChanges_iid)) {
      *obj = static_cast<Vst::IParameterChanges*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  int32 PLUGIN_API getParameterCount() override { return 0; }
  Vst::IParamValueQueue* PLUGIN_API getParameterData(int32) override {
    return nullptr;
  }
  Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id,
                                                     int32& index) override {
    last_id = id;
    index = 0;
    return this;
  }

  Vst::ParamID PLUGIN_API getParameterId() override { return last_id; }
  int32 PLUGIN_API getPointCount() override { return 0; }
  tresult PLUGIN_API getPoint(int32, int32&, Vst::ParamValue&) override {
    return kResultFalse;
  }
  tresult PLUGIN_API addPoint(int32, Vst::ParamValue, int32& index) override {
    index = 0;
    return kResultOk;  // accepted and discarded
  }
};

// Note events for the block, filled from the strip's MIDI chain.
struct EventList : Vst::IEventList {
  static constexpr int32 kMaxEvents = 64;
  Vst::Event events[kMaxEvents];
  int32 count = 0;

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IEventList_iid)) {
      *obj = this;
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  int32 PLUGIN_API getEventCount() override { return count; }
  tresult PLUGIN_API getEvent(int32 index, Vst::Event& event) override {
    if (index < 0 || index >= count) return kResultFalse;
    event = events[index];
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Vst::Event&) override { return kResultFalse; }
};

// --- the editor's frame and run loop ----------------------------------------

// Linux VST3 editors do not run their own event loop: they hand the host file
// descriptors and timers and expect to be driven, exactly like CLAP. This is
// both the IPlugFrame the view resizes through and the IRunLoop it registers
// its plumbing with, pumped from the GUI idle tick.
struct RunLoopFrame : IPlugFrame, Linux::IRunLoop {
  struct Timer {
    Linux::ITimerHandler* handler;
    uint64 interval_ms;
    std::chrono::steady_clock::time_point due;
  };
  std::vector<Timer> timers;
  std::vector<std::pair<Linux::IEventHandler*, int>> fds;

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (same_iid(iid, FUnknown_iid) || same_iid(iid, IPlugFrame_iid)) {
      *obj = static_cast<IPlugFrame*>(this);
      return kResultOk;
    }
    if (same_iid(iid, Linux::IRunLoop_iid)) {
      *obj = static_cast<Linux::IRunLoop*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }

  // The window follows the plugin's child window through adoptChild, so the
  // request only needs acknowledging.
  tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override {
    if (view != nullptr && size != nullptr) view->onSize(size);
    return kResultOk;
  }

  tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler,
                                          Linux::FileDescriptor fd) override {
    if (handler == nullptr) return kInvalidArgument;
    fds.emplace_back(handler, fd);
    return kResultOk;
  }
  tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override {
    std::erase_if(fds, [handler](const auto& entry) { return entry.first == handler; });
    return kResultOk;
  }
  tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler,
                                   Linux::TimerInterval ms) override {
    if (handler == nullptr) return kInvalidArgument;
    timers.push_back({handler, std::max<uint64>(ms, 1),
                      std::chrono::steady_clock::now()});
    return kResultOk;
  }
  tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override {
    std::erase_if(timers, [handler](const Timer& t) { return t.handler == handler; });
    return kResultOk;
  }

  // Handlers register and unregister from inside their own callbacks — a JUCE
  // editor does it when opening a menu — so nothing here iterates a container a
  // callback can mutate. Due handlers are snapshotted first, and each one is
  // checked to still be registered before its call, since an earlier callback
  // may have torn a later one down.
  void pump() {
    const auto now = std::chrono::steady_clock::now();

    std::vector<Linux::ITimerHandler*> due;
    for (Timer& timer : timers) {
      if (now < timer.due) continue;
      timer.due = now + std::chrono::milliseconds(timer.interval_ms);
      due.push_back(timer.handler);
    }
    for (Linux::ITimerHandler* handler : due) {
      const bool alive = std::any_of(
          timers.begin(), timers.end(),
          [handler](const Timer& t) { return t.handler == handler; });
      if (alive) handler->onTimer();
    }

    if (fds.empty()) return;
    std::vector<std::pair<Linux::IEventHandler*, int>> watched = fds;
    std::vector<pollfd> set;
    set.reserve(watched.size());
    for (const auto& [handler, fd] : watched) set.push_back({fd, POLLIN, 0});
    if (poll(set.data(), set.size(), 0) <= 0) return;
    for (size_t i = 0; i < set.size(); ++i) {
      if (!(set[i].revents & POLLIN)) continue;
      Linux::IEventHandler* handler = watched[i].first;
      const bool alive = std::any_of(
          fds.begin(), fds.end(),
          [handler](const auto& entry) { return entry.first == handler; });
      if (alive) handler->onFDIsSet(set[i].fd);
    }
  }
};

// --- module ------------------------------------------------------------------

using FactoryProc = IPluginFactory* (*)();
using ModuleEntryProc = bool (*)(void*);
using ModuleExitProc = bool (*)();

// A dlopen'd bundle. Kept alive by every instance from it, same as CLAP.
class Vst3Module {
 public:
  static std::shared_ptr<Vst3Module> open(const fs::path& binary) {
    void* handle = dlopen(binary.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr) return nullptr;

    bool entered = false;
    if (auto entry = reinterpret_cast<ModuleEntryProc>(dlsym(handle, "ModuleEntry"))) {
      if (!entry(handle)) {
        dlclose(handle);
        return nullptr;
      }
      entered = true;
    }

    // Unloading a module whose entry ran, without the matching exit, pulls the
    // ground from under whatever state that entry set up.
    auto bail = [handle, entered]() {
      if (entered)
        if (auto exit = reinterpret_cast<ModuleExitProc>(dlsym(handle, "ModuleExit")))
          exit();
      dlclose(handle);
    };

    auto factory_proc = reinterpret_cast<FactoryProc>(dlsym(handle, "GetPluginFactory"));
    if (factory_proc == nullptr) {
      bail();
      return nullptr;
    }
    IPluginFactory* factory = factory_proc();
    if (factory == nullptr) {
      bail();
      return nullptr;
    }
    return std::shared_ptr<Vst3Module>(new Vst3Module(handle, factory));
  }

  ~Vst3Module() {
    factory_->release();
    if (auto exit = reinterpret_cast<ModuleExitProc>(dlsym(handle_, "ModuleExit")))
      exit();
    dlclose(handle_);
  }

  IPluginFactory* factory() { return factory_; }

 private:
  Vst3Module(void* handle, IPluginFactory* factory)
      : handle_(handle), factory_(factory) {}
  void* handle_;
  IPluginFactory* factory_;
};

// --- the instance ------------------------------------------------------------

class Vst3Gui;

class Vst3Instance : public PluginInstance {
 public:
  Vst3Instance(PluginDescriptor desc, std::shared_ptr<Vst3Module> module)
      : desc_(std::move(desc)), module_(std::move(module)) {
    handler_.owner = this;
  }

  ~Vst3Instance() override {
    deactivate();

    // The workflow demands disconnect before terminate; a controller sending a
    // message during teardown must not hit a dangling peer.
    if (component_cp_ != nullptr && controller_cp_ != nullptr) {
      component_cp_->disconnect(controller_cp_);
      controller_cp_->disconnect(component_cp_);
    }
    if (component_cp_ != nullptr) component_cp_->release();
    if (controller_cp_ != nullptr) controller_cp_->release();
    if (midi_mapping_ != nullptr) midi_mapping_->release();

    if (controller_ != nullptr) {
      controller_->setComponentHandler(nullptr);
      // A combined component/controller is terminated once, through the
      // component; its controller reference still came from queryInterface and
      // still has to go back.
      if (controller_distinct_) controller_->terminate();
      controller_->release();
    }
    if (processor_ != nullptr) processor_->release();
    if (component_ != nullptr) {
      component_->terminate();
      component_->release();
    }
  }

  bool create(const TUID class_id) {
    IPluginFactory* factory = module_->factory();
    if (factory->createInstance(class_id, Vst::IComponent_iid,
                                reinterpret_cast<void**>(&component_)) != kResultOk ||
        component_ == nullptr)
      return false;
    if (component_->initialize(&host_) != kResultOk) return false;

    // The controller is either its own class or the same object; both shapes
    // are legal and both exist in the wild.
    TUID controller_cid = {};
    if (component_->getControllerClassId(controller_cid) == kResultOk) {
      if (factory->createInstance(controller_cid, Vst::IEditController_iid,
                                  reinterpret_cast<void**>(&controller_)) == kResultOk &&
          controller_ != nullptr) {
        controller_distinct_ = true;
        if (controller_->initialize(&host_) != kResultOk) return false;
      }
    }
    if (controller_ == nullptr)
      component_->queryInterface(Vst::IEditController_iid,
                                 reinterpret_cast<void**>(&controller_));
    if (controller_ != nullptr) controller_->setComponentHandler(&handler_);

    // A split component and controller talk through connection points; the
    // host's job is only to introduce them.
    if (controller_distinct_) {
      component_->queryInterface(Vst::IConnectionPoint_iid,
                                 reinterpret_cast<void**>(&component_cp_));
      controller_->queryInterface(Vst::IConnectionPoint_iid,
                                  reinterpret_cast<void**>(&controller_cp_));
      if (component_cp_ != nullptr && controller_cp_ != nullptr) {
        component_cp_->connect(controller_cp_);
        controller_cp_->connect(component_cp_);
      }

      // The controller starts from the component's state, or its editor shows
      // defaults over a plugin that is not at them.
      std::vector<uint8_t> state;
      MemStream stream(&state);
      if (component_->getState(&stream) == kResultOk && !state.empty()) {
        MemStream replay(&state);
        controller_->setComponentState(&replay);
      }
    }

    if (component_->queryInterface(Vst::IAudioProcessor_iid,
                                   reinterpret_cast<void**>(&processor_)) != kResultOk ||
        processor_ == nullptr)
      return false;

    if (controller_ != nullptr)
      controller_->queryInterface(Vst::IMidiMapping_iid,
                                  reinterpret_cast<void**>(&midi_mapping_));

    read_bus_layout();
    return true;
  }

  void set_channel_layout(int channels) override { strip_channels_ = channels; }

  bool activate(double sample_rate, uint32_t max_block_frames) override {
    if (processor_ == nullptr) return false;
    if (active_) deactivate();

    Vst::ProcessSetup setup{};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(max_block_frames);
    setup.sampleRate = sample_rate;
    if (processor_->setupProcessing(setup) != kResultOk) return false;

    // Main buses asked to be stereo; whatever the plugin settles on is read
    // back and honoured.
    Vst::SpeakerArrangement stereo = Vst::SpeakerArr::kStereo;
    std::vector<Vst::SpeakerArrangement> ins(input_buses_.size(), stereo);
    std::vector<Vst::SpeakerArrangement> outs(output_buses_.size(), stereo);
    processor_->setBusArrangements(ins.data(), static_cast<int32>(ins.size()),
                                   outs.data(), static_cast<int32>(outs.size()));
    read_bus_layout();

    sample_rate_hint_ = sample_rate;
    build_midi_map_cache();
    allocate_buffers(max_block_frames);

    for (size_t i = 0; i < input_buses_.size(); ++i)
      component_->activateBus(Vst::kAudio, Vst::kInput, static_cast<int32>(i), true);
    for (size_t i = 0; i < output_buses_.size(); ++i)
      component_->activateBus(Vst::kAudio, Vst::kOutput, static_cast<int32>(i), true);
    if (has_event_input_)
      component_->activateBus(Vst::kEvent, Vst::kInput, 0, true);

    if (component_->setActive(true) != kResultOk) return false;
    processor_->setProcessing(true);
    active_ = true;
    return true;
  }

  void deactivate() override {
    if (!active_) return;
    processor_->setProcessing(false);
    component_->setActive(false);
    active_ = false;
  }

  void set_transport(const TransportInfo& transport) override {
    transport_ = transport;
  }

  void queue_midi(const MidiEvent& event) override {
    pending_midi_.push(event);
  }

  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override {
    if (!active_) return;

    // Feed the main input bus, duplicating the last strip channel when the
    // plugin is wider, exactly as the other backends do.
    if (!input_buses_.empty()) {
      auto& main = bus_channel_ptrs_in_[0];
      for (size_t ch = 0; ch < main.size(); ++ch) {
        const int source = std::min(static_cast<int>(ch), strip_channels_ - 1);
        std::copy_n(inputs[source], frames, main[ch]);
      }
    }

    // Edits queued by the UI become this block's parameter changes. Capacity
    // is checked before the pop: popping first would consume an event only to
    // throw it away, and a discarded note-off is a stuck note.
    param_changes_.count = 0;
    ParamEdit edit;
    while (param_edits_.pop(edit)) param_changes_.add(edit.id, edit.value);

    events_.count = 0;
    MidiEvent midi;
    while (events_.count < EventList::kMaxEvents && pending_midi_.pop(midi)) {
      const uint8_t status = midi.data[0] & 0xf0;
      const int32 channel = midi.data[0] & 0x0f;
      Vst::Event& event = events_.events[events_.count];
      std::memset(&event, 0, sizeof(event));
      event.sampleOffset = static_cast<int32>(midi.frame);
      if (status == 0x90 && midi.data[2] > 0) {
        event.type = Vst::Event::kNoteOnEvent;
        event.noteOn.channel = static_cast<int16>(channel);
        event.noteOn.pitch = midi.data[1];
        event.noteOn.velocity = midi.data[2] / 127.0f;
        event.noteOn.noteId = -1;
        ++events_.count;
      } else if (status == 0x80 || (status == 0x90 && midi.data[2] == 0)) {
        event.type = Vst::Event::kNoteOffEvent;
        event.noteOff.channel = static_cast<int16>(channel);
        event.noteOff.pitch = midi.data[1];
        event.noteOff.velocity = 0.0f;
        event.noteOff.noteId = -1;
        ++events_.count;
      } else if (status == 0xb0 || status == 0xe0 || status == 0xd0) {
        // VST3 takes no raw CC: expression arrives as parameter changes,
        // through the plugin's own controller-to-parameter map cached at
        // activate. Without this, sustain and pitch bend die at the door.
        int16 controller = 0;
        double normal = 0.0;
        if (status == 0xb0) {
          controller = midi.data[1];
          normal = midi.data[2] / 127.0;
        } else if (status == 0xe0) {
          controller = Vst::kPitchBend;
          normal = ((midi.data[2] << 7) | midi.data[1]) / 16383.0;
        } else {
          controller = Vst::kAfterTouch;
          normal = midi.data[1] / 127.0;
        }
        const Vst::ParamID mapped = midi_map_cache_[channel][controller];
        if (mapped != Vst::kNoParamId) param_changes_.add(mapped, normal);
      }
    }

    Vst::ProcessContext context{};
    context.state = Vst::ProcessContext::kTempoValid |
                    Vst::ProcessContext::kTimeSigValid |
                    Vst::ProcessContext::kProjectTimeMusicValid;
    if (transport_.playing) context.state |= Vst::ProcessContext::kPlaying;
    context.sampleRate = sample_rate_hint_;
    context.projectTimeSamples = static_cast<Vst::TSamples>(transport_.frame);
    context.projectTimeMusic = transport_.beats;
    context.tempo = transport_.tempo_bpm;
    context.timeSigNumerator = transport_.numerator;
    context.timeSigDenominator = transport_.denominator;

    Vst::ProcessData data{};
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = static_cast<int32>(frames);
    data.numInputs = static_cast<int32>(bus_buffers_in_.size());
    data.numOutputs = static_cast<int32>(bus_buffers_out_.size());
    data.inputs = bus_buffers_in_.data();
    data.outputs = bus_buffers_out_.data();
    data.inputParameterChanges = &param_changes_;
    data.outputParameterChanges = &out_param_changes_;
    data.inputEvents = has_event_input_ ? &events_ : nullptr;
    out_events_.count = 0;
    data.outputEvents = &out_events_;
    data.processContext = &context;

    processor_->process(data);

    if (!output_buses_.empty()) {
      auto& main = bus_channel_ptrs_out_[0];
      for (int ch = 0; ch < strip_channels_; ++ch) {
        const size_t source = std::min(static_cast<size_t>(ch), main.size() - 1);
        std::copy_n(main[source], frames, outputs[ch]);
      }
    }

    // Whatever the plugin emitted becomes MIDI for the inserts below it.
    produced_count_ = 0;
    for (int32 i = 0; i < out_events_.count &&
                      produced_count_ < static_cast<int>(kMaxProduced); ++i) {
      const Vst::Event& event = out_events_.events[i];
      MidiEvent& out = produced_[produced_count_];
      out.frame = static_cast<uint32_t>(std::max<int32>(0, event.sampleOffset));
      out.size = 3;
      if (event.type == Vst::Event::kNoteOnEvent && event.noteOn.pitch >= 0) {
        out.data[0] = static_cast<uint8_t>(0x90 | (event.noteOn.channel & 0x0f));
        out.data[1] = static_cast<uint8_t>(event.noteOn.pitch);
        out.data[2] = static_cast<uint8_t>(
            std::clamp(event.noteOn.velocity, 0.0f, 1.0f) * 127.0f);
        ++produced_count_;
      } else if (event.type == Vst::Event::kNoteOffEvent &&
                 event.noteOff.pitch >= 0) {
        out.data[0] = static_cast<uint8_t>(0x80 | (event.noteOff.channel & 0x0f));
        out.data[1] = static_cast<uint8_t>(event.noteOff.pitch);
        out.data[2] = 0;
        ++produced_count_;
      }
    }
  }

  size_t take_midi_output(MidiEvent* out, size_t capacity) override {
    const size_t count = std::min(capacity, static_cast<size_t>(produced_count_));
    std::copy_n(produced_, count, out);
    produced_count_ = 0;
    return count;
  }

  std::vector<ParameterInfo> parameters() const override {
    std::vector<ParameterInfo> out;
    if (controller_ == nullptr) return out;
    const int32 count = controller_->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
      Vst::ParameterInfo info{};
      if (controller_->getParameterInfo(i, info) != kResultOk) continue;
      if (info.flags & Vst::ParameterInfo::kIsReadOnly) continue;
      // Everything is presented normalised: it is the only scale the interface
      // guarantees for every parameter.
      out.push_back({static_cast<uint32_t>(info.id), utf16_to_utf8(info.title),
                     0.0, 1.0, info.defaultNormalizedValue});
    }
    return out;
  }

  double parameter_value(uint32_t id) const override {
    if (controller_ == nullptr) return 0.0;
    return controller_->getParamNormalized(id);
  }

  void set_parameter(uint32_t id, double value) override {
    const double clamped = std::clamp(value, 0.0, 1.0);
    // The controller drives what an editor displays; the queue carries the same
    // edit to the processor on its next block.
    if (controller_ != nullptr) controller_->setParamNormalized(id, clamped);
    param_edits_.push({id, clamped});
  }

  std::vector<uint8_t> save_state() const override {
    std::vector<uint8_t> blob;
    if (component_ == nullptr) return blob;
    MemStream stream(&blob);
    component_->getState(&stream);
    return blob;
  }

  bool load_state(const std::vector<uint8_t>& blob) override {
    if (component_ == nullptr || blob.empty()) return false;
    std::vector<uint8_t> copy = blob;
    MemStream stream(&copy);
    if (component_->setState(&stream) != kResultOk) return false;
    if (controller_ != nullptr) {
      MemStream replay(&copy);
      replay.cursor = 0;
      controller_->setComponentState(&replay);
    }
    return true;
  }

  const PluginDescriptor& descriptor() const override { return desc_; }

  std::unique_ptr<PluginGui> create_gui() override;

  Vst::IEditController* controller() { return controller_; }

 private:
  struct ParamEdit {
    Vst::ParamID id;
    Vst::ParamValue value;
  };

  void read_bus_layout() {
    input_buses_.clear();
    output_buses_.clear();
    const int32 ins = component_->getBusCount(Vst::kAudio, Vst::kInput);
    for (int32 i = 0; i < ins; ++i) {
      Vst::BusInfo info{};
      if (component_->getBusInfo(Vst::kAudio, Vst::kInput, i, info) == kResultOk)
        input_buses_.push_back(info.channelCount);
    }
    const int32 outs = component_->getBusCount(Vst::kAudio, Vst::kOutput);
    for (int32 i = 0; i < outs; ++i) {
      Vst::BusInfo info{};
      if (component_->getBusInfo(Vst::kAudio, Vst::kOutput, i, info) == kResultOk)
        output_buses_.push_back(info.channelCount);
    }
    has_event_input_ = component_->getBusCount(Vst::kEvent, Vst::kInput) > 0;

    if (!input_buses_.empty()) desc_.audio_inputs = input_buses_[0];
    if (!output_buses_.empty()) desc_.audio_outputs = output_buses_[0];
    desc_.has_midi_input = has_event_input_;
  }

  // The controller's CC-to-parameter map, asked once on the UI thread: the
  // audio thread must not call into the controller per event.
  void build_midi_map_cache() {
    for (auto& channel : midi_map_cache_)
      channel.fill(Vst::kNoParamId);
    if (midi_mapping_ == nullptr) return;
    for (int16 channel = 0; channel < 16; ++channel) {
      for (int16 controller = 0; controller < Vst::kCountCtrlNumber; ++controller) {
        Vst::ParamID id = Vst::kNoParamId;
        if (midi_mapping_->getMidiControllerAssignment(0, channel, controller,
                                                       id) == kResultOk)
          midi_map_cache_[channel][controller] = id;
      }
    }
  }

  // Every bus gets real buffers, sidechains included: a plugin handed a null
  // sidechain pointer is within its rights to crash.
  void allocate_buffers(uint32_t max_block_frames) {
    auto build = [max_block_frames](const std::vector<int32>& layout,
                                    std::vector<std::vector<std::vector<float>>>& store,
                                    std::vector<std::vector<float*>>& ptrs,
                                    std::vector<Vst::AudioBusBuffers>& buses) {
      store.clear();
      ptrs.clear();
      buses.clear();
      for (int32 channels : layout) {
        store.emplace_back(std::max<int32>(channels, 1),
                           std::vector<float>(max_block_frames, 0.0f));
        ptrs.emplace_back();
        for (auto& channel : store.back()) ptrs.back().push_back(channel.data());
        Vst::AudioBusBuffers bus{};
        bus.numChannels = channels;
        bus.channelBuffers32 = ptrs.back().data();
        buses.push_back(bus);
      }
    };
    build(input_buses_, bus_store_in_, bus_channel_ptrs_in_, bus_buffers_in_);
    build(output_buses_, bus_store_out_, bus_channel_ptrs_out_, bus_buffers_out_);
  }

  PluginDescriptor desc_;
  std::shared_ptr<Vst3Module> module_;
  HostApplication host_;

  struct Handler : Vst::IComponentHandler {
    Vst3Instance* owner = nullptr;
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
      if (same_iid(iid, FUnknown_iid) || same_iid(iid, Vst::IComponentHandler_iid)) {
        *obj = this;
        return kResultOk;
      }
      *obj = nullptr;
      return kNoInterface;
    }
    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
    // The editor's own knob moves arrive here and go to the DSP the same way
    // the generic editor's do.
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) override {
      owner->param_edits_.push({id, value});
      return kResultOk;
    }
    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
  } handler_;

  Vst::IComponent* component_ = nullptr;
  Vst::IEditController* controller_ = nullptr;
  Vst::IAudioProcessor* processor_ = nullptr;
  bool controller_distinct_ = false;
  bool active_ = false;
  bool has_event_input_ = false;
  int strip_channels_ = 2;
  double sample_rate_hint_ = 48000.0;

  std::vector<int32> input_buses_, output_buses_;
  std::vector<std::vector<std::vector<float>>> bus_store_in_, bus_store_out_;
  std::vector<std::vector<float*>> bus_channel_ptrs_in_, bus_channel_ptrs_out_;
  std::vector<Vst::AudioBusBuffers> bus_buffers_in_, bus_buffers_out_;

  ParamChanges param_changes_;
  OutParamChanges out_param_changes_;
  EventList events_;
  OutEventList out_events_;

  static constexpr size_t kMaxProduced = 64;
  MidiEvent produced_[kMaxProduced];
  int produced_count_ = 0;

  Vst::IConnectionPoint* component_cp_ = nullptr;
  Vst::IConnectionPoint* controller_cp_ = nullptr;
  Vst::IMidiMapping* midi_mapping_ = nullptr;
  std::array<std::array<Vst::ParamID, Vst::kCountCtrlNumber>, 16> midi_map_cache_{};
  RtQueue<ParamEdit, 256> param_edits_;
  RtQueue<MidiEvent, 64> pending_midi_;
  TransportInfo transport_;

  friend struct Handler;
};

// --- editor ------------------------------------------------------------------

class Vst3Gui : public PluginGui {
 public:
  explicit Vst3Gui(Vst3Instance* owner) : owner_(owner) {}
  ~Vst3Gui() override { detach(); }

  bool attach(uintptr_t parent_window) override {
    Vst::IEditController* controller = owner_->controller();
    if (controller == nullptr) return false;

    view_ = controller->createView(Vst::ViewType::kEditor);
    if (view_ == nullptr) return false;

    if (view_->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
      view_->release();
      view_ = nullptr;
      return false;
    }

    // The frame goes in before attach: the view registers its timers and file
    // descriptors during attach, and without a run loop it never draws.
    view_->setFrame(&frame_);
    if (view_->attached(reinterpret_cast<void*>(parent_window),
                        kPlatformTypeX11EmbedWindowID) != kResultOk) {
      view_->setFrame(nullptr);
      view_->release();
      view_ = nullptr;
      // The view may have registered plumbing before failing; a later attach
      // must not pump handlers belonging to a view that no longer exists.
      frame_.timers.clear();
      frame_.fds.clear();
      return false;
    }
    return true;
  }

  void detach() override {
    if (view_ == nullptr) return;
    view_->removed();
    view_->setFrame(nullptr);
    view_->release();
    view_ = nullptr;
    frame_.timers.clear();
    frame_.fds.clear();
  }

  void idle() override { frame_.pump(); }

  bool preferred_size(int* width, int* height) const override {
    if (view_ == nullptr) return false;
    ViewRect rect{};
    if (view_->getSize(&rect) != kResultOk) return false;
    *width = rect.getWidth();
    *height = rect.getHeight();
    return *width > 0 && *height > 0;
  }

 private:
  Vst3Instance* owner_;
  IPlugView* view_ = nullptr;
  RunLoopFrame frame_;
};

std::unique_ptr<PluginGui> Vst3Instance::create_gui() {
  if (controller_ == nullptr) return nullptr;
  return std::make_unique<Vst3Gui>(this);
}

// --- backend -----------------------------------------------------------------

std::vector<fs::path> vst3_search_paths() {
  std::vector<fs::path> paths{"/usr/lib/vst3", "/usr/local/lib/vst3"};
  if (const char* home = std::getenv("HOME")) paths.emplace_back(fs::path(home) / ".vst3");
  return paths;
}

// Bundle dir -> the shared object inside it, empty when the layout is wrong.
fs::path bundle_binary(const fs::path& bundle) {
  const fs::path dir = bundle / "Contents" / "x86_64-linux";
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec))
    if (entry.path().extension() == ".so") return entry.path();
  return {};
}

class Vst3Backend : public PluginBackend {
 public:
  PluginFormat format() const override { return PluginFormat::Vst3; }

  std::vector<PluginDescriptor> scan() override {
    std::vector<PluginDescriptor> found;
    for (const fs::path& dir : vst3_search_paths()) {
      std::error_code ec;
      if (!fs::is_directory(dir, ec)) continue;
      // Recursive: installers group bundles in vendor subfolders, which the
      // spec allows. A found bundle is not descended into — its own tree is
      // the plugin's business.
      fs::recursive_directory_iterator it(dir, ec), end;
      for (; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".vst3") continue;
        scan_bundle(it->path(), found);
        it.disable_recursion_pending();
      }
    }
    return found;
  }

  std::unique_ptr<PluginInstance> instantiate(const PluginDescriptor& desc) override {
    std::shared_ptr<Vst3Module> module = module_for(desc.path);
    if (module == nullptr) return nullptr;

    // The uid stores the class id as hex, factory order being unstable across
    // rescans.
    // Parsed by hand: a corrupted session must give a null plugin back, not an
    // exception escaping into std::terminate.
    TUID cid = {};
    if (desc.uid.size() != 32) return nullptr;
    for (int i = 0; i < 32; ++i) {
      const char c = desc.uid[i];
      int nibble = 0;
      if (c >= '0' && c <= '9') {
        nibble = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        nibble = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        nibble = c - 'A' + 10;
      } else {
        return nullptr;
      }
      cid[i / 2] = static_cast<char>((cid[i / 2] << 4) | nibble);
    }

    auto instance = std::make_unique<Vst3Instance>(desc, std::move(module));
    if (!instance->create(cid)) return nullptr;
    return instance;
  }

 private:
  std::shared_ptr<Vst3Module> module_for(const std::string& bundle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = modules_.find(bundle);
    if (it != modules_.end())
      if (std::shared_ptr<Vst3Module> alive = it->second.lock()) return alive;

    const fs::path binary = bundle_binary(bundle);
    if (binary.empty()) return nullptr;
    std::shared_ptr<Vst3Module> module = Vst3Module::open(binary);
    if (module != nullptr) modules_[bundle] = module;
    return module;
  }

  void scan_bundle(const fs::path& bundle, std::vector<PluginDescriptor>& out) {
    std::shared_ptr<Vst3Module> module = module_for(bundle.string());
    if (module == nullptr) return;

    IPluginFactory* factory = module->factory();
    PFactoryInfo factory_info{};
    factory->getFactoryInfo(&factory_info);

    const int32 count = factory->countClasses();
    for (int32 i = 0; i < count; ++i) {
      PClassInfo info{};
      if (factory->getClassInfo(i, &info) != kResultOk) continue;
      if (std::strcmp(info.category, kVstAudioEffectClass) != 0) continue;

      PluginDescriptor desc;
      desc.format = PluginFormat::Vst3;
      desc.name = info.name;
      desc.vendor = factory_info.vendor;
      desc.path = bundle.string();
      char hex[33] = {};
      for (int b = 0; b < 16; ++b)
        std::snprintf(hex + b * 2, 3, "%02x",
                      static_cast<unsigned char>(info.cid[b]));
      desc.uid = hex;
      out.push_back(std::move(desc));
    }
  }

  std::mutex mutex_;
  std::map<std::string, std::weak_ptr<Vst3Module>> modules_;
};

}  // namespace

std::unique_ptr<PluginBackend> make_vst3_backend() {
  return std::make_unique<Vst3Backend>();
}

}  // namespace nirbija
