#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/fx_pad.h"

namespace {

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

float run(nirbija::FxPadInstance& fx, float level) {
  constexpr uint32_t kBlock = 256;
  std::vector<float> in_l(kBlock, level), in_r(kBlock, level);
  std::vector<float> out_l(kBlock), out_r(kBlock);
  const float* ins[2] = {in_l.data(), in_r.data()};
  float* outs[2] = {out_l.data(), out_r.data()};
  nirbija::TransportInfo transport;
  transport.playing = true;
  transport.rolling = true;
  transport.tempo_bpm = 120.0;
  fx.set_transport(transport);
  fx.process(ins, outs, kBlock);
  float peak = 0.0f;
  for (float s : out_l) peak = std::max(peak, std::fabs(s));
  return peak;
}

// Runs a steady tone through one pad and reports what came out. Two things the
// warm-up is for: the tempo-synced pads read a history buffer, and one caught
// cold has nothing but silence behind it - which looks exactly like a broken
// pad; and the wet mix is smoothed in, so measuring across the press would
// count the ramp's dry leakage as the pad doing something.
struct Measured {
  double rms = 0.0;
  double flat_fraction = 0.0;  // samples identical to the one before
  double window_swing = 1.0;   // loudest window over quietest
};

Measured sweep(int pad, int windows, int blocks_per_window) {
  constexpr uint32_t kBlock = 256;
  constexpr int kPressBlock = 200;
  constexpr int kWarmBlocks = 400;

  nirbija::FxPadInstance fx;
  fx.set_channel_layout(2);
  fx.activate(48000.0, kBlock);

  std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock), out_r(kBlock);
  const float* ins[2] = {in_l.data(), in_r.data()};
  float* outs[2] = {out_l.data(), out_r.data()};

  nirbija::TransportInfo transport;
  transport.playing = true;
  transport.rolling = true;
  transport.tempo_bpm = 120.0;
  transport.numerator = 4;
  transport.denominator = 4;

  double phase = 0.0;
  float previous = 1e9f;
  long flat = 0, counted = 0;
  double total_energy = 0.0;
  double loudest = 0.0, quietest = 1e9;

  const int total_blocks = kWarmBlocks + windows * blocks_per_window;
  double window_energy = 0.0;
  long window_counted = 0;

  for (int b = 0; b < total_blocks; ++b) {
    if (b == kPressBlock) fx.set_pad(pad, true);
    for (uint32_t i = 0; i < kBlock; ++i) {
      in_l[i] = in_r[i] = 0.5f * static_cast<float>(std::sin(phase));
      phase += 2.0 * 3.14159265358979 * 220.0 / 48000.0;
    }
    transport.seconds = b * static_cast<double>(kBlock) / 48000.0;
    transport.beats = transport.seconds * transport.tempo_bpm / 60.0;
    fx.set_transport(transport);
    fx.process(ins, outs, kBlock);

    if (b < kWarmBlocks) continue;
    for (uint32_t i = 0; i < kBlock; ++i) {
      total_energy += out_l[i] * out_l[i];
      window_energy += out_l[i] * out_l[i];
      if (std::fabs(out_l[i] - previous) < 1e-7f) ++flat;
      previous = out_l[i];
      ++counted;
      ++window_counted;
    }
    if ((b - kWarmBlocks + 1) % blocks_per_window == 0) {
      const double rms = std::sqrt(window_energy / window_counted);
      loudest = std::max(loudest, rms);
      quietest = std::min(quietest, rms);
      window_energy = 0.0;
      window_counted = 0;
    }
  }

  Measured out;
  out.rms = std::sqrt(total_energy / counted);
  out.flat_fraction = static_cast<double>(flat) / counted;
  out.window_swing = quietest > 1e-9 ? loudest / quietest : 1.0;
  return out;
}

}  // namespace

int main() {
  nirbija::FxPadInstance fx;
  fx.set_channel_layout(2);
  fx.activate(48000.0, 256);

  if (run(fx, 0.5f) < 0.45f) fail("dry signal does not pass through");

  fx.set_pad(nirbija::FxPadInstance::Dirty, true);
  float dirty = 0.0f;
  for (int i = 0; i < 8; ++i) dirty = std::max(dirty, run(fx, 0.2f));
  if (dirty < 0.25f) fail("dirty did not saturate");

  fx.set_pad(nirbija::FxPadInstance::Dirty, false);
  for (int i = 0; i < 8; ++i) run(fx, 0.0f);
  if (run(fx, 0.0f) > 1e-3f) fail("silence in is not silence out with pads off");

  fx.set_hold(true);
  fx.set_pad(nirbija::FxPadInstance::Crush, true);
  if (!fx.pad_on(nirbija::FxPadInstance::Crush)) fail("hold did not keep crush down");
  fx.set_hold(false);
  if (fx.pad_on(nirbija::FxPadInstance::Crush)) fail("dropping hold left crush on");

  const auto blob = fx.save_state();
  fx.set_pad(nirbija::FxPadInstance::Reverb, true);
  fx.set_hold(true);
  if (!fx.load_state(blob)) fail("load_state refused its own blob");
  if (fx.hold()) fail("restored hold from a blob that had it off");
  if (fx.pad_on(nirbija::FxPadInstance::Reverb))
    fail("a pad turned on after save survived a load of the old state");

  fx.set_hold(true);
  fx.set_pad(nirbija::FxPadInstance::Dirty, true);
  fx.set_pad(nirbija::FxPadInstance::Reverb, true);
  const auto kept = fx.save_state();
  nirbija::FxPadInstance restored;
  restored.activate(48000.0, 256);
  if (!restored.load_state(kept)) fail("load_state refused a blob with pads on");
  if (!restored.hold()) fail("hold did not survive the session");
  if (!restored.pad_on(nirbija::FxPadInstance::Dirty) ||
      !restored.pad_on(nirbija::FxPadInstance::Reverb))
    fail("latched pads did not come back");

  // Every pad has to actually do something to a tone. Reverse used to hold a
  // single sample forever - its read point advanced in step with the write
  // head instead of against it - and passed every check above, because none of
  // them listened to what a pad put out.
  for (int pad = 0; pad < nirbija::FxPadInstance::kPads; ++pad) {
    const Measured m = sweep(pad, 4, 150);
    const std::string name = nirbija::FxPadInstance::pad_name(pad);
    if (m.rms < 0.02)
      fail(name + " put out nothing at all (rms " + std::to_string(m.rms) + ")");
    // Gate and Cutter chop, so half their samples repeat a zero; a pad frozen
    // on one value repeats every single one.
    if (m.flat_fraction > 0.99)
      fail(name + " froze on one sample value");
  }

  // The talkbox sweeps its vowel on an LFO of its own. It used to read the
  // flanger's phase, which only advances while that pad is held, so the vowel
  // stood still unless both were down at once.
  const Measured talkbox = sweep(nirbija::FxPadInstance::Talkbox, 8, 150);
  if (talkbox.window_swing < 1.1)
    fail("the talkbox vowel is not moving (swing " +
         std::to_string(talkbox.window_swing) + ")");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
