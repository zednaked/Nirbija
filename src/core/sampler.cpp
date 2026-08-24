#include "core/sampler.h"

#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace nirbija {
namespace {

enum Params : uint32_t {
  kGain = 0,
  kRecord = 1,
  kFocus = 2,
  kQuantize = 3,
  kClear = 4,
  kOneShot = 5,
  kPitch = 6,
  kVolume = 7,
  kPan = 8,
  kNote = 9,
};

constexpr char kMagic1[] = "NJSMP01\n";
constexpr char kMagic2[] = "NJSMP02\n";

struct Seed {
  int note;
  const char* name;
};

constexpr Seed kSeed[SamplerInstance::kPads] = {
    {36, "Kick"},        {38, "Snare"},      {42, "Closed Hat"},
    {46, "Open Hat"},    {39, "Clap"},       {37, "Rim"},
    {41, "Floor Tom"},   {49, "Crash"},      {51, "Ride"},
    {47, "Mid Tom"},     {43, "Low Tom"},    {45, "High Tom"},
    {40, "E-Snare"},     {56, "Cowbell"},    {54, "Tambourine"},
    {53, "Ride Bell"},
};

int clamp_int(double value, int low, int high) {
  const int rounded = static_cast<int>(value + (value >= 0.0 ? 0.5 : -0.5));
  return std::clamp(rounded, low, high);
}

float clamp_float(double value, float low, float high) {
  return std::clamp(static_cast<float>(value), low, high);
}

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

std::shared_ptr<SamplerInstance::Buffer> decode_file(const std::string& path) {
  SF_INFO info{};
  SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
  if (file == nullptr || info.frames <= 0 || info.channels <= 0) {
    if (file != nullptr) sf_close(file);
    return nullptr;
  }

  auto buffer = std::make_shared<SamplerInstance::Buffer>();
  buffer->frames = static_cast<uint64_t>(info.frames);
  buffer->sample_rate = info.samplerate;
  buffer->samples.resize(static_cast<size_t>(info.frames) * 2);

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
  buffer->frames = frame;
  buffer->samples.resize(static_cast<size_t>(frame) * 2);
  return buffer;
}

float read_sample(const SamplerInstance::Buffer& buffer, uint64_t index,
                  int channel) {
  if (buffer.frames == 0 || index >= buffer.frames) return 0.0f;
  return buffer.samples[index * 2 + static_cast<size_t>(channel)];
}

}  // namespace

PluginDescriptor SamplerInstance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.sampler";
  descriptor.name = "Sampler";
  descriptor.vendor = "Nirbija";
  // Audio in so Rec can hear the strip; MIDI in so a sequencer above this
  // chip plays it. kind is set by hand because the port counts alone would
  // call this an effect.
  descriptor.audio_inputs = 2;
  descriptor.audio_outputs = 2;
  descriptor.has_midi_input = true;
  descriptor.category = "Sampler";
  descriptor.kind = PluginKind::Instrument;
  return descriptor;
}

SamplerInstance::SamplerInstance() : descriptor_(make_descriptor()) {
  seed_defaults();
}

SamplerInstance::~SamplerInstance() = default;

void SamplerInstance::seed_defaults() {
  for (int i = 0; i < kPads; ++i) {
    pads_[i].note.store(kSeed[i].note, std::memory_order_relaxed);
    pads_[i].one_shot.store(true, std::memory_order_relaxed);
    pads_[i].volume.store(1.0f, std::memory_order_relaxed);
    pads_[i].pan.store(0.0f, std::memory_order_relaxed);
    pads_[i].pitch.store(0.0f, std::memory_order_relaxed);
    pads_[i].start.store(0.0, std::memory_order_relaxed);
    pads_[i].end.store(1.0, std::memory_order_relaxed);
    pads_[i].name = kSeed[i].name;
    pads_[i].path.clear();
    pads_[i].live.store(nullptr, std::memory_order_relaxed);
    pads_[i].owned.reset();
  }
}

void SamplerInstance::publish(int pad, std::shared_ptr<Buffer> buffer) {
  if (pad < 0 || pad >= kPads) return;
  stop_mask_.fetch_or(1u << pad, std::memory_order_relaxed);
  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  std::erase_if(retired_, [now](const RetiredBuffer& item) {
    return now >= item.generation + 2;
  });
  Pad& target = pads_[pad];
  if (target.owned != nullptr) retired_.push_back({target.owned, now});
  target.owned = std::move(buffer);
  target.live.store(target.owned.get(), std::memory_order_release);
}

