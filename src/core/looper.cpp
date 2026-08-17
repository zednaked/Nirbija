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
};

// The longest loop kept: a minute of stereo. Sized once, up front, because the
// audio thread must never allocate mid-take.
constexpr double kMaxLoopSeconds = 60.0;

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
  play_pos_ = 0.0;
  tone_lpf_[0] = tone_lpf_[1] = 0.0f;
  undo_ = {};

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
    // A trim or fade shaped for the phrase that just got thrown away means
    // nothing to whatever gets recorded next.
    trim_start_.store(0.0, std::memory_order_relaxed);
    trim_end_.store(1.0, std::memory_order_relaxed);
    fade_in_.store(0.0, std::memory_order_relaxed);
    fade_out_.store(0.0, std::memory_order_relaxed);
    record_active_ = record_request_.load(std::memory_order_relaxed);
    return;
  }

  const bool record = record_request_.load(std::memory_order_acquire);
  if (record == record_active_) return;
  if (!at_boundary(frames)) return;

  record_active_ = record;
  if (record) {
    if (stage_ == Stage::Empty) {
      written_.store(0, std::memory_order_relaxed);
      stage_ = Stage::Defining;
    } else {
      // Overdub from the start of the audible window, not the raw buffer:
      // a trimmed loop's "top" is the trim start.
      const uint64_t length = length_.load(std::memory_order_relaxed);
      uint64_t start = trim_start_frames(length);
      uint64_t end = trim_end_frames(length, start);
      if (end <= start || start >= length) {
        start = 0;
        end = length;
      }
      play_pos_ = static_cast<double>(start);
      stage_ = Stage::Overdubbing;
    }
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
      length_.store(snapped, std::memory_order_relaxed);
      play_pos_ = 0.0;
      const double unit = unit_beats();
      if (unit > 0.0 && transport_.tempo_bpm > 0.0 && sample_rate_ > 0.0) {
        loop_beats_.store(static_cast<double>(snapped) / sample_rate_ *
                              transport_.tempo_bpm / 60.0,
                          std::memory_order_relaxed);
      } else {
        loop_beats_.store(0.0, std::memory_order_relaxed);
      }
      stage_ = Stage::Playing;
    } else if (stage_ == Stage::Overdubbing) {
      stage_ = Stage::Playing;
    }
  }
}

