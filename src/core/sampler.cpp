// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/sampler.h"

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
  kCountIn = 10,
  kUndo = 11,
};

constexpr char kMagic1[] = "NJSMP01\n";
constexpr char kMagic2[] = "NJSMP02\n";
constexpr char kPackMagic[] = "NJSMPK1\n";
constexpr float kFadeInv = 1.0f / static_cast<float>(SamplerInstance::kFade);

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
    pads_[i].fade_in.store(0.0, std::memory_order_relaxed);
    pads_[i].fade_out.store(0.0, std::memory_order_relaxed);
    pads_[i].name = kSeed[i].name;
    pads_[i].path.clear();
    pads_[i].live.store(nullptr, std::memory_order_relaxed);
    pads_[i].owned.reset();
    pads_[i].undo_owned.reset();
    pads_[i].undo_path.clear();
    pads_[i].undo_start = 0.0;
    pads_[i].undo_end = 1.0;
    pads_[i].undo_fade_in = 0.0;
    pads_[i].undo_fade_out = 0.0;
    pads_[i].has_undo = false;
  }
}

void SamplerInstance::stash_undo(int pad) {
  if (pad < 0 || pad >= kPads) return;
  Pad& p = pads_[pad];
  p.undo_owned = p.owned;
  p.undo_path = p.path;
  p.undo_start = p.start.load(std::memory_order_relaxed);
  p.undo_end = p.end.load(std::memory_order_relaxed);
  p.undo_fade_in = p.fade_in.load(std::memory_order_relaxed);
  p.undo_fade_out = p.fade_out.load(std::memory_order_relaxed);
  p.has_undo = true;
}

void SamplerInstance::reclaim_retired(bool audio_running) {
  // Sem thread de audio nenhuma nao ha o que esperar: o portao conta blocos de
  // process(), e sem eles ele nunca abre. O app segue inteiro sem servidor de
  // audio, entao sem isto tudo o que fosse trocado ali ficaria ate o fim.
  if (!audio_running)
    reclaimed_without_audio_.store(true, std::memory_order_release);
  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  retired_.reclaim(now, audio_running);
}