bool SamplerInstance::load(int pad, const std::string& path) {
  if (pad < 0 || pad >= kPads) return false;
  // Remember the name even if the file is missing: a session can travel
  // ahead of its samples folder, and resolve_paths fills the pad later.
  pads_[pad].path = path;
  auto buffer = decode_file(path);
  if (buffer == nullptr) return false;
  const uint64_t cap =
      rec_capacity_ > 0
          ? rec_capacity_
          : static_cast<uint64_t>(
                std::max(1.0, buffer->sample_rate) * kMaxSeconds);
  if (buffer->frames > cap) {
    buffer->frames = cap;
    buffer->samples.resize(static_cast<size_t>(cap) * 2);
  }
  publish(pad, std::move(buffer));
  return true;
}

bool SamplerInstance::commit_take() {
  const uint64_t frames = rec_ready_frames_.load(std::memory_order_acquire);
  const int pad = rec_ready_pad_.load(std::memory_order_acquire);
  if (frames == 0 || pad < 0 || pad >= kPads) return false;
  if (rec_buffer_.size() < frames * 2) return false;

  auto buffer = std::make_shared<Buffer>();
  buffer->frames = frames;
  buffer->sample_rate = engine_rate_ > 0.0 ? engine_rate_ : 48000.0;
  buffer->samples.assign(rec_buffer_.begin(),
                         rec_buffer_.begin() + static_cast<std::ptrdiff_t>(frames * 2));
  pads_[pad].path.clear();
  publish(pad, std::move(buffer));
  rec_ready_frames_.store(0, std::memory_order_release);
  rec_ready_pad_.store(-1, std::memory_order_release);
  return true;
}

void SamplerInstance::clear_pad(int pad) {
  if (pad < 0 || pad >= kPads) return;
  pads_[pad].path.clear();
  publish(pad, nullptr);
  pads_[pad].start.store(0.0, std::memory_order_relaxed);
  pads_[pad].end.store(1.0, std::memory_order_relaxed);
}

std::string SamplerInstance::pad_path(int pad) const {
  if (pad < 0 || pad >= kPads) return {};
  return pads_[pad].path;
}

void SamplerInstance::resolve_paths(std::string_view base_dir) {
  namespace fs = std::filesystem;
  const fs::path base{base_dir};
  for (int i = 0; i < kPads; ++i) {
    const std::string stored = pads_[i].path;
    if (stored.empty()) continue;
    fs::path candidate{stored};
    if (!fs::exists(candidate) && !base.empty() && candidate.is_relative()) {
      const fs::path next = base / candidate;
      if (fs::exists(next)) candidate = next;
    }
    if (!fs::exists(candidate)) continue;
    auto buffer = decode_file(candidate.string());
    if (buffer == nullptr) continue;
    const uint64_t cap =
        rec_capacity_ > 0
            ? rec_capacity_
            : static_cast<uint64_t>(
                  std::max(1.0, buffer->sample_rate) * kMaxSeconds);
    if (buffer->frames > cap) {
      buffer->frames = cap;
      buffer->samples.resize(static_cast<size_t>(cap) * 2);
    }
    // Keep the stored path as it was written, so a session that travelled
    // with its samples/ folder still saves a relative name.
    publish(i, std::move(buffer));
  }
}

void SamplerInstance::set_pad_name(int pad, std::string name) {
  if (pad < 0 || pad >= kPads) return;
  if (name.size() > 31) name.resize(31);
  pads_[pad].name = std::move(name);
}

void SamplerInstance::set_trim(int pad, double start, double end) {
  if (pad < 0 || pad >= kPads) return;
  start = std::clamp(start, 0.0, 1.0);
  end = std::clamp(end, 0.0, 1.0);
  if (end < start) std::swap(start, end);
  if (end - start < 0.001) end = std::min(1.0, start + 0.001);
  pads_[pad].start.store(start, std::memory_order_relaxed);
  pads_[pad].end.store(end, std::memory_order_relaxed);
}

