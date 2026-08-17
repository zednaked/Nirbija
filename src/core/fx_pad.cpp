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
    // Two seconds: tap() is a distance behind a write head that also
    // advances, so covering one second of reverse needs twice the tape.
    reverse_[ch].setup(two_sec);
    stutter_[ch].setup(one_sec);
    flange_[ch].setup(static_cast<size_t>(sample_rate_ * 0.02) + 8);
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
  pitch_pos_[0] = pitch_pos_[1] = 0.0f;
  filter_lp_[0] = filter_lp_[1] = 0.0f;
  filter_bp_[0] = filter_bp_[1] = 0.0f;
  std::memset(talk_lp_, 0, sizeof(talk_lp_));
  env_peak_ = 0.0f;
  comp_gain_ = 1.0f;
  vib_phase_ = 0.0f;
  talk_phase_ = 0.0f;
  gate_phase_ = 0.0;
  stutter_len_ = 0;
  stutter_pos_ = 0;
  reverse_play_ = 0;
  return true;
}

bool FxPadInstance::pad_on(int pad) const {
  if (pad < 0 || pad >= kPads) return false;
  return pad_[static_cast<size_t>(pad)].load(std::memory_order_relaxed);
}

void FxPadInstance::set_pad(int pad, bool on) {
  if (pad < 0 || pad >= kPads) return;
  pad_[static_cast<size_t>(pad)].store(on, std::memory_order_relaxed);
}

void FxPadInstance::set_hold(bool on) {
  hold_.store(on, std::memory_order_relaxed);
  if (!on) {
    for (auto& p : pad_) p.store(false, std::memory_order_relaxed);
  }
}

