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
  w = (w + 1) % data.size();
}

float FxPadInstance::Delay::tap(size_t delay) const {
  if (data.empty()) return 0.0f;
  const size_t n = data.size();
  const size_t d = std::min(delay, n - 1);
  return data[(w + n - d) % n];
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
  stutter_len_ = 0;
  stutter_cap_ = 0;
  stutter_pos_ = 0;
  reverse_play_ = 0;
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
      // Capture the longest slice the pad can ask for. Playback length
      // then follows the amount, so dragging after the press shortens
      // the loop instead of recapturing and clicking.
      const size_t n = stutter_[0].data.size();
      stutter_cap_ = std::min(static_cast<uint32_t>(n),
                               std::max(32u, static_cast<uint32_t>(fpb * 0.5f)));
      stutter_len_ = stutter_cap_;
      stutter_pos_ = 0;
      for (int ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < stutter_cap_ && i < n; ++i)
          stutter_[ch].data[i] = hist_[ch].tap(stutter_cap_ - i);
      }
      break;
    }
    case Reverse:
      reverse_play_ = 1;
      break;
    case Gate:
    case Cutter:
      gate_phase_ = 0.0;
      break;
    default:
      break;
  }
}

void FxPadInstance::process_sample(float* left, float* right) {
  float s[2] = {*left, *right};
  for (int ch = 0; ch < 2; ++ch) hist_[ch].push(s[ch]);

  const float fpb = frames_per_beat(transport_, sample_rate_);
  const float sr = static_cast<float>(sample_rate_);

  // Wet is |amount|: bipolar pads use the sign for character, not for
  // whether they are in the path.
  auto wet = [&](int pad, float dry, float processed) {
    const float w = std::fabs(mix_[static_cast<size_t>(pad)]);
    return dry + w * (processed - dry);
  };

  // Crush: more amount holds longer and folds the word shorter.
  if (mix_[Crush] > 1e-4f) {
    const float a = mix_[Crush];
    const uint32_t period =
        2u + static_cast<uint32_t>(a * 22.0f);  // 2..24 samples
    const float steps = 48.0f - a * 44.0f;      // 48..4
    if (crush_count_ == 0) {
      crush_hold_[0] = s[0];
      crush_hold_[1] = s[1];
    }
    ++crush_count_;
    if (crush_count_ >= period) crush_count_ = 0;
    for (int ch = 0; ch < 2; ++ch) {
      float q = std::round(crush_hold_[ch] * steps) / steps;
      s[ch] = wet(Crush, s[ch], q);
    }
  }

  // Pitch: grain-read of history. Amount is octaves, so -1 is half speed
  // and +1 is double. The increment is 1-rate: growing delay plays slower,
  // shrinking delay plays faster against a write head that only goes forward.
  if (std::fabs(mix_[Pitch]) > 1e-4f) {
    const float rate = std::exp2(mix_[Pitch]);
    const float grain = std::max(64.0f, fpb * 0.35f);
    for (int ch = 0; ch < 2; ++ch) {
      pitch_pos_[ch] += 1.0f - rate;
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

  // Comb: own feedback line. Down lengthens (hollow), up shortens (metallic).
  if (std::fabs(mix_[Comb]) > 1e-4f) {
    const float amt = mix_[Comb];
    const float ms = 4.4f * std::exp2(-amt * 1.8f);  // ~15 ms .. ~1.3 ms
    const float d = std::max(2.0f, sr * ms * 0.001f);
    const float fb = 0.5f + 0.4f * std::fabs(amt);
    for (int ch = 0; ch < 2; ++ch) {
      const float z = comb_[ch].tap_lerp(d);
      const float y = s[ch] + z * fb;
      comb_[ch].push(y);
      s[ch] = wet(Comb, s[ch], y);
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) comb_[ch].push(s[ch] * 0.4f);
  }

  // Ring: amount sets the modulator frequency, both sides of ~90 Hz.
  if (std::fabs(mix_[Ring]) > 1e-4f) {
    const float hz = 28.0f * std::exp2((mix_[Ring] + 1.0f) * 2.25f);
    ring_phase_ = wrap01(ring_phase_ + hz / sr);
    const float m = std::sin(2.0f * kPi * ring_phase_);
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Ring, s[ch], s[ch] * m);
  }

  // Reverb: four combs and two allpasses. Amount is wet and decay together,
  // so a light press is a room and a slam is a hall, not the same hall quieter.
  if (mix_[Reverb] > 1e-4f) {
    const float decay = 0.52f + 0.4f * mix_[Reverb];
    for (int ch = 0; ch < 2; ++ch) {
      float acc = 0.0f;
      for (int c = 0; c < 4; ++c) {
        float z = reverb_comb_[c][ch].tap(reverb_comb_[c][ch].data.size() - 1);
        z = s[ch] + z * decay;
        reverb_comb_[c][ch].push(z);
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

  // Stutter: loop a captured slice. Amount shortens it, 1/2 beat down to 1/32.
  if (mix_[Stutter] > 1e-4f && stutter_cap_ > 0) {
    const float beats = 0.5f * std::exp2(-3.0f * mix_[Stutter]);
    const uint32_t play = std::clamp(
        static_cast<uint32_t>(fpb * beats), 32u, stutter_cap_);
    stutter_len_ = play;
    const uint32_t i = stutter_pos_ % stutter_len_;
    stutter_pos_ = (stutter_pos_ + 1) % stutter_len_;
    for (int ch = 0; ch < 2; ++ch)
      s[ch] = wet(Stutter, s[ch], stutter_[ch].data[i]);
  }

  // Gate: more amount is a faster, narrower chop.
  if (mix_[Gate] > 1e-4f) {
    const float beats = 0.5f * std::exp2(-2.0f * mix_[Gate]);
    const float duty = 0.58f - 0.36f * mix_[Gate];
    const double step = 1.0 / std::max(1.0f, fpb * beats);
    gate_phase_ = std::fmod(gate_phase_ + step, 1.0);
    const float g = gate_phase_ < duty ? 1.0f : 0.0f;
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Gate, s[ch], s[ch] * g);
  }

  // Filter: down is a closing lowpass, up is an opening highpass. Near zero
  // the cutoff sits where the filter barely colours, so the wet fade is
  // enough; parked at a fixed 700 Hz it only ever sounded like one EQ.
  if (std::fabs(mix_[Filter]) > 1e-4f) {
    const float amt = mix_[Filter];
    const float hz = amt < 0.0f ? 8000.0f * std::exp2(amt * 5.5f)
                                : 80.0f * std::exp2(amt * 5.5f);
    const float f = std::clamp(hz / sr, 0.001f, 0.35f);
    const float q = 0.18f + 0.5f * std::fabs(amt);
    for (int ch = 0; ch < 2; ++ch) {
      filter_lp_[ch] += f * filter_bp_[ch];
      const float hp = s[ch] - filter_lp_[ch] - q * filter_bp_[ch];
      filter_bp_[ch] += f * hp;
      const float y = amt < 0.0f ? filter_lp_[ch] : hp;
      s[ch] = wet(Filter, s[ch], y);
    }
  }

  // Cutter: 1/8 to 1/32 on-off, opposite of a gate so it chops rather than
  // breathes. Amount is how often, and how little stays open.
  if (mix_[Cutter] > 1e-4f) {
    const double rate = 2.0 + 6.0 * static_cast<double>(mix_[Cutter]);
    const double phase = std::fmod(std::fabs(transport_.beats) * rate, 1.0);
    const float duty = 0.55f - 0.25f * mix_[Cutter];
    const float g = phase < duty ? 1.0f : 0.0f;
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Cutter, s[ch], s[ch] * g);
  }

  // Reverse: last second backwards. Distance grows by two so the tap walks
  // one sample back against a write head that is also moving.
  if (mix_[Reverse] > 1e-4f) {
    const size_t n = reverse_[0].data.size();
    for (int ch = 0; ch < 2; ++ch) {
      reverse_[ch].push(s[ch]);
      s[ch] = wet(Reverse, s[ch], reverse_[ch].tap(reverse_play_));
    }
    reverse_play_ += 2;
    if (reverse_play_ + 1 >= n) reverse_play_ = 1;
  } else {
    for (int ch = 0; ch < 2; ++ch) reverse_[ch].push(s[ch]);
  }

  // Dub: long feedback delay with a dark loop. Amount is the feedback.
  if (mix_[Dub] > 1e-4f) {
    const float d = fpb * 0.75f;
    const float fb = 0.35f + 0.52f * mix_[Dub];
    for (int ch = 0; ch < 2; ++ch) {
      float z = delay_[ch].tap_lerp(d);
      z = z * fb + s[ch];
      z += (delay_[ch].tap_lerp(d * 0.5f) - z) * 0.35f;
      delay_[ch].push(z);
      s[ch] = wet(Dub, s[ch], z);
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) delay_[ch].push(s[ch]);
  }

  // Tempo delay: a clean eighth. Own tape so it does not fight Dub.
  if (mix_[TempoDelay] > 1e-4f) {
    const float d = fpb * 0.5f;
    const float fb = 0.18f + 0.55f * mix_[TempoDelay];
    for (int ch = 0; ch < 2; ++ch) {
      const float z = s[ch] + echo_[ch].tap_lerp(d) * fb;
      echo_[ch].push(z);
      s[ch] = wet(TempoDelay, s[ch], z);
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) echo_[ch].push(s[ch]);
  }

  // Talkbox: three formants, vowel swept slowly. Its own accumulator, not a
  // scaled read of the flanger's: that one is only advanced inside the
  // Vibroflange branch below, so the vowel sat frozen unless both pads
  // happened to be down, and the 0.15 scaling made it jump rather than turn
  // over every time the flanger's phase wrapped. Amount is wet and how far
  // the vowel travels.
  talk_phase_ = wrap01(talk_phase_ + 0.0825f / sr);
  if (mix_[Talkbox] > 1e-4f) {
    const float vowel =
        0.5f + 0.5f * mix_[Talkbox] * std::sin(2.0f * kPi * talk_phase_);
    const float f1 = (400.0f + 400.0f * vowel) / sr;
    const float f2 = (800.0f + 1400.0f * vowel) / sr;
    const float f3 = 2400.0f / sr;
    const float freqs[3] = {f1, f2, f3};
    const float gains[3] = {1.0f, 0.7f, 0.35f};
    for (int ch = 0; ch < 2; ++ch) {
      float acc = 0.0f;
      for (int b = 0; b < 3; ++b) {
        talk_lp_[b][ch] += freqs[b] * (s[ch] - talk_lp_[b][ch]);
        acc += talk_lp_[b][ch] * gains[b];
      }
      s[ch] = wet(Talkbox, s[ch], acc);
    }
  }

  // Vibroflange: short modulated delay. Amount is depth and rate together.
  if (mix_[Vibroflange] > 1e-4f) {
    const float rate = 0.18f + 0.85f * mix_[Vibroflange];
    vib_phase_ = wrap01(vib_phase_ + rate / sr);
    const float lfo = std::sin(2.0f * kPi * vib_phase_);
    const float depth = 6.0f + 22.0f * mix_[Vibroflange];
    for (int ch = 0; ch < 2; ++ch) {
      flange_[ch].push(s[ch]);
      const float d = 16.0f + lfo * depth + static_cast<float>(ch) * 3.0f;
      s[ch] = wet(Vibroflange, s[ch], s[ch] + flange_[ch].tap_lerp(d));
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) flange_[ch].push(s[ch]);
  }

  // Dirty: more amount is more drive, not just more of the same fold.
  if (mix_[Dirty] > 1e-4f) {
    const float drive = 1.4f + 10.0f * mix_[Dirty];
    for (int ch = 0; ch < 2; ++ch) {
      const float d = std::tanh(s[ch] * drive);
      s[ch] = wet(Dirty, s[ch], d);
    }
  }

  // Compressor: flatten peaks so a pad after Dirty still has somewhere to go.
  // Amount lowers the threshold and raises the makeup.
  if (mix_[Compressor] > 1e-4f) {
    const float peak = std::max(std::fabs(s[0]), std::fabs(s[1]));
    env_peak_ = peak > env_peak_ ? peak : env_peak_ * 0.9995f;
    const float thresh = 0.55f - 0.42f * mix_[Compressor];
    const float want = env_peak_ > thresh ? thresh / env_peak_ : 1.0f;
    comp_gain_ += (want - comp_gain_) * 0.01f;
    const float makeup = 1.05f + 0.7f * mix_[Compressor];
    for (int ch = 0; ch < 2; ++ch)
      s[ch] = wet(Compressor, s[ch], s[ch] * comp_gain_ * makeup);
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
  info.push_back({kHoldId, "Hold", 0.0, 1.0, 0.0});
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
    text += std::to_string(pad_amount(p));
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