void SamplerInstance::publish(int pad, std::shared_ptr<Buffer> buffer) {
  if (pad < 0 || pad >= kPads) return;
  reclaim_retired(/*audio_running=*/true);
  Pad& target = pads_[pad];
  // The order is dsp::RetiredList's: publish, then read the generation that
  // may still hold the old buffer, then retire against it. Reading the
  // generation first left a window in which a voice started on the old
  // buffer after the read and kept it past the generation it was retired
  // against. The audio thread notices the swap itself, by pointer, and
  // fades the voice out inside the block that first sees it.
  std::shared_ptr<Buffer> old = std::move(target.owned);
  target.owned = std::move(buffer);
  target.live.store(target.owned.get(), std::memory_order_release);
  const uint64_t now = process_generation_.load(std::memory_order_acquire);
  retired_.retire(std::move(old), now);
  target.version.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<SamplerInstance::Buffer> SamplerInstance::decode_for_pad(
    const std::string& path) const {
  // A pad holds kMaxSeconds at most. Before activate() the engine rate is
  // unknown, so the cap is taken at the file's own rate.
  auto buffer = decode_audio_file(path, rec_capacity_);
  if (buffer == nullptr) return nullptr;
  const uint64_t cap =
      rec_capacity_ > 0
          ? rec_capacity_
          : static_cast<uint64_t>(std::max(1.0, buffer->sample_rate) * kMaxSeconds);
  if (buffer->frames > cap) {
    buffer->frames = cap;
    buffer->samples.resize(static_cast<size_t>(cap) * 2);
  }
  return buffer;
}

float SamplerInstance::pad_position(int pad) const {
  if (pad < 0 || pad >= kPads) return -1.0f;
  return pads_[pad].position.load(std::memory_order_relaxed);
}

int SamplerInstance::pad_version(int pad) const {
  if (pad < 0 || pad >= kPads) return 0;
  return pads_[pad].version.load(std::memory_order_relaxed);
}

bool SamplerInstance::load(int pad, const std::string& path) {
  if (pad < 0 || pad >= kPads) return false;
  stash_undo(pad);
  // Remember the name even if the file is missing: a session can travel
  // ahead of its samples folder, and resolve_paths fills the pad later.
  pads_[pad].path = path;
  auto buffer = decode_for_pad(path);
  if (buffer == nullptr) return false;
  publish(pad, std::move(buffer));
  return true;
}

bool SamplerInstance::commit_take() {
  const uint64_t frames = rec_ready_frames_.load(std::memory_order_acquire);
  const int pad = rec_ready_pad_.load(std::memory_order_acquire);
  if (frames == 0 || pad < 0 || pad >= kPads) return false;
  if (rec_buffer_.size() < frames * 2) return false;

  stash_undo(pad);
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
  stash_undo(pad);
  pads_[pad].path.clear();
  publish(pad, nullptr);
  pads_[pad].start.store(0.0, std::memory_order_relaxed);
  pads_[pad].end.store(1.0, std::memory_order_relaxed);
  pads_[pad].fade_in.store(0.0, std::memory_order_relaxed);
  pads_[pad].fade_out.store(0.0, std::memory_order_relaxed);
}

bool SamplerInstance::undo_pad(int pad) {
  if (pad < 0 || pad >= kPads) return false;
  Pad& p = pads_[pad];
  if (!p.has_undo) return false;

  std::shared_ptr<Buffer> swap_owned = std::move(p.undo_owned);
  std::string swap_path = std::move(p.undo_path);
  const double swap_start = p.undo_start;
  const double swap_end = p.undo_end;
  const double swap_fade_in = p.undo_fade_in;
  const double swap_fade_out = p.undo_fade_out;

  p.undo_owned = p.owned;
  p.undo_path = p.path;
  p.undo_start = p.start.load(std::memory_order_relaxed);
  p.undo_end = p.end.load(std::memory_order_relaxed);
  p.undo_fade_in = p.fade_in.load(std::memory_order_relaxed);
  p.undo_fade_out = p.fade_out.load(std::memory_order_relaxed);

  p.path = std::move(swap_path);
  publish(pad, std::move(swap_owned));
  p.start.store(swap_start, std::memory_order_relaxed);
  p.end.store(swap_end, std::memory_order_relaxed);
  p.fade_in.store(swap_fade_in, std::memory_order_relaxed);
  p.fade_out.store(swap_fade_out, std::memory_order_relaxed);
  return true;
}

bool SamplerInstance::pad_can_undo(int pad) const {
  if (pad < 0 || pad >= kPads) return false;
  return pads_[pad].has_undo;
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
    const fs::path written{stored};

    // Where the file might be, most specific first: as written; next to the
    // file that named it; and, for a name that was absolute somewhere else,
    // the last folder and name of it next to this file - which is how a kit
    // that travelled as `samples/kick.wav` is found again after a move.
    fs::path candidates[4];
    size_t count = 0;
    candidates[count++] = written;
    if (!base.empty()) {
      if (written.is_relative()) candidates[count++] = base / written;
      const fs::path name = written.filename();
      const fs::path folder = written.parent_path().filename();
      if (!folder.empty() && !name.empty())
        candidates[count++] = base / folder / name;
      if (!name.empty()) candidates[count++] = base / name;
    }
    fs::path found;
    for (size_t c = 0; c < count; ++c) {
      std::error_code ec;
      if (fs::exists(candidates[c], ec)) {
        found = candidates[c];
        break;
      }
    }
    if (found.empty()) continue;

    auto buffer = decode_for_pad(found.string());
    if (buffer == nullptr) continue;
    // The pad remembers where the file actually was, in full. A relative
    // name kept as written only ever resolved against the folder of the
    // file it came from - and the autosave lives in the user's data folder,
    // next to no samples at all, so the next start found every pad empty.
    std::error_code ec;
    const fs::path absolute = fs::absolute(found, ec);
    pads_[i].path = ec ? found.string() : absolute.lexically_normal().string();
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

void SamplerInstance::set_fades(int pad, double fade_in, double fade_out) {
  if (pad < 0 || pad >= kPads) return;
  pads_[pad].fade_in.store(std::clamp(fade_in, 0.0, 1.0),
                           std::memory_order_relaxed);
  pads_[pad].fade_out.store(std::clamp(fade_out, 0.0, 1.0),
                            std::memory_order_relaxed);
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

void SamplerInstance::assign_note(int pad, int note) {
  if (pad < 0 || pad >= kPads) return;
  note = std::clamp(note, 0, 127);
  const int previous = pads_[pad].note.load(std::memory_order_relaxed);
  if (previous == note) return;
  const int other = pad_for_note(note);
  pads_[pad].note.store(note, std::memory_order_relaxed);
  if (other >= 0 && other != pad)
    pads_[other].note.store(previous, std::memory_order_relaxed);
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

double SamplerInstance::pad_fade_in(int pad) const {
  if (pad < 0 || pad >= kPads) return 0.0;
  return pads_[pad].fade_in.load(std::memory_order_relaxed);
}

double SamplerInstance::pad_fade_out(int pad) const {
  if (pad < 0 || pad >= kPads) return 0.0;
  return pads_[pad].fade_out.load(std::memory_order_relaxed);
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
  counting_ = false;
  count_in_left_.store(0, std::memory_order_relaxed);
  click_ = {};
  hit_flash_frames_ = {};
  hit_flash_mask_.store(0, std::memory_order_relaxed);
  ping_mask_.store(0, std::memory_order_relaxed);
  last_midi_note_.store(-1, std::memory_order_relaxed);
  last_midi_cc_.store(-1, std::memory_order_relaxed);
  return true;
}

void SamplerInstance::deactivate() {
  voices_ = {};
  incoming_count_ = 0;
  recording_.store(false, std::memory_order_relaxed);
  rec_waiting_ = false;
  sounding_mask_.store(0, std::memory_order_relaxed);
  counting_ = false;
  count_in_left_.store(0, std::memory_order_relaxed);
  click_ = {};
  hit_flash_frames_ = {};
  hit_flash_mask_.store(0, std::memory_order_relaxed);
  ping_mask_.store(0, std::memory_order_relaxed);
  last_midi_note_.store(-1, std::memory_order_relaxed);
  last_midi_cc_.store(-1, std::memory_order_relaxed);
}

void SamplerInstance::queue_midi(const MidiEvent& event) {
  if (incoming_count_ >= kMaxEvents) return;
  incoming_[incoming_count_++] = event;
}

int SamplerInstance::pad_for_note(int note) const {
  for (int i = 0; i < kPads; ++i)
    if (pads_[i].note.load(std::memory_order_relaxed) == note) return i;
  int local = -1;
  if (note >= 36 && note < 36 + kPads)
    local = note - 36;
  else if (note >= 0 && note < 36)
    local = note & (kPads - 1);
  if (local < 0) return -1;
  // The SMC-PAD's 4×4 is top row first, then the last row where the
  // second should be (and the second where the last should be). The
  // other two rows already match the editor.
  const int row = local / 4;
  const int col = local % 4;
  const int mapped = (row == 1) ? 3 : (row == 3) ? 1 : row;
  return mapped * 4 + col;
}

void SamplerInstance::shorten_release(Voice& voice, int frames) {
  // Refit the release to finish within `frames`, starting from the level it
  // is at now, so the change is a steeper slope and not a step.
  frames = std::max(1, frames);
  if (!voice.releasing) {
    voice.releasing = true;
    voice.release_left = std::min(kFade, frames);
    voice.release_inv = 1.0f / static_cast<float>(voice.release_left);
    return;
  }
  if (voice.release_left <= frames) return;
  const float level =
      static_cast<float>(std::max(0, voice.release_left)) * voice.release_inv;
  voice.release_left = frames;
  voice.release_inv = level / static_cast<float>(frames);
}

void SamplerInstance::refresh_voice_fades(Voice& voice) const {
  const Pad& pad = pads_[static_cast<size_t>(voice.pad)];
  const uint64_t span =
      voice.end_frame > voice.start_frame ? voice.end_frame - voice.start_frame : 0;
  const double fade_in_frac =
      std::clamp(pad.fade_in.load(std::memory_order_relaxed), 0.0, 1.0);
  const double fade_out_frac =
      std::clamp(pad.fade_out.load(std::memory_order_relaxed), 0.0, 1.0);
  // Each fade is capped at half the trim window, the same as the looper's,
  // so a short pad with both turned up crossfades through the middle
  // instead of one swallowing the other's tail.
  voice.fade_in_frames =
      static_cast<uint64_t>(fade_in_frac * static_cast<double>(span) / 2.0);
  voice.fade_out_frames =
      static_cast<uint64_t>(fade_out_frac * static_cast<double>(span) / 2.0);
  voice.fade_in_inv = voice.fade_in_frames > 0
                          ? 1.0f / static_cast<float>(voice.fade_in_frames)
                          : 0.0f;
  voice.fade_out_inv = voice.fade_out_frames > 0
                           ? 1.0f / static_cast<float>(voice.fade_out_frames)
                           : 0.0f;
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

  // A retrigger does not restart the sounding voice in place - that cut the
  // wave wherever it was. It goes to its short release and the new hit takes
  // another slot, the two overlapping for kFade frames.
  Voice* slot = nullptr;
  Voice* oldest = nullptr;
  Voice* shortest = nullptr;
  for (Voice& voice : voices_) {
    if (voice.pad < 0) {
      if (slot == nullptr) slot = &voice;
      continue;
    }
    if (voice.pad == pad && !voice.releasing) shorten_release(voice, kFade);
    if (oldest == nullptr || voice.serial < oldest->serial) oldest = &voice;
    if (voice.releasing &&
        (shortest == nullptr || voice.release_left < shortest->release_left))
      shortest = &voice;
  }
  // Pool full: steal the release nearest its end, else the oldest voice.
  if (slot == nullptr) slot = shortest != nullptr ? shortest : oldest;
  if (slot == nullptr) return;

  Voice& voice = *slot;
  voice.pad = pad;
  voice.buffer = buffer;
  voice.position = static_cast<double>(start_frame);
  voice.step = step > 0.0 ? step : 1.0;
  voice.start_frame = start_frame;
  voice.end_frame = end_frame;
  voice.gain_l = amp * pan_l;
  voice.gain_r = amp * pan_r;
  voice.attack = 0;
  voice.release_left = kFade;
  voice.release_inv = kFadeInv;
  voice.releasing = false;
  voice.serial = ++voice_serial_;
  refresh_voice_fades(voice);
}

void SamplerInstance::release_voice(int pad) {
  if (pad < 0 || pad >= kPads) return;
  if (pads_[pad].one_shot.load(std::memory_order_relaxed)) return;
  for (Voice& voice : voices_)
    if (voice.pad == pad && !voice.releasing) shorten_release(voice, kFade);
}

void SamplerInstance::fire_count_click(bool downbeat) {
  if (engine_rate_ <= 0.0) return;
  click_.start(engine_rate_, downbeat);
}

void SamplerInstance::flash_pad(int pad) {
  if (pad < 0 || pad >= kPads) return;
  // Long enough to survive a couple of UI polls and still read as a
  // hit, short enough not to look latched.
  hit_flash_frames_[static_cast<size_t>(pad)] =
      static_cast<int>(engine_rate_ * 0.35);
}

void SamplerInstance::hear_note(int note, int velocity, bool down) {
  last_midi_note_.store(note, std::memory_order_relaxed);
  last_midi_cc_.store(-1, std::memory_order_relaxed);
  if (!down || velocity <= 0) return;
  const int pad = pad_for_note(note);
  if (pad < 0) return;
  ping_mask_.fetch_or(1u << pad, std::memory_order_relaxed);
  focused_.store(pad, std::memory_order_relaxed);
}

void SamplerInstance::hear_cc(int cc) {
  last_midi_cc_.store(cc, std::memory_order_relaxed);
  last_midi_note_.store(-1, std::memory_order_relaxed);
}

void SamplerInstance::handle_midi(const MidiEvent& event) {
  if (event.size < 2) return;
  const uint8_t status = event.data[0] & 0xf0;
  const int note = event.data[1];
  const int velocity = event.size >= 3 ? event.data[2] : 0;
  if (status == 0x90 && velocity > 0) {
    last_midi_note_.store(note, std::memory_order_relaxed);
    last_midi_cc_.store(-1, std::memory_order_relaxed);
    const int pad = pad_for_note(note);
    if (pad >= 0) {
      start_voice(pad, velocity, event.frame);
      flash_pad(pad);
      // Last pad hit is the one Rec and the knobs talk about, so a
      // controller punch selects the pad the same way tapping the
      // editor does.
      focused_.store(pad, std::memory_order_relaxed);
    }
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

  // A pad whose audio changed hands since the voice started: the voice fades
  // out on the buffer it holds, which is alive for this block and no longer,
  // so the fade must finish inside it. With no audio thread the host may
  // already have freed that buffer; then the voice is dropped outright -
  // nothing was sounding to click.
  const bool dropped =
      reclaimed_without_audio_.exchange(false, std::memory_order_acquire);
  for (Voice& voice : voices_) {
    if (voice.pad < 0) continue;
    Buffer* live = pads_[static_cast<size_t>(voice.pad)].live.load(
        std::memory_order_acquire);
    if (voice.buffer != live) {
      if (dropped)
        voice = Voice{};
      else
        shorten_release(voice, static_cast<int>(frames));
    }
  }

  const uint32_t pings = ping_mask_.exchange(0, std::memory_order_relaxed);
  for (int p = 0; p < kPads; ++p)
    if ((pings & (1u << p)) != 0) flash_pad(p);

  Preview preview;
  while (preview_.pop(preview)) {
    if (preview.down) {
      start_voice(preview.pad, preview.velocity, 0);
      flash_pad(preview.pad);
    } else {
      release_voice(preview.pad);
    }
  }

  const bool want_rec = rec_request_.load(std::memory_order_acquire);
  const int quantize =
      std::clamp(quantize_.load(std::memory_order_relaxed), 0, 2);
  const bool rolling = transport_.rolling || transport_.playing;

  // Count-in: dropping Rec, or turning Count-in off mid-count, cancels the
  // whole pending take rather than just silencing the click — a count that
  // finishes into a Rec nobody asked for anymore would be worse than none.
  if (counting_) {
    if (!want_rec || !count_in_.load(std::memory_order_relaxed)) {
      counting_ = false;
      count_in_left_.store(0, std::memory_order_relaxed);
    }
  } else if (want_rec && !recording_.load(std::memory_order_relaxed) &&
             count_in_.load(std::memory_order_relaxed)) {
    counting_ = true;
    count_phase_ = 0.0;
    count_total_ = std::max(1, transport_.numerator);
    count_in_left_.store(count_total_, std::memory_order_relaxed);
    fire_count_click(true);
  }

  // The frame inside this block where the quantize grid next falls, on the
  // same arithmetic the looper uses, so the two punch on the same sample.
  auto boundary_frame = [&](uint32_t from) -> uint32_t {
    if (quantize <= 0 || !rolling) return from;
    const double unit =
        quantize == 1 ? 1.0 : static_cast<double>(std::max(1, transport_.numerator));
    return dsp::boundary_frame(transport_.beats, transport_.tempo_bpm,
                               engine_rate_, unit, from, frames);
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
  } else if (!recording_.load(std::memory_order_relaxed) && !counting_) {
    rec_waiting_ = quantize > 0 && rolling;
    if (!rec_waiting_) {
      rec_written_ = 0;
      rec_ready_frames_.store(0, std::memory_order_relaxed);
      rec_pad_.store(focused_.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
      recording_.store(true, std::memory_order_relaxed);
    }
  }

  // The pad fades are read once a block per voice, not once a sample.
  for (Voice& voice : voices_)
    if (voice.pad >= 0) refresh_voice_fades(voice);

  size_t midi_i = 0;
  uint32_t sounding = 0;
  float block_peak = 0.0f;

  for (uint32_t i = 0; i < frames; ++i) {
    while (midi_i < incoming_count_ && incoming_[midi_i].frame <= i)
      handle_midi(incoming_[midi_i++]);

    if (counting_) {
      const double tempo =
          transport_.tempo_bpm > 0.0 ? transport_.tempo_bpm : 120.0;
      const double beats_per_frame = tempo / 60.0 / std::max(engine_rate_, 1.0);
      const double before = count_phase_;
      count_phase_ += beats_per_frame;
      if (count_phase_ >= static_cast<double>(count_total_)) {
        // The count is the wait: skip the quantize grid so Rec does not
        // wait a bar for the count and another for the boundary.
        counting_ = false;
        count_in_left_.store(0, std::memory_order_relaxed);
        rec_written_ = 0;
        rec_ready_frames_.store(0, std::memory_order_relaxed);
        rec_pad_.store(focused_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        recording_.store(true, std::memory_order_relaxed);
        rec_waiting_ = false;
      } else if (std::floor(before) != std::floor(count_phase_)) {
        const int beat = static_cast<int>(std::floor(count_phase_));
        const int bar = std::max(1, transport_.numerator);
        fire_count_click(beat % bar == 0);
        const int left = static_cast<int>(
            std::ceil(static_cast<double>(count_total_) - count_phase_));
        count_in_left_.store(std::max(left, 1), std::memory_order_relaxed);
      }
    }

    if (want_rec && rec_waiting_ && !recording_.load(std::memory_order_relaxed) &&
        !counting_) {
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

    for (Voice& voice : voices_) {
      if (voice.pad < 0 || voice.buffer == nullptr) continue;
      // The trim end and the buffer end both start the release; the read
      // past them returns silence, so the release covers the cut.
      if (!voice.releasing &&
          (voice.position >= static_cast<double>(voice.end_frame) ||
           voice.position >= static_cast<double>(voice.buffer->frames)))
        shorten_release(voice, kFade);

      float env = 1.0f;
      if (voice.attack < kFade) {
        env *= static_cast<float>(voice.attack + 1) * kFadeInv;
        ++voice.attack;
      }
      if (voice.releasing) {
        env *= static_cast<float>(std::max(0, voice.release_left)) *
               voice.release_inv;
        --voice.release_left;
        if (voice.release_left < 0) {
          voice = Voice{};
          continue;
        }
      }

      const uint64_t index = static_cast<uint64_t>(voice.position);
      const float s_l = read_interpolated(*voice.buffer, voice.position, 0);
      const float s_r = read_interpolated(*voice.buffer, voice.position, 1);
      if (voice.fade_in_frames > 0 && index >= voice.start_frame) {
        const uint64_t into = index - voice.start_frame;
        if (into < voice.fade_in_frames)
          env *= static_cast<float>(into) * voice.fade_in_inv;
      }
      if (voice.fade_out_frames > 0 && index < voice.end_frame) {
        const uint64_t remaining = voice.end_frame - index;
        if (remaining < voice.fade_out_frames)
          env *= static_cast<float>(remaining) * voice.fade_out_inv;
      }
      out_l += s_l * voice.gain_l * env;
      out_r += s_r * voice.gain_r * env;
      voice.position += voice.step;
      sounding |= 1u << voice.pad;
    }

    // One sample of the count-in click, on the exact frame the beat fell.
    click_.render(&out_l, &out_r, 1, engine_rate_);

    block_peak = std::max(block_peak, std::max(std::fabs(out_l), std::fabs(out_r)));

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
  level_.store(block_peak, std::memory_order_relaxed);

  // Mirrors for the editor, once a block: where each pad's newest voice is
  // in its audio, whether Rec is still waiting for the grid, how full the
  // take is.
  std::array<float, kPads> at;
  std::array<uint64_t, kPads> at_serial{};
  at.fill(-1.0f);
  for (const Voice& voice : voices_) {
    if (voice.pad < 0 || voice.buffer == nullptr || voice.buffer->frames == 0)
      continue;
    const size_t p = static_cast<size_t>(voice.pad);
    if (at[p] >= 0.0f && voice.serial < at_serial[p]) continue;
    at_serial[p] = voice.serial;
    at[p] = static_cast<float>(std::clamp(
        voice.position / static_cast<double>(voice.buffer->frames), 0.0, 1.0));
  }
  for (int p = 0; p < kPads; ++p)
    pads_[static_cast<size_t>(p)].position.store(at[static_cast<size_t>(p)],
                                                 std::memory_order_relaxed);
  armed_.store(want_rec && rec_waiting_ &&
                   !recording_.load(std::memory_order_relaxed) && !counting_,
               std::memory_order_relaxed);
  rec_fill_.store(
      recording_.load(std::memory_order_relaxed) && rec_capacity_ > 0
          ? static_cast<float>(static_cast<double>(rec_written_) /
                               static_cast<double>(rec_capacity_))
          : 0.0f,
      std::memory_order_relaxed);

  uint32_t hit_flash = 0;
  for (int p = 0; p < kPads; ++p) {
    int& left = hit_flash_frames_[static_cast<size_t>(p)];
    if (left <= 0) continue;
    left -= static_cast<int>(frames);
    if (left > 0) hit_flash |= 1u << p;
  }
  hit_flash_mask_.store(hit_flash, std::memory_order_relaxed);

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
      {kCountIn, "Count-in", 0.0, 1.0, 0.0},
      {kUndo, "Undo pad", 0.0, 1.0, 0.0},
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
    case kCountIn: return count_in_.load(std::memory_order_relaxed) ? 1.0 : 0.0;
    case kUndo: return 0.0;
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
    case kCountIn: {
      const bool on = value >= 0.5;
      count_in_.store(on, std::memory_order_relaxed);
      // Turning the toggle off mid-count aborts the pending take rather
      // than leaving Rec armed for a count that no longer runs.
      if (!on && count_in_left_.load(std::memory_order_relaxed) > 0)
        rec_request_.store(false, std::memory_order_release);
      break;
    }
    case kUndo:
      if (value >= 0.5) undo_pad(focused_.load(std::memory_order_relaxed));
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

void SamplerInstance::write_pads_to(std::vector<uint8_t>& out) const {
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
  // Trailing and optional, the same append-only trick as Count-in: a reader
  // that predates fades just sees extra bytes it does not look for. One
  // pair per pad, in pad order, since pad_count above is always kPads on
  // anything this engine writes.
  for (int i = 0; i < kPads; ++i) {
    append_pod(out, pads_[i].fade_in.load(std::memory_order_relaxed));
    append_pod(out, pads_[i].fade_out.load(std::memory_order_relaxed));
  }
}

bool SamplerInstance::read_pads_from(const uint8_t*& cursor, const uint8_t* end,
                                     int pad_count, bool has_path_field) {
  const int n = std::min(pad_count, kPads);
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
    if (has_path_field) {
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
      auto loaded = decode_for_pad(path);
      if (loaded != nullptr) publish(i, std::move(loaded));
      else publish(i, nullptr);
    } else {
      publish(i, nullptr);
    }
  }
  // Trailing and optional: older blobs simply end before the fades, and
  // those pads stay at no fade rather than aborting the whole load.
  for (int i = 0; i < n; ++i) {
    double fade_in = 0.0, fade_out = 0.0;
    if (!read_pod(cursor, end, &fade_in) || !read_pod(cursor, end, &fade_out))
      break;
    set_fades(i, fade_in, fade_out);
  }
  return true;
}

std::vector<uint8_t> SamplerInstance::save_pads() const {
  // Rec takes waiting to be committed are pads too, the same as save_state.
  const_cast<SamplerInstance*>(this)->commit_take();
  std::vector<uint8_t> out;
  out.insert(out.end(), kPackMagic, kPackMagic + 8);
  write_pads_to(out);
  return out;
}

bool SamplerInstance::load_pads(const std::vector<uint8_t>& blob) {
  if (blob.size() < 8 || std::memcmp(blob.data(), kPackMagic, 8) != 0)
    return false;
  const uint8_t* cursor = blob.data() + 8;
  const uint8_t* end = blob.data() + blob.size();
  int32_t pad_count = 0;
  if (!read_pod(cursor, end, &pad_count)) return false;
  return read_pads_from(cursor, end, pad_count, /*has_path_field=*/true);
}

std::vector<uint8_t> SamplerInstance::save_state() const {
  // A take that Rec just closed is still in rec_buffer_ until commit_take
  // runs. The caller is on the UI thread and, whenever a take is waiting,
  // has parked the graph first (save_needs_quiet() says so), so we can
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
  write_pads_to(out);
  // Trailing and optional, the way the looper's own count-in byte is: older
  // blobs simply end before it, and load_state below just leaves the toggle
  // at its default when the bytes are not there.
  const int32_t count_in = count_in_.load(std::memory_order_relaxed) ? 1 : 0;
  append_pod(out, count_in);
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

  read_pads_from(cursor, end, pad_count, /*has_path_field=*/v2);

  // Trailing and optional: a blob saved before Count-in existed simply ends
  // here, and the toggle stays at its default of off.
  int32_t count_in = 0;
  if (read_pod(cursor, end, &count_in)) set_parameter(kCountIn, count_in);
  return true;
}

}  // namespace nirbija
