// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/fx_pad.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace nirbija {
namespace {

constexpr float kPi = 3.14159265358979f;

float wrap01(float x) {
  x -= std::floor(x);
  return x < 0.0f ? x + 1.0f : x;
}

float frames_per_beat(const TransportInfo& t, double sample_rate) {
  const double tempo = t.tempo_bpm > 0.0 ? t.tempo_bpm : 120.0;
  return static_cast<float>(sample_rate * 60.0 / tempo);
}

}  // namespace

void FxPadInstance::Delay::setup(size_t n) {
  data.assign(std::max(n, size_t{2}), 0.0f);
  w = 0;
}

void FxPadInstance::Delay::push(float x) {
  if (data.empty()) return;
  data[w] = x;
  w = dsp::ring_next(w, data.size());
}

float FxPadInstance::Delay::tap(size_t delay) const {
  if (data.empty()) return 0.0f;
  const size_t n = data.size();
  const size_t d = std::min(delay, n - 1);
  return data[dsp::ring_back(w, d, n)];
}

float FxPadInstance::Delay::tap_lerp(float delay) const {
  if (data.empty()) return 0.0f;
  const float maxd = static_cast<float>(data.size() - 1);
  delay = std::clamp(delay, 1.0f, maxd);
  const size_t i0 = static_cast<size_t>(delay);
  const float frac = delay - static_cast<float>(i0);
  return tap(i0) * (1.0f - frac) + tap(i0 + 1) * frac;
}

const char* FxPadInstance::pad_name(int pad) {
  static constexpr const char* kNames[kPads] = {
      "Crush",        "Pitch",      "Comb",         "Ring",
      "Reverb",       "Stutter",    "Gate",         "Filter",
      "Cutter",       "Reverse",    "Dub",          "Tempo Delay",
      "Talkbox",      "Vibroflange", "Dirty",       "Compressor",
  };
  if (pad < 0 || pad >= kPads) return "";
  return kNames[pad];
}

bool FxPadInstance::pad_bipolar(int pad) {
  switch (pad) {
    case Pitch:
    case Comb:
    case Ring:
    case Filter:
      return true;
    default:
      return false;
  }
}

PluginDescriptor FxPadInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.fxpad";
  descriptor.name = "FX Pad";
  descriptor.vendor = "Nirbija";
  descriptor.audio_inputs = 2;
  descriptor.audio_outputs = 2;
  descriptor.has_midi_input = false;
  descriptor.category = "FX Pad";
  descriptor.kind = PluginKind::Effect;
  return descriptor;
}

FxPadInstance::FxPadInstance() : descriptor_(make_descriptor()) {}

bool FxPadInstance::activate(double sample_rate, uint32_t) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  const size_t one_sec = static_cast<size_t>(sample_rate_);
  const size_t two_sec = one_sec * 2;
  for (int ch = 0; ch < 2; ++ch) {
    hist_[ch].setup(two_sec);
    delay_[ch].setup(two_sec);
    echo_[ch].setup(two_sec);
    // Two seconds: tap() is a distance behind a write head that also
    // advances, so covering one second of reverse needs twice the tape.
    reverse_[ch].setup(two_sec);
    stutter_[ch].setup(one_sec);
    flange_[ch].setup(static_cast<size_t>(sample_rate_ * 0.02) + 8);
    comb_[ch].setup(static_cast<size_t>(sample_rate_ * 0.04) + 8);
    static constexpr float kCombMs[4] = {29.7f, 37.1f, 41.1f, 43.7f};
    for (int c = 0; c < 4; ++c)
      reverb_comb_[c][ch].setup(
          static_cast<size_t>(sample_rate_ * kCombMs[c] * 0.001) + 2);
    reverb_ap_[0][ch].setup(static_cast<size_t>(sample_rate_ * 0.005) + 2);
    reverb_ap_[1][ch].setup(static_cast<size_t>(sample_rate_ * 0.0168) + 2);
  }
  std::fill(mix_.begin(), mix_.end(), 0.0f);
  std::fill(was_on_.begin(), was_on_.end(), false);
  crush_hold_[0] = crush_hold_[1] = 0.0f;
  crush_count_ = 0;
  ring_phase_ = 0.0f;
  pitch_pos_[0] = pitch_pos_[1] = 8.0f;
  filter_lp_[0] = filter_lp_[1] = 0.0f;
  filter_bp_[0] = filter_bp_[1] = 0.0f;
  std::memset(talk_lp_, 0, sizeof(talk_lp_));
  env_peak_ = 0.0f;
  comp_gain_ = 1.0f;
  vib_phase_ = 0.0f;
  talk_phase_ = 0.0f;
  gate_phase_ = 0.0;
  cutter_phase_ = 0.0;
  edge_frames_ = std::max<uint32_t>(
      2, static_cast<uint32_t>(sample_rate_ * kEdgeSeconds));
  gate_ramp_.set_length(edge_frames_);
  gate_ramp_.reset(1.0f);
  cutter_ramp_.set_length(edge_frames_);
  cutter_ramp_.reset(1.0f);
  stutter_cap_ = 0;
  stutter_captured_ = 0;
  stutter_pos_ = 0;
  stutter_first_ = true;
  reverse_play_ = 0;
  reverse_alt_ = 0;
  return true;
}