void FxPadInstance::attack(int pad) {
  const float fpb = frames_per_beat(transport_, sample_rate_);
  switch (pad) {
    case Stutter: {
      stutter_len_ = std::max(32u, static_cast<uint32_t>(fpb * 0.25f));
      stutter_pos_ = 0;
      const size_t n = stutter_[0].data.size();
      for (int ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < stutter_len_ && i < n; ++i)
          stutter_[ch].data[i] = hist_[ch].tap(stutter_len_ - i);
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

  auto wet = [&](int pad, float dry, float processed) {
    return dry + mix_[static_cast<size_t>(pad)] * (processed - dry);
  };

  // Crush: hold a sample and fold the word length.
  if (mix_[Crush] > 1e-4f) {
    if (crush_count_ == 0) {
      crush_hold_[0] = s[0];
      crush_hold_[1] = s[1];
    }
    crush_count_ = (crush_count_ + 1) % 12;
    const float steps = 8.0f;
    for (int ch = 0; ch < 2; ++ch) {
      float q = std::round(crush_hold_[ch] * steps) / steps;
      s[ch] = wet(Crush, s[ch], q);
    }
  }

  // Pitch: read the history slower, two grains crossfading.
  if (mix_[Pitch] > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      pitch_pos_[ch] += 0.72f;
      const float grain = fpb * 0.35f;
      if (pitch_pos_[ch] >= grain) pitch_pos_[ch] -= grain;
      const float a = hist_[ch].tap_lerp(pitch_pos_[ch] + 8.0f);
      const float b = hist_[ch].tap_lerp(pitch_pos_[ch] + grain * 0.5f + 8.0f);
      const float x = pitch_pos_[ch] / std::max(1.0f, grain);
      const float g = (x < 0.5f) ? x * 2.0f : (1.0f - x) * 2.0f;
      s[ch] = wet(Pitch, s[ch], a * (1.0f - g) + b * g);
    }
  }

  // Comb.
  if (mix_[Comb] > 1e-4f) {
    const float d = sr * 0.0065f;
    for (int ch = 0; ch < 2; ++ch) {
      const float c = s[ch] + hist_[ch].tap_lerp(d) * 0.7f;
      s[ch] = wet(Comb, s[ch], c);
    }
  }

  // Ring mod.
  if (mix_[Ring] > 1e-4f) {
    ring_phase_ = wrap01(ring_phase_ + 92.0f / sr);
    const float m = std::sin(2.0f * kPi * ring_phase_);
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Ring, s[ch], s[ch] * m);
  }

  // Reverb: four combs and two allpasses, cheap and dense enough.
  if (mix_[Reverb] > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      float acc = 0.0f;
      for (int c = 0; c < 4; ++c) {
        float z = reverb_comb_[c][ch].tap(reverb_comb_[c][ch].data.size() - 1);
        z = s[ch] + z * 0.82f;
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

  // Stutter: loop the captured slice.
  if (mix_[Stutter] > 1e-4f && stutter_len_ > 0) {
    const uint32_t i = stutter_pos_ % stutter_len_;
    stutter_pos_ = (stutter_pos_ + 1) % stutter_len_;
    for (int ch = 0; ch < 2; ++ch)
      s[ch] = wet(Stutter, s[ch], stutter_[ch].data[i]);
  }

  // Gate: half an eighth-note open.
  if (mix_[Gate] > 1e-4f) {
    const double step = 1.0 / std::max(1.0f, fpb * 0.5f);
    gate_phase_ = std::fmod(gate_phase_ + step, 1.0);
    const float g = gate_phase_ < 0.5 ? 1.0f : 0.0f;
    for (int ch = 0; ch < 2; ++ch) s[ch] = wet(Gate, s[ch], s[ch] * g);
  }

  // Filter: resonant lowpass, parked low so it reads as a pad not an EQ.
  if (mix_[Filter] > 1e-4f) {
    const float f = 700.0f / sr;
    const float q = 0.35f;
    for (int ch = 0; ch < 2; ++ch) {
      filter_lp_[ch] += f * filter_bp_[ch];
      const float hp = s[ch] - filter_lp_[ch] - q * filter_bp_[ch];
      filter_bp_[ch] += f * hp;
      s[ch] = wet(Filter, s[ch], filter_lp_[ch]);
    }
  }

  // Cutter: 1/16 on-off, opposite of a gate so it chops rather than breathes.
  if (mix_[Cutter] > 1e-4f) {
    const double step = 1.0 / std::max(1.0f, fpb * 0.25f);
    const double phase = std::fmod(transport_.beats * 4.0, 1.0);
    const float g = (phase < 0.5 || step <= 0.0) ? 1.0f : 0.0f;
    (void)step;
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

  // Dub: long feedback delay with a dark loop.
  if (mix_[Dub] > 1e-4f) {
    const float d = fpb * 0.75f;
    for (int ch = 0; ch < 2; ++ch) {
      float z = delay_[ch].tap_lerp(d);
      z = z * 0.72f + s[ch];
      z += (delay_[ch].tap_lerp(d * 0.5f) - z) * 0.35f;
      delay_[ch].push(z);
      s[ch] = wet(Dub, s[ch], z);
    }
  }

  // Tempo delay: a clean eighth.
  if (mix_[TempoDelay] > 1e-4f) {
    const float d = fpb * 0.5f;
    for (int ch = 0; ch < 2; ++ch) {
      const float z = s[ch] + delay_[ch].tap_lerp(d) * 0.45f;
      if (mix_[Dub] <= 1e-4f) delay_[ch].push(z);
      s[ch] = wet(TempoDelay, s[ch], z);
    }
  } else if (mix_[Dub] <= 1e-4f && mix_[Comb] <= 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) delay_[ch].push(s[ch]);
  }

  // Talkbox: three formants, vowel swept slowly. Its own accumulator, not a
  // scaled read of the flanger's: that one is only advanced inside the
  // Vibroflange branch below, so the vowel sat frozen unless both pads
  // happened to be down, and the 0.15 scaling made it jump rather than turn
  // over every time the flanger's phase wrapped.
  talk_phase_ = wrap01(talk_phase_ + 0.0825f / sr);
  if (mix_[Talkbox] > 1e-4f) {
    const float vowel = 0.5f + 0.5f * std::sin(2.0f * kPi * talk_phase_);
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

  // Vibroflange: short modulated delay.
  if (mix_[Vibroflange] > 1e-4f) {
    vib_phase_ = wrap01(vib_phase_ + 0.55f / sr);
    const float lfo = std::sin(2.0f * kPi * vib_phase_);
    for (int ch = 0; ch < 2; ++ch) {
      flange_[ch].push(s[ch]);
      const float d = 24.0f + lfo * 18.0f + static_cast<float>(ch) * 3.0f;
      s[ch] = wet(Vibroflange, s[ch], s[ch] + flange_[ch].tap_lerp(d));
    }
  } else {
    for (int ch = 0; ch < 2; ++ch) flange_[ch].push(s[ch]);
  }

  // Dirty.
  if (mix_[Dirty] > 1e-4f) {
    for (int ch = 0; ch < 2; ++ch) {
      const float d = std::tanh(s[ch] * 6.0f);
      s[ch] = wet(Dirty, s[ch], d);
    }
  }

  // Compressor: flatten peaks so a pad after Dirty still has somewhere to go.
  if (mix_[Compressor] > 1e-4f) {
    const float peak = std::max(std::fabs(s[0]), std::fabs(s[1]));
    env_peak_ = peak > env_peak_ ? peak : env_peak_ * 0.9995f;
    const float thresh = 0.25f;
    const float want = env_peak_ > thresh ? thresh / env_peak_ : 1.0f;
    comp_gain_ += (want - comp_gain_) * 0.01f;
    for (int ch = 0; ch < 2; ++ch)
      s[ch] = wet(Compressor, s[ch], s[ch] * comp_gain_ * 1.4f);
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
    const float target = pad_[static_cast<size_t>(p)].load(std::memory_order_relaxed)
                             ? 1.0f
                             : 0.0f;
    mix_[static_cast<size_t>(p)] +=
        coeff * (target - mix_[static_cast<size_t>(p)]);
    const bool on = mix_[static_cast<size_t>(p)] > 0.5f;
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
  for (int p = 0; p < kPads; ++p)
    info.push_back({static_cast<uint32_t>(p), pad_name(p), 0.0, 1.0, 0.0});
  info.push_back({kHoldId, "Hold", 0.0, 1.0, 0.0});
  return info;
}

double FxPadInstance::parameter_value(uint32_t id) const {
  if (id == kHoldId) return hold() ? 1.0 : 0.0;
  if (id < kPads) return pad_on(static_cast<int>(id)) ? 1.0 : 0.0;
  return 0.0;
}

void FxPadInstance::set_parameter(uint32_t id, double value) {
  if (id == kHoldId) {
    set_hold(value >= 0.5);
    return;
  }
  if (id < kPads) set_pad(static_cast<int>(id), value >= 0.5);
}

std::vector<uint8_t> FxPadInstance::save_state() const {
  std::string text = hold() ? "1\n" : "0\n";
  for (int p = 0; p < kPads; ++p) {
    text += pad_on(p) ? '1' : '0';
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
      else if (field - 1 < kPads) set_pad(field - 1, value >= 0.5);
    }
    ++field;
    if (split == std::string::npos) break;
    start = split + 1;
  }
  return field > 0;
}

}  // namespace nirbija
