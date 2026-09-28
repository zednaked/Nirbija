// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
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

struct Tone {
  double rms = 0.0;
  double zc_per_sec = 0.0;
  double flat_fraction = 0.0;
  double hf_ratio = 0.0;
};

// A held amount through a sine, after the wet mix has finished ramping.
// Frequency and the two-tone mix are chosen per call so pitch can count
// zero-crossings and the filter can compare high-band energy.
Tone held(int pad, float amount, double freq, bool two_tone, int measure_blocks) {
  constexpr uint32_t kBlock = 256;
  constexpr int kWarm = 250;
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

  fx.set_pad_amount(pad, amount);

  double phase = 0.0;
  double phase_hi = 0.0;
  float previous = 0.0f;
  long flat = 0, counted = 0, zc = 0;
  double energy = 0.0, hf_energy = 0.0;
  float lp = 0.0f;

  const int total = kWarm + measure_blocks;
  for (int b = 0; b < total; ++b) {
    for (uint32_t i = 0; i < kBlock; ++i) {
      const float lo = 0.45f * static_cast<float>(std::sin(phase));
      const float hi = 0.45f * static_cast<float>(std::sin(phase_hi));
      in_l[i] = in_r[i] = two_tone ? lo + hi : lo;
      phase += 2.0 * 3.14159265358979 * freq / 48000.0;
      phase_hi += 2.0 * 3.14159265358979 * 3000.0 / 48000.0;
    }
    transport.seconds = b * static_cast<double>(kBlock) / 48000.0;
    transport.beats = transport.seconds * transport.tempo_bpm / 60.0;
    fx.set_transport(transport);
    fx.process(ins, outs, kBlock);
    if (b < kWarm) continue;
    for (uint32_t i = 0; i < kBlock; ++i) {
      const float s = out_l[i];
      energy += s * s;
      lp += 0.08f * (s - lp);
      const float hf = s - lp;
      hf_energy += hf * hf;
      if (previous <= 0.0f && s > 0.0f) ++zc;
      if (std::fabs(s - previous) < 1e-7f) ++flat;
      previous = s;
      ++counted;
    }
  }

  Tone out;
  out.rms = std::sqrt(energy / counted);
  out.zc_per_sec = zc * (48000.0 / counted);
  out.flat_fraction = static_cast<double>(flat) / counted;
  out.hf_ratio = energy > 1e-12 ? hf_energy / energy : 0.0;
  return out;
}