bool FxPadInstance::pad_on(int pad) const {
  return std::fabs(pad_amount(pad)) > 1e-3f;
}

float FxPadInstance::pad_amount(int pad) const {
  if (pad < 0 || pad >= kPads) return 0.0f;
  return amount_[static_cast<size_t>(pad)].load(std::memory_order_relaxed);
}

void FxPadInstance::set_pad(int pad, bool on) {
  set_pad_amount(pad, on ? 1.0f : 0.0f);
}

void FxPadInstance::set_pad_amount(int pad, float amount) {
  if (pad < 0 || pad >= kPads) return;
  if (pad_bipolar(pad))
    amount = std::clamp(amount, -1.0f, 1.0f);
  else
    amount = std::clamp(amount, 0.0f, 1.0f);
  amount_[static_cast<size_t>(pad)].store(amount, std::memory_order_relaxed);
}

void FxPadInstance::set_hold(bool on) {
  hold_.store(on, std::memory_order_relaxed);
  if (!on) {
    for (auto& a : amount_) a.store(0.0f, std::memory_order_relaxed);
  }
}

void FxPadInstance::attack(int pad) {
  const float fpb = frames_per_beat(transport_, sample_rate_);
  switch (pad) {
    case Stutter: {
      // Capture the longest slice the pad can ask for, plus one seam's
      // worth beyond it: the head of each repeat is crossfaded with what
      // followed the slice, so the seam is not a step and the period stays
      // exactly the division. Playback length then follows the amount, so
      // dragging after the press shortens the loop instead of recapturing
      // and clicking.
      const size_t n = stutter_[0].data.size();
      stutter_cap_ = std::min(static_cast<uint32_t>(n),
                               std::max(32u, static_cast<uint32_t>(fpb * 0.5f)));
      stutter_captured_ =
          std::min(static_cast<uint32_t>(n), stutter_cap_ + edge_frames_);
      stutter_pos_ = 0;
      stutter_first_ = true;
      for (int ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < stutter_captured_; ++i)
          stutter_[ch].data[i] = hist_[ch].tap(stutter_captured_ - i);
      }
      break;
    }
    case Reverse:
      reverse_play_ = 1;
      reverse_alt_ = 0;
      break;
    case Gate:
    case Cutter:
      gate_phase_ = 0.0;
      break;
    default:
      break;
  }
}

