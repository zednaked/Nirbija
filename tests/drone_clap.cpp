// Loads the extracted Drone - the .clap built next to the host - back through
// the host's own CLAP backend, and plays it. What the wrapper promises to a
// host (ports, parameters, events, state) is checked here by a host that did
// not write it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/drone.h"
#include "core/plugin.h"
#include "hosting/clap_backend.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

constexpr uint32_t kBlock = 256;
constexpr double kRate = 48000.0;

struct Run {
  float peak = 0.0f;
  double zc_per_sec = 0.0;
  bool finite = true;
};

Run run(nirbija::PluginInstance& plugin, int blocks, int skip = 0) {
  std::vector<float> l(kBlock), r(kBlock);
  float* outs[2] = {l.data(), r.data()};
  std::vector<float> silence(kBlock, 0.0f);
  const float* ins[2] = {silence.data(), silence.data()};
  Run out;
  long zc = 0, counted = 0;
  float previous = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    plugin.process(ins, outs, kBlock);
    if (b < skip) continue;
    for (uint32_t i = 0; i < kBlock; ++i) {
      const float s = l[i];
      if (!std::isfinite(s)) out.finite = false;
      out.peak = std::max(out.peak, std::fabs(s));
      if (previous <= 0.0f && s > 0.0f) ++zc;
      previous = s;
      ++counted;
    }
  }
  if (counted > 0) out.zc_per_sec = zc * (kRate / counted);
  return out;
}

int seconds(double s) { return static_cast<int>(s * kRate / kBlock); }

}  // namespace

int main() {
  // The backend also walks the machine's own CLAP folders; that is fine, the
  // one we want is found by id whatever else turns up.
  setenv("CLAP_PATH", NIRBIJA_DRONE_CLAP_DIR, 1);
  auto backend = nirbija::make_clap_backend();
  const nirbija::PluginDescriptor* found = nullptr;
  const std::vector<nirbija::PluginDescriptor> all = backend->scan();
  for (const auto& desc : all)
    if (desc.uid == "com.nirbija.drone") found = &desc;
  if (found == nullptr) {
    std::fprintf(stderr, "FAIL the Drone .clap was not found under %s\n",
                 NIRBIJA_DRONE_CLAP_DIR);
    return 1;
  }
  if (found->kind != nirbija::PluginKind::Instrument)
    fail("the CLAP does not declare itself an instrument");
  if (found->name != "Nirbija Drone") fail("the CLAP's name is not Nirbija Drone");

  auto plugin = backend->instantiate(*found);
  if (plugin == nullptr) {
    std::fprintf(stderr, "FAIL the Drone .clap did not instantiate\n");
    return 1;
  }
  plugin->set_channel_layout(2);
  if (!plugin->activate(kRate, kBlock)) {
    std::fprintf(stderr, "FAIL the Drone .clap did not activate\n");
    return 1;
  }
  if (plugin->descriptor().audio_inputs != 0 ||
      plugin->descriptor().audio_outputs != 2)
    fail("the CLAP does not present no-in, stereo-out");
  if (!plugin->descriptor().has_midi_input)
    fail("the CLAP does not present a note input");

  const auto params = plugin->parameters();
  if (params.size() != nirbija::DroneInstance::kParamCount)
    fail("the CLAP does not list every parameter (" +
         std::to_string(params.size()) + ")");
  else if (params[nirbija::DroneInstance::Swell].name != "Swell")
    fail("parameter 24 through CLAP is not Swell");

  // One sine string at A2, swell straight up: sound, at 110 Hz.
  for (int v = 0; v < nirbija::DroneInstance::kVoices; ++v) {
    const uint32_t base = static_cast<uint32_t>(v) * nirbija::DroneInstance::kVoiceStride;
    plugin->set_parameter(base + nirbija::DroneInstance::Level, v == 0 ? 1.0 : 0.0);
    plugin->set_parameter(base + nirbija::DroneInstance::Shape, 0.0);
    plugin->set_parameter(base + nirbija::DroneInstance::Detune, 0.0);
    plugin->set_parameter(base + nirbija::DroneInstance::Interval, 0.0);
  }
  plugin->set_parameter(nirbija::DroneInstance::Drift, 0.0);
  plugin->set_parameter(nirbija::DroneInstance::Motion, 0.0);
  plugin->set_parameter(nirbija::DroneInstance::Space, 0.0);
  plugin->set_parameter(nirbija::DroneInstance::Grit, 0.0);
  plugin->set_parameter(nirbija::DroneInstance::Width, 0.0);
  plugin->set_parameter(nirbija::DroneInstance::Cutoff, 1.0);
  plugin->set_parameter(nirbija::DroneInstance::Glide, 0.0);
  plugin->set_parameter(nirbija::DroneInstance::Rise, 0.05);
  plugin->set_parameter(nirbija::DroneInstance::Swell, 1.0);
  plugin->set_parameter(nirbija::DroneInstance::Root, 45.0);
  const Run tone = run(*plugin, seconds(3.0), seconds(1.0));
  if (!tone.finite) fail("the CLAP put out a NaN");
  if (tone.peak < 0.05f)
    fail("the CLAP is silent with the swell up (peak " +
         std::to_string(tone.peak) + ")");
  if (std::fabs(tone.zc_per_sec - 110.0) > 3.0)
    fail("the CLAP's root string is not at 110 Hz (zc " +
         std::to_string(tone.zc_per_sec) + ")");
  if (std::fabs(plugin->parameter_value(nirbija::DroneInstance::Root) - 45.0) > 1e-9)
    fail("a parameter written through CLAP did not read back");

  // A note through the host's MIDI path re-roots it, one octave up.
  nirbija::MidiEvent on;
  on.size = 3;
  on.data[0] = 0x90;
  on.data[1] = 57;
  on.data[2] = 100;
  plugin->queue_midi(on);
  const Run moved = run(*plugin, seconds(3.0), seconds(1.0));
  if (std::fabs(moved.zc_per_sec - 220.0) > 5.0)
    fail("a note through CLAP did not re-root the drone (zc " +
         std::to_string(moved.zc_per_sec) + ")");
  if (std::fabs(plugin->parameter_value(nirbija::DroneInstance::Root) - 57.0) > 1e-9)
    fail("the root a note set is not visible through CLAP params");

  // State through the CLAP stream, into a second instance.
  const std::vector<uint8_t> blob = plugin->save_state();
  if (blob.empty()) fail("the CLAP saved an empty state");
  auto other = backend->instantiate(*found);
  if (other == nullptr) {
    fail("a second instance did not instantiate");
  } else {
    other->set_channel_layout(2);
    other->activate(kRate, kBlock);
    if (!other->load_state(blob)) fail("the CLAP refused its own state");
    // The host applies loaded state on the next process call; give it one.
    run(*other, 1);
    if (std::fabs(other->parameter_value(nirbija::DroneInstance::Root) - 57.0) > 1e-9)
      fail("the root did not survive a CLAP state round trip");
    if (std::fabs(other->parameter_value(nirbija::DroneInstance::Swell) - 1.0) > 1e-9)
      fail("the swell did not survive a CLAP state round trip");
  }

  plugin->deactivate();
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
