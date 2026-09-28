// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

// Small DSP helpers shared by the built-in plugins and the graph. Everything
// here is header-only, allocation-free and safe on the audio thread. Each
// helper exists because at least two plugins had grown their own copy.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace nirbija::dsp {

// A value that walks linearly from where it is to where it was told to go,
// over a fixed number of samples. Zipper-free gain, pan, pitch: read the
// target once per block with set_target(), then next() once per sample.
class LinearRamp {
 public:
  void reset(float value) { current_ = target_ = value; step_ = 0.0f; left_ = 0; }
  void set_length(uint32_t samples) { length_ = std::max<uint32_t>(1, samples); }
  void set_target(float target) {
    if (target == target_) return;
    target_ = target;
    step_ = (target_ - current_) / static_cast<float>(length_);
    left_ = length_;
  }
  float next() {
    if (left_ == 0) return current_;
    current_ += step_;
    if (--left_ == 0) current_ = target_;
    return current_;
  }
  float value() const { return current_; }
  float target() const { return target_; }
  bool settled() const { return left_ == 0; }

 private:
  float current_ = 0.0f;
  float target_ = 0.0f;
  float step_ = 0.0f;
  uint32_t left_ = 0;
  uint32_t length_ = 64;
};

// One-pole smoother: exponential approach, coefficient from a time constant.
class OnePole {
 public:
  void set_time(double seconds, double sample_rate) {
    coeff_ = static_cast<float>(std::exp(-1.0 / std::max(1e-6, seconds * sample_rate)));
  }
  void reset(float value) { value_ = value; }
  float next(float target) {
    value_ += (1.0f - coeff_) * (target - value_);
    return value_;
  }
  float value() const { return value_; }

 private:
  float coeff_ = 0.0f;
  float value_ = 0.0f;
};

// 4-point, 3rd-order Hermite interpolation. y1 is the sample at the integer
// position, frac in [0, 1) the distance towards y2. Sounds far less dull than
// linear when pitching down and aliases far less when pitching up.
inline float hermite4(float y0, float y1, float y2, float y3, float frac) {
  const float c0 = y1;
  const float c1 = 0.5f * (y2 - y0);
  const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
  return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

// Equal-power crossfade weights for t in [0, 1]: (fade-out gain, fade-in
// gain). Two signals of equal loudness keep their loudness through the cross.
inline std::pair<float, float> equal_power(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float a = t * 1.57079632679f;
  return {std::cos(a), std::sin(a)};
}

// Ring buffer index arithmetic without the integer division a `%` costs per
// sample.
inline size_t ring_next(size_t index, size_t size) {
  return (++index == size) ? 0 : index;
}
inline size_t ring_back(size_t index, size_t delay, size_t size) {
  return index >= delay ? index - delay : index + size - delay;
}

// Given a transport at `beats` (with `tempo_bpm` and `sample_rate`), the frame
// inside a block of `frames` where the next boundary of `unit_beats` falls,
// looking from frame `from`. Returns `frames` when the boundary is beyond
// this block and `from` when quantisation is off. Sample-exact: the looper,
// the sampler and anything else that punches on the grid share this so they
// agree to the frame.
inline uint32_t boundary_frame(double beats, double tempo_bpm, double sample_rate,
                               double unit_beats, uint32_t from, uint32_t frames) {
  if (unit_beats <= 0.0 || tempo_bpm <= 0.0 || sample_rate <= 0.0) return from;
  const double beats_per_frame = (tempo_bpm / 60.0) / sample_rate;
  if (beats_per_frame <= 0.0) return from;
  const double now = beats + static_cast<double>(from) * beats_per_frame;
  double next = std::ceil(now / unit_beats) * unit_beats;
  if (next <= now + 1e-9) next += unit_beats;
  const double delta = (next - beats) / beats_per_frame;
  if (delta <= 0.0) return from;
  if (delta >= static_cast<double>(frames)) return frames;
  return static_cast<uint32_t>(delta);
}

// Whether a block of `frames` starting at transport `beats` crosses a
// boundary of `unit_beats`; the companion to boundary_frame for the callers
// that only need yes or no.
inline bool block_crosses(double beats, double tempo_bpm, double sample_rate,
                          double unit_beats, uint32_t frames) {
  if (unit_beats <= 0.0 || tempo_bpm <= 0.0 || sample_rate <= 0.0) return true;
  const double block_beats = tempo_bpm / 60.0 * (static_cast<double>(frames) / sample_rate);
  const double before = beats / unit_beats;
  const double after = (beats + block_beats) / unit_beats;
  return std::floor(before) != std::floor(after) || beats == 0.0;
}

// A short metronome click: one sine burst with a linear decay. Every count-in
// and every click in the box sounds the same because they all come from here.
struct ClickTone {
  double phase = 0.0;
  uint32_t left = 0;
  uint32_t length = 0;
  float hz = 1000.0f;
  float gain = 0.5f;

  void start(double sample_rate, bool accent, double seconds = 0.03) {
    length = std::max<uint32_t>(1, static_cast<uint32_t>(sample_rate * seconds));
    left = length;
    phase = 0.0;
    hz = accent ? 1500.0f : 1000.0f;
  }
  bool active() const { return left > 0; }
  // Adds the click into `out_l`/`out_r` for `frames` samples.
  void render(float* out_l, float* out_r, uint32_t frames, double sample_rate) {
    if (left == 0 || sample_rate <= 0.0) return;
    const double inc = 2.0 * 3.14159265358979323846 * hz / sample_rate;
    const uint32_t n = std::min(frames, left);
    for (uint32_t i = 0; i < n; ++i) {
      const float env = static_cast<float>(left) / static_cast<float>(length);
      const float s = static_cast<float>(std::sin(phase)) * env * gain;
      phase += inc;
      out_l[i] += s;
      if (out_r != nullptr) out_r[i] += s;
      --left;
    }
  }
};

// Buffers handed from the UI thread to the audio thread by pointer, kept
// alive until the audio thread has provably left the old one. The order of
// operations is what makes it safe and it is easy to get backwards, so it
// lives in one place:
//
//   publish(new)                  (UI thread)
//     live.store(new)             the audio thread sees it from the next block
//     gen = generation.load()     the block that may still hold the old one
//     retired.push({old, gen})
//   reclaim()                     (UI thread, any later time)
//     frees entries whose gen + 2 <= generation, i.e. two whole blocks after
//     the swap, so a block that loaded the old pointer before the store and
//     is still running has certainly finished.
//
// `generation` is the counter the audio thread bumps at the end of every
// process() call with release ordering.
template <typename T>
class RetiredList {
 public:
  void retire(std::shared_ptr<T> old, uint64_t generation_now) {
    if (old != nullptr) retired_.push_back({std::move(old), generation_now});
  }
  // `audio_running` false means there is no audio thread at all, so nothing
  // can still be inside a retired buffer and everything goes at once.
  void reclaim(uint64_t generation_now, bool audio_running) {
    if (!audio_running) {
      retired_.clear();
      return;
    }
    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
                       [generation_now](const Entry& e) {
                         return e.generation + 2 <= generation_now;
                       }),
        retired_.end());
  }
  size_t size() const { return retired_.size(); }
  bool empty() const { return retired_.empty(); }

 private:
  struct Entry {
    std::shared_ptr<T> buffer;
    uint64_t generation;
  };
  std::vector<Entry> retired_;
};

}  // namespace nirbija::dsp