void SamplerInstance::set_pad(int pad, int note, bool one_shot, float volume,
                              float pan, float pitch) {
  if (pad < 0 || pad >= kPads) return;
  pads_[pad].note.store(std::clamp(note, 0, 127), std::memory_order_relaxed);
  pads_[pad].one_shot.store(one_shot, std::memory_order_relaxed);
  pads_[pad].volume.store(std::clamp(volume, 0.0f, 2.0f),
                          std::memory_order_relaxed);
  pads_[pad].pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
  pads_[pad].pitch.store(std::clamp(pitch, -24.0f, 24.0f),
                         std::memory_order_relaxed);
}

std::string SamplerInstance::pad_name(int pad) const {
  if (pad < 0 || pad >= kPads) return {};
  return pads_[pad].name;
}

int SamplerInstance::pad_note(int pad) const {
  if (pad < 0 || pad >= kPads) return 0;
  return pads_[pad].note.load(std::memory_order_relaxed);
}

bool SamplerInstance::pad_one_shot(int pad) const {
  if (pad < 0 || pad >= kPads) return true;
  return pads_[pad].one_shot.load(std::memory_order_relaxed);
}

float SamplerInstance::pad_volume(int pad) const {
  if (pad < 0 || pad >= kPads) return 0.0f;
  return pads_[pad].volume.load(std::memory_order_relaxed);
}

float SamplerInstance::pad_pan(int pad) const {
  if (pad < 0 || pad >= kPads) return 0.0f;
  return pads_[pad].pan.load(std::memory_order_relaxed);
}

float SamplerInstance::pad_pitch(int pad) const {
  if (pad < 0 || pad >= kPads) return 0.0f;
  return pads_[pad].pitch.load(std::memory_order_relaxed);
}

double SamplerInstance::pad_start(int pad) const {
  if (pad < 0 || pad >= kPads) return 0.0;
  return pads_[pad].start.load(std::memory_order_relaxed);
}

double SamplerInstance::pad_end(int pad) const {
  if (pad < 0 || pad >= kPads) return 1.0;
  return pads_[pad].end.load(std::memory_order_relaxed);
}

bool SamplerInstance::pad_has_audio(int pad) const {
  if (pad < 0 || pad >= kPads) return false;
  const Buffer* buffer = pads_[pad].live.load(std::memory_order_acquire);
  return buffer != nullptr && buffer->frames > 0;
}

std::vector<float> SamplerInstance::waveform(int pad, int buckets) const {
  std::vector<float> peaks;
  if (pad < 0 || pad >= kPads || buckets <= 0) return peaks;
  const Buffer* buffer = pads_[pad].live.load(std::memory_order_acquire);
  if (buffer == nullptr || buffer->frames == 0) return peaks;
  peaks.assign(static_cast<size_t>(buckets), 0.0f);
  const double start = pads_[pad].start.load(std::memory_order_relaxed);
  const double end = pads_[pad].end.load(std::memory_order_relaxed);
  const uint64_t a = static_cast<uint64_t>(
      std::clamp(start, 0.0, 1.0) * static_cast<double>(buffer->frames));
  uint64_t b = static_cast<uint64_t>(
      std::clamp(end, 0.0, 1.0) * static_cast<double>(buffer->frames));
  if (b <= a) b = std::min(buffer->frames, a + 1);
  const uint64_t span = b - a;
  for (uint64_t i = 0; i < span; ++i) {
    const int bucket = static_cast<int>(i * static_cast<uint64_t>(buckets) / span);
    if (bucket < 0 || bucket >= buckets) continue;
    const float l = std::fabs(read_sample(*buffer, a + i, 0));
    const float r = std::fabs(read_sample(*buffer, a + i, 1));
    peaks[static_cast<size_t>(bucket)] =
        std::max(peaks[static_cast<size_t>(bucket)], std::max(l, r));
  }
  return peaks;
}

void SamplerInstance::preview_down(int pad, int velocity) {
  if (pad < 0 || pad >= kPads) return;
  Preview event;
  event.pad = static_cast<uint8_t>(pad);
  event.velocity = static_cast<uint8_t>(std::clamp(velocity, 1, 127));
  event.down = true;
  preview_.push(event);
}

void SamplerInstance::preview_up(int pad) {
  if (pad < 0 || pad >= kPads) return;
  Preview event;
  event.pad = static_cast<uint8_t>(pad);
  event.down = false;
  preview_.push(event);
}

