// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/file_player.h"

#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nirbija {
namespace {

enum Params : uint32_t { kGain = 0, kLoop = 1 };

}  // namespace

std::shared_ptr<DecodedAudio> decode_audio_file(const std::string& path,
                                                uint64_t max_frames) {
  SF_INFO info{};
  SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
  if (file == nullptr || info.frames <= 0 || info.channels <= 0) {
    if (file != nullptr) sf_close(file);
    return nullptr;
  }

  uint64_t want = static_cast<uint64_t>(info.frames);
  if (max_frames > 0) want = std::min(want, max_frames);

  auto buffer = std::make_shared<DecodedAudio>();
  buffer->sample_rate = info.samplerate;
  buffer->samples.resize(static_cast<size_t>(want) * 2);

  const size_t channels = static_cast<size_t>(info.channels);
  std::vector<float> chunk(4096 * channels);
  sf_count_t read = 0;
  uint64_t frame = 0;
  while (frame < want && (read = sf_readf_float(file, chunk.data(), 4096)) > 0) {
    const uint64_t take =
        std::min<uint64_t>(static_cast<uint64_t>(read), want - frame);
    for (uint64_t i = 0; i < take; ++i) {
      const float left = chunk[i * channels];
      const float right = channels > 1 ? chunk[i * channels + 1] : left;
      buffer->samples[(frame + i) * 2] = left;
      buffer->samples[(frame + i) * 2 + 1] = right;
    }
    frame += take;
  }
  sf_close(file);
  // A header that promised more than the file held: keep what was read.
  buffer->frames = frame;
  buffer->samples.resize(static_cast<size_t>(frame) * 2);
  if (frame == 0) return nullptr;
  return buffer;
}

PluginDescriptor FilePlayerInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.fileplayer";
  descriptor.name = "File Player";
  descriptor.vendor = "Nirbija";
  descriptor.audio_inputs = 0;
  descriptor.audio_outputs = 2;
  descriptor.has_midi_input = false;
  descriptor.category = "Player";
  descriptor.kind = PluginKind::Utility;
  return descriptor;
}

FilePlayerInstance::FilePlayerInstance() : descriptor_(make_descriptor()) {
  gain_ramp_.reset(1.0f);
  gate_.reset(0.0f);
}

FilePlayerInstance::~FilePlayerInstance() = default;

bool FilePlayerInstance::load(const std::string& path) {
  auto buffer = decode_audio_file(path);
  if (buffer == nullptr) return false;
  path_ = path;

  // The audio thread may be inside the old buffer right now, so it is retired
  // rather than freed. The order matters and is the one dsp::RetiredList
  // documents: publish first, then read the generation that may still hold
  // the old buffer, then retire it against that generation. Read the other
  // way round, a block that started between the two reads kept the old
  // pointer past the generation it was retired against.
  reclaim_retired(/*audio_running=*/true);
  std::shared_ptr<Buffer> old = std::move(owned_);
  owned_ = std::move(buffer);
  rewind_.store(true, std::memory_order_relaxed);
  live_.store(owned_.get(), std::memory_order_release);
  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  retired_.retire(std::move(old), now);
  return true;
}

void FilePlayerInstance::reclaim_retired(bool audio_running) {
  // Sem thread de audio nenhuma nao ha o que esperar: o portao conta blocos de
  // process(), e sem eles ele nunca abre. O app segue inteiro sem servidor de
  // audio, entao sem isto tudo o que fosse trocado ali ficaria ate o fim.
  if (!audio_running) forget_tail_.store(true, std::memory_order_release);
  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  retired_.reclaim(now, audio_running);
}