void FxPadInstance::prepare_block(uint32_t frames) {
  const float fpb = frames_per_beat(transport_, sample_rate_);
  const float sr = static_cast<float>(sample_rate_);
  BlockCoefs& c = co_;

  // Wet is |amount|: bipolar pads use the sign for character, not for
  // whether they are in the path.
  for (int p = 0; p < kPads; ++p)
    c.wet[p] = std::fabs(mix_[static_cast<size_t>(p)]);

  // Crush: more amount holds longer and folds the word shorter.
  c.crush_period = 2u + static_cast<uint32_t>(mix_[Crush] * 22.0f);  // 2..24
  c.crush_steps = 48.0f - mix_[Crush] * 44.0f;                        // 48..4

  // Pitch: amount is octaves, so -1 is half speed and +1 is double.
  c.pitch_rate = std::exp2(mix_[Pitch]);
  c.pitch_grain = std::max(64.0f, fpb * 0.35f);

  // Comb: down lengthens (hollow), up shortens (metallic).
  {
    const float amt = mix_[Comb];
    const float ms = 4.4f * std::exp2(-amt * 1.8f);  // ~15 ms .. ~1.3 ms
    c.comb_delay = std::max(2.0f, sr * ms * 0.001f);
    c.comb_fb = 0.5f + 0.4f * std::fabs(amt);
  }

  // Ring: amount sets the modulator frequency, both sides of ~90 Hz.
  c.ring_inc = 28.0f * std::exp2((mix_[Ring] + 1.0f) * 2.25f) / sr;

  // Reverb: amount is wet and decay together.
  c.reverb_decay = 0.52f + 0.4f * mix_[Reverb];

  // Stutter: amount shortens the slice, 1/2 beat down to 1/32. The seam is
  // as long as the material past the slice allows.
  if (stutter_cap_ > 0) {
    const float beats = 0.5f * std::exp2(-3.0f * mix_[Stutter]);
    c.stutter_play =
        std::clamp(static_cast<uint32_t>(fpb * beats), 32u, stutter_cap_);
    const uint32_t spare =
        stutter_captured_ > c.stutter_play ? stutter_captured_ - c.stutter_play : 0;
    c.stutter_xf = std::min({edge_frames_, spare, c.stutter_play / 2});
  }

  // Gate: more amount is a faster, narrower chop.
  {
    const float beats = 0.5f * std::exp2(-2.0f * mix_[Gate]);
    c.gate_step = 1.0 / std::max(1.0f, fpb * beats);
    c.gate_duty = 0.58f - 0.36f * mix_[Gate];
  }

  // Filter: down is a closing lowpass, up is an opening highpass. Near zero
  // the cutoff sits where the filter barely colours, so the wet fade is
  // enough; parked at a fixed 700 Hz it only ever sounded like one EQ.
  //
  // On the mapping: this is a Chamberlin state-variable filter, whose
  // coefficient for a cutoff fc is 2·sin(π·fc/sr). Using hz/sr directly, as
  // here, puts the actual cutoff near hz/(2π): the "8000 Hz" end of the
  // lowpass sits around 1.3 kHz and the "80 Hz" end of the highpass around
  // 13 Hz. The sweep was tuned by ear against this curve and nothing in the
  // editor shows the number, so it stays; the labels in the formula below
  // are the knob's scale, not frequencies.
  {
    const float amt = mix_[Filter];
    const float hz = amt < 0.0f ? 8000.0f * std::exp2(amt * 5.5f)
                                : 80.0f * std::exp2(amt * 5.5f);
    c.filter_f = std::clamp(hz / sr, 0.001f, 0.35f);
    c.filter_q = 0.18f + 0.5f * std::fabs(amt);
    c.filter_low = amt < 0.0f;
  }

  // Cutter: 1/8 to 1/32 on-off, locked to the transport. The phase is
  // taken from the song position at the block start and advanced per
  // sample, so the chop lands on the grid to the frame rather than moving
  // only at block edges.
  {
    const double rate = 2.0 + 6.0 * static_cast<double>(mix_[Cutter]);
    cutter_phase_ = std::fmod(std::fabs(transport_.beats) * rate, 1.0);
    c.cutter_step = rate / static_cast<double>(std::max(1.0f, fpb));
    c.cutter_duty = 0.55f - 0.25f * mix_[Cutter];
  }

  // Dub: long feedback delay with a dark loop. Amount is the feedback.
  c.dub_delay = fpb * 0.75f;
  c.dub_fb = 0.35f + 0.52f * mix_[Dub];

  // Tempo delay: a clean eighth.
  c.echo_delay = fpb * 0.5f;
  c.echo_fb = 0.18f + 0.55f * mix_[TempoDelay];

  // Talkbox: three formants, vowel swept slowly on its own LFO. It used to
  // read the flanger's phase, which only advances while that pad is held,
  // so the vowel stood still unless both were down at once. The vowel
  // moves once a block: at 0.0825 Hz that is a fraction of a hertz per
  // block on the formants, well below anything that steps.
  //
  // The formant filters are one-poles stepped as lp += f·(x - lp) with
  // f = hz/sr, so their real corner is near hz/(2π) - about a sixth of the
  // named frequency. Tuned by ear on that basis; no label shows these.
  talk_phase_ = wrap01(talk_phase_ + 0.0825f * static_cast<float>(frames) / sr);
  {
    const float vowel =
        0.5f + 0.5f * mix_[Talkbox] * std::sin(2.0f * kPi * talk_phase_);
    c.talk_freqs[0] = (400.0f + 400.0f * vowel) / sr;
    c.talk_freqs[1] = (800.0f + 1400.0f * vowel) / sr;
    c.talk_freqs[2] = 2400.0f / sr;
  }

  // Vibroflange: amount is depth and rate together.
  c.vib_inc = (0.18f + 0.85f * mix_[Vibroflange]) / sr;
  c.vib_depth = 6.0f + 22.0f * mix_[Vibroflange];

  // Dirty: more amount is more drive, not just more of the same fold.
  c.dirty_drive = 1.4f + 10.0f * mix_[Dirty];

  // Compressor: amount lowers the threshold and raises the makeup.
  c.comp_thresh = 0.55f - 0.42f * mix_[Compressor];
  c.comp_makeup = 1.05f + 0.7f * mix_[Compressor];
}

