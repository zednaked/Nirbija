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

constexpr float kPi = 3.14159265358979f;

// The longest loop kept: a minute of stereo. Sized once, up front, because the
// audio thread must never allocate mid-take.
constexpr double kMaxLoopSeconds = 60.0;

// A phrase is a run of input above this, split by this much silence. Undo
// peels the last of those, not the whole Rec pass.
constexpr float kPhraseFloor = 0.02f;
constexpr double kPhraseGapSeconds = 0.1;

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

LooperInstance::LooperInstance() : descriptor_(make_descriptor()) {}

bool LooperInstance::activate(double sample_rate, uint32_t) {
  if (sample_rate <= 0.0) return false;
  if (sample_rate_ == sample_rate && !buffer_.empty()) return true;

  // A later prepare() — JACK changing rate after the session has loaded
  // the tape — used to allocate a fresh silent buffer and forget the loop.
  // Keep what is already on the tape and resample it if the rate moved.
  const uint64_t keep = tape_frames();
  const bool closed = length_.load(std::memory_order_relaxed) > 0;
  std::vector<float> kept;
  const double old_rate = sample_rate_;
  if (keep > 0 && buffer_.size() >= keep * 2)
    kept.assign(buffer_.data(), buffer_.data() + keep * 2);

  sample_rate_ = sample_rate;
  capacity_frames_ = static_cast<uint64_t>(sample_rate * kMaxLoopSeconds);
  buffer_.assign(capacity_frames_ * 2, 0.0f);
  recorded_.assign(capacity_frames_, 0);
  layer_.assign(capacity_frames_, 0);
  current_layer_ = 0;
  peels_.clear();
  play_pos_ = 0.0;
  tone_lpf_[0] = tone_lpf_[1] = 0.0f;
  undo_ = {};
  stop_count_in();

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
  if (src == nullptr || src_frames == 0 || buffer_.empty() ||
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

  if (out_frames == src_frames &&
      (src_rate <= 0.0 || std::abs(src_rate - sample_rate_) <= 0.5)) {
    std::memcpy(buffer_.data(), src, src_frames * 2 * sizeof(float));
    return out_frames;
  }

  const double step = static_cast<double>(src_frames) /
                      static_cast<double>(std::max<uint64_t>(out_frames, 1));
  for (uint64_t i = 0; i < out_frames; ++i) {
    const double at = std::min(static_cast<double>(src_frames - 1),
                               static_cast<double>(i) * step);
    const uint64_t i0 = static_cast<uint64_t>(at);
    const uint64_t i1 = std::min(src_frames - 1, i0 + 1);
    const float frac = static_cast<float>(at - static_cast<double>(i0));
    buffer_[i * 2] = src[i0 * 2] + (src[i1 * 2] - src[i0 * 2]) * frac;
    buffer_[i * 2 + 1] =
        src[i0 * 2 + 1] + (src[i1 * 2 + 1] - src[i0 * 2 + 1]) * frac;
  }
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
  fire_count_click(true);
}

void LooperInstance::stop_count_in() {
  counting_ = false;
  count_phase_ = 0.0;
  count_total_ = 0;
  count_beats_left_.store(0, std::memory_order_relaxed);
}

void LooperInstance::begin_record() {
  // A new Rec pass starts a new set of phrases. Leaving Rec down through
  // the auto-close does not come through here, so the first take and the
  // overdubs after it stay one list of islands.
  clear_recorded();
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
}

void LooperInstance::fire_count_click(bool downbeat) {
  if (sample_rate_ <= 0.0) return;
  click_length_ = static_cast<uint32_t>(sample_rate_ * 0.03);
  click_remaining_ = click_length_;
  click_phase_ = 0.0;
  click_step_ = 2.0 * kPi * (downbeat ? 1568.0 : 1046.5) / sample_rate_;
}

float LooperInstance::tone_sample(int channel, float sample) {
  const float tone =
      std::clamp(tone_.load(std::memory_order_relaxed), 0.0f, 1.0f);
  if (tone > 0.98f) return sample;
  // 300 Hz at 0, ~18 kHz at 1: a dark loop stays musical, an open one
  // is almost the dry buffer.
  const float cutoff =
      300.0f * std::pow(18000.0f / 300.0f, tone);
  const float coeff = 1.0f - std::exp(-2.0f * 3.14159265358979f * cutoff /
                                      static_cast<float>(sample_rate_));
  tone_lpf_[channel] += coeff * (sample - tone_lpf_[channel]);
  return tone_lpf_[channel];
}

// True when this block crosses the quantise line the pending action waits for.
// The grid is live while Play is on *or* the metronome is rolling the same
// clock with Play off. With neither, or with quantise off, every block is
// a boundary.
bool LooperInstance::at_boundary(uint32_t frames) const {
  const int quantize = std::clamp(quantize_.load(std::memory_order_relaxed),
                                  0, kQuantizeMax);
  const bool grid = transport_.rolling || transport_.playing;
  if (quantize == 0 || !grid) return true;

  // Start and stop wait on the beat or the bar, not on a 4- or 8-bar
  // downbeat: waiting that long to punch in is unplayable. Length is
  // what snaps to 2/4/8 bars, in snap_length().
  const double beats_per_unit =
      quantize == 1 ? 1.0 : std::max(1, transport_.numerator);
  const double block_beats =
      transport_.tempo_bpm / 60.0 * (static_cast<double>(frames) / sample_rate_);

  const double before = transport_.beats / beats_per_unit;
  const double after = (transport_.beats + block_beats) / beats_per_unit;
  return std::floor(before) != std::floor(after) || transport_.beats == 0.0;
}

void LooperInstance::apply_requests(uint32_t frames) {
  if (clear_request_.exchange(false, std::memory_order_acquire)) {
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
    stop_count_in();
    return;
  }

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
    return;
  }
  if (record == record_active_) return;

  if (record && count_in_.load(std::memory_order_relaxed)) {
    // The count is the wait: one bar of clicks, then punch in. Skipping
    // at_boundary so we do not wait a bar for the count and another for
    // the grid.
    start_count_in();
    return;
  }

  if (!at_boundary(frames)) return;

  record_active_ = record;
  if (record) {
    begin_record();
  } else {
    const uint64_t written = written_.load(std::memory_order_relaxed);
    if (stage_ == Stage::Defining && written > 0) {
      // Closing snaps to the chosen grid so a phrase that ran a little
      // long or short still lands on a number of bars you can play to.
      const uint64_t snapped = snap_length(written);
      if (snapped > written) {
        std::memset(buffer_.data() + written * 2, 0,
                    (snapped - written) * 2 * sizeof(float));
      }
      close_loop(snapped, Stage::Playing);
    } else if (stage_ == Stage::Overdubbing) {
      stage_ = Stage::Playing;
    }
  }
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

void LooperInstance::process(const float* const* inputs, float* const* outputs,
                             uint32_t frames) {
  apply_requests(frames);

  const bool play = play_request_.load(std::memory_order_relaxed);
  const float gain = gain_.load(std::memory_order_relaxed);
  const bool reverse = reverse_.load(std::memory_order_relaxed);
  const bool replace = replace_.load(std::memory_order_relaxed);
  const bool once = once_.load(std::memory_order_relaxed);
  const float feedback =
      std::clamp(feedback_.load(std::memory_order_relaxed), 0.0f, 1.0f);
  const float speed =
      std::clamp(speed_.load(std::memory_order_relaxed), 0.25f, 4.0f);
  const int width = std::min(channels_, 2);
  // The loop's own contribution to the output this block, dry input not
  // included - see loop_peak() above.
  float block_peak = 0.0f;

  for (uint32_t i = 0; i < frames; ++i) {
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
          fire_count_click(beat % bar == 0);
        }
        const int left = static_cast<int>(
            std::ceil(static_cast<double>(count_total_) - count_phase_));
        count_beats_left_.store(std::max(left, 1), std::memory_order_relaxed);
      }
    }

    switch (stage_) {
      case Stage::Defining: {
        const uint64_t written = written_.load(std::memory_order_relaxed);
        if (written < capacity_frames_) {
          buffer_[written * 2] = in[0];
          buffer_[written * 2 + 1] = in[1];
          if (written < recorded_.size() && input_loud(in[0], in[1]))
            recorded_[written] = 1;
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
        if (length > 0) {
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
          const uint64_t i1 = (i0 + 1 >= end) ? start : i0 + 1;
          const float frac = static_cast<float>(play_pos_ - static_cast<double>(i0));

          if (stage_ == Stage::Overdubbing) {
            if (i0 < recorded_.size() && input_loud(in[0], in[1]))
              recorded_[i0] = 1;
            if (i0 < layer_.size() && input_loud(in[0], in[1]))
              layer_[i0] = static_cast<uint8_t>(current_layer_);
            if (replace) {
              buffer_[i0 * 2] = in[0];
              buffer_[i0 * 2 + 1] = in[1];
            } else if (feedback >= 1.0f) {
              // Unity feedback must not scale the old layer at all: a multiply
              // by 1 every sample looks harmless until speed is below 1 and
              // the same cell is visited twice, or a 0.999999 load turns a
              // held Rec into a fade.
              buffer_[i0 * 2] += in[0];
              buffer_[i0 * 2 + 1] += in[1];
            } else {
              buffer_[i0 * 2] = buffer_[i0 * 2] * feedback + in[0];
              buffer_[i0 * 2 + 1] = buffer_[i0 * 2 + 1] * feedback + in[1];
            }
          }
          if (play) {
            const float env = envelope_at(i0, start, end);
            const float raw_l = buffer_[i0 * 2] +
                                (buffer_[i1 * 2] - buffer_[i0 * 2]) * frac;
            const float raw_r = buffer_[i0 * 2 + 1] +
                                (buffer_[i1 * 2 + 1] - buffer_[i0 * 2 + 1]) * frac;
            const float wet_l = tone_sample(0, raw_l * gain * env);
            const float wet_r = tone_sample(1, raw_r * gain * env);
            out[0] += wet_l;
            out[1] += wet_r;
            block_peak = std::max(block_peak, std::max(std::fabs(wet_l), std::fabs(wet_r)));
          }
          const float pitch = std::clamp(
              pitch_.load(std::memory_order_relaxed), -12.0f, 12.0f);
          const double rate =
              std::pow(2.0, static_cast<double>(pitch) / 12.0) *
              static_cast<double>(speed);
          play_pos_ += reverse ? -rate : rate;
          if (wrap_play_pos(start_d, end_d, reverse) && once) {
            play_request_.store(false, std::memory_order_relaxed);
            play_pos_ = reverse && end > start ? end_d - 1.0 : start_d;
          }
        }
        break;
      }

      case Stage::Empty:
      case Stage::Stopped:
        break;
    }

    if (click_remaining_ > 0 && click_length_ > 0) {
      const float envelope = static_cast<float>(click_remaining_) /
                             static_cast<float>(click_length_);
      const float tick =
          static_cast<float>(std::sin(click_phase_)) * envelope * envelope * 0.4f;
      click_phase_ += click_step_;
      --click_remaining_;
      out[0] += tick;
      out[1] += tick;
    }

    for (int ch = 0; ch < width; ++ch) outputs[ch][i] = out[ch];
  }

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
  if (len == 0) return out;
  // How far into `len` real audio reaches; buckets past this stay at 0 so
  // the untouched part of the grid reads as empty space, not a guess.
  const uint64_t recorded_span = closed_len > 0 ? len : std::min(written, len);

  // buffer_ itself is not atomic - the audio thread can still be writing
  // into it here, during Defining or Overdubbing. Reading it unguarded is
  // the accepted tradeoff for a waveform overview: worst case this draws one
  // stray peak from a torn sample, corrected on the next refresh, never a
  // crash.

  for (size_t b = 0; b < out.size(); ++b) {
    const uint64_t from = len * b / out.size();
    if (from >= recorded_span) continue;
    const uint64_t to = std::max(from + 1, len * (b + 1) / out.size());
    float peak = 0.0f;
    for (uint64_t i = from; i < to && i < recorded_span; ++i) {
      peak = std::max(peak, std::fabs(buffer_[i * 2]));
      peak = std::max(peak, std::fabs(buffer_[i * 2 + 1]));
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
  // Sized once. The caller collects these with the graph parked, so the master
  // is silent for as long as this runs: a minute of stereo tape is 23 MB, and
  // growing into it a doubling at a time would copy most of that several times
  // over before the audio comes back.
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
  if (frames > 0 && buffer_.size() >= 2) {
    const uint64_t copied = std::min(have, frames);
    if (copied > 0 && buffer_.size() >= copied * 2) {
      const auto* samples = reinterpret_cast<const uint8_t*>(buffer_.data());
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

    if (buffer_.empty())
      activate(sample_rate_ > 0.0 ? sample_rate_ : (saved_rate > 0.0 ? saved_rate
                                                                    : 48000.0),
               256);
    if (buffer_.empty() || capacity_frames_ == 0) return true;

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
    loop_beats_.store(beats, std::memory_order_relaxed);
    peels_.clear();
    clear_recorded();
    undo_ = {};
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

void LooperInstance::clear_recorded() {
  if (!recorded_.empty())
    std::fill(recorded_.begin(), recorded_.end(), static_cast<uint8_t>(0));
}

void LooperInstance::capture_undo() {
  const uint64_t length = length_.load(std::memory_order_relaxed);
  undo_.length = length;
  undo_.beats = loop_beats_.load(std::memory_order_relaxed);
  undo_.trim_start = trim_start_.load(std::memory_order_relaxed);
  undo_.trim_end = trim_end_.load(std::memory_order_relaxed);
  undo_.fade_in = fade_in_.load(std::memory_order_relaxed);
  undo_.fade_out = fade_out_.load(std::memory_order_relaxed);
  undo_.audio.assign(length * 2, 0.0f);
  if (length > 0 && buffer_.size() >= length * 2)
    std::memcpy(undo_.audio.data(), buffer_.data(), length * 2 * sizeof(float));
  undo_.valid = true;
  undo_.undone = false;
  peels_.clear();
  clear_recorded();
}

void LooperInstance::capture_undo_empty() {
  undo_.audio.clear();
  undo_.length = 0;
  undo_.beats = 0.0;
  undo_.trim_start = 0.0;
  undo_.trim_end = 1.0;
  undo_.fade_in = 0.0;
  undo_.fade_out = 0.0;
  undo_.valid = true;
  undo_.undone = false;
  peels_.clear();
  clear_recorded();
}

void LooperInstance::swap_undo() {
  if (!undo_.valid) return;

  UndoLayer current;
  current.length = length_.load(std::memory_order_relaxed);
  current.beats = loop_beats_.load(std::memory_order_relaxed);
  current.trim_start = trim_start_.load(std::memory_order_relaxed);
  current.trim_end = trim_end_.load(std::memory_order_relaxed);
  current.fade_in = fade_in_.load(std::memory_order_relaxed);
  current.fade_out = fade_out_.load(std::memory_order_relaxed);
  current.audio.assign(current.length * 2, 0.0f);
  if (current.length > 0 && buffer_.size() >= current.length * 2)
    std::memcpy(current.audio.data(), buffer_.data(),
                current.length * 2 * sizeof(float));
  current.valid = true;
  current.undone = !undo_.undone;

  const uint64_t n = std::min(undo_.length, capacity_frames_);
  if (n > 0 && buffer_.size() >= n * 2)
    std::memcpy(buffer_.data(), undo_.audio.data(), n * 2 * sizeof(float));
  length_.store(n, std::memory_order_relaxed);
  written_.store(0, std::memory_order_relaxed);
  play_pos_ = 0.0;
  loop_beats_.store(undo_.beats, std::memory_order_relaxed);
  set_trim(undo_.trim_start, undo_.trim_end);
  set_fades(undo_.fade_in, undo_.fade_out);
  stage_ = n > 0 ? Stage::Playing : Stage::Empty;
  record_active_ = false;
  record_request_.store(false, std::memory_order_relaxed);
  clear_request_.store(false, std::memory_order_relaxed);

  undo_ = std::move(current);
  peels_.clear();
  clear_recorded();
}

bool LooperInstance::last_burst(uint64_t* start, uint64_t* end) const {
  const uint64_t n = tape_frames();
  if (n == 0 || recorded_.size() < n) return false;
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

bool LooperInstance::can_undo() const {
  if (last_burst(nullptr, nullptr)) return true;
  return undo_.valid && !undo_.undone && peels_.empty();
}

bool LooperInstance::can_redo() const {
  return !peels_.empty() || (undo_.valid && undo_.undone);
}

bool LooperInstance::peel_last_burst() {
  uint64_t start = 0;
  uint64_t stop = 0;
  if (!last_burst(&start, &stop)) return false;
  if (stop <= start || buffer_.size() < stop * 2) return false;

  Peel peel;
  peel.start = start;
  peel.end = stop;
  peel.audio.assign((stop - start) * 2, 0.0f);
  std::memcpy(peel.audio.data(), buffer_.data() + start * 2,
              (stop - start) * 2 * sizeof(float));

  for (uint64_t i = start; i < stop; ++i) {
    if (i < undo_.length && undo_.audio.size() >= (i + 1) * 2) {
      buffer_[i * 2] = undo_.audio[i * 2];
      buffer_[i * 2 + 1] = undo_.audio[i * 2 + 1];
    } else {
      buffer_[i * 2] = 0.0f;
      buffer_[i * 2 + 1] = 0.0f;
    }
    if (i < recorded_.size()) recorded_[i] = 0;
  }

  // Last island of a first take: the tape is now empty against an empty
  // snapshot, so drop the loop. Leaving a silent closed take made Undo
  // look like it had done nothing.
  if (undo_.length == 0 && !last_burst(nullptr, nullptr)) {
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
    }
  }

  peels_.push_back(std::move(peel));
  return true;
}

void LooperInstance::restore_peel() {
  if (peels_.empty()) return;
  Peel peel = std::move(peels_.back());
  peels_.pop_back();
  if (peel.dropped_length > 0) {
    length_.store(peel.dropped_length, std::memory_order_relaxed);
    loop_beats_.store(peel.dropped_beats, std::memory_order_relaxed);
    if (stage_ == Stage::Empty || stage_ == Stage::Defining)
      stage_ = Stage::Playing;
  }
  const uint64_t n = peel.end - peel.start;
  if (n > 0 && buffer_.size() >= peel.end * 2 && peel.audio.size() >= n * 2) {
    std::memcpy(buffer_.data() + peel.start * 2, peel.audio.data(),
                n * 2 * sizeof(float));
  }
  for (uint64_t i = peel.start; i < peel.end && i < recorded_.size(); ++i)
    recorded_[i] = 1;
}

void LooperInstance::undo() {
  if (peel_last_burst()) return;
  if (can_undo()) swap_undo();
}

void LooperInstance::redo() {
  if (!peels_.empty()) {
    restore_peel();
    return;
  }
  if (can_redo()) swap_undo();
}

bool LooperInstance::can_multiply() const {
  const uint64_t n = length_.load(std::memory_order_relaxed);
  return n > 0 && n <= capacity_frames_ / 2;
}

void LooperInstance::multiply() {
  const uint64_t n = length_.load(std::memory_order_relaxed);
  if (n == 0 || n > capacity_frames_ / 2) return;
  if (buffer_.size() < n * 4) return;
  std::memcpy(buffer_.data() + n * 2, buffer_.data(), n * 2 * sizeof(float));
  if (layer_.size() >= n * 2)
    std::memcpy(layer_.data() + n, layer_.data(), n * sizeof(uint8_t));
  length_.store(n * 2, std::memory_order_relaxed);
  loop_beats_.store(loop_beats_.load(std::memory_order_relaxed) * 2.0,
                    std::memory_order_relaxed);
}

}  // namespace nirbija
