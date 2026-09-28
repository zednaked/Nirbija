// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The Drone .clap opened straight from its file, the way a host that is not
// Nirbija would: what it says back to the host, and how it takes what the
// host says. Three promises of the wrapper are checked here that the host's
// own backend cannot see - the Root it reports when a played note re-roots
// the drone, a note at velocity zero treated as a release, and reset()
// leaving it silent without touching activate().

#include <clap/clap.h>
#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/drone.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

constexpr uint32_t kBlock = 256;
constexpr double kRate = 48000.0;

// A host that answers nothing and asks for nothing.
const void* host_get_extension(const clap_host_t*, const char*) { return nullptr; }
void host_noop(const clap_host_t*) {}

const clap_host_t kHost = {
    CLAP_VERSION_INIT, nullptr, "drone_clap_events", "Nirbija", "", "0",
    host_get_extension, host_noop, host_noop, host_noop,
};

// Input events: whatever the test queued for this block.
struct InEvents {
  clap_input_events_t list;
  std::vector<clap_event_note_t> notes;
  std::vector<clap_event_param_value_t> params;
  std::vector<const clap_event_header_t*> order;

  InEvents() {
    list.ctx = this;
    list.size = [](const clap_input_events_t* l) -> uint32_t {
      return static_cast<uint32_t>(static_cast<InEvents*>(l->ctx)->order.size());
    };
    list.get = [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* {
      auto* self = static_cast<InEvents*>(l->ctx);
      return i < self->order.size() ? self->order[i] : nullptr;
    };
  }
  void note_on(int16_t key, double velocity) {
    clap_event_note_t e{};
    e.header.size = sizeof(e);
    e.header.time = 0;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_NOTE_ON;
    e.note_id = -1;
    e.port_index = 0;
    e.channel = 0;
    e.key = key;
    e.velocity = velocity;
    notes.push_back(e);
  }
  void param(clap_id id, double value) {
    clap_event_param_value_t e{};
    e.header.size = sizeof(e);
    e.header.time = 0;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = id;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = value;
    params.push_back(e);
  }
  // Pointers are taken once everything is queued, so the vectors are still.
  void seal() {
    order.clear();
    for (const auto& n : notes) order.push_back(&n.header);
    for (const auto& p : params) order.push_back(&p.header);
  }
  void clear() {
    notes.clear();
    params.clear();
    order.clear();
  }
};

// Output events: every parameter value the plugin pushed.
struct OutEvents {
  clap_output_events_t list;
  std::vector<clap_event_param_value_t> params;
  int others = 0;

  OutEvents() {
    list.ctx = this;
    list.try_push = [](const clap_output_events_t* l, const clap_event_header_t* h) -> bool {
      auto* self = static_cast<OutEvents*>(l->ctx);
      if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        self->params.push_back(*reinterpret_cast<const clap_event_param_value_t*>(h));
      else
        ++self->others;
      return true;
    };
  }
};

struct Session {
  void* handle = nullptr;
  const clap_plugin_entry_t* entry = nullptr;
  const clap_plugin_t* plugin = nullptr;
  const clap_plugin_params_t* params = nullptr;
  std::vector<float> left = std::vector<float>(kBlock);
  std::vector<float> right = std::vector<float>(kBlock);
  InEvents in;
  OutEvents out;

  // One block. Returns the left channel's peak.
  float process() {
    float* data[2] = {left.data(), right.data()};
    clap_audio_buffer_t buffer{};
    buffer.data32 = data;
    buffer.channel_count = 2;
    clap_process_t p{};
    p.steady_time = -1;
    p.frames_count = kBlock;
    p.transport = nullptr;
    p.audio_inputs = nullptr;
    p.audio_inputs_count = 0;
    p.audio_outputs = &buffer;
    p.audio_outputs_count = 1;
    in.seal();
    p.in_events = &in.list;
    p.out_events = &out.list;
    plugin->process(plugin, &p);
    in.clear();
    float peak = 0.0f;
    for (float s : left) peak = std::max(peak, std::fabs(s));
    return peak;
  }

  double root() const {
    double value = 0.0;
    params->get_value(plugin, nirbija::DroneInstance::Root, &value);
    return value;
  }
};

}  // namespace

