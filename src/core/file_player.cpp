#include "core/file_player.h"

#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nirbija {
namespace {

enum Params : uint32_t { kGain = 0, kLoop = 1 };

}  // namespace

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

FilePlayerInstance::FilePlayerInstance() : descriptor_(make_descriptor()) {}

FilePlayerInstance::~FilePlayerInstance() = default;

bool FilePlayerInstance::load(const std::string& path) {
  SF_INFO info{};
  SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
  if (file == nullptr || info.frames <= 0 || info.channels <= 0) {
    if (file != nullptr) sf_close(file);
    return false;
  }

  auto buffer = std::make_shared<Buffer>();
  buffer->frames = static_cast<uint64_t>(info.frames);
  buffer->sample_rate = info.samplerate;
  buffer->samples.resize(static_cast<size_t>(info.frames) * 2);

  // Decoded a chunk at a time and widened to stereo on the way in, so process()
  // never has to care how many channels the file had.
  std::vector<float> chunk(4096 * static_cast<size_t>(info.channels));
  sf_count_t read = 0;
  uint64_t frame = 0;
  while ((read = sf_readf_float(file, chunk.data(), 4096)) > 0) {
    for (sf_count_t i = 0; i < read; ++i) {
      const float left = chunk[i * info.channels];
      const float right =
          info.channels > 1 ? chunk[i * info.channels + 1] : left;
      buffer->samples[(frame + i) * 2] = left;
      buffer->samples[(frame + i) * 2 + 1] = right;
    }
    frame += static_cast<uint64_t>(read);
  }
  sf_close(file);

  path_ = path;

  // The audio thread may be inside the old buffer right now, so it is retired
  // rather than freed. Position is reset before the pointer is published, or
  // one block of the new file plays at the old file's offset.
  reclaim_retired(/*audio_running=*/true);
  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  if (owned_ != nullptr) retired_.push_back({owned_, now});
  owned_ = std::move(buffer);
  rewind_.store(true, std::memory_order_relaxed);
  live_.store(owned_.get(), std::memory_order_release);
  return true;
}

void FilePlayerInstance::reclaim_retired(bool audio_running) {
  // Sem thread de audio nenhuma nao ha o que esperar: o portao conta blocos de
  // process(), e sem eles ele nunca abre. O app segue inteiro sem servidor de
  // audio, entao sem isto tudo o que fosse trocado ali ficaria ate o fim.
  if (!audio_running) {
    retired_.clear();
    return;
  }
  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  std::erase_if(retired_, [now](const auto& item) {
    return now >= item.generation + 2;
  });
}

bool FilePlayerInstance::activate(double sample_rate, uint32_t) {
  engine_rate_ = sample_rate;
  return true;
}

void FilePlayerInstance::set_transport(const TransportInfo& transport) {
  // A jump back to the start rewinds the file too, so the play button behaves
  // like a song rather than a radio.
  if (transport.changed && transport.frame == 0) position_ = 0.0;
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
  for (int ch = 0; ch < channels_; ++ch)
    std::fill_n(outputs[ch], frames, 0.0f);

  Buffer* buffer = live_.load(std::memory_order_acquire);
  if (buffer == nullptr || buffer->frames == 0 || !transport_playing_) return;
  if (rewind_.exchange(false, std::memory_order_relaxed)) position_ = 0.0;

  const float gain = gain_.load(std::memory_order_relaxed);
  const bool loop = loop_.load(std::memory_order_relaxed);
  // The file keeps its own rate; the ratio stretches it to the engine's.
  const double step = buffer->sample_rate / engine_rate_;
  const double length = static_cast<double>(buffer->frames);

  for (uint32_t i = 0; i < frames; ++i) {
    if (position_ >= length) {
      if (!loop) return;  // played out; the rest of the block stays silent
      position_ = std::fmod(position_, length);
    }

    // Linear interpolation between neighbouring frames: cheap, and clean
    // enough for playback at ordinary rate ratios.
    const uint64_t index = static_cast<uint64_t>(position_);
    const double fraction = position_ - static_cast<double>(index);
    const uint64_t next = (index + 1 < buffer->frames) ? index + 1
                          : (loop ? 0 : index);

    for (int ch = 0; ch < std::min(channels_, 2); ++ch) {
      const float a = buffer->samples[index * 2 + ch];
      const float b = buffer->samples[next * 2 + ch];
      outputs[ch][i] = static_cast<float>(a + (b - a) * fraction) * gain;
    }
    position_ += step;
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
