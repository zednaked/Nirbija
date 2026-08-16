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
  if (sample_rate_ == sample_rate && !buffer_.empty()) return true;
  sample_rate_ = sample_rate;
  capacity_frames_ = static_cast<uint64_t>(sample_rate * kMaxLoopSeconds);
  buffer_.assign(capacity_frames_ * 2, 0.0f);
  length_ = 0;
  written_ = 0;
  position_ = 0;
  stage_ = Stage::Empty;
  return true;
}

// True when this block crosses the quantise line the pending action waits for.
// Quantisation only means something while the transport supplies a grid; with
// it stopped, or with quantise off, every block is a boundary.
bool LooperInstance::at_boundary(uint32_t frames) const {
  const int quantize = quantize_.load(std::memory_order_relaxed);
  if (quantize == 0 || !transport_.playing) return true;

  const double beats_per_unit = quantize == 1 ? 1.0 : transport_.numerator;
  const double block_beats =
      transport_.tempo_bpm / 60.0 * (static_cast<double>(frames) / sample_rate_);

  const double before = transport_.beats / beats_per_unit;
  const double after = (transport_.beats + block_beats) / beats_per_unit;
  return std::floor(before) != std::floor(after) || transport_.beats == 0.0;
}

void LooperInstance::apply_requests(uint32_t frames) {
  if (clear_request_.exchange(false, std::memory_order_acquire)) {
    length_ = 0;
    written_ = 0;
    position_ = 0;
    stage_ = Stage::Empty;
    record_active_ = record_request_.load(std::memory_order_relaxed);
    return;
  }

  const bool record = record_request_.load(std::memory_order_acquire);
  if (record == record_active_) return;
  if (!at_boundary(frames)) return;

  record_active_ = record;
  if (record) {
    if (stage_ == Stage::Empty) {
      written_ = 0;
      stage_ = Stage::Defining;
    } else {
      position_ = 0;  // overdub layers from the top of the loop
      stage_ = Stage::Overdubbing;
    }
  } else {
    if (stage_ == Stage::Defining && written_ > 0) {
      // Closing the record pass is what defines the loop.
      length_ = written_;
      position_ = 0;
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
      case Stage::Defining:
        if (written_ < capacity_frames_) {
          buffer_[written_ * 2] = in[0];
          buffer_[written_ * 2 + 1] = in[1];
          ++written_;
        }
        break;

      case Stage::Overdubbing:
        if (length_ > 0) {
          buffer_[position_ * 2] += in[0];
          buffer_[position_ * 2 + 1] += in[1];
        }
        [[fallthrough]];

      case Stage::Playing:
        if (length_ > 0) {
          if (play) {
            out[0] += buffer_[position_ * 2] * gain;
            out[1] += buffer_[position_ * 2 + 1] * gain;
          }
          position_ = (position_ + 1) % length_;
        }
        break;

      case Stage::Empty:
      case Stage::Stopped:
        break;
    }

    for (int ch = 0; ch < width; ++ch) outputs[ch][i] = out[ch];
  }
}

std::vector<ParameterInfo> LooperInstance::parameters() const {
  return {
      {kRecord, "Record", 0.0, 1.0, 0.0},
      {kPlay, "Play", 0.0, 1.0, 1.0},
      {kClear, "Clear", 0.0, 1.0, 0.0},
      {kQuantize, "Quantize (0 off, 1 beat, 2 bar)", 0.0, 2.0, 2.0},
      {kGain, "Loop gain", 0.0, 2.0, 1.0},
  };
}

double LooperInstance::parameter_value(uint32_t id) const {
  switch (id) {
    case kRecord: return record_request_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kPlay: return play_request_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kClear: return 0.0;  // a trigger reads as resting
    case kQuantize: return quantize_.load(std::memory_order_relaxed);
    case kGain: return gain_.load(std::memory_order_relaxed);
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
      quantize_.store(static_cast<int>(std::lround(value)), std::memory_order_relaxed);
      break;
    case kGain:
      gain_.store(static_cast<float>(value), std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

std::vector<uint8_t> LooperInstance::save_state() const {
  const std::string text = std::to_string(quantize_.load(std::memory_order_relaxed)) +
                           "\n" + std::to_string(gain_.load(std::memory_order_relaxed));
  return std::vector<uint8_t>(text.begin(), text.end());
}

bool LooperInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  const size_t split = text.find('\n');
  if (split == std::string::npos) return false;

  double quantize = 0.0;
  double gain = 0.0;
  const std::string_view view(text);
  if (!parse_number(view.substr(0, split), &quantize)) return false;
  if (!parse_number(view.substr(split + 1), &gain)) return false;

  set_parameter(kQuantize, quantize);
  set_parameter(kGain, gain);
  return true;
}

}  // namespace nirbija
