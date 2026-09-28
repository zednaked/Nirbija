// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/looper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nirbija {
namespace {

enum Params : uint32_t {
  kRecord = 0,
  kPlay = 1,
  kClear = 2,
  kQuantize = 3,
  kGain = 4,
  kPitch = 5,
  kTone = 6,
  kReverse = 7,
  kFeedback = 8,
  kReplace = 9,
  kOnce = 10,
  kSpeed = 11,
  kCountIn = 12,
};

// The longest loop kept: a minute of stereo. Sized once, up front, because the
// audio thread must never allocate mid-take.
constexpr double kMaxLoopSeconds = 60.0;

// A phrase is a run of input above this, split by this much silence. Undo
// peels the last of those, not the whole Rec pass.
constexpr float kPhraseFloor = 0.02f;
constexpr double kPhraseGapSeconds = 0.1;

// The record-edge ramps, the Play ramp and the wrap crossfade: long enough
// that a hard edge stops clicking, short enough to be no fade to the ear.
constexpr double kEdgeSeconds = 0.005;

// Frames the shadow sweep syncs per block: 256 KB of copy, a few tens of
// microseconds, so a minute of tape catches up within a second while the
// block stays well inside its budget.
constexpr uint64_t kSweepFramesPerBlock = 32768;

bool input_loud(float left, float right) {
  return std::fabs(left) > kPhraseFloor || std::fabs(right) > kPhraseFloor;
}

}  // namespace

PluginDescriptor LooperInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.looper";
  descriptor.name = "Looper";
  descriptor.vendor = "Nirbija";
  descriptor.audio_inputs = 2;
  descriptor.audio_outputs = 2;
  descriptor.has_midi_input = false;
  descriptor.category = "Looper";
  descriptor.kind = PluginKind::Effect;
  return descriptor;
}

LooperInstance::LooperInstance() : descriptor_(make_descriptor()) {
  gain_ramp_.reset(1.0f);
  play_ramp_.reset(1.0f);
  rec_ramp_.reset(0.0f);
  click_.gain = 0.4f;
}

