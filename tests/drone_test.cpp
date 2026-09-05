#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/channel_strip.h"
#include "core/drone.h"
#include "core/step_sequencer.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

constexpr uint32_t kBlock = 256;
constexpr double kRate = 48000.0;

using nirbija::DroneInstance;

// The instance goes on the heap: it carries a reverb's worth of buffers and
// the ASan pass runs the tests with a smaller stack than the desktop has.
std::unique_ptr<DroneInstance> make() {
  auto drone = std::make_unique<DroneInstance>();
  drone->set_channel_layout(2);
  drone->activate(kRate, kBlock);
  return drone;
}

// One string, a sine, nothing moving, no room, filter wide open. The
// configuration every pitch measurement below starts from.
void bare(DroneInstance& drone, int interval, double detune_cents = 0.0) {
  for (int v = 0; v < DroneInstance::kVoices; ++v) {
    const uint32_t base = static_cast<uint32_t>(v) * DroneInstance::kVoiceStride;
    drone.set_parameter(base + DroneInstance::Level, v == 0 ? 1.0 : 0.0);
    drone.set_parameter(base + DroneInstance::Shape, 0.0);
    drone.set_parameter(base + DroneInstance::Detune, v == 0 ? detune_cents : 0.0);
    drone.set_parameter(base + DroneInstance::Interval, v == 0 ? interval : 0.0);
  }
  drone.set_parameter(DroneInstance::Drift, 0.0);
  drone.set_parameter(DroneInstance::Motion, 0.0);
  drone.set_parameter(DroneInstance::Space, 0.0);
  drone.set_parameter(DroneInstance::Grit, 0.0);
  drone.set_parameter(DroneInstance::Width, 0.0);
  drone.set_parameter(DroneInstance::Resonance, 0.0);
  drone.set_parameter(DroneInstance::Cutoff, 1.0);
  drone.set_parameter(DroneInstance::Glide, 0.0);
  drone.set_parameter(DroneInstance::Rise, 0.05);
  drone.set_parameter(DroneInstance::Swell, 1.0);
}

struct Run {
  double rms = 0.0;
  float peak = 0.0f;
  float max_delta = 0.0f;  // largest sample-to-sample jump
  double zc_per_sec = 0.0;
  bool finite = true;
};

// Runs `blocks` blocks and listens to the left channel. `skip` leading
// blocks are processed but not counted, so a measurement can wait for the
// swell and the smoothing to settle.
Run run(DroneInstance& drone, int blocks, int skip = 0) {
  std::vector<float> l(kBlock), r(kBlock);
  float* outs[2] = {l.data(), r.data()};
  const float* ins[2] = {nullptr, nullptr};
  Run out;
  double energy = 0.0;
  long counted = 0, zc = 0;
  float previous = 0.0f;
  bool have_previous = false;
  for (int b = 0; b < blocks; ++b) {
    drone.process(ins, outs, kBlock);
    if (b < skip) {
      previous = l[kBlock - 1];
      have_previous = true;
      continue;
    }
    for (uint32_t i = 0; i < kBlock; ++i) {
      const float s = l[i];
      if (!std::isfinite(s)) out.finite = false;
      energy += static_cast<double>(s) * s;
      out.peak = std::max(out.peak, std::fabs(s));
      if (have_previous) {
        out.max_delta = std::max(out.max_delta, std::fabs(s - previous));
        if (previous <= 0.0f && s > 0.0f) ++zc;
      }
      previous = s;
      have_previous = true;
      ++counted;
    }
  }
  if (counted > 0) {
    out.rms = std::sqrt(energy / counted);
    out.zc_per_sec = zc * (kRate / counted);
  }
  return out;
}

int seconds(double s) { return static_cast<int>(s * kRate / kBlock); }

}  // namespace