// The largest sample-to-sample step a held pad puts out on a steady sine,
// once the wet mix and the tempo pads have settled. A sine at `freq` and
// 0.5 moves at most 0.5·2π·freq/48000 a sample; anything much above that is
// an edge, a seam or a return that was not smoothed.
float held_max_step(int pad, float amount, double freq, int measure_blocks) {
  constexpr uint32_t kBlock = 256;
  constexpr int kWarm = 300;
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
  float previous = 0.0f, worst = 0.0f;
  for (int b = 0; b < kWarm + measure_blocks; ++b) {
    // Pressed once the history holds a full second, like a hand would.
    if (b == 200) fx.set_pad_amount(pad, amount);
    for (uint32_t i = 0; i < kBlock; ++i) {
      in_l[i] = in_r[i] = 0.5f * static_cast<float>(std::sin(phase));
      phase += 2.0 * 3.14159265358979 * freq / 48000.0;
    }
    transport.seconds = b * static_cast<double>(kBlock) / 48000.0;
    transport.beats = transport.seconds * transport.tempo_bpm / 60.0;
    fx.set_transport(transport);
    fx.process(ins, outs, kBlock);
    for (uint32_t i = 0; i < kBlock; ++i) {
      if (b >= kWarm) worst = std::max(worst, std::fabs(out_l[i] - previous));
      previous = out_l[i];
    }
  }
  return worst;
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

  // A click 0.75 s behind the press must come back: a 1 s tape walked with
  // delay += 2 only covers half a second and would miss it.
  {
    constexpr uint32_t kBlock = 256;
    constexpr double kRate = 48000.0;
    nirbija::FxPadInstance fx;
    fx.set_channel_layout(2);
    fx.activate(kRate, kBlock);

    std::vector<float> in_l(kBlock, 0.0f), in_r(kBlock, 0.0f);
    std::vector<float> out_l(kBlock), out_r(kBlock);
    const float* ins[2] = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    nirbija::TransportInfo transport;
    transport.playing = true;
    transport.rolling = true;
    transport.tempo_bpm = 120.0;

    const int fill = static_cast<int>(kRate / kBlock) + 4;
    for (int b = 0; b < fill; ++b) {
      fx.set_transport(transport);
      fx.process(ins, outs, kBlock);
    }

    in_l[0] = in_r[0] = 1.0f;
    fx.process(ins, outs, kBlock);
    in_l[0] = in_r[0] = 0.0f;

    const int behind = static_cast<int>(0.75 * kRate / kBlock);
    for (int b = 0; b < behind; ++b) fx.process(ins, outs, kBlock);

    fx.set_pad(nirbija::FxPadInstance::Reverse, true);
    float peak = 0.0f;
    int peak_block = -1;
    const int listen = static_cast<int>(1.1 * kRate / kBlock);
    for (int b = 0; b < listen; ++b) {
      fx.process(ins, outs, kBlock);
      for (float s : out_l) {
        const float a = std::fabs(s);
        if (a > peak) {
          peak = a;
          peak_block = b;
        }
      }
    }
    if (peak < 0.2f)
      fail("reverse never reached a click 0.75 s back (peak " +
           std::to_string(peak) + ")");
    const double peak_at = peak_block * kBlock / kRate;
    if (peak_at < 0.55 || peak_at > 0.95)
      fail("reverse returned the 0.75 s click at " + std::to_string(peak_at) +
           " s, not around 0.75 s");
  }

  // The talkbox sweeps its vowel on an LFO of its own. It used to read the
  // flanger's phase, which only advances while that pad is held, so the vowel
  // stood still unless both were down at once.
  const Measured talkbox = sweep(nirbija::FxPadInstance::Talkbox, 8, 150);
  if (talkbox.window_swing < 1.1)
    fail("the talkbox vowel is not moving (swing " +
         std::to_string(talkbox.window_swing) + ")");

  // Amounts, not switches. A pad stored as 0.37 has to come back as 0.37,
  // and a bipolar pad has to keep its sign — the old 0/1 blob is still the
  // ends of the same range, so a session from before this still loads.
  {
    nirbija::FxPadInstance fx;
    fx.activate(48000.0, 256);
    fx.set_hold(true);
    fx.set_pad_amount(nirbija::FxPadInstance::Crush, 0.37f);
    fx.set_pad_amount(nirbija::FxPadInstance::Pitch, -0.55f);
    fx.set_pad_amount(nirbija::FxPadInstance::Filter, 0.8f);
    const auto kept = fx.save_state();
    nirbija::FxPadInstance restored;
    restored.activate(48000.0, 256);
    if (!restored.load_state(kept)) fail("load_state refused a blob with amounts");
    if (std::fabs(restored.pad_amount(nirbija::FxPadInstance::Crush) - 0.37f) > 1e-4f)
      fail("crush amount did not survive the session");
    if (std::fabs(restored.pad_amount(nirbija::FxPadInstance::Pitch) + 0.55f) > 1e-4f)
      fail("pitch amount lost its sign");
    if (std::fabs(restored.pad_amount(nirbija::FxPadInstance::Filter) - 0.8f) > 1e-4f)
      fail("filter amount did not survive the session");
    if (!restored.hold()) fail("hold did not survive a blob with amounts");
  }

  {
    const std::string legacy = "1\n0\n1\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n";
    nirbija::FxPadInstance fx;
    fx.activate(48000.0, 256);
    if (!fx.load_state(std::vector<uint8_t>(legacy.begin(), legacy.end())))
      fail("a 0/1 blob from before amounts was refused");
    if (!fx.hold()) fail("legacy hold did not load");
    if (fx.pad_amount(nirbija::FxPadInstance::Crush) != 0.0f)
      fail("legacy crush-off did not stay off");
    if (std::fabs(fx.pad_amount(nirbija::FxPadInstance::Pitch) - 1.0f) > 1e-4f)
      fail("legacy pitch-on did not become full up");
  }

  {
    const auto params = nirbija::FxPadInstance().parameters();
    if (params.size() < 16) fail("parameters() dropped pads");
    if (params[nirbija::FxPadInstance::Pitch].min_value >= 0.0)
      fail("pitch is not exposed as bipolar");
    if (params[nirbija::FxPadInstance::Filter].min_value >= 0.0)
      fail("filter is not exposed as bipolar");
    if (params[nirbija::FxPadInstance::Crush].min_value < 0.0)
      fail("crush was marked bipolar");
  }

  fx.set_pad_amount(nirbija::FxPadInstance::Crush, -0.4f);
  if (fx.pad_amount(nirbija::FxPadInstance::Crush) < 0.0f)
    fail("a unipolar pad accepted a negative amount");
  fx.set_pad_amount(nirbija::FxPadInstance::Pitch, 2.0f);
  if (fx.pad_amount(nirbija::FxPadInstance::Pitch) > 1.0f)
    fail("a bipolar pad was not clamped to 1");

  // Crush at full hold must flatten more of the wave than a light press.
  // If amount only faded the same 12-sample hold in, both would look alike
  // after the wet mix ramped, and the pad would still be two states.
  {
    const Tone light = held(nirbija::FxPadInstance::Crush, 0.2f, 220.0, false, 80);
    const Tone heavy = held(nirbija::FxPadInstance::Crush, 1.0f, 220.0, false, 80);
    if (heavy.flat_fraction <= light.flat_fraction + 0.02)
      fail("crush amount did not change how hard it folds (light " +
           std::to_string(light.flat_fraction) + " heavy " +
           std::to_string(heavy.flat_fraction) + ")");
  }

  // Pitch down must lower the tone, pitch up must raise it. Wet-only would
  // leave the zero-crossing rate of a 220 Hz sine alone.
  {
    const Tone down = held(nirbija::FxPadInstance::Pitch, -1.0f, 220.0, false, 120);
    const Tone up = held(nirbija::FxPadInstance::Pitch, 1.0f, 220.0, false, 120);
    if (down.zc_per_sec > 180.0)
      fail("pitch down did not drop the tone (zc " +
           std::to_string(down.zc_per_sec) + ")");
    if (up.zc_per_sec < 300.0)
      fail("pitch up did not raise the tone (zc " +
           std::to_string(up.zc_per_sec) + ")");
    if (up.zc_per_sec <= down.zc_per_sec)
      fail("pitch up and down landed on the same tone");
  }

  // A 110 + 3000 Hz pair: closing the lowpass has to kill the high tone,
  // opening the highpass has to kill the low one.
  {
    const Tone lp = held(nirbija::FxPadInstance::Filter, -1.0f, 110.0, true, 80);
    const Tone hp = held(nirbija::FxPadInstance::Filter, 1.0f, 110.0, true, 80);
    if (lp.hf_ratio >= hp.hf_ratio)
      fail("filter down kept as much high band as filter up (lp " +
           std::to_string(lp.hf_ratio) + " hp " + std::to_string(hp.hf_ratio) +
           ")");
    if (lp.hf_ratio > 0.35)
      fail("filter down did not close (hf " + std::to_string(lp.hf_ratio) + ")");
    if (hp.hf_ratio < 0.55)
      fail("filter up did not open (hf " + std::to_string(hp.hf_ratio) + ")");
  }

  // A light press used to never cross the old mix > 0.5 attack line, so
  // reverse sat on a write-head tap and put out nothing from the past.
  {
    constexpr uint32_t kBlock = 256;
    constexpr double kRate = 48000.0;
    nirbija::FxPadInstance fx;
    fx.set_channel_layout(2);
    fx.activate(kRate, kBlock);
    std::vector<float> in_l(kBlock, 0.0f), in_r(kBlock, 0.0f);
    std::vector<float> out_l(kBlock), out_r(kBlock);
    const float* ins[2] = {in_l.data(), in_r.data()};
    float* outs[2] = {out_l.data(), out_r.data()};
    const int fill = static_cast<int>(kRate / kBlock) + 4;
    for (int b = 0; b < fill; ++b) fx.process(ins, outs, kBlock);
    in_l[0] = in_r[0] = 1.0f;
    fx.process(ins, outs, kBlock);
    in_l[0] = in_r[0] = 0.0f;
    const int behind = static_cast<int>(0.4 * kRate / kBlock);
    for (int b = 0; b < behind; ++b) fx.process(ins, outs, kBlock);
    fx.set_pad_amount(nirbija::FxPadInstance::Reverse, 0.25f);
    float peak = 0.0f;
    const int listen = static_cast<int>(0.8 * kRate / kBlock);
    for (int b = 0; b < listen; ++b) {
      fx.process(ins, outs, kBlock);
      for (float s : out_l) peak = std::max(peak, std::fabs(s));
    }
    if (peak < 0.05f)
      fail("a light reverse press never attacked (peak " +
           std::to_string(peak) + ")");
  }

  // No edges. A 220 Hz sine at 0.5 moves 0.0144 a sample on its own; the
  // gate's and the cutter's 1.5 ms ramps add a hundredth or so. The old
  // 1-or-0 gain stepped by the whole sample, up to 0.5, at every open and
  // close - a click on every division.
  {
    const float gate = held_max_step(nirbija::FxPadInstance::Gate, 1.0f, 220.0, 200);
    if (gate > 0.05f)
      fail("the gate's edge steps by " + std::to_string(gate));
    const float cutter = held_max_step(nirbija::FxPadInstance::Cutter, 1.0f, 220.0, 200);
    if (cutter > 0.05f)
      fail("the cutter's edge steps by " + std::to_string(cutter));
  }

  // No seam in the stutter. At 1/32 of 120 BPM a slice is 750 samples, and
  // 220 Hz does not fit that a whole number of times, so the wrap from the
  // slice's last sample to its first used to be a jump.
  {
    const float step = held_max_step(nirbija::FxPadInstance::Stutter, 1.0f, 220.0, 200);
    if (step > 0.06f)
      fail("the stutter's seam steps by " + std::to_string(step));
  }

  // No jump when the reverse tape runs out and comes back to now. Over the
  // two seconds of tape 220.25 Hz is a half cycle off itself, so the return
  // used to land on the far side of the wave.
  {
    const float step = held_max_step(nirbija::FxPadInstance::Reverse, 1.0f, 220.25, 260);
    if (step > 0.06f)
      fail("the reverse's return steps by " + std::to_string(step));
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