void FxPadInstance::process_sample(float* left, float* right) {
  float s[2] = {*left, *right};
  for (int ch = 0; ch < 2; ++ch) hist_[ch].push(s[ch]);
  const BlockCoefs& c = co_;

  auto wet = [&](int pad, float dry, float processed) {
    return dry + c.wet[pad] * (processed - dry);
  };

  if (mix_[Crush] > 1e-4f) {
    if (crush_count_ == 0) {
      crush_hold_[0] = s[0];
      crush_hold_[1] = s[1];
    }
    ++crush_count_;
    if (crush_count_ >= c.crush_period) crush_count_ = 0;
    for (int ch = 0; ch < 2; ++ch) {
      float q = std::round(crush_hold_[ch] * c.crush_steps) / c.crush_steps;
      s[ch] = wet(Crush, s[ch], q);
    }
  }

  // Pitch: grain-read of history. The increment is 1-rate: growing delay
  // plays slower, shrinking delay plays faster against a write head that
  // only goes forward.
  if (std::fabs(mix_[Pitch]) > 1e-4f) {
    const float grain = c.pitch_grain;
    for (int ch = 0; ch < 2; ++ch) {
      pitch_pos_[ch] += 1.0f - c.pitch_rate;
      if (pitch_pos_[ch] < 8.0f) pitch_pos_[ch] += grain;
      if (pitch_pos_[ch] >= grain + 8.0f) pitch_pos_[ch] -= grain;
      const float a = hist_[ch].tap_lerp(pitch_pos_[ch]);
      const float b = hist_[ch].tap_lerp(pitch_pos_[ch] + grain * 0.5f);
      const float x =
          std::clamp((pitch_pos_[ch] - 8.0f) / grain, 0.0f, 1.0f);
      const float g = (x < 0.5f) ? x * 2.0f : (1.0f - x) * 2.0f;
      s[ch] = wet(Pitch, s[ch], a * (1.0f - g) + b * g);
    }
  }

  // Comb: own feedback line.
  if (std::fabs(mix_[Comb]) > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      const float z = comb_[ch].tap_lerp(c.comb_delay);
      const float y = s[ch] + z * c.comb_fb;
      comb_[ch].push(y);
      s[ch] = wet(Comb, s[ch], y);
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) comb_[ch].push(s[ch] * 0.4f);
  }

  if (std::fabs(mix_[Ring]) > 1e-4f) {
    ring_phase_ = wrap01(ring_phase_ + c.ring_inc);
    const float m = std::sin(2.0f * kPi * ring_phase_);
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Ring, s[ch], s[ch] * m);
  }

  // Reverb: four combs and two allpasses. Amount is wet and decay together,
  // so a light press is a room and a slam is a hall, not the same hall quieter.
  if (mix_[Reverb] > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      float acc = 0.0f;
      for (int k = 0; k < 4; ++k) {
        float z = reverb_comb_[k][ch].tap(reverb_comb_[k][ch].data.size() - 1);
        z = s[ch] + z * c.reverb_decay;
        reverb_comb_[k][ch].push(z);
        acc += z;
      }
      acc *= 0.25f;
      for (int a = 0; a < 2; ++a) {
        const float d = reverb_ap_[a][ch].tap(reverb_ap_[a][ch].data.size() - 1);
        const float y = acc + d * -0.5f;
        reverb_ap_[a][ch].push(y);
        acc = d + y * 0.5f;
      }
      s[ch] = wet(Reverb, s[ch], acc);
    }
  }

  // Stutter: loop the captured slice. The first `xf` frames of each repeat
  // after the first are blended, equal power, with the frames that followed
  // the slice end, so the wrap from the last frame to the first is a
  // continuation and not a step.
  if (mix_[Stutter] > 1e-4f && stutter_cap_ > 0) {
    const uint32_t play = c.stutter_play;
    if (stutter_pos_ >= play) {
      stutter_pos_ %= play;
      stutter_first_ = false;
    }
    const uint32_t i = stutter_pos_;
    float head_w = 1.0f, tail_w = 0.0f;
    if (!stutter_first_ && c.stutter_xf > 0 && i < c.stutter_xf) {
      const auto weights = dsp::equal_power(
          static_cast<float>(i) / static_cast<float>(c.stutter_xf));
      tail_w = weights.first;
      head_w = weights.second;
    }
    for (int ch = 0; ch < 2; ++ch) {
      float v = stutter_[ch].data[i] * head_w;
      if (tail_w > 0.0f) v += stutter_[ch].data[play + i] * tail_w;
      s[ch] = wet(Stutter, s[ch], v);
    }
    if (++stutter_pos_ >= play) {
      stutter_pos_ = 0;
      stutter_first_ = false;
    }
  }

  // Gate: more amount is a faster, narrower chop. The edge is a ramp, not a
  // step: a step multiplied into a tone is a click every gate period.
  if (mix_[Gate] > 1e-4f) {
    gate_phase_ += c.gate_step;
    if (gate_phase_ >= 1.0) gate_phase_ -= std::floor(gate_phase_);
    gate_ramp_.set_target(gate_phase_ < c.gate_duty ? 1.0f : 0.0f);
    const float g = gate_ramp_.next();
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Gate, s[ch], s[ch] * g);
  }

  if (std::fabs(mix_[Filter]) > 1e-4f) {
    const float f = c.filter_f;
    const float q = c.filter_q;
    for (int ch = 0; ch < 2; ++ch) {
      filter_lp_[ch] += f * filter_bp_[ch];
      const float hp = s[ch] - filter_lp_[ch] - q * filter_bp_[ch];
      filter_bp_[ch] += f * hp;
      const float y = c.filter_low ? filter_lp_[ch] : hp;
      s[ch] = wet(Filter, s[ch], y);
    }
  }

  // Cutter: opposite of a gate so it chops rather than breathes. Amount is
  // how often, and how little stays open. Same ramped edge as the gate.
  if (mix_[Cutter] > 1e-4f) {
    cutter_ramp_.set_target(cutter_phase_ < c.cutter_duty ? 1.0f : 0.0f);
    const float g = cutter_ramp_.next();
    cutter_phase_ += c.cutter_step;
    if (cutter_phase_ >= 1.0) cutter_phase_ -= std::floor(cutter_phase_);
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Cutter, s[ch], s[ch] * g);
  }

  // Reverse: last second backwards. Distance grows by two so the tap walks
  // one sample back against a write head that is also moving. When it
  // reaches the end of the tape it has to come back to now, and that is a
  // jump of a second; so a fresh tap starts from now a seam early and the
  // two are crossfaded, equal power, before the old one is dropped.
  if (mix_[Reverse] > 1e-4f) {
    const size_t n = reverse_[0].data.size();
    const size_t steps_left = (n - 1 - reverse_play_) / 2;
    float old_w = 1.0f, new_w = 0.0f;
    if (steps_left < edge_frames_) {
      if (reverse_alt_ == 0) reverse_alt_ = 1;
      const auto weights = dsp::equal_power(
          1.0f - static_cast<float>(steps_left) / static_cast<float>(edge_frames_));
      old_w = weights.first;
      new_w = weights.second;
    }
    for (int ch = 0; ch < 2; ++ch) {
      reverse_[ch].push(s[ch]);
      float v = reverse_[ch].tap(reverse_play_) * old_w;
      if (new_w > 0.0f) v += reverse_[ch].tap(reverse_alt_) * new_w;
      s[ch] = wet(Reverse, s[ch], v);
    }
    reverse_play_ += 2;
    if (reverse_alt_ != 0) reverse_alt_ += 2;
    if (reverse_play_ + 1 >= n) {
      reverse_play_ = reverse_alt_ != 0 ? reverse_alt_ : 1;
      reverse_alt_ = 0;
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) reverse_[ch].push(s[ch]);
  }

  if (mix_[Dub] > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      float z = delay_[ch].tap_lerp(c.dub_delay);
      z = z * c.dub_fb + s[ch];
      z += (delay_[ch].tap_lerp(c.dub_delay * 0.5f) - z) * 0.35f;
      delay_[ch].push(z);
      s[ch] = wet(Dub, s[ch], z);
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) delay_[ch].push(s[ch]);
  }

  // Tempo delay: own tape so it does not fight Dub.
  if (mix_[TempoDelay] > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      const float z = s[ch] + echo_[ch].tap_lerp(c.echo_delay) * c.echo_fb;
      echo_[ch].push(z);
      s[ch] = wet(TempoDelay, s[ch], z);
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) echo_[ch].push(s[ch]);
  }

  if (mix_[Talkbox] > 1e-4f) {
    static constexpr float kGains[3] = {1.0f, 0.7f, 0.35f};
    for (int ch = 0; ch < 2; ++ch) {
      float acc = 0.0f;
      for (int b = 0; b < 3; ++b) {
        talk_lp_[b][ch] += c.talk_freqs[b] * (s[ch] - talk_lp_[b][ch]);
        acc += talk_lp_[b][ch] * kGains[b];
      }
      s[ch] = wet(Talkbox, s[ch], acc);
    }
  }

  // Vibroflange: short modulated delay.
  if (mix_[Vibroflange] > 1e-4f) {
    vib_phase_ = wrap01(vib_phase_ + c.vib_inc);
    const float lfo = std::sin(2.0f * kPi * vib_phase_);
    for (int ch = 0; ch < 2; ++ch) {
      flange_[ch].push(s[ch]);
      const float d = 16.0f + lfo * c.vib_depth + static_cast<float>(ch) * 3.0f;
      s[ch] = wet(Vibroflange, s[ch], s[ch] + flange_[ch].tap_lerp(d));
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) flange_[ch].push(s[ch]);
  }

  if (mix_[Dirty] > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      const float d = std::tanh(s[ch] * c.dirty_drive);
      s[ch] = wet(Dirty, s[ch], d);
    }
  }

  // Compressor: flatten peaks so a pad after Dirty still has somewhere to go.
  if (mix_[Compressor] > 1e-4f) {
    const float peak = std::max(std::fabs(s[0]), std::fabs(s[1]));
    env_peak_ = peak > env_peak_ ? peak : env_peak_ * 0.9995f;
    const float want = env_peak_ > c.comp_thresh ? c.comp_thresh / env_peak_ : 1.0f;
    comp_gain_ += (want - comp_gain_) * 0.01f;
    for (int ch = 0; ch < 2; ++ch)
      s[ch] = wet(Compressor, s[ch], s[ch] * comp_gain_ * c.comp_makeup);
  }

  *left = s[0];
  *right = s[1];
}