int main() {
  // The tuning tables are the instrument's whole claim to being a drone
  // rather than a synth, so they are checked as numbers first.
  {
    if (std::fabs(DroneInstance::interval_ratio(7, true) - 1.5) > 1e-12)
      fail("a just fifth is not 3:2");
    if (std::fabs(DroneInstance::interval_ratio(12, true) - 2.0) > 1e-12)
      fail("a just octave is not 2:1");
    if (std::fabs(DroneInstance::interval_ratio(-12, true) - 0.5) > 1e-12)
      fail("a just octave down is not 1:2");
    if (std::fabs(DroneInstance::interval_ratio(-5, true) - 0.75) > 1e-12)
      fail("a just fourth down is not 3:4");
    if (std::fabs(DroneInstance::interval_ratio(19, true) - 3.0) > 1e-12)
      fail("a just twelfth is not 3:1");
    if (std::fabs(DroneInstance::interval_ratio(7, false) - 1.4983070768766815) > 1e-9)
      fail("a tempered fifth is not 2^(7/12)");
    if (std::fabs(DroneInstance::midi_to_hz(69) - 440.0) > 1e-9)
      fail("A4 is not 440");
  }

  // A fresh drone sounds without being asked, but not at once: it rises out
  // of nothing over the default four seconds.
  {
    auto drone = make();
    const Run first = run(*drone, seconds(0.2));
    if (!first.finite) fail("a fresh drone put out a NaN");
    if (first.peak > 0.02f)
      fail("a fresh drone came in too fast (peak " + std::to_string(first.peak) + ")");
    const Run later = run(*drone, seconds(5.0), seconds(4.5));
    if (later.rms < 0.02)
      fail("a fresh drone never came up (rms " + std::to_string(later.rms) + ")");
  }

  // Push the swell and the whole set comes up, and comes up smoothly: the
  // largest sample-to-sample step has to be what six low tones would make
  // on their own, not a click.
  {
    auto drone = make();
    drone->set_parameter(DroneInstance::Rise, 1.0);
    drone->set_parameter(DroneInstance::Swell, 1.0);
    const Run ramp = run(*drone, seconds(1.2));
    if (!ramp.finite) fail("the swell put out a NaN");
    if (ramp.rms < 0.02)
      fail("the swell did not bring the drone up (rms " +
           std::to_string(ramp.rms) + ")");
    if (ramp.max_delta > 0.12f)
      fail("the swell clicked (step " + std::to_string(ramp.max_delta) + ")");
    if (drone->swell_position() < 0.99f)
      fail("swell_position did not reach the top after the rise");
    for (int v = 0; v < DroneInstance::kVoices; ++v)
      if (drone->voice_gain(v) <= 0.0f)
        fail("string " + std::to_string(v + 1) + " reports no gain with the swell up");
    // Pull it down: the level has to fall, and without a step either.
    drone->set_parameter(DroneInstance::Swell, 0.0);
    const Run fall = run(*drone, seconds(1.5));
    if (fall.max_delta > 0.12f)
      fail("the swell clicked on the way down (step " +
           std::to_string(fall.max_delta) + ")");
    const Run after = run(*drone, seconds(0.5));
    if (after.rms > ramp.rms * 0.5)
      fail("the drone did not come down with the swell");
  }

  // Pitch. One sine string at the root, and the zero-crossing rate is the
  // root's frequency; move the string a just fifth and it is three halves.
  {
    auto drone = make();
    bare(*drone, 0);
    drone->set_parameter(DroneInstance::Root, 45);  // A2, 110 Hz
    drone->set_parameter(DroneInstance::Just, 1.0);
    const Run root = run(*drone, seconds(3.0), seconds(1.0));
    if (std::fabs(root.zc_per_sec - 110.0) > 3.0)
      fail("the root string does not sound at 110 Hz (zc " +
           std::to_string(root.zc_per_sec) + ")");
    drone->set_parameter(DroneInstance::Interval, 7);
    const Run fifth = run(*drone, seconds(3.0), seconds(1.0));
    if (std::fabs(fifth.zc_per_sec - 165.0) > 4.0)
      fail("a fifth above 110 Hz does not sound near 165 Hz (zc " +
           std::to_string(fifth.zc_per_sec) + ")");
    drone->set_parameter(DroneInstance::Interval, -12);
    const Run below = run(*drone, seconds(3.0), seconds(1.0));
    if (std::fabs(below.zc_per_sec - 55.0) > 2.0)
      fail("an octave below 110 Hz does not sound near 55 Hz (zc " +
           std::to_string(below.zc_per_sec) + ")");
  }

  // A MIDI note re-roots the drone. Nothing about the note-off matters.
  {
    auto drone = make();
    bare(*drone, 0);
    drone->set_parameter(DroneInstance::Root, 45);
    run(*drone, seconds(0.5));
    nirbija::MidiEvent on;
    on.size = 3;
    on.data[0] = 0x90;
    on.data[1] = 57;  // A3
    on.data[2] = 100;
    drone->queue_midi(on);
    nirbija::MidiEvent off = on;
    off.data[0] = 0x80;
    off.data[2] = 0;
    drone->queue_midi(off);
    if (drone->parameter_value(DroneInstance::Root) != 57.0)
      fail("a note-on did not re-root the drone");
    const Run moved = run(*drone, seconds(3.0), seconds(1.0));
    if (std::fabs(moved.zc_per_sec - 220.0) > 5.0)
      fail("the drone did not glide to the new root (zc " +
           std::to_string(moved.zc_per_sec) + ")");
    if (std::fabs(drone->root_now() - 57.0) > 0.01)
      fail("root_now did not arrive at the new root");
    // A note outside the root's range is clamped, not refused.
    on.data[1] = 5;
    drone->queue_midi(on);
    if (drone->parameter_value(DroneInstance::Root) != 24.0)
      fail("a note below the range was not clamped to the floor");
  }

  // The glide is a glide: with a long one, the pitch a second after a root
  // change is still between where it was and where it is going.
  {
    auto drone = make();
    bare(*drone, 0);
    drone->set_parameter(DroneInstance::Root, 45);
    run(*drone, seconds(1.0));
    drone->set_parameter(DroneInstance::Glide, 0.75);  // ~2.2 s
    drone->set_parameter(DroneInstance::Root, 57);
    const Run mid = run(*drone, seconds(1.0), seconds(0.5));
    if (mid.zc_per_sec < 120.0 || mid.zc_per_sec > 205.0)
      fail("a long glide jumped instead of sliding (zc " +
           std::to_string(mid.zc_per_sec) + ")");
  }

  // The room outlives the swell. With Space up, cut the swell hard and there
  // is still sound a second later; with Space at zero there is not.
  {
    auto drone = make();
    bare(*drone, 0);
    drone->set_parameter(DroneInstance::Space, 1.0);
    run(*drone, seconds(2.0));
    drone->set_parameter(DroneInstance::Swell, 0.0);
    run(*drone, seconds(0.1));
    const Run tail = run(*drone, seconds(1.0));
    if (tail.rms < 0.005)
      fail("space left no tail after the swell was cut (rms " +
           std::to_string(tail.rms) + ")");
    if (!tail.finite) fail("the reverb tail went non-finite");

    auto dry = make();
    bare(*dry, 0);
    run(*dry, seconds(2.0));
    dry->set_parameter(DroneInstance::Swell, 0.0);
    run(*dry, seconds(0.3));
    const Run none = run(*dry, seconds(0.5));
    if (none.peak > 1e-3f)
      fail("with no space, sound remained after the swell was cut (peak " +
           std::to_string(none.peak) + ")");
  }

  // The filter closes. A saw string through a low cutoff has less energy
  // above the fundamental than the same string wide open.
  {
    auto open = make();
    bare(*open, 0);
    open->set_parameter(DroneInstance::Shape, 1.0);
    open->set_parameter(DroneInstance::Cutoff, 1.0);
    const Run bright = run(*open, seconds(2.0), seconds(1.0));

    auto closed = make();
    bare(*closed, 0);
    closed->set_parameter(DroneInstance::Shape, 1.0);
    closed->set_parameter(DroneInstance::Cutoff, 0.15);  // ~100 Hz
    const Run dark = run(*closed, seconds(2.0), seconds(1.0));
    // A saw has many zero crossings per cycle only if its edges ring; the
    // cleaner test is that the closed one is quieter and has the smaller
    // maximum step, since the step of a saw is its edge.
    if (dark.max_delta >= bright.max_delta * 0.6)
      fail("closing the filter did not soften the saw's edge (open " +
           std::to_string(bright.max_delta) + " closed " +
           std::to_string(dark.max_delta) + ")");
  }

  // The tide moves. With everything wandering fast, the filter's breath
  // reads both positive and negative within a few seconds.
  {
    auto drone = make();
    drone->set_parameter(DroneInstance::Tide, 1.0);  // 1 Hz
    drone->set_parameter(DroneInstance::Swell, 1.0);
    float lo = 1.0f, hi = -1.0f;
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[2] = {l.data(), r.data()};
    const float* ins[2] = {nullptr, nullptr};
    for (int b = 0; b < seconds(3.0); ++b) {
      drone->process(ins, outs, kBlock);
      lo = std::min(lo, drone->filter_breath());
      hi = std::max(hi, drone->filter_breath());
    }
    if (hi - lo < 1.0f)
      fail("the breath did not move over three seconds at full tide (" +
           std::to_string(lo) + ".." + std::to_string(hi) + ")");
  }

  // Drift makes two strings at the same pitch wander apart. With drift off
  // they are at exactly the same pitch and the left channel is one tone;
  // with drift on the pitch of the root string is measurably off from its
  // undrifted value at some point in a slow run.
  {
    auto drone = make();
    bare(*drone, 0);
    drone->set_parameter(DroneInstance::Root, 57);  // 220 Hz
    drone->set_parameter(DroneInstance::Drift, 1.0);
    drone->set_parameter(DroneInstance::Tide, 1.0);
    run(*drone, seconds(1.0));
    double lo = 1e9, hi = 0.0;
    for (int w = 0; w < 6; ++w) {
      const Run window = run(*drone, seconds(0.5));
      lo = std::min(lo, window.zc_per_sec);
      hi = std::max(hi, window.zc_per_sec);
    }
    // Twelve cents at 220 Hz is about 1.5 Hz either way.
    if (hi - lo < 0.5)
      fail("full drift did not move the root string (zc " +
           std::to_string(lo) + ".." + std::to_string(hi) + ")");
  }

  // Mono. A one-channel strip gets the left side and nothing is written
  // past it.
  {
    auto drone = std::make_unique<DroneInstance>();
    drone->set_channel_layout(1);
    drone->activate(kRate, kBlock);
    drone->set_parameter(DroneInstance::Rise, 0.05);
    drone->set_parameter(DroneInstance::Swell, 1.0);
    std::vector<float> l(kBlock);
    float* outs[1] = {l.data()};
    const float* ins[1] = {nullptr};
    float peak = 0.0f;
    for (int b = 0; b < seconds(0.5); ++b) {
      drone->process(ins, outs, kBlock);
      for (float s : l) peak = std::max(peak, std::fabs(s));
    }
    if (peak < 0.01f) fail("a mono drone is silent");
  }

  // State. Every parameter survives the round trip; a blob from another
  // plugin is refused; a blob missing a parameter leaves that one at its
  // default rather than at zero.
  {
    auto drone = make();
    drone->set_parameter(DroneInstance::Root, 50);
    drone->set_parameter(DroneInstance::Just, 0);
    drone->set_parameter(DroneInstance::Swell, 0.37);
    drone->set_parameter(DroneInstance::Rise, 12.5);
    drone->set_parameter(DroneInstance::Space, 0.81);
    drone->set_parameter(2 * DroneInstance::kVoiceStride + DroneInstance::Interval, -19);
    drone->set_parameter(2 * DroneInstance::kVoiceStride + DroneInstance::Detune, -33);
    drone->set_parameter(5 * DroneInstance::kVoiceStride + DroneInstance::Shape, 0.125);
    const auto blob = drone->save_state();

    auto restored = make();
    if (!restored->load_state(blob)) fail("load_state refused its own blob");
    for (uint32_t id = 0; id < DroneInstance::kParamCount; ++id) {
      if (std::fabs(restored->parameter_value(id) - drone->parameter_value(id)) > 1e-5)
        fail(std::string("parameter ") + DroneInstance::param_name(id) +
             " did not survive the session");
    }

    const std::string foreign = "1\n0\n1\n0\n";
    if (restored->load_state(std::vector<uint8_t>(foreign.begin(), foreign.end())))
      fail("a blob from another plugin was accepted as a drone");
    if (restored->load_state({})) fail("an empty blob was accepted");

    const std::string partial = "drone 1\n26 60\n";
    auto sparse = make();
    if (!sparse->load_state(std::vector<uint8_t>(partial.begin(), partial.end())))
      fail("a partial blob was refused");
    if (sparse->parameter_value(DroneInstance::Root) != 60.0)
      fail("the one parameter in a partial blob did not load");
    if (std::fabs(sparse->parameter_value(DroneInstance::Space) -
                  DroneInstance::param_default(DroneInstance::Space)) > 1e-9)
      fail("a parameter missing from the blob did not keep its default");
  }

  // Parameters clamp and round. An interval is whole semitones, a root is
  // a whole note, and nothing takes a NaN.
  {
    auto drone = make();
    drone->set_parameter(DroneInstance::Interval, 7.4);
    if (drone->parameter_value(DroneInstance::Interval) != 7.0)
      fail("an interval was not rounded to a semitone");
    drone->set_parameter(DroneInstance::Interval, 40.0);
    if (drone->parameter_value(DroneInstance::Interval) != 24.0)
      fail("an interval was not clamped to two octaves");
    drone->set_parameter(DroneInstance::Swell, std::nan(""));
    if (!std::isfinite(drone->parameter_value(DroneInstance::Swell)))
      fail("a NaN got into the swell");
    drone->set_parameter(DroneInstance::Rise, 0.0);
    if (drone->parameter_value(DroneInstance::Rise) < 0.05)
      fail("rise accepted zero, which would divide the ramp by it");
    const auto params = drone->parameters();
    if (params.size() != DroneInstance::kParamCount)
      fail("parameters() does not list every parameter");
    if (params[DroneInstance::Swell].name != "Swell")
      fail("the swell is not named Swell");
    if (params[DroneInstance::Detune].name != "String 1 Detune")
      fail("a string parameter is not named for its string");
  }

  // Presets. Each one lands every string and the tuning exactly, and leaves
  // the root, the swell and the weather alone - a preset is the strings,
  // not the performance.
  {
    if (DroneInstance::preset_count() < 6) fail("fewer than six presets");
    auto drone = make();
    drone->set_parameter(DroneInstance::Root, 50);
    drone->set_parameter(DroneInstance::Swell, 0.33);
    drone->set_parameter(DroneInstance::Space, 0.91);
    for (int i = 0; i < DroneInstance::preset_count(); ++i) {
      const DroneInstance::Preset& p = DroneInstance::preset(i);
      if (p.name == nullptr || p.name[0] == '\0')
        fail("preset " + std::to_string(i) + " has no name");
      drone->apply_preset(i);
      for (int v = 0; v < DroneInstance::kVoices; ++v) {
        const uint32_t base = static_cast<uint32_t>(v) * DroneInstance::kVoiceStride;
        if (drone->parameter_value(base + DroneInstance::Interval) != p.interval[v] ||
            std::fabs(drone->parameter_value(base + DroneInstance::Detune) - p.detune[v]) > 1e-9 ||
            std::fabs(drone->parameter_value(base + DroneInstance::Level) - p.level[v]) > 1e-9 ||
            std::fabs(drone->parameter_value(base + DroneInstance::Shape) - p.shape[v]) > 1e-9)
          fail(std::string("preset ") + p.name + " did not land string " +
               std::to_string(v + 1));
        // Every preset has to be representable: no interval past the range.
        if (p.interval[v] < -24 || p.interval[v] > 24)
          fail(std::string("preset ") + p.name + " has an interval out of range");
      }
      if ((drone->parameter_value(DroneInstance::Just) >= 0.5) != p.just)
        fail(std::string("preset ") + p.name + " did not set the tuning");
    }
    if (drone->parameter_value(DroneInstance::Root) != 50.0 ||
        std::fabs(drone->parameter_value(DroneInstance::Swell) - 0.33) > 1e-9 ||
        std::fabs(drone->parameter_value(DroneInstance::Space) - 0.91) > 1e-9)
      fail("a preset touched the root, the swell or the weather");
    drone->apply_preset(-1);
    drone->apply_preset(DroneInstance::preset_count());
    // The harmonic series preset, under just tuning, is exact harmonics.
    for (int i = 0; i < DroneInstance::preset_count(); ++i) {
      const DroneInstance::Preset& p = DroneInstance::preset(i);
      if (std::string(p.name) != "Harmonic series") continue;
      const double base = DroneInstance::interval_ratio(p.interval[0], true);
      for (int v = 0; v < DroneInstance::kVoices; ++v) {
        const double ratio = DroneInstance::interval_ratio(p.interval[v], true) / base;
        if (std::fabs(ratio - (v + 1)) > 1e-9)
          fail("harmonic series string " + std::to_string(v + 1) + " is " +
               std::to_string(ratio) + " times the lowest, not " +
               std::to_string(v + 1));
      }
    }
  }

  // A step sequencer above the drone in one strip re-roots it step by step:
  // the strip hands the sequencer's notes to the next insert, and each one
  // is a new root. Nothing in the host had to change for this; the test is
  // here so that stays true.
  {
    nirbija::ChannelStrip strip("drone", 2);
    strip.prepare(kRate, kBlock);
    auto seq = std::make_unique<nirbija::StepSequencerInstance>();
    // Two notes a bar apart on lane 0: sixteen 1/16 steps at 120 BPM is two
    // seconds round.
    seq->set_cell(0, 0, 0, 45, 100, true, 1.0f);
    seq->set_cell(0, 0, 8, 52, 100, true, 1.0f);
    auto drone = std::make_unique<DroneInstance>();
    bare(*drone, 0);
    DroneInstance* drone_ptr = drone.get();
    if (!strip.add_insert(std::move(seq))) fail("the strip refused the sequencer");
    if (!strip.add_insert(std::move(drone))) fail("the strip refused the drone");

    std::vector<float> l(kBlock), r(kBlock);
    float* buffers[2] = {l.data(), r.data()};
    bool saw_45 = false, saw_52 = false;
    float peak = 0.0f;
    const int total = seconds(6.0);
    for (int b = 0; b < total; ++b) {
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
      // Stopped for the first blocks: a sequencer starts on the transition
      // into play, not on finding the transport already moving.
      nirbija::TransportInfo transport;
      const int rolling = b - 2;
      transport.playing = rolling >= 0;
      transport.rolling = transport.playing;
      transport.tempo_bpm = 120.0;
      transport.frame = static_cast<uint64_t>(std::max(0, rolling)) * kBlock;
      transport.seconds = static_cast<double>(transport.frame) / kRate;
      transport.beats = transport.seconds * 2.0;
      transport.changed = rolling == 0;
      strip.process(buffers, kBlock, nullptr, 0, &transport);
      for (float s : l) peak = std::max(peak, std::fabs(s));
      const double root = drone_ptr->parameter_value(DroneInstance::Root);
      if (root == 45.0) saw_45 = true;
      if (root == 52.0) saw_52 = true;
    }
    if (!saw_45 || !saw_52)
      fail("the sequencer above the drone did not re-root it to both notes (45: " +
           std::to_string(saw_45) + ", 52: " + std::to_string(saw_52) + ")");
    if (peak < 0.05f)
      fail("the strip with sequencer and drone is silent (peak " +
           std::to_string(peak) + ")");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