bool SamplerInstance::activate(double sample_rate, uint32_t) {
  if (sample_rate <= 0.0) return false;
  engine_rate_ = sample_rate;
  rec_capacity_ = static_cast<uint64_t>(sample_rate * kMaxSeconds);
  if (rec_capacity_ < 1) rec_capacity_ = 1;
  rec_buffer_.assign(rec_capacity_ * 2, 0.0f);
  rec_written_ = 0;
  rec_waiting_ = false;
  incoming_count_ = 0;
  voices_ = {};
  recording_.store(false, std::memory_order_relaxed);
  return true;
}

void SamplerInstance::deactivate() {
  voices_ = {};
  incoming_count_ = 0;
  recording_.store(false, std::memory_order_relaxed);
  rec_waiting_ = false;
  sounding_mask_.store(0, std::memory_order_relaxed);
}

void SamplerInstance::queue_midi(const MidiEvent& event) {
  if (incoming_count_ >= kMaxEvents) return;
  incoming_[incoming_count_++] = event;
}

int SamplerInstance::pad_for_note(int note) const {
  for (int i = 0; i < kPads; ++i)
    if (pads_[i].note.load(std::memory_order_relaxed) == note) return i;
  return -1;
}

void SamplerInstance::chase_pad(int pad) {
  if (pad < 0 || pad >= kPads) return;
  voices_[static_cast<size_t>(pad)] = Voice{};
}

void SamplerInstance::start_voice(int pad, int velocity, uint32_t /*frame*/) {
  if (pad < 0 || pad >= kPads) return;
  Pad& target = pads_[pad];
  Buffer* buffer = target.live.load(std::memory_order_acquire);
  if (buffer == nullptr || buffer->frames == 0) return;

  const double start = std::clamp(target.start.load(std::memory_order_relaxed),
                                  0.0, 1.0);
  const double end =
      std::clamp(target.end.load(std::memory_order_relaxed), 0.0, 1.0);
  uint64_t start_frame =
      static_cast<uint64_t>(start * static_cast<double>(buffer->frames));
  uint64_t end_frame =
      static_cast<uint64_t>(end * static_cast<double>(buffer->frames));
  if (end_frame <= start_frame)
    end_frame = std::min(buffer->frames, start_frame + 1);

  const float volume = target.volume.load(std::memory_order_relaxed);
  const float pan = target.pan.load(std::memory_order_relaxed);
  const float pitch = target.pitch.load(std::memory_order_relaxed);
  const float master = gain_.load(std::memory_order_relaxed);
  const float vel = static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f;
  const float amp = volume * master * vel;
  const float pan_l = pan <= 0.0f ? 1.0f : 1.0f - pan;
  const float pan_r = pan >= 0.0f ? 1.0f : 1.0f + pan;

  const double rate = buffer->sample_rate > 0.0 ? buffer->sample_rate : engine_rate_;
  const double ratio = engine_rate_ > 0.0 ? rate / engine_rate_ : 1.0;
  const double step = ratio * std::pow(2.0, static_cast<double>(pitch) / 12.0);

  Voice& voice = voices_[static_cast<size_t>(pad)];
  voice.pad = pad;
  voice.buffer = buffer;
  voice.position = static_cast<double>(start_frame);
  voice.step = step > 0.0 ? step : 1.0;
  voice.end_frame = end_frame;
  voice.gain_l = amp * pan_l;
  voice.gain_r = amp * pan_r;
  voice.attack = 0;
  voice.release = kFade;
  voice.releasing = false;
}

void SamplerInstance::release_voice(int pad) {
  if (pad < 0 || pad >= kPads) return;
  Voice& voice = voices_[static_cast<size_t>(pad)];
  if (voice.pad < 0) return;
  if (pads_[pad].one_shot.load(std::memory_order_relaxed)) return;
  voice.releasing = true;
}

void SamplerInstance::handle_midi(const MidiEvent& event) {
  if (event.size < 2) return;
  const uint8_t status = event.data[0] & 0xf0;
  const int note = event.data[1];
  const int velocity = event.size >= 3 ? event.data[2] : 0;
  if (status == 0x90 && velocity > 0) {
    const int pad = pad_for_note(note);
    if (pad >= 0) start_voice(pad, velocity, event.frame);
  } else if (status == 0x80 || (status == 0x90 && velocity == 0)) {
    const int pad = pad_for_note(note);
    if (pad >= 0) release_voice(pad);
  }
}