void FxPadInstance::process(const float* const* inputs, float* const* outputs,
                            uint32_t frames) {
  const int width = std::min(channels_, 2);
  const float coeff =
      1.0f - std::exp(-static_cast<float>(frames) /
                      (0.008f * static_cast<float>(sample_rate_)));

  for (int p = 0; p < kPads; ++p) {
    const float target =
        amount_[static_cast<size_t>(p)].load(std::memory_order_relaxed);
    mix_[static_cast<size_t>(p)] +=
        coeff * (target - mix_[static_cast<size_t>(p)]);
    // Attack used to wait for mix > 0.5, which meant a light press never
    // armed stutter, gate or reverse at all.
    const bool on = std::fabs(mix_[static_cast<size_t>(p)]) > 0.02f;
    if (on && !was_on_[static_cast<size_t>(p)]) attack(p);
    was_on_[static_cast<size_t>(p)] = on;
  }
  prepare_block(frames);

  for (uint32_t i = 0; i < frames; ++i) {
    float l = inputs[0][i];
    float r = width > 1 ? inputs[1][i] : l;
    process_sample(&l, &r);
    outputs[0][i] = l;
    if (width > 1) outputs[1][i] = r;
  }
}

std::vector<ParameterInfo> FxPadInstance::parameters() const {
  std::vector<ParameterInfo> info;
  info.reserve(kPads + 1);
  for (int p = 0; p < kPads; ++p) {
    const bool bi = pad_bipolar(p);
    info.push_back({static_cast<uint32_t>(p), pad_name(p),
                    bi ? -1.0 : 0.0, 1.0, 0.0});
  }
  info.push_back({kHoldId, "Hold", 0.0, 1.0, 0.0, true});
  return info;
}