void LooperInstance::process(const float* const* inputs, float* const* outputs,
                             uint32_t frames) {
  apply_requests(frames);

  const bool play = play_request_.load(std::memory_order_relaxed);
  const float gain = gain_.load(std::memory_order_relaxed);
  const int width = std::min(channels_, 2);

  for (uint32_t i = 0; i < frames; ++i) {
    float in[2] = {0.0f, 0.0f};
    for (int ch = 0; ch < width; ++ch) in[ch] = inputs[ch][i];
    if (width == 1) in[1] = in[0];

    // The live signal always passes through: a looper that silences the
    // instrument while recording is unplayable.
    float out[2] = {in[0], in[1]};

    switch (stage_) {
      case Stage::Defining: {
        const uint64_t written = written_.load(std::memory_order_relaxed);
        if (written < capacity_frames_) {
          buffer_[written * 2] = in[0];
          buffer_[written * 2 + 1] = in[1];
          written_.store(written + 1, std::memory_order_relaxed);
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
          if (play_pos_ < static_cast<double>(start) ||
              play_pos_ >= static_cast<double>(end))
            play_pos_ = static_cast<double>(start);
          if (play_pos_ >= static_cast<double>(length)) break;

          const uint64_t i0 = static_cast<uint64_t>(play_pos_);
          const uint64_t i1 = (i0 + 1 >= end) ? start : i0 + 1;
          const float frac = static_cast<float>(play_pos_ - static_cast<double>(i0));

          if (stage_ == Stage::Overdubbing) {
            buffer_[i0 * 2] += in[0];
            buffer_[i0 * 2 + 1] += in[1];
          }
          if (play) {
            const float env = envelope_at(i0, start, end);
            const float raw_l = buffer_[i0 * 2] +
                                (buffer_[i1 * 2] - buffer_[i0 * 2]) * frac;
            const float raw_r = buffer_[i0 * 2 + 1] +
                                (buffer_[i1 * 2 + 1] - buffer_[i0 * 2 + 1]) * frac;
            out[0] += tone_sample(0, raw_l * gain * env);
            out[1] += tone_sample(1, raw_r * gain * env);
          }
          const float pitch = std::clamp(
              pitch_.load(std::memory_order_relaxed), -12.0f, 12.0f);
          const double rate = std::pow(2.0, static_cast<double>(pitch) / 12.0);
          play_pos_ += rate;
          if (play_pos_ >= static_cast<double>(end)) {
            const double span = static_cast<double>(end - start);
            if (span > 0.0)
              play_pos_ = static_cast<double>(start) +
                          std::fmod(play_pos_ - static_cast<double>(start), span);
            else
              play_pos_ = static_cast<double>(start);
          }
        }
        break;
      }

      case Stage::Empty:
      case Stage::Stopped:
        break;
    }

    for (int ch = 0; ch < width; ++ch) outputs[ch][i] = out[ch];
  }

  const uint64_t length = length_.load(std::memory_order_relaxed);
  const uint64_t written = written_.load(std::memory_order_relaxed);
  // While the first pass is still open the playhead sits at the write head,
  // so the editor can show something moving before the loop exists.
  position_fraction_.store(
      length > 0 ? play_pos_ / static_cast<double>(length)
                 : (written > 0 ? 1.0 : -1.0),
      std::memory_order_relaxed);
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
  uint64_t len = length_.load(std::memory_order_relaxed);
  // The first pass has no closed length yet; draw what has been written so
  // the editor is not blank for the whole take.
  if (len == 0) len = written_.load(std::memory_order_relaxed);
  if (len == 0) return out;

  // buffer_ itself is not atomic - the audio thread can still be writing
  // into it here, during Defining or Overdubbing. Reading it unguarded is
  // the accepted tradeoff for a waveform overview: worst case this draws one
  // stray peak from a torn sample, corrected on the next refresh, never a
  // crash.

  for (size_t b = 0; b < out.size(); ++b) {
    const uint64_t from = len * b / out.size();
    const uint64_t to = std::max(from + 1, len * (b + 1) / out.size());
    float peak = 0.0f;
    for (uint64_t i = from; i < to && i < len; ++i) {
      peak = std::max(peak, std::fabs(buffer_[i * 2]));
      peak = std::max(peak, std::fabs(buffer_[i * 2 + 1]));
    }
    out[b] = peak;
  }
  return out;
}

std::vector<ParameterInfo> LooperInstance::parameters() const {
  return {
      {kRecord, "Record", 0.0, 1.0, 0.0},
      {kPlay, "Play", 0.0, 1.0, 1.0},
      {kClear, "Clear", 0.0, 1.0, 0.0},
      {kQuantize, "Quantize (0 off, 1 beat, 2 1bar, 3 2, 4 4, 5 8)", 0.0,
       static_cast<double>(kQuantizeMax), 2.0},
      {kGain, "Loop gain", 0.0, 2.0, 1.0},
      {kPitch, "Pitch (semitones)", -12.0, 12.0, 0.0},
      {kTone, "Tone", 0.0, 1.0, 1.0},
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
    default:
      break;
  }
}

namespace {

constexpr char kLoopMagic[] = "NLOOP1\n";

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
  out.insert(out.end(), kLoopMagic, kLoopMagic + 7);
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
  return out;
}

bool LooperInstance::load_state(const std::vector<uint8_t>& blob) {
  if (blob.size() >= 7 &&
      std::memcmp(blob.data(), kLoopMagic, 7) == 0) {
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

    set_parameter(kQuantize, quantize);
    set_parameter(kGain, gain);
    set_trim(trim_start, trim_end);
    set_fades(fade_in, fade_out);
    set_parameter(kPitch, pitch);
    set_parameter(kTone, tone);

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
}

void LooperInstance::undo() {
  if (can_undo()) swap_undo();
}

void LooperInstance::redo() {
  if (can_redo()) swap_undo();
}

}  // namespace nirbija