bool LooperInstance::activate(double sample_rate, uint32_t) {
  if (sample_rate <= 0.0) return false;
  if (sample_rate_ == sample_rate && !tapes_[0].empty()) return true;

  // A later prepare() — JACK changing rate after the session has loaded
  // the tape — used to allocate a fresh silent buffer and forget the loop.
  // Keep what is already on the tape and resample it if the rate moved.
  const uint64_t keep = tape_frames();
  const bool closed = length_.load(std::memory_order_relaxed) > 0;
  std::vector<float> kept;
  const double old_rate = sample_rate_;
  if (keep > 0 && tapes_[0].size() >= keep * 2)
    kept.assign(tape(), tape() + keep * 2);

  sample_rate_ = sample_rate;
  capacity_frames_ = static_cast<uint64_t>(sample_rate * kMaxLoopSeconds);
  // Both tapes, the undo layer included, come from here and never grow.
  tapes_[0].assign(capacity_frames_ * 2, 0.0f);
  tapes_[1].assign(capacity_frames_ * 2, 0.0f);
  live_.store(0, std::memory_order_relaxed);
  shadow_state_.assign(capacity_frames_, kShadowSynced);
  touched_lo_ = touched_hi_ = 0;
  stale_pos_ = stale_hi_ = 0;
  recorded_.assign(capacity_frames_, 0);
  recorded_hi_ = 0;
  layer_.assign(capacity_frames_, 0);
  rec_weight_.assign(capacity_frames_, 0);
  current_layer_ = 0;
  peel_count_ = 0;
  play_pos_ = 0.0;
  tone_lpf_[0] = tone_lpf_[1] = 0.0f;
  undo_meta_ = {};
  stop_count_in();

  edge_frames_ = std::max<uint32_t>(
      1, static_cast<uint32_t>(sample_rate * kEdgeSeconds));
  gain_ramp_.set_length(edge_frames_);
  play_ramp_.set_length(edge_frames_);
  rec_ramp_.set_length(edge_frames_);
  gain_ramp_.reset(gain_.load(std::memory_order_relaxed));
  play_ramp_.reset(play_request_.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
  rec_ramp_.reset(0.0f);
  record_active_ = false;

  if (kept.empty()) {
    length_.store(0, std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    loop_beats_.store(0.0, std::memory_order_relaxed);
    stage_ = Stage::Empty;
    return true;
  }

  const uint64_t out_frames =
      import_audio(kept.data(), keep, old_rate > 0.0 ? old_rate : sample_rate);
  if (closed) {
    length_.store(out_frames, std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    stage_ = Stage::Playing;
  } else {
    length_.store(0, std::memory_order_relaxed);
    written_.store(out_frames, std::memory_order_relaxed);
    stage_ = Stage::Defining;
  }
  return true;
}

uint64_t LooperInstance::tape_frames() const {
  const uint64_t length = length_.load(std::memory_order_relaxed);
  if (length > 0) return length;
  return written_.load(std::memory_order_relaxed);
}

uint64_t LooperInstance::import_audio(const float* src, uint64_t src_frames,
                                      double src_rate) {
  if (src == nullptr || src_frames == 0 || tapes_[0].empty() ||
      capacity_frames_ == 0)
    return 0;

  uint64_t out_frames = src_frames;
  if (src_rate > 0.0 && sample_rate_ > 0.0 &&
      std::abs(src_rate - sample_rate_) > 0.5) {
    out_frames = static_cast<uint64_t>(
        std::lround(static_cast<double>(src_frames) * sample_rate_ / src_rate));
  }
  out_frames = std::min(out_frames, capacity_frames_);
  if (out_frames == 0) return 0;

  float* dst = tapes_[0].data();
  if (out_frames == src_frames &&
      (src_rate <= 0.0 || std::abs(src_rate - sample_rate_) <= 0.5)) {
    std::memcpy(dst, src, src_frames * 2 * sizeof(float));
  } else {
    const double step = static_cast<double>(src_frames) /
                        static_cast<double>(std::max<uint64_t>(out_frames, 1));
    const int64_t last = static_cast<int64_t>(src_frames) - 1;
    auto tap = [&](int64_t index, int ch) {
      index = std::clamp<int64_t>(index, 0, last);
      return src[static_cast<uint64_t>(index) * 2 + ch];
    };
    for (uint64_t i = 0; i < out_frames; ++i) {
      const double at = std::min(static_cast<double>(last),
                                 static_cast<double>(i) * step);
      const int64_t i1 = static_cast<int64_t>(at);
      const float frac = static_cast<float>(at - static_cast<double>(i1));
      for (int ch = 0; ch < 2; ++ch) {
        dst[i * 2 + ch] = dsp::hermite4(tap(i1 - 1, ch), tap(i1, ch),
                                        tap(i1 + 1, ch), tap(i1 + 2, ch), frac);
      }
    }
  }
  // The shadow starts equal to the tape so the first undo layer has nothing
  // to catch up on. UI thread, graph quiet: a plain copy is fine here.
  std::memcpy(tapes_[1].data(), dst, out_frames * 2 * sizeof(float));
  live_.store(0, std::memory_order_relaxed);
  std::fill(shadow_state_.begin(), shadow_state_.end(), kShadowSynced);
  touched_lo_ = touched_hi_ = 0;
  stale_pos_ = stale_hi_ = 0;
  return out_frames;
}

double LooperInstance::unit_beats() const {
  const int quantize = std::clamp(quantize_.load(std::memory_order_relaxed),
                                  0, kQuantizeMax);
  // Sync: follow whatever the host last measured on the looper this one is
  // set to chase, pushed in through set_sync_beats(). 0 until it has one,
  // which grid_frames() already treats the same as free-running.
  if (quantize == kQuantizeSync)
    return sync_beats_.load(std::memory_order_relaxed);
  if (quantize <= 0) return 0.0;
  if (quantize == 1) return 1.0;
  const double bar = std::max(1, transport_.numerator);
  static constexpr int kBars[] = {1, 2, 4, 8};
  return bar * kBars[quantize - 2];
}

uint64_t LooperInstance::snap_length(uint64_t written) const {
  const double unit = unit_beats();
  if (unit <= 0.0 || transport_.tempo_bpm <= 0.0 || sample_rate_ <= 0.0)
    return written;
  const double frames_per_beat =
      sample_rate_ * 60.0 / transport_.tempo_bpm;
  const double unit_frames = unit * frames_per_beat;
  if (unit_frames < 1.0) return written;
  const double units = static_cast<double>(written) / unit_frames;
  const uint64_t snapped_units =
      static_cast<uint64_t>(std::max<long>(1, std::lround(units)));
  const uint64_t snapped = static_cast<uint64_t>(
      std::lround(static_cast<double>(snapped_units) * unit_frames));
  return std::clamp(snapped, uint64_t{1}, capacity_frames_);
}

uint64_t LooperInstance::grid_frames() const {
  const double unit = unit_beats();
  if (unit <= 0.0 || transport_.tempo_bpm <= 0.0 || sample_rate_ <= 0.0)
    return 0;
  const uint64_t frames = static_cast<uint64_t>(
      std::lround(unit * sample_rate_ * 60.0 / transport_.tempo_bpm));
  return std::clamp(frames, uint64_t{1}, capacity_frames_);
}

void LooperInstance::close_loop(uint64_t frames, Stage next) {
  frames = std::clamp(frames, uint64_t{1}, capacity_frames_);
  length_.store(frames, std::memory_order_relaxed);
  play_pos_ = 0.0;
  if (transport_.tempo_bpm > 0.0 && sample_rate_ > 0.0) {
    loop_beats_.store(static_cast<double>(frames) / sample_rate_ *
                          transport_.tempo_bpm / 60.0,
                      std::memory_order_relaxed);
  } else {
    loop_beats_.store(0.0, std::memory_order_relaxed);
  }
  stage_ = next;
}

void LooperInstance::set_count_in(bool on) {
  count_in_.store(on, std::memory_order_relaxed);
  // Drop Rec from this thread so the button goes dark without waiting
  // for the audio thread to notice. process() still stops the click.
  if (!on && count_beats_left_.load(std::memory_order_relaxed) > 0)
    record_request_.store(false, std::memory_order_release);
}

void LooperInstance::start_count_in() {
  counting_ = true;
  count_phase_ = 0.0;
  count_total_ = std::max(1, transport_.numerator);
  count_beats_left_.store(count_total_, std::memory_order_relaxed);
  click_.start(sample_rate_, true);
}

void LooperInstance::stop_count_in() {
  counting_ = false;
  count_phase_ = 0.0;
  count_total_ = 0;
  count_beats_left_.store(0, std::memory_order_relaxed);
}

void LooperInstance::begin_record() {
  // A new Rec pass is a new undo layer and a new set of phrases. Leaving
  // Rec down through the auto-close does not come through here, so the
  // first take and the overdubs after it stay one list of islands.
  capture_undo();
  if (stage_ == Stage::Empty) {
    written_.store(0, std::memory_order_relaxed);
    stage_ = Stage::Defining;
    current_layer_ = 0;
  } else {
    // A fresh overdub pass, one layer past whatever was on top before -
    // clamped short of wrapping the byte layer_ is stored in.
    current_layer_ = std::min(current_layer_ + 1, 254);
    // Overdub from the start of the audible window, not the raw buffer:
    // a trimmed loop's "top" is the trim start.
    const uint64_t length = length_.load(std::memory_order_relaxed);
    uint64_t start = trim_start_frames(length);
    uint64_t end = trim_end_frames(length, start);
    if (end <= start || start >= length) {
      start = 0;
      end = length;
    }
    const bool reverse = reverse_.load(std::memory_order_relaxed);
    play_pos_ = reverse && end > start
                    ? static_cast<double>(end) - 1.0
                    : static_cast<double>(start);
    stage_ = Stage::Overdubbing;
  }
  // The input fades onto the tape rather than landing on it: a hard punch
  // is a click that plays back every time round.
  rec_ramp_.set_target(1.0f);
}

void LooperInstance::punch_out() {
  const uint64_t written = written_.load(std::memory_order_relaxed);
  if (stage_ == Stage::Defining && written > 0) {
    // Closing snaps to the chosen grid so a phrase that ran a little
    // long or short still lands on a number of bars you can play to.
    const uint64_t snapped = snap_length(written);
    if (snapped > written) {
      snapshot_range(written, snapped);
      std::memset(tape() + written * 2, 0,
                  (snapped - written) * 2 * sizeof(float));
    }
    close_loop(snapped, Stage::Playing);
  } else if (stage_ == Stage::Overdubbing) {
    stage_ = Stage::Playing;
  }
  // The ramp down keeps writing for ~5 ms past the edge, onto the head of
  // the loop it just closed: the tail crossfades into the start, so the
  // wrap is seamless instead of a step from the last frame to the first.
  rec_ramp_.set_target(0.0f);
}

// Beats to the next quantise line, for the UI. The grid is live while Play
// is on *or* the metronome is rolling the same clock with Play off. With
// neither, or with quantise off, every block is a boundary.
double LooperInstance::beats_to_boundary(const TransportInfo& transport) const {
  const int quantize = std::clamp(quantize_.load(std::memory_order_relaxed),
                                  0, kQuantizeMax);
  const bool grid = transport.rolling || transport.playing;
  if (quantize == 0 || !grid) return -1.0;
  // The same unit punch_frame() waits on: a beat, or a bar - never the
  // 2/4/8-bar Length.
  const double beats_per_unit =
      quantize == 1 ? 1.0 : std::max(1, transport.numerator);
  const double into = std::fmod(transport.beats, beats_per_unit);
  return beats_per_unit - into;
}

uint32_t LooperInstance::punch_frame(uint32_t frames) const {
  const int quantize = std::clamp(quantize_.load(std::memory_order_relaxed),
                                  0, kQuantizeMax);
  const bool grid = transport_.rolling || transport_.playing;
  if (quantize == 0 || !grid) return 0;
  if (transport_.tempo_bpm <= 0.0 || sample_rate_ <= 0.0) return 0;
  // The transport just started: the downbeat is this frame.
  if (transport_.beats == 0.0) return 0;

  // Start and stop wait on the beat or the bar, not on a 4- or 8-bar
  // downbeat: waiting that long to punch in is unplayable. Length is
  // what snaps to 2/4/8 bars, in snap_length().
  const double unit = quantize == 1 ? 1.0 : std::max(1, transport_.numerator);
  const double beats_per_frame = transport_.tempo_bpm / 60.0 / sample_rate_;

  // A line within half a frame of the block start belongs to frame 0 - the
  // block before stopped half a frame short of it (see below), so nothing
  // is punched twice and nothing falls between two blocks.
  const double into = transport_.beats -
                      std::floor(transport_.beats / unit) * unit;
  const double half = 0.5 * beats_per_frame;
  if (into < half || unit - into < half) return 0;

  // boundary_frame() truncates the distance to the line; asking from half a
  // frame earlier turns that into rounding to the nearest frame, which is
  // what the rule above assumes.
  return dsp::boundary_frame(transport_.beats - half, transport_.tempo_bpm,
                             sample_rate_, unit, 0, frames);
}

void LooperInstance::apply_clear() {
  capture_undo();
  length_.store(0, std::memory_order_relaxed);
  written_.store(0, std::memory_order_relaxed);
  play_pos_ = 0.0;
  loop_beats_.store(0.0, std::memory_order_relaxed);
  tone_lpf_[0] = tone_lpf_[1] = 0.0f;
  stage_ = Stage::Empty;
  current_layer_ = 0;
  // A trim or fade shaped for the phrase that just got thrown away means
  // nothing to whatever gets recorded next.
  trim_start_.store(0.0, std::memory_order_relaxed);
  trim_end_.store(1.0, std::memory_order_relaxed);
  fade_in_.store(0.0, std::memory_order_relaxed);
  fade_out_.store(0.0, std::memory_order_relaxed);
  // Clear is a stop, not a punch-in: leaving Rec armed started a new
  // take on the next block, so the button looked stuck and the empty
  // tape began filling again.
  record_active_ = false;
  record_request_.store(false, std::memory_order_relaxed);
  rec_ramp_.reset(0.0f);
  stop_count_in();
}

uint32_t LooperInstance::apply_requests(uint32_t frames) {
  if (clear_request_.exchange(false, std::memory_order_acquire)) {
    apply_clear();
    return frames;
  }
  // Undo and redo wait for the sweep: until the shadow has caught up with
  // what the previous layer changed, swapping it in would bring back an
  // older state than the one before the last pass. A few blocks at most.
  if (undo_request_.load(std::memory_order_acquire) && sweep_done()) {
    undo_request_.store(false, std::memory_order_relaxed);
    apply_undo();
  }
  if (redo_request_.load(std::memory_order_acquire) && sweep_done()) {
    redo_request_.store(false, std::memory_order_relaxed);
    apply_redo();
  }
  if (multiply_request_.exchange(false, std::memory_order_acquire))
    apply_multiply();

  const bool record = record_request_.load(std::memory_order_acquire);
  if (counting_) {
    // Count is a latch: turning it off mid-bar drops the count and Rec,
    // the same as pressing Rec again. Leaving it on and dropping Rec is
    // the other way out.
    if (!record || !count_in_.load(std::memory_order_relaxed)) {
      stop_count_in();
      record_active_ = false;
      record_request_.store(false, std::memory_order_relaxed);
    }
    return frames;
  }
  if (record == record_active_) return frames;

  if (record && count_in_.load(std::memory_order_relaxed)) {
    // The count is the wait: one bar of clicks, then punch in. Skipping
    // the grid so we do not wait a bar for the count and another for
    // the line.
    start_count_in();
    return frames;
  }

  pending_record_ = record;
  return punch_frame(frames);
}

void LooperInstance::apply_record_edge() {
  record_active_ = pending_record_;
  if (pending_record_)
    begin_record();
  else
    punch_out();
}

bool LooperInstance::wrap_play_pos(double start, double end, bool reverse) {
  const double span = end - start;
  if (span <= 0.0) {
    play_pos_ = start;
    return false;
  }
  if (!reverse) {
    if (play_pos_ < end) return false;
    play_pos_ = start + std::fmod(play_pos_ - start, span);
    return true;
  }
  if (play_pos_ >= start) return false;
  play_pos_ = end - std::fmod(start - play_pos_, span);
  if (play_pos_ >= end) play_pos_ = end - 1.0 / std::max(sample_rate_, 1.0);
  if (play_pos_ < start) play_pos_ = start;
  return true;
}

void LooperInstance::read_frame(const float* buffer, double position,
                                uint64_t lo, uint64_t hi, float out[2]) const {
  const uint64_t span = hi - lo;
  const uint64_t i1 = static_cast<uint64_t>(position);
  const float frac = static_cast<float>(position - static_cast<double>(i1));
  const uint64_t i0 = i1 > lo ? i1 - 1 : hi - 1;
  const uint64_t i2 = (i1 + 1 >= hi) ? lo : i1 + 1;
  const uint64_t i3 = (i2 + 1 >= hi) ? lo : i2 + 1;
  if (span < 4) {
    out[0] = buffer[i1 * 2];
    out[1] = buffer[i1 * 2 + 1];
    return;
  }
  for (int ch = 0; ch < 2; ++ch) {
    out[ch] = dsp::hermite4(buffer[i0 * 2 + ch], buffer[i1 * 2 + ch],
                            buffer[i2 * 2 + ch], buffer[i3 * 2 + ch], frac);
  }
}

void LooperInstance::write_frame(uint64_t frame, const float in[2],
                                 float ramp) {
  snapshot(frame);
  float* cell = tape() + frame * 2;
  if (stage_ == Stage::Defining) {
    cell[0] = in[0] * ramp;
    cell[1] = in[1] * ramp;
    rec_weight_[frame] = 255;
    return;
  }
  // Replace is feedback 0 with a different name: the old layer goes as the
  // input comes in.
  const float feedback =
      replace_.load(std::memory_order_relaxed)
          ? 0.0f
          : std::clamp(feedback_.load(std::memory_order_relaxed), 0.0f, 1.0f);
  // How much of a pass this frame has already had. A frame that had a whole
  // one starts over: this is the next time round, and feedback compounds
  // once a cycle the way a tape loop's does.
  float before = static_cast<float>(rec_weight_[frame]) / 255.0f;
  if (before >= 0.98f) before = 0.0f;
  const float after = std::min(1.0f, before + ramp);
  if (feedback < 1.0f) {
    // The old layer is scaled by the same weight the input arrives on, so
    // across the edges nothing steps; over a whole pass it ends at feedback.
    // This touch supplies the ratio between where the frame was and where
    // it should be after it.
    const float keep_before = 1.0f - before * (1.0f - feedback);
    const float keep_after = 1.0f - after * (1.0f - feedback);
    const float keep = keep_before > 1e-6f ? keep_after / keep_before : 0.0f;
    cell[0] *= keep;
    cell[1] *= keep;
  }
  // Unity feedback must not scale the old layer at all: a multiply by 1
  // every sample looks harmless until speed is below 1 and the same cell is
  // visited twice, or a 0.999999 load turns a held Rec into a fade.
  cell[0] += in[0] * ramp;
  cell[1] += in[1] * ramp;
  rec_weight_[frame] = static_cast<uint8_t>(std::lround(after * 255.0f));
}

void LooperInstance::process(const float* const* inputs, float* const* outputs,
                             uint32_t frames) {
  const uint32_t punch = apply_requests(frames);
  sweep_shadow();

  // Everything read from a knob is read here, once a block: the per-sample
  // loop only steps ramps and reads what this worked out.
  const bool play = play_request_.load(std::memory_order_relaxed);
  play_ramp_.set_target(play ? 1.0f : 0.0f);
  gain_ramp_.set_target(gain_.load(std::memory_order_relaxed));
  const bool reverse = reverse_.load(std::memory_order_relaxed);
  const bool once = once_.load(std::memory_order_relaxed);
  const float speed =
      std::clamp(speed_.load(std::memory_order_relaxed), 0.25f, 4.0f);
  const float pitch = std::clamp(
      pitch_.load(std::memory_order_relaxed), -12.0f, 12.0f);
  rate_ = std::pow(2.0, static_cast<double>(pitch) / 12.0) *
          static_cast<double>(speed);
  {
    const float tone =
        std::clamp(tone_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    tone_bypass_ = tone > 0.98f;
    if (!tone_bypass_) {
      // 300 Hz at 0, ~18 kHz at 1: a dark loop stays musical, an open one
      // is almost the dry buffer.
      const float cutoff = 300.0f * std::pow(18000.0f / 300.0f, tone);
      tone_coeff_ = 1.0f - std::exp(-2.0f * 3.14159265358979f * cutoff /
                                    static_cast<float>(sample_rate_));
    }
  }
  const int width = std::min(channels_, 2);
  // The loop's own contribution to the output this block, dry input not
  // included - see loop_peak() above.
  float block_peak = 0.0f;

  for (uint32_t i = 0; i < frames; ++i) {
    // The record edge lands on its frame inside the block, not at the
    // block's start: a punch quantised to the block is up to a block early.
    if (i == punch) apply_record_edge();

    float in[2] = {0.0f, 0.0f};
    for (int ch = 0; ch < width; ++ch) in[ch] = inputs[ch][i];
    if (width == 1) in[1] = in[0];

    // The live signal always passes through: a looper that silences the
    // instrument while recording is unplayable.
    float out[2] = {in[0], in[1]};

    if (counting_ && !count_in_.load(std::memory_order_relaxed)) {
      stop_count_in();
      record_active_ = false;
      record_request_.store(false, std::memory_order_relaxed);
    }

    if (counting_) {
      const double tempo =
          transport_.tempo_bpm > 0.0 ? transport_.tempo_bpm : 120.0;
      const double beats_per_frame =
          tempo / 60.0 / std::max(sample_rate_, 1.0);
      const double before = count_phase_;
      count_phase_ += beats_per_frame;
      if (count_phase_ >= static_cast<double>(count_total_)) {
        stop_count_in();
        record_active_ = true;
        begin_record();
      } else {
        if (std::floor(before) != std::floor(count_phase_)) {
          const int beat = static_cast<int>(std::floor(count_phase_));
          const int bar = std::max(1, transport_.numerator);
          click_.start(sample_rate_, beat % bar == 0);
        }
        const int left = static_cast<int>(
            std::ceil(static_cast<double>(count_total_) - count_phase_));
        count_beats_left_.store(std::max(left, 1), std::memory_order_relaxed);
      }
    }

    const float rec = rec_ramp_.next();
    const float gain = gain_ramp_.next();
    const float play_gain = play_ramp_.next();

    switch (stage_) {
      case Stage::Defining: {
        const uint64_t written = written_.load(std::memory_order_relaxed);
        if (written < capacity_frames_) {
          write_frame(written, in, rec);
          if (record_active_ && input_loud(in[0], in[1]))
            mark_recorded(written);
          if (written < layer_.size())
            layer_[written] = static_cast<uint8_t>(current_layer_);
          const uint64_t next = written + 1;
          written_.store(next, std::memory_order_relaxed);
          // Length is how long the first take is, not only a snap at punch-out.
          // Leaving Rec down used to keep writing silence onto the end, so the
          // phrase sat at the head of a growing tape and seemed to fade away.
          // Close on the grid and stay in overdub: the loop plays, Rec still
          // layers, and the old take is not eaten unless Feedback is down.
          const uint64_t grid = grid_frames();
          if (grid > 0 && next >= grid) close_loop(grid, Stage::Overdubbing);
        }
        break;
      }

      case Stage::Overdubbing:
      case Stage::Playing: {
        const uint64_t length = length_.load(std::memory_order_relaxed);
        if (length == 0) break;
        uint64_t start = trim_start_frames(length);
        uint64_t end = trim_end_frames(length, start);
        // An empty or inverted window (trim start dragged to 1, or the two
        // handles crossed) plays the whole loop. Resetting only `end` left
        // start == length and the next sample indexed one past the buffer.
        if (end <= start || start >= length) {
          start = 0;
          end = length;
        }
        const double start_d = static_cast<double>(start);
        const double end_d = static_cast<double>(end);
        if (play_pos_ < start_d || play_pos_ >= end_d)
          play_pos_ = reverse && end > start ? end_d - 1.0 : start_d;
        if (play_pos_ >= static_cast<double>(length) || play_pos_ < 0.0)
          break;

        const uint64_t i0 = static_cast<uint64_t>(play_pos_);

        // Writing happens while Rec is down and for the ramp either side
        // of it, so the tail of a pass fades onto the tape instead of
        // stopping dead.
        if (rec > 0.0f) {
          write_frame(i0, in, rec);
          if (stage_ == Stage::Overdubbing && input_loud(in[0], in[1])) {
            mark_recorded(i0);
            if (i0 < layer_.size())
              layer_[i0] = static_cast<uint8_t>(current_layer_);
          }
        }

        const float* buffer = tape();
        float raw[2];
        read_frame(buffer, play_pos_, start, end, raw);

        // Wrap crossfade for a trimmed window: the last ~5 ms before the
        // window's end blend into the material just before its start, so
        // the jump back to the start is continuous with what precedes it.
        // The whole, untrimmed loop has no such material - its wrap is made
        // seamless when it is recorded, by the ramp in punch_out().
        {
          const uint64_t span = end - start;
          const uint64_t fade = std::min<uint64_t>(edge_frames_, span / 2);
          if (fade > 0) {
            if (!reverse && start >= fade) {
              const double remaining = end_d - play_pos_;
              if (remaining < static_cast<double>(fade)) {
                float pre[2];
                read_frame(buffer, start_d - remaining, 0, length, pre);
                const auto [fo, fi] = dsp::equal_power(
                    1.0f - static_cast<float>(remaining) /
                               static_cast<float>(fade));
                raw[0] = raw[0] * fo + pre[0] * fi;
                raw[1] = raw[1] * fo + pre[1] * fi;
              }
            } else if (reverse && end + fade <= length) {
              const double remaining = play_pos_ - start_d;
              if (remaining < static_cast<double>(fade)) {
                float post[2];
                read_frame(buffer, end_d + remaining, 0, length, post);
                const auto [fo, fi] = dsp::equal_power(
                    1.0f - static_cast<float>(remaining) /
                               static_cast<float>(fade));
                raw[0] = raw[0] * fo + post[0] * fi;
                raw[1] = raw[1] * fo + post[1] * fi;
              }
            }
          }
        }

        if (play || play_gain > 0.0f) {
          const float env = envelope_at(i0, start, end) * gain;
          float wet[2] = {raw[0] * env, raw[1] * env};
          if (!tone_bypass_) {
            for (int ch = 0; ch < 2; ++ch) {
              tone_lpf_[ch] += tone_coeff_ * (wet[ch] - tone_lpf_[ch]);
              wet[ch] = tone_lpf_[ch];
            }
          } else {
            // Keep the filter primed so turning Tone down later starts
            // from the signal, not from wherever it was last left.
            tone_lpf_[0] = wet[0];
            tone_lpf_[1] = wet[1];
          }
          wet[0] *= play_gain;
          wet[1] *= play_gain;
          out[0] += wet[0];
          out[1] += wet[1];
          block_peak = std::max(block_peak,
                                std::max(std::fabs(wet[0]), std::fabs(wet[1])));
        }
        play_pos_ += reverse ? -rate_ : rate_;
        if (wrap_play_pos(start_d, end_d, reverse) && once) {
          play_request_.store(false, std::memory_order_relaxed);
          play_ramp_.set_target(0.0f);
          play_pos_ = reverse && end > start ? end_d - 1.0 : start_d;
        }
        break;
      }

      case Stage::Empty:
        break;
    }

    if (click_.active()) click_.render(&out[0], &out[1], 1, sample_rate_);

    for (int ch = 0; ch < width; ++ch) outputs[ch][i] = out[ch];
  }
  // A record edge exactly at the block's end belongs to the next block's
  // frame 0; punch_frame() only ever returns `frames` for "not here".

  loop_peak_.store(block_peak, std::memory_order_relaxed);

  const uint64_t length = length_.load(std::memory_order_relaxed);
  const uint64_t written = written_.load(std::memory_order_relaxed);
  // While the first pass is still open there is no play_pos_ to report yet -
  // show how far into the Length grid the write head has gotten instead, so
  // the playhead actually moves across the take instead of jumping straight
  // to the far edge the moment anything is heard.
  double fraction = -1.0;
  if (length > 0) {
    fraction = play_pos_ / static_cast<double>(length);
  } else if (written > 0) {
    const uint64_t grid = grid_frames();
    fraction = grid > 0
                   ? std::min(1.0, static_cast<double>(written) /
                                       static_cast<double>(grid))
                   : 1.0;
  }
  position_fraction_.store(fraction, std::memory_order_relaxed);

  writing_.store(record_active_ && (stage_ == Stage::Defining ||
                                    stage_ == Stage::Overdubbing),
                 std::memory_order_relaxed);
  beats_to_boundary_.store(beats_to_boundary(transport_),
                           std::memory_order_relaxed);
  can_undo_.store(undo_possible(), std::memory_order_relaxed);
  can_redo_.store(redo_possible(), std::memory_order_relaxed);
}

uint64_t LooperInstance::trim_start_frames(uint64_t length) const {
  if (length == 0) return 0;
  const double frac = std::clamp(trim_start_.load(std::memory_order_relaxed),
                                 0.0, 1.0);
  return static_cast<uint64_t>(frac * static_cast<double>(length));
}

uint64_t LooperInstance::trim_end_frames(uint64_t length, uint64_t start) const {
  if (length == 0) return 0;
  const double frac = std::clamp(trim_end_.load(std::memory_order_relaxed),
                                 0.0, 1.0);
  const uint64_t end = static_cast<uint64_t>(frac * static_cast<double>(length));
  // A window closed to nothing (or inverted, mid-drag) plays the whole loop
  // rather than going silent - the fraction is not clamped against the start
  // in set_trim() itself, so the two handles can cross while dragging and
  // still resolve to something audible until they are let go. process() also
  // resets start to 0 when this fires with start == length, or the play
  // position would land one sample past the buffer.
  return end > start ? end : length;
}

float LooperInstance::envelope_at(uint64_t position, uint64_t start,
                                  uint64_t end) const {
  if (end <= start) return 1.0f;
  const uint64_t span = end - start;
  const uint64_t into = position - start;
  const uint64_t remaining = end - position;

  const double fade_in_frac = std::clamp(
      fade_in_.load(std::memory_order_relaxed), 0.0, 1.0);
  const double fade_out_frac = std::clamp(
      fade_out_.load(std::memory_order_relaxed), 0.0, 1.0);
  // The two fades are each capped at half the window so a short loop with
  // both turned up crossfades through the middle instead of one swallowing
  // the other's tail.
  const uint64_t fade_in_frames =
      static_cast<uint64_t>(fade_in_frac * static_cast<double>(span) / 2.0);
  const uint64_t fade_out_frames =
      static_cast<uint64_t>(fade_out_frac * static_cast<double>(span) / 2.0);

  float gain = 1.0f;
  if (fade_in_frames > 0 && into < fade_in_frames)
    gain = std::min(gain, static_cast<float>(into) /
                              static_cast<float>(fade_in_frames));
  if (fade_out_frames > 0 && remaining < fade_out_frames)
    gain = std::min(gain, static_cast<float>(remaining) /
                              static_cast<float>(fade_out_frames));
  return gain;
}

void LooperInstance::set_trim(double start, double end) {
  trim_start_.store(std::clamp(start, 0.0, 1.0), std::memory_order_relaxed);
  trim_end_.store(std::clamp(end, 0.0, 1.0), std::memory_order_relaxed);
}

void LooperInstance::set_fades(double fade_in, double fade_out) {
  fade_in_.store(std::clamp(fade_in, 0.0, 1.0), std::memory_order_relaxed);
  fade_out_.store(std::clamp(fade_out, 0.0, 1.0), std::memory_order_relaxed);
}

std::vector<float> LooperInstance::waveform(int buckets) const {
  std::vector<float> out(static_cast<size_t>(std::max(buckets, 1)), 0.0f);
  const uint64_t closed_len = length_.load(std::memory_order_relaxed);
  const uint64_t written = written_.load(std::memory_order_relaxed);
  uint64_t len = closed_len;
  if (len == 0) {
    // The first pass has no closed length yet. Scale the view to the length
    // it will snap to on Length's grid, not to what has been written so
    // far - otherwise the picture keeps restretching to fill the widget on
    // every refresh and never shows how much room is actually left.
    const uint64_t grid = grid_frames();
    len = grid > 0 ? grid : written;
  }
  if (len == 0 || tapes_[0].empty()) return out;
  // How far into `len` real audio reaches; buckets past this stay at 0 so
  // the untouched part of the grid reads as empty space, not a guess.
  const uint64_t recorded_span = closed_len > 0 ? len : std::min(written, len);

  // The tape itself is not atomic - the audio thread can still be writing
  // into it here, during Defining or Overdubbing, or flip which buffer is
  // live under an undo. Reading it unguarded is the accepted tradeoff for a
  // waveform overview: worst case this draws one stray peak from a torn
  // sample, corrected on the next refresh, never a crash.
  const float* buffer = tape();

  for (size_t b = 0; b < out.size(); ++b) {
    const uint64_t from = len * b / out.size();
    if (from >= recorded_span) continue;
    const uint64_t to = std::max(from + 1, len * (b + 1) / out.size());
    float peak = 0.0f;
    for (uint64_t i = from; i < to && i < recorded_span; ++i) {
      peak = std::max(peak, std::fabs(buffer[i * 2]));
      peak = std::max(peak, std::fabs(buffer[i * 2 + 1]));
    }
    out[b] = peak;
  }
  return out;
}

std::vector<int> LooperInstance::layer_map(int buckets) const {
  std::vector<int> out(static_cast<size_t>(std::max(buckets, 1)), 0);
  const uint64_t closed_len = length_.load(std::memory_order_relaxed);
  const uint64_t written = written_.load(std::memory_order_relaxed);
  uint64_t len = closed_len;
  if (len == 0) {
    const uint64_t grid = grid_frames();
    len = grid > 0 ? grid : written;
  }
  if (len == 0) return out;
  const uint64_t recorded_span = closed_len > 0 ? len : std::min(written, len);
  if (layer_.size() < recorded_span) return out;

  // Same unguarded-read tradeoff as waveform() above, and the same
  // best-effort story Undo does not rewind - see layer_map() in the header.
  for (size_t b = 0; b < out.size(); ++b) {
    const uint64_t from = len * b / out.size();
    if (from >= recorded_span) continue;
    const uint64_t to = std::max(from + 1, len * (b + 1) / out.size());
    int newest = 0;
    for (uint64_t i = from; i < to && i < recorded_span; ++i)
      newest = std::max(newest, static_cast<int>(layer_[i]));
    out[b] = newest;
  }
  return out;
}

std::vector<ParameterInfo> LooperInstance::parameters() const {
  return {
      {kRecord, "Record", 0.0, 1.0, 0.0},
      {kPlay, "Play", 0.0, 1.0, 1.0},
      {kClear, "Clear", 0.0, 1.0, 0.0},
      {kQuantize,
       "Quantize (0 off, 1 beat, 2 1bar, 3 2, 4 4, 5 8, 6 sync to another Looper)",
       0.0, static_cast<double>(kQuantizeMax), 2.0},
      {kGain, "Loop gain", 0.0, 2.0, 1.0},
      {kPitch, "Pitch (semitones)", -12.0, 12.0, 0.0},
      {kTone, "Tone", 0.0, 1.0, 1.0},
      {kReverse, "Reverse", 0.0, 1.0, 0.0},
      {kFeedback, "Overdub feedback", 0.0, 1.0, 1.0},
      {kReplace, "Replace", 0.0, 1.0, 0.0},
      {kOnce, "Play once", 0.0, 1.0, 0.0},
      {kSpeed, "Speed", 0.25, 4.0, 1.0},
      {kCountIn, "Count in", 0.0, 1.0, 0.0},
  };
}

double LooperInstance::parameter_value(uint32_t id) const {
  switch (id) {
    case kRecord: return record_request_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kPlay: return play_request_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kClear: return 0.0;  // a trigger reads as resting
    case kQuantize: return quantize_.load(std::memory_order_relaxed);
    case kGain: return gain_.load(std::memory_order_relaxed);
    case kPitch: return pitch_.load(std::memory_order_relaxed);
    case kTone: return tone_.load(std::memory_order_relaxed);
    case kReverse: return reverse_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kFeedback: return feedback_.load(std::memory_order_relaxed);
    case kReplace: return replace_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kOnce: return once_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kSpeed: return speed_.load(std::memory_order_relaxed);
    case kCountIn: return count_in_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    default: return 0.0;
  }
}

void LooperInstance::set_parameter(uint32_t id, double value) {
  switch (id) {
    case kRecord:
      record_request_.store(value >= 0.5, std::memory_order_release);
      break;
    case kPlay:
      play_request_.store(value >= 0.5, std::memory_order_relaxed);
      break;
    case kClear:
      if (value >= 0.5) clear_request_.store(true, std::memory_order_release);
      break;
    case kQuantize:
      quantize_.store(std::clamp(static_cast<int>(std::lround(value)), 0,
                                 kQuantizeMax),
                      std::memory_order_relaxed);
      break;
    case kGain:
      gain_.store(static_cast<float>(value), std::memory_order_relaxed);
      break;
    case kPitch:
      pitch_.store(static_cast<float>(std::clamp(value, -12.0, 12.0)),
                   std::memory_order_relaxed);
      break;
    case kTone:
      tone_.store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                  std::memory_order_relaxed);
      break;
    case kReverse:
      reverse_.store(value >= 0.5, std::memory_order_relaxed);
      break;
    case kFeedback:
      feedback_.store(static_cast<float>(std::clamp(value, 0.0, 1.0)),
                      std::memory_order_relaxed);
      break;
    case kReplace:
      replace_.store(value >= 0.5, std::memory_order_relaxed);
      break;
    case kOnce:
      once_.store(value >= 0.5, std::memory_order_relaxed);
      break;
    case kSpeed:
      speed_.store(static_cast<float>(std::clamp(value, 0.25, 4.0)),
                   std::memory_order_relaxed);
      break;
    case kCountIn:
      set_count_in(value >= 0.5);
      break;
    default:
      break;
  }
}

namespace {

constexpr char kLoopMagic1[] = "NLOOP1\n";
constexpr char kLoopMagic2[] = "NLOOP2\n";

template <typename T>
void append_pod(std::vector<uint8_t>& out, const T& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool read_pod(const uint8_t*& cursor, const uint8_t* end, T* value) {
  if (static_cast<size_t>(end - cursor) < sizeof(T)) return false;
  std::memcpy(value, cursor, sizeof(T));
  cursor += sizeof(T);
  return true;
}

}  // namespace

std::vector<uint8_t> LooperInstance::save_state() const {
  const uint64_t have = std::min(tape_frames(), capacity_frames_);
  // An open first pass is stored as a closed loop of the length it would
  // have snapped to, so quitting before the punch-out still keeps the take.
  const uint64_t frames =
      (length_.load(std::memory_order_relaxed) == 0 && have > 0)
          ? snap_length(have)
          : have;
  double beats = loop_beats_.load(std::memory_order_relaxed);
  if (beats <= 0.0 && frames > 0 && transport_.tempo_bpm > 0.0 &&
      sample_rate_ > 0.0) {
    beats = static_cast<double>(frames) / sample_rate_ * transport_.tempo_bpm /
            60.0;
  }

  std::vector<uint8_t> out;
  // Sized once. This runs under a playing mixer between takes and under a
  // parked one while Rec is down, and either way a minute of stereo tape is
  // 23 MB: growing into it a doubling at a time would copy most of that
  // several times over on the UI thread for nothing.
  static constexpr size_t kHeaderBytes =
      7 + sizeof(int) + 5 * sizeof(float) + 6 * sizeof(double) +
      3 * sizeof(int) + sizeof(uint64_t);
  out.reserve(kHeaderBytes + static_cast<size_t>(frames) * 2 * sizeof(float));
  out.insert(out.end(), kLoopMagic2, kLoopMagic2 + 7);
  append_pod(out, quantize_.load(std::memory_order_relaxed));
  append_pod(out, gain_.load(std::memory_order_relaxed));
  append_pod(out, trim_start_.load(std::memory_order_relaxed));
  append_pod(out, trim_end_.load(std::memory_order_relaxed));
  append_pod(out, fade_in_.load(std::memory_order_relaxed));
  append_pod(out, fade_out_.load(std::memory_order_relaxed));
  append_pod(out, pitch_.load(std::memory_order_relaxed));
  append_pod(out, tone_.load(std::memory_order_relaxed));
  append_pod(out, sample_rate_);
  append_pod(out, beats);
  append_pod(out, frames);
  const int reverse = reverse_.load(std::memory_order_relaxed) ? 1 : 0;
  const int once = once_.load(std::memory_order_relaxed) ? 1 : 0;
  const int replace = replace_.load(std::memory_order_relaxed) ? 1 : 0;
  append_pod(out, reverse);
  append_pod(out, once);
  append_pod(out, replace);
  append_pod(out, speed_.load(std::memory_order_relaxed));
  append_pod(out, feedback_.load(std::memory_order_relaxed));
  if (frames > 0 && !tapes_[0].empty()) {
    const uint64_t copied = std::min(have, frames);
    if (copied > 0 && tapes_[0].size() >= copied * 2) {
      const auto* samples = reinterpret_cast<const uint8_t*>(tape());
      out.insert(out.end(), samples, samples + copied * 2 * sizeof(float));
    }
    if (frames > copied) {
      const std::vector<uint8_t> pad((frames - copied) * 2 * sizeof(float), 0);
      out.insert(out.end(), pad.begin(), pad.end());
    }
  }
  // Trailer: older blobs stop at the tape. A missing int keeps Count off.
  const int count_in = count_in_.load(std::memory_order_relaxed) ? 1 : 0;
  append_pod(out, count_in);
  // Also trailer-only: which Looper this one follows when Length reads
  // Sync. A blob without it leaves the default -1/-1, i.e. nothing picked.
  append_pod(out, sync_target_row_.load(std::memory_order_relaxed));
  append_pod(out, sync_target_slot_.load(std::memory_order_relaxed));
  return out;
}

bool LooperInstance::load_state(const std::vector<uint8_t>& blob) {
  const bool v2 = blob.size() >= 7 &&
                  std::memcmp(blob.data(), kLoopMagic2, 7) == 0;
  const bool v1 = blob.size() >= 7 &&
                  std::memcmp(blob.data(), kLoopMagic1, 7) == 0;
  if (v1 || v2) {
    const uint8_t* cursor = blob.data() + 7;
    const uint8_t* end = blob.data() + blob.size();
    int quantize = 0;
    float gain = 1.0f;
    double trim_start = 0.0, trim_end = 1.0, fade_in = 0.0, fade_out = 0.0;
    float pitch = 0.0f, tone = 1.0f;
    double saved_rate = 0.0, beats = 0.0;
    uint64_t length = 0;
    if (!read_pod(cursor, end, &quantize) || !read_pod(cursor, end, &gain) ||
        !read_pod(cursor, end, &trim_start) || !read_pod(cursor, end, &trim_end) ||
        !read_pod(cursor, end, &fade_in) || !read_pod(cursor, end, &fade_out) ||
        !read_pod(cursor, end, &pitch) || !read_pod(cursor, end, &tone) ||
        !read_pod(cursor, end, &saved_rate) || !read_pod(cursor, end, &beats) ||
        !read_pod(cursor, end, &length))
      return false;

    int reverse = 0, once = 0, replace = 0;
    float speed = 1.0f, feedback = 1.0f;
    if (v2 &&
        (!read_pod(cursor, end, &reverse) || !read_pod(cursor, end, &once) ||
         !read_pod(cursor, end, &replace) || !read_pod(cursor, end, &speed) ||
         !read_pod(cursor, end, &feedback)))
      return false;

    set_parameter(kQuantize, quantize);
    set_parameter(kGain, gain);
    set_trim(trim_start, trim_end);
    set_fades(fade_in, fade_out);
    set_parameter(kPitch, pitch);
    set_parameter(kTone, tone);
    set_parameter(kReverse, reverse);
    set_parameter(kOnce, once);
    set_parameter(kReplace, replace);
    set_parameter(kSpeed, speed);
    set_parameter(kFeedback, feedback);

    if (tapes_[0].empty())
      activate(sample_rate_ > 0.0 ? sample_rate_ : (saved_rate > 0.0 ? saved_rate
                                                                    : 48000.0),
               256);
    if (tapes_[0].empty() || capacity_frames_ == 0) return true;

    const uint64_t available =
        static_cast<uint64_t>(end - cursor) / (2 * sizeof(float));
    const uint64_t saved_frames = std::min(length, available);
    if (saved_frames == 0) {
      length_.store(0, std::memory_order_relaxed);
      written_.store(0, std::memory_order_relaxed);
      stage_ = Stage::Empty;
      loop_beats_.store(0.0, std::memory_order_relaxed);
      int count_in = 0;
      if (read_pod(cursor, end, &count_in)) set_count_in(count_in != 0);
      int sync_row = -1, sync_slot = -1;
      if (read_pod(cursor, end, &sync_row) && read_pod(cursor, end, &sync_slot))
        set_sync_target(sync_row, sync_slot);
      return true;
    }

    const float* src = reinterpret_cast<const float*>(cursor);
    const uint64_t out_frames = import_audio(src, saved_frames, saved_rate);
    length_.store(out_frames, std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    play_pos_ = 0.0;
    stage_ = Stage::Playing;
    record_active_ = false;
    record_request_.store(false, std::memory_order_relaxed);
    rec_ramp_.reset(0.0f);
    loop_beats_.store(beats, std::memory_order_relaxed);
    peel_count_ = 0;
    clear_recorded();
    undo_meta_ = {};
    stop_count_in();
    const uint8_t* after_audio =
        cursor + saved_frames * 2 * sizeof(float);
    int count_in = 0;
    if (after_audio + sizeof(int) <= end &&
        read_pod(after_audio, end, &count_in))
      set_count_in(count_in != 0);
    int sync_row = -1, sync_slot = -1;
    if (read_pod(after_audio, end, &sync_row) &&
        read_pod(after_audio, end, &sync_slot))
      set_sync_target(sync_row, sync_slot);
    return true;
  }

  const std::string text(blob.begin(), blob.end());
  const std::string_view view(text);

  std::vector<std::string_view> fields;
  size_t start = 0;
  while (true) {
    const size_t split = view.find('\n', start);
    const bool last = split == std::string_view::npos;
    fields.push_back(view.substr(start, last ? std::string_view::npos
                                             : split - start));
    if (last) break;
    start = split + 1;
  }
  if (fields.size() < 2) return false;

  double quantize = 0.0;
  double gain = 0.0;
  if (!parse_number(fields[0], &quantize)) return false;
  if (!parse_number(fields[1], &gain)) return false;
  set_parameter(kQuantize, quantize);
  set_parameter(kGain, gain);

  // Trim and fade are newer fields - a session saved before they existed
  // just keeps the defaults set at construction: the whole loop, no fade.
  if (fields.size() >= 6) {
    double trim_start = 0.0, trim_end = 1.0, fade_in = 0.0, fade_out = 0.0;
    if (parse_number(fields[2], &trim_start) &&
        parse_number(fields[3], &trim_end) &&
        parse_number(fields[4], &fade_in) &&
        parse_number(fields[5], &fade_out)) {
      set_trim(trim_start, trim_end);
      set_fades(fade_in, fade_out);
    }
  }
  if (fields.size() >= 8) {
    double pitch = 0.0, tone = 1.0;
    if (parse_number(fields[6], &pitch) && parse_number(fields[7], &tone)) {
      set_parameter(kPitch, pitch);
      set_parameter(kTone, tone);
    }
  }
  return true;
}

// --- undo machinery ----------------------------------------------------------

void LooperInstance::mark_recorded(uint64_t frame) {
  if (frame >= recorded_.size()) return;
  if (recorded_[frame] == 0) ++recorded_count_;
  recorded_[frame] = 1;
  recorded_hi_ = std::max(recorded_hi_, frame + 1);
}

void LooperInstance::clear_recorded() {
  const uint64_t n = std::min<uint64_t>(recorded_hi_, recorded_.size());
  if (n > 0) std::memset(recorded_.data(), 0, n);
  recorded_hi_ = 0;
  recorded_count_ = 0;
}

void LooperInstance::snapshot(uint64_t frame) {
  if (frame >= shadow_state_.size()) return;
  if (shadow_state_[frame] == kShadowSnapped) return;
  float* dst = shadow() + frame * 2;
  if (frame < undo_meta_.length) {
    const float* src = tape() + frame * 2;
    dst[0] = src[0];
    dst[1] = src[1];
  } else {
    dst[0] = dst[1] = 0.0f;
  }
  shadow_state_[frame] = kShadowSnapped;
  rec_weight_[frame] = 0;
  if (touched_lo_ >= touched_hi_) {
    touched_lo_ = frame;
    touched_hi_ = frame + 1;
  } else {
    touched_lo_ = std::min(touched_lo_, frame);
    touched_hi_ = std::max(touched_hi_, frame + 1);
  }
}

void LooperInstance::snapshot_range(uint64_t lo, uint64_t hi) {
  hi = std::min<uint64_t>(hi, shadow_state_.size());
  for (uint64_t i = lo; i < hi; ++i) snapshot(i);
}

void LooperInstance::sweep_shadow() {
  if (sweep_done()) return;
  const uint64_t stop = std::min(stale_hi_, stale_pos_ + kSweepFramesPerBlock);
  const float* src = tape();
  float* dst = shadow();
  for (uint64_t i = stale_pos_; i < stop; ++i) {
    if (shadow_state_[i] != kShadowStale) continue;
    dst[i * 2] = src[i * 2];
    dst[i * 2 + 1] = src[i * 2 + 1];
    shadow_state_[i] = kShadowSynced;
  }
  stale_pos_ = stop;
  if (sweep_done()) stale_pos_ = stale_hi_ = 0;
}

void LooperInstance::capture_undo() {
  // Whatever the layer being replaced still differs from the tape on is
  // now permanent, so the shadow has to catch up there before the next
  // undo can swap it in. Queue that for the sweep rather than doing it
  // here: it can be the whole loop, and this runs inside a block.
  if (touched_lo_ < touched_hi_) {
    uint64_t lo = touched_lo_;
    uint64_t hi = touched_hi_;
    if (!sweep_done()) {
      lo = std::min(lo, stale_pos_);
      hi = std::max(hi, stale_hi_);
    }
    hi = std::min<uint64_t>(hi, shadow_state_.size());
    if (hi > lo) std::memset(shadow_state_.data() + lo, kShadowStale, hi - lo);
    stale_pos_ = lo;
    stale_hi_ = hi;
  }
  touched_lo_ = touched_hi_ = 0;

  undo_meta_.length = length_.load(std::memory_order_relaxed);
  undo_meta_.beats = loop_beats_.load(std::memory_order_relaxed);
  undo_meta_.trim_start = trim_start_.load(std::memory_order_relaxed);
  undo_meta_.trim_end = trim_end_.load(std::memory_order_relaxed);
  undo_meta_.fade_in = fade_in_.load(std::memory_order_relaxed);
  undo_meta_.fade_out = fade_out_.load(std::memory_order_relaxed);
  undo_meta_.valid = true;
  undo_meta_.undone = false;
  peel_count_ = 0;
  clear_recorded();
}

void LooperInstance::swap_undo() {
  if (!undo_meta_.valid) return;

  UndoMeta current;
  current.length = length_.load(std::memory_order_relaxed);
  current.beats = loop_beats_.load(std::memory_order_relaxed);
  current.trim_start = trim_start_.load(std::memory_order_relaxed);
  current.trim_end = trim_end_.load(std::memory_order_relaxed);
  current.fade_in = fade_in_.load(std::memory_order_relaxed);
  current.fade_out = fade_out_.load(std::memory_order_relaxed);
  current.valid = true;
  current.undone = !undo_meta_.undone;

  // The audio half of the swap is one store: the shadow becomes the tape.
  // Frames the layer never touched are identical in both (the sweep saw to
  // that), frames it did touch hold the old value in the shadow.
  live_.store(live_.load(std::memory_order_relaxed) ^ 1,
              std::memory_order_relaxed);

  const uint64_t n = std::min(undo_meta_.length, capacity_frames_);
  length_.store(n, std::memory_order_relaxed);
  written_.store(0, std::memory_order_relaxed);
  play_pos_ = 0.0;
  loop_beats_.store(undo_meta_.beats, std::memory_order_relaxed);
  set_trim(undo_meta_.trim_start, undo_meta_.trim_end);
  set_fades(undo_meta_.fade_in, undo_meta_.fade_out);
  stage_ = n > 0 ? Stage::Playing : Stage::Empty;
  record_active_ = false;
  record_request_.store(false, std::memory_order_relaxed);
  clear_request_.store(false, std::memory_order_relaxed);
  rec_ramp_.reset(0.0f);

  undo_meta_ = current;
  peel_count_ = 0;
  clear_recorded();
}

bool LooperInstance::last_burst(uint64_t* start, uint64_t* end) const {
  const uint64_t n = std::min({tape_frames(), recorded_hi_,
                               static_cast<uint64_t>(recorded_.size())});
  if (n == 0) return false;
  const uint64_t gap = std::max<uint64_t>(
      1, static_cast<uint64_t>(sample_rate_ * kPhraseGapSeconds));

  uint64_t last = n;
  while (last > 0 && recorded_[last - 1] == 0) --last;
  if (last == 0) return false;

  uint64_t first = last;
  uint64_t quiet = 0;
  uint64_t i = last;
  while (i > 0) {
    --i;
    if (recorded_[i] != 0) {
      quiet = 0;
      first = i;
    } else {
      ++quiet;
      if (quiet >= gap) break;
    }
  }
  if (last <= first) return false;
  if (start != nullptr) *start = first;
  if (end != nullptr) *end = last;
  return true;
}

bool LooperInstance::undo_possible() const {
  if (has_burst()) return true;
  return undo_meta_.valid && !undo_meta_.undone && peel_count_ == 0;
}

bool LooperInstance::redo_possible() const {
  return peel_count_ > 0 || (undo_meta_.valid && undo_meta_.undone);
}

void LooperInstance::swap_range(uint64_t lo, uint64_t hi) {
  hi = std::min<uint64_t>(hi, capacity_frames_);
  float* a = tape();
  float* b = shadow();
  for (uint64_t i = lo * 2; i < hi * 2; ++i) std::swap(a[i], b[i]);
}

bool LooperInstance::peel_last_burst() {
  if (peel_count_ >= kMaxPeels) return false;
  uint64_t start = 0;
  uint64_t stop = 0;
  if (!last_burst(&start, &stop)) return false;
  if (stop <= start || stop > capacity_frames_) return false;

  // The phrase's frames were all written this pass, so the shadow holds
  // what was there before it; swapping the range puts that back and parks
  // the phrase in the shadow for redo. Bounded by the phrase, not the tape.
  Peel peel;
  peel.start = start;
  peel.end = stop;
  swap_range(start, stop);
  for (uint64_t i = start; i < stop; ++i) {
    if (recorded_[i] != 0) --recorded_count_;
    recorded_[i] = 0;
  }

  // Last island of a first take: the tape is now empty against an empty
  // snapshot, so drop the loop. Leaving a silent closed take made Undo
  // look like it had done nothing.
  if (undo_meta_.length == 0 && !has_burst()) {
    peel.dropped_length = length_.load(std::memory_order_relaxed);
    peel.dropped_beats = loop_beats_.load(std::memory_order_relaxed);
    length_.store(0, std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    play_pos_ = 0.0;
    loop_beats_.store(0.0, std::memory_order_relaxed);
    if (record_request_.load(std::memory_order_relaxed)) {
      stage_ = Stage::Defining;
    } else {
      stage_ = Stage::Empty;
      record_active_ = false;
      record_request_.store(false, std::memory_order_relaxed);
      rec_ramp_.reset(0.0f);
    }
  }

  peels_[peel_count_++] = peel;
  return true;
}

void LooperInstance::restore_peel() {
  if (peel_count_ == 0) return;
  const Peel peel = peels_[--peel_count_];
  if (peel.dropped_length > 0) {
    length_.store(peel.dropped_length, std::memory_order_relaxed);
    loop_beats_.store(peel.dropped_beats, std::memory_order_relaxed);
    if (stage_ == Stage::Empty || stage_ == Stage::Defining)
      stage_ = Stage::Playing;
  }
  if (peel.end > peel.start && peel.end <= capacity_frames_) {
    swap_range(peel.start, peel.end);
    for (uint64_t i = peel.start; i < peel.end; ++i) {
      if (recorded_[i] == 0) ++recorded_count_;
      recorded_[i] = 1;
    }
    recorded_hi_ = std::max(recorded_hi_, peel.end);
  }
}

void LooperInstance::apply_undo() {
  if (peel_last_burst()) return;
  if (undo_possible()) swap_undo();
}

void LooperInstance::apply_redo() {
  if (peel_count_ > 0) {
    restore_peel();
    return;
  }
  if (redo_possible()) swap_undo();
}

bool LooperInstance::can_multiply() const {
  const uint64_t n = length_.load(std::memory_order_relaxed);
  return n > 0 && n <= capacity_frames_ / 2;
}

void LooperInstance::apply_multiply() {
  const uint64_t n = length_.load(std::memory_order_relaxed);
  if (n == 0 || n > capacity_frames_ / 2) return;
  // Its own undo layer, like a Rec pass: the new half is written through
  // the snapshot so undo knows it was silent (and short) before.
  capture_undo();
  snapshot_range(n, n * 2);
  float* buffer = tape();
  std::memcpy(buffer + n * 2, buffer, n * 2 * sizeof(float));
  if (layer_.size() >= n * 2)
    std::memcpy(layer_.data() + n, layer_.data(), n * sizeof(uint8_t));
  length_.store(n * 2, std::memory_order_relaxed);
  loop_beats_.store(loop_beats_.load(std::memory_order_relaxed) * 2.0,
                    std::memory_order_relaxed);
}

}  // namespace nirbija