void SamplerInstance::process(const float* const* inputs, float* const* outputs,
                              uint32_t frames) {
  for (int ch = 0; ch < channels_; ++ch) {
    if (outputs != nullptr && outputs[ch] != nullptr)
      std::fill_n(outputs[ch], frames, 0.0f);
  }

  const uint32_t stops = stop_mask_.exchange(0, std::memory_order_relaxed);
  for (int p = 0; p < kPads; ++p)
    if ((stops & (1u << p)) != 0) chase_pad(p);

  Preview preview;
  while (preview_.pop(preview)) {
    if (preview.down)
      start_voice(preview.pad, preview.velocity, 0);
    else
      release_voice(preview.pad);
  }

  const bool want_rec = rec_request_.load(std::memory_order_acquire);
  const int quantize =
      std::clamp(quantize_.load(std::memory_order_relaxed), 0, 2);
  const bool rolling = transport_.rolling || transport_.playing;

  auto boundary_frame = [&](uint32_t from) -> uint32_t {
    if (quantize <= 0 || !rolling || transport_.tempo_bpm <= 0.0 ||
        engine_rate_ <= 0.0)
      return from;
    const double unit =
        quantize == 1 ? 1.0 : static_cast<double>(std::max(1, transport_.numerator));
    const double beats_per_frame =
        (transport_.tempo_bpm / 60.0) / engine_rate_;
    if (beats_per_frame <= 0.0) return from;
    const double now = transport_.beats + static_cast<double>(from) * beats_per_frame;
    double next = std::ceil(now / unit) * unit;
    if (next <= now + 1e-9) next += unit;
    const double delta = (next - transport_.beats) / beats_per_frame;
    if (delta <= 0.0) return from;
    if (delta >= static_cast<double>(frames)) return frames;
    return static_cast<uint32_t>(delta);
  };

  if (!want_rec) {
    rec_waiting_ = false;
    if (recording_.load(std::memory_order_relaxed)) {
      const uint32_t stop_at = boundary_frame(0);
      if (stop_at == 0 || quantize <= 0 || !rolling) {
        recording_.store(false, std::memory_order_relaxed);
        rec_ready_frames_.store(rec_written_, std::memory_order_release);
        rec_ready_pad_.store(rec_pad_.load(std::memory_order_relaxed),
                             std::memory_order_release);
      }
    }
  } else if (!recording_.load(std::memory_order_relaxed)) {
    rec_waiting_ = quantize > 0 && rolling;
    if (!rec_waiting_) {
      rec_written_ = 0;
      rec_ready_frames_.store(0, std::memory_order_relaxed);
      rec_pad_.store(focused_.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
      recording_.store(true, std::memory_order_relaxed);
    }
  }

  size_t midi_i = 0;
  uint32_t sounding = 0;

  for (uint32_t i = 0; i < frames; ++i) {
    while (midi_i < incoming_count_ && incoming_[midi_i].frame <= i)
      handle_midi(incoming_[midi_i++]);

    if (want_rec && rec_waiting_ && !recording_.load(std::memory_order_relaxed)) {
      if (i >= boundary_frame(0)) {
        rec_written_ = 0;
        rec_ready_frames_.store(0, std::memory_order_relaxed);
        rec_pad_.store(focused_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        recording_.store(true, std::memory_order_relaxed);
        rec_waiting_ = false;
      }
    }

    if (!want_rec && recording_.load(std::memory_order_relaxed) &&
        quantize > 0 && rolling) {
      if (i >= boundary_frame(0)) {
        recording_.store(false, std::memory_order_relaxed);
        rec_ready_frames_.store(rec_written_, std::memory_order_release);
        rec_ready_pad_.store(rec_pad_.load(std::memory_order_relaxed),
                             std::memory_order_release);
      }
    }

    const float in_l =
        (inputs != nullptr && inputs[0] != nullptr) ? inputs[0][i] : 0.0f;
    const float in_r = (inputs != nullptr && channels_ > 1 && inputs[1] != nullptr)
                           ? inputs[1][i]
                           : in_l;

    if (recording_.load(std::memory_order_relaxed) && rec_written_ < rec_capacity_ &&
        rec_buffer_.size() >= (rec_written_ + 1) * 2) {
      rec_buffer_[rec_written_ * 2] = in_l;
      rec_buffer_[rec_written_ * 2 + 1] = in_r;
      ++rec_written_;
    }

    float out_l = recording_.load(std::memory_order_relaxed) ? in_l : 0.0f;
    float out_r = recording_.load(std::memory_order_relaxed) ? in_r : 0.0f;

    for (int p = 0; p < kPads; ++p) {
      Voice& voice = voices_[static_cast<size_t>(p)];
      if (voice.pad < 0 || voice.buffer == nullptr) continue;
      if (voice.position >= static_cast<double>(voice.end_frame) ||
          voice.position >= static_cast<double>(voice.buffer->frames)) {
        voice.releasing = true;
      }

      float env = 1.0f;
      if (voice.attack < kFade) {
        env *= static_cast<float>(voice.attack + 1) / static_cast<float>(kFade);
        ++voice.attack;
      }
      if (voice.releasing) {
        env *= static_cast<float>(std::max(0, voice.release)) /
               static_cast<float>(kFade);
        --voice.release;
        if (voice.release < 0) {
          voice = Voice{};
          continue;
        }
      }

      const uint64_t index = static_cast<uint64_t>(voice.position);
      const double fraction = voice.position - static_cast<double>(index);
      const uint64_t next =
          (index + 1 < voice.buffer->frames) ? index + 1 : index;
      const float a_l = read_sample(*voice.buffer, index, 0);
      const float b_l = read_sample(*voice.buffer, next, 0);
      const float a_r = read_sample(*voice.buffer, index, 1);
      const float b_r = read_sample(*voice.buffer, next, 1);
      const float s_l = static_cast<float>(a_l + (b_l - a_l) * fraction);
      const float s_r = static_cast<float>(a_r + (b_r - a_r) * fraction);
      out_l += s_l * voice.gain_l * env;
      out_r += s_r * voice.gain_r * env;
      voice.position += voice.step;
      sounding |= 1u << p;
    }

    if (outputs != nullptr) {
      if (channels_ <= 1) {
        if (outputs[0] != nullptr) outputs[0][i] = 0.5f * (out_l + out_r);
      } else {
        if (outputs[0] != nullptr) outputs[0][i] = out_l;
        if (outputs[1] != nullptr) outputs[1][i] = out_r;
      }
    }
  }

  while (midi_i < incoming_count_) handle_midi(incoming_[midi_i++]);
  incoming_count_ = 0;
  sounding_mask_.store(sounding, std::memory_order_relaxed);
  process_generation_.fetch_add(1, std::memory_order_release);
}

std::vector<ParameterInfo> SamplerInstance::parameters() const {
  return {
      {kGain, "Gain", 0.0, 2.0, 1.0},
      {kRecord, "Record", 0.0, 1.0, 0.0},
      {kFocus, "Pad", 1.0, 16.0, 1.0},
      {kQuantize, "Quantize", 0.0, 2.0, 0.0},
      {kClear, "Clear pad", 0.0, 1.0, 0.0},
      {kOneShot, "One-shot", 0.0, 1.0, 1.0},
      {kPitch, "Pitch", -24.0, 24.0, 0.0},
      {kVolume, "Pad volume", 0.0, 2.0, 1.0},
      {kPan, "Pad pan", -1.0, 1.0, 0.0},
      {kNote, "Pad note", 0.0, 127.0, 36.0},
  };
}

double SamplerInstance::parameter_value(uint32_t id) const {
  const int pad = focused_.load(std::memory_order_relaxed);
  switch (id) {
    case kGain: return gain_.load(std::memory_order_relaxed);
    case kRecord:
      return rec_request_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kFocus: return focused_.load(std::memory_order_relaxed) + 1;
    case kQuantize: return quantize_.load(std::memory_order_relaxed);
    case kClear: return 0.0;
    case kOneShot: return pad_one_shot(pad) ? 1.0 : 0.0;
    case kPitch: return pad_pitch(pad);
    case kVolume: return pad_volume(pad);
    case kPan: return pad_pan(pad);
    case kNote: return pad_note(pad);
    default: return 0.0;
  }
}

void SamplerInstance::apply_focused_param(uint32_t id, double value) {
  const int pad = focused_.load(std::memory_order_relaxed);
  if (pad < 0 || pad >= kPads) return;
  switch (id) {
    case kOneShot:
      pads_[pad].one_shot.store(value >= 0.5, std::memory_order_relaxed);
      break;
    case kPitch:
      pads_[pad].pitch.store(clamp_float(value, -24.0f, 24.0f),
                             std::memory_order_relaxed);
      break;
    case kVolume:
      pads_[pad].volume.store(clamp_float(value, 0.0f, 2.0f),
                              std::memory_order_relaxed);
      break;
    case kPan:
      pads_[pad].pan.store(clamp_float(value, -1.0f, 1.0f),
                           std::memory_order_relaxed);
      break;
    case kNote:
      pads_[pad].note.store(clamp_int(value, 0, 127),
                            std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

void SamplerInstance::set_parameter(uint32_t id, double value) {
  switch (id) {
    case kGain:
      gain_.store(clamp_float(value, 0.0f, 2.0f), std::memory_order_relaxed);
      break;
    case kRecord:
      rec_request_.store(value >= 0.5, std::memory_order_release);
      break;
    case kFocus:
      focused_.store(clamp_int(value, 1, kPads) - 1, std::memory_order_relaxed);
      break;
    case kQuantize:
      quantize_.store(clamp_int(value, 0, 2), std::memory_order_relaxed);
      break;
    case kClear:
      if (value >= 0.5)
        clear_pad(focused_.load(std::memory_order_relaxed));
      break;
    default:
      apply_focused_param(id, value);
      break;
  }
}

std::vector<NoteName> SamplerInstance::note_names() const {
  std::vector<NoteName> names;
  names.reserve(kPads);
  for (int i = 0; i < kPads; ++i) {
    NoteName entry;
    entry.key = pads_[i].note.load(std::memory_order_relaxed);
    entry.name = pads_[i].name;
    names.push_back(std::move(entry));
  }
  return names;
}

std::vector<uint8_t> SamplerInstance::save_state() const {
  // A take that Rec just closed is still in rec_buffer_ until commit_take
  // runs. The caller is on the UI thread with the graph parked, so we can
  // publish it before walking the pads. const_cast is the same trick the
  // looper uses when an open pass has to become a closed loop on save.
  const_cast<SamplerInstance*>(this)->commit_take();

  uint64_t total_frames = 0;
  for (int i = 0; i < kPads; ++i) {
    if (!pads_[i].path.empty()) continue;
    const Buffer* buffer = pads_[i].live.load(std::memory_order_acquire);
    if (buffer != nullptr) total_frames += buffer->frames;
  }

  std::vector<uint8_t> out;
  out.reserve(8 + 16 + static_cast<size_t>(kPads) * 96 +
              static_cast<size_t>(total_frames) * 2 * sizeof(float));
  out.insert(out.end(), kMagic2, kMagic2 + 8);
  append_pod(out, gain_.load(std::memory_order_relaxed));
  append_pod(out, focused_.load(std::memory_order_relaxed));
  append_pod(out, quantize_.load(std::memory_order_relaxed));
  const int32_t pad_count = kPads;
  append_pod(out, pad_count);

  for (int i = 0; i < kPads; ++i) {
    const Pad& pad = pads_[i];
    const Buffer* buffer = pad.live.load(std::memory_order_acquire);
    const int32_t note = pad.note.load(std::memory_order_relaxed);
    const int32_t one_shot = pad.one_shot.load(std::memory_order_relaxed) ? 1 : 0;
    const float volume = pad.volume.load(std::memory_order_relaxed);
    const float pan = pad.pan.load(std::memory_order_relaxed);
    const float pitch = pad.pitch.load(std::memory_order_relaxed);
    const double start = pad.start.load(std::memory_order_relaxed);
    const double end = pad.end.load(std::memory_order_relaxed);
    // A file pad is a path. Embedding it as well would make the session
    // carry the same audio twice, which is what Koala does not do.
    const bool file_pad = !pad.path.empty();
    const uint64_t frames =
        file_pad ? 0 : (buffer != nullptr ? buffer->frames : 0);
    const double rate = buffer != nullptr ? buffer->sample_rate : engine_rate_;
    append_pod(out, note);
    append_pod(out, one_shot);
    append_pod(out, volume);
    append_pod(out, pan);
    append_pod(out, pitch);
    append_pod(out, start);
    append_pod(out, end);
    append_pod(out, frames);
    append_pod(out, rate);
    const uint32_t name_len =
        static_cast<uint32_t>(std::min<size_t>(pad.name.size(), 31));
    append_pod(out, name_len);
    out.insert(out.end(), pad.name.begin(), pad.name.begin() + name_len);
    const uint32_t path_len =
        static_cast<uint32_t>(std::min<size_t>(pad.path.size(), 1024));
    append_pod(out, path_len);
    out.insert(out.end(), pad.path.begin(), pad.path.begin() + path_len);
    if (frames > 0 && buffer != nullptr &&
        buffer->samples.size() >= frames * 2) {
      const auto* samples =
          reinterpret_cast<const uint8_t*>(buffer->samples.data());
      out.insert(out.end(), samples, samples + frames * 2 * sizeof(float));
    }
  }
  return out;
}

bool SamplerInstance::load_state(const std::vector<uint8_t>& blob) {
  if (blob.empty()) return true;
  const bool v2 = blob.size() >= 8 && std::memcmp(blob.data(), kMagic2, 8) == 0;
  const bool v1 = blob.size() >= 8 && std::memcmp(blob.data(), kMagic1, 8) == 0;
  if (!v1 && !v2) return false;

  const uint8_t* cursor = blob.data() + 8;
  const uint8_t* end = blob.data() + blob.size();
  float gain = 1.0f;
  int focused = 0;
  int quantize = 0;
  int32_t pad_count = 0;
  if (!read_pod(cursor, end, &gain) || !read_pod(cursor, end, &focused) ||
      !read_pod(cursor, end, &quantize) || !read_pod(cursor, end, &pad_count))
    return false;

  set_parameter(kGain, gain);
  set_parameter(kFocus, focused + 1);
  set_parameter(kQuantize, quantize);

  const int n = std::min(static_cast<int>(pad_count), kPads);
  for (int i = 0; i < n; ++i) {
    int32_t note = kSeed[i].note;
    int32_t one_shot = 1;
    float volume = 1.0f, pan = 0.0f, pitch = 0.0f;
    double trim_start = 0.0, trim_end = 1.0;
    uint64_t frames = 0;
    double rate = 48000.0;
    uint32_t name_len = 0;
    if (!read_pod(cursor, end, &note) || !read_pod(cursor, end, &one_shot) ||
        !read_pod(cursor, end, &volume) || !read_pod(cursor, end, &pan) ||
        !read_pod(cursor, end, &pitch) || !read_pod(cursor, end, &trim_start) ||
        !read_pod(cursor, end, &trim_end) || !read_pod(cursor, end, &frames) ||
        !read_pod(cursor, end, &rate) || !read_pod(cursor, end, &name_len))
      break;
    if (static_cast<size_t>(end - cursor) < name_len) break;
    std::string name(reinterpret_cast<const char*>(cursor), name_len);
    cursor += name_len;
    std::string path;
    if (v2) {
      uint32_t path_len = 0;
      if (!read_pod(cursor, end, &path_len)) break;
      if (static_cast<size_t>(end - cursor) < path_len) break;
      path.assign(reinterpret_cast<const char*>(cursor), path_len);
      cursor += path_len;
    }
    set_pad(i, note, one_shot != 0, volume, pan, pitch);
    set_trim(i, trim_start, trim_end);
    set_pad_name(i, std::move(name));
    pads_[i].path = path;

    const size_t bytes = static_cast<size_t>(frames) * 2 * sizeof(float);
    if (frames > 0) {
      if (static_cast<size_t>(end - cursor) < bytes) break;
      auto buffer = std::make_shared<Buffer>();
      buffer->frames = frames;
      buffer->sample_rate = rate > 0.0 ? rate : 48000.0;
      buffer->samples.resize(static_cast<size_t>(frames) * 2);
      std::memcpy(buffer->samples.data(), cursor, bytes);
      cursor += bytes;
      publish(i, std::move(buffer));
    } else if (!path.empty()) {
      auto buffer = decode_file(path);
      if (buffer != nullptr) publish(i, std::move(buffer));
      else publish(i, nullptr);
    } else {
      publish(i, nullptr);
    }
  }
  return true;
}

}  // namespace nirbija