double FxPadInstance::parameter_value(uint32_t id) const {
  if (id == kHoldId) return hold() ? 1.0 : 0.0;
  if (id < kPads) return pad_amount(static_cast<int>(id));
  return 0.0;
}

void FxPadInstance::set_parameter(uint32_t id, double value) {
  if (id == kHoldId) {
    set_hold(value >= 0.5);
    return;
  }
  if (id < kPads) set_pad_amount(static_cast<int>(id), static_cast<float>(value));
}

std::vector<uint8_t> FxPadInstance::save_state() const {
  std::string text = hold() ? "1\n" : "0\n";
  for (int p = 0; p < kPads; ++p) {
    text += format_number(pad_amount(p), 6);
    text += '\n';
  }
  return {text.begin(), text.end()};
}

bool FxPadInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  size_t start = 0;
  int field = 0;
  while (start <= text.size()) {
    const size_t split = text.find('\n', start);
    const std::string_view line =
        std::string_view(text).substr(start, split == std::string::npos
                                                 ? std::string_view::npos
                                                 : split - start);
    double value = 0.0;
    if (parse_number(line, &value)) {
      if (field == 0) set_hold(value >= 0.5);
      else if (field - 1 < kPads)
        set_pad_amount(field - 1, static_cast<float>(value));
    }
    ++field;
    if (split == std::string::npos) break;
    start = split + 1;
  }
  return field > 0;
}

}  // namespace nirbija