bool FilePlayerInstance::activate(double sample_rate, uint32_t) {
  engine_rate_ = sample_rate;
  fade_frames_ = std::max<uint32_t>(
      1, static_cast<uint32_t>(sample_rate * kFadeSeconds));
  gain_ramp_.set_length(fade_frames_);
  gate_.set_length(fade_frames_);
  gate_.reset(0.0f);
  gain_ramp_.reset(gain_.load(std::memory_order_relaxed));
  playing_buffer_ = nullptr;
  first_pass_ = true;
  return true;
}

void FilePlayerInstance::deactivate() {
  // No audio thread from here on, so nothing sounds that a fade could
  // protect, and the buffer the last block read may be freed before the next.
  playing_buffer_ = nullptr;
  gate_.reset(0.0f);
}

void FilePlayerInstance::set_transport(const TransportInfo& transport) {
  // A jump back to the start rewinds the file too, so the play button behaves
  // like a song rather than a radio. The rewind itself happens inside the
  // next block, under a crossfade, rather than here as a jump.
  if (transport.changed && transport.frame == 0)
    rewind_.store(true, std::memory_order_relaxed);
  transport_playing_ = transport.playing;
}

void FilePlayerInstance::process(const float* const* inputs,
                                 float* const* outputs, uint32_t frames) {
  render(inputs, outputs, frames);
  // Counted on the way out, so a retired buffer two generations old is one no
  // render can still be reading.
  process_generation_.fetch_add(1, std::memory_order_release);
}

void FilePlayerInstance::render(const float* const*, float* const* outputs,
                                uint32_t frames) {
  const int width = std::min(channels_, 2);
  for (int ch = 0; ch < channels_; ++ch)
    std::fill_n(outputs[ch], frames, 0.0f);

  if (forget_tail_.exchange(false, std::memory_order_acquire))
    playing_buffer_ = nullptr;

  Buffer* buffer = live_.load(std::memory_order_acquire);
  if (buffer == nullptr || buffer->frames == 0) {
    playing_buffer_ = nullptr;
    gate_.reset(0.0f);
    return;
  }

  // The transport gate: a stop is a 5 ms fade to nothing, a start a fade
  // from it. Position holds while stopped, so Play resumes where it paused.
  gate_.set_target(transport_playing_ ? 1.0f : 0.0f);
  const bool swapped = buffer != playing_buffer_;
  const bool rewind = rewind_.exchange(false, std::memory_order_relaxed);
  const bool audible = gate_.value() > 0.0f || !gate_.settled();

  // A swap or a rewind while sound is coming out crossfades what was playing
  // into the new start, inside this block. A retired buffer is alive for the
  // whole of the block that first sees its replacement and for no block
  // after, so the tail never outlives the block.
  const Buffer* tail_buffer = nullptr;
  double tail_position = 0.0;
  uint32_t tail_frames = 0;
  if (swapped || rewind) {
    if (audible && playing_buffer_ != nullptr &&
        position_ < static_cast<double>(playing_buffer_->frames)) {
      tail_buffer = playing_buffer_;
      tail_position = position_;
      tail_frames = std::min(fade_frames_, frames);
    }
    position_ = 0.0;
    first_pass_ = true;
  }
  playing_buffer_ = buffer;
  if (!audible) return;

  gain_ramp_.set_target(gain_.load(std::memory_order_relaxed));
  const bool loop = loop_.load(std::memory_order_relaxed);
  // The file keeps its own rate; the ratio stretches it to the engine's.
  const double step = buffer->sample_rate / engine_rate_;
  const double length = static_cast<double>(buffer->frames);
  const double tail_step =
      tail_buffer != nullptr ? tail_buffer->sample_rate / engine_rate_ : 0.0;

  // Loop seam: the first `xf` frames of the file fade in under the last `xf`
  // rather than following them as a step, equal power so the level holds.
  // The loop therefore repeats every length - xf frames; at 5 ms that is a
  // quarter of a percent on a two second loop and inaudible against a seam.
  const double xf =
      loop ? std::floor(std::min(length * 0.25, buffer->sample_rate * kFadeSeconds))
           : 0.0;
  const double period = length - xf;

  for (uint32_t i = 0; i < frames; ++i) {
    const float gain = gain_ramp_.next() * gate_.next();

    if (position_ >= period && loop && xf > 0.0) {
      position_ -= period;
      first_pass_ = false;
    } else if (position_ >= length) {
      if (!loop) {
        // Played out; the rest of the block stays silent, apart from a tail
        // still fading.
        if (tail_frames == 0) return;
        position_ = length;
      } else {
        position_ = std::fmod(position_, length);
        first_pass_ = false;
      }
    }

    // Weights for the loop seam, when inside it and not on the first pass.
    float head_w = 1.0f;
    float seam_w = 0.0f;
    if (!first_pass_ && xf > 0.0 && position_ < xf) {
      const auto weights =
          dsp::equal_power(static_cast<float>(position_ / xf));
      seam_w = weights.first;  // the tail, fading out
      head_w = weights.second; // the head, fading in
    }

    for (int ch = 0; ch < width; ++ch) {
      float s = 0.0f;
      if (position_ < length) {
        s = read_interpolated(*buffer, position_, ch) * head_w;
        if (seam_w > 0.0f)
          s += read_interpolated(*buffer, position_ + period, ch) * seam_w;
      }
      if (tail_frames > 0) {
        const auto weights = dsp::equal_power(
            static_cast<float>(i) / static_cast<float>(tail_frames));
        s = s * weights.second +
            read_interpolated(*tail_buffer, tail_position, ch) * weights.first;
      }
      outputs[ch][i] = s * gain;
    }
    position_ += step;
    if (tail_frames > 0) {
      tail_position += tail_step;
      if (i + 1 >= tail_frames ||
          tail_position >= static_cast<double>(tail_buffer->frames))
        tail_frames = 0;
    }
  }
}