int main() {
  const std::string path = std::string(NIRBIJA_DRONE_CLAP_DIR) + "/Nirbija Drone.clap";
  Session s;
  s.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (s.handle == nullptr) {
    std::fprintf(stderr, "FAIL could not open %s: %s\n", path.c_str(), dlerror());
    return 1;
  }
  s.entry = static_cast<const clap_plugin_entry_t*>(dlsym(s.handle, "clap_entry"));
  if (s.entry == nullptr || !s.entry->init(path.c_str())) {
    std::fprintf(stderr, "FAIL the .clap has no usable clap_entry\n");
    return 1;
  }
  const auto* factory = static_cast<const clap_plugin_factory_t*>(
      s.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  s.plugin = factory != nullptr ? factory->create_plugin(factory, &kHost, "com.nirbija.drone")
                                : nullptr;
  if (s.plugin == nullptr || !s.plugin->init(s.plugin)) {
    std::fprintf(stderr, "FAIL the Drone did not instantiate\n");
    return 1;
  }
  s.params = static_cast<const clap_plugin_params_t*>(
      s.plugin->get_extension(s.plugin, CLAP_EXT_PARAMS));
  if (s.params == nullptr) {
    std::fprintf(stderr, "FAIL the Drone has no params extension\n");
    return 1;
  }
  if (!s.plugin->activate(s.plugin, kRate, 32, kBlock)) fail("activate refused");
  s.plugin->start_processing(s.plugin);

  const double factory_root = nirbija::DroneInstance::param_default(nirbija::DroneInstance::Root);
  s.process();
  if (!s.out.params.empty())
    fail("the Drone reported a parameter change before anything changed");

  // A played note re-roots the drone, and the host is told: one Root event
  // out, carrying the key, in the block the note arrived in - and only once.
  s.in.note_on(57, 0.8);
  s.process();
  if (s.root() != 57.0) fail("a note-on did not re-root the drone");
  if (s.out.params.size() != 1)
    fail("expected one Root report after a note, got " +
         std::to_string(s.out.params.size()));
  else if (s.out.params[0].param_id != nirbija::DroneInstance::Root ||
           s.out.params[0].value != 57.0)
    fail("the report after a note was not Root = 57");
  s.out.params.clear();
  s.process();
  if (!s.out.params.empty()) fail("the Root was reported again with nothing new");

  // A note-on at velocity zero is a release: the drone stays where it is and
  // nothing is reported. It used to be rounded up to velocity 1 and re-root.
  s.in.note_on(40, 0.0);
  s.process();
  if (s.root() != 57.0) fail("a velocity-zero note-on re-rooted the drone");
  if (!s.out.params.empty()) fail("a velocity-zero note-on produced a report");

  // A Root the host set itself is not echoed back to it.
  s.in.param(nirbija::DroneInstance::Root, 50.0);
  s.process();
  if (s.root() != 50.0) fail("a host Root change did not land");
  if (!s.out.params.empty()) fail("the Drone echoed the host's own Root change");

  // Notes at the same key as the current root say nothing new.
  s.in.note_on(50, 1.0);
  s.process();
  if (!s.out.params.empty()) fail("a note at the current root was reported");

  // reset() on a sounding drone: the next block is as quiet as a fresh one's,
  // and finite. Swell full, rise at its shortest, so the drone is loud first.
  s.in.param(nirbija::DroneInstance::Swell, 1.0);
  s.in.param(nirbija::DroneInstance::Rise, 0.05);
  float loud = 0.0f;
  for (int b = 0; b < 40; ++b) loud = std::max(loud, s.process());
  if (loud < 0.05f) fail("the drone did not come up before the reset");
  s.plugin->reset(s.plugin);
  const float quiet = s.process();
  bool finite = true;
  for (float v : s.left) finite = finite && std::isfinite(v);
  if (!finite) fail("the block after reset is not finite");
  // A fresh drone at rise 0.05 s is well under half way up in one 5 ms
  // block; a drone that was not reset is at full level.
  if (quiet > loud * 0.5f)
    fail("reset left the drone sounding (peak " + std::to_string(quiet) +
         " against " + std::to_string(loud) + ")");
  (void)factory_root;

  s.plugin->stop_processing(s.plugin);
  s.plugin->deactivate(s.plugin);
  s.plugin->destroy(s.plugin);
  s.entry->deinit();
  dlclose(s.handle);

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