std::vector<ParameterInfo> FilePlayerInstance::parameters() const {
  return {
      {kGain, "Gain", 0.0, 2.0, 1.0},
      {kLoop, "Loop", 0.0, 1.0, 1.0},
  };
}

double FilePlayerInstance::parameter_value(uint32_t id) const {
  switch (id) {
    case kGain: return gain_.load(std::memory_order_relaxed);
    case kLoop: return loop_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    default: return 0.0;
  }
}

void FilePlayerInstance::set_parameter(uint32_t id, double value) {
  switch (id) {
    case kGain: gain_.store(static_cast<float>(value), std::memory_order_relaxed); break;
    case kLoop: loop_.store(value >= 0.5, std::memory_order_relaxed); break;
    default: break;
  }
}

// The state is the path plus the two parameters, as text: readable in the
// session file, and versionable by adding lines.
std::vector<uint8_t> FilePlayerInstance::save_state() const {
  const std::string text = path_ + "\n" +
                           format_number(parameter_value(kGain), 6) + "\n" +
                           format_number(parameter_value(kLoop), 6);
  return std::vector<uint8_t>(text.begin(), text.end());
}

bool FilePlayerInstance::load_state(const std::vector<uint8_t>& blob) {
  const std::string text(blob.begin(), blob.end());
  const size_t first = text.find('\n');
  if (first == std::string::npos) return false;
  const size_t second = text.find('\n', first + 1);
  if (second == std::string::npos) return false;

  // A malformed number leaves that one parameter at its default rather than
  // failing the whole load: the path is the part worth rescuing.
  const std::string_view view(text);
  double gain = 1.0;
  double loop = 1.0;
  if (parse_number(view.substr(first + 1, second - first - 1), &gain))
    set_parameter(kGain, gain);
  if (parse_number(view.substr(second + 1), &loop)) set_parameter(kLoop, loop);

  const std::string saved_path = text.substr(0, first);
  // A file that has gone missing since the save leaves a silent player rather
  // than a failed session.
  if (!saved_path.empty()) load(saved_path);
  return true;
}

}  // namespace nirbija
