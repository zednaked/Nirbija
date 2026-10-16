// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/fm1.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace nirbija {

PluginDescriptor Fm1Instance::make_descriptor() {
  PluginDescriptor descriptor;
  descriptor.format = PluginFormat::Internal;
  descriptor.uid = "nirbija.fm1";
  descriptor.name = "FM-1";
  descriptor.vendor = "Nirbija";
  // In and out: the strip's input is the FM-1's USB audio, and it passes.
  descriptor.audio_inputs = 2;
  descriptor.audio_outputs = 2;
  descriptor.has_midi_input = true;
  descriptor.category = "Hardware";
  descriptor.kind = PluginKind::Instrument;
  return descriptor;
}

Fm1Instance::Fm1Instance() : Fm1Instance(Fm1Link::Timing{}) {}

Fm1Instance::Fm1Instance(Fm1Link::Timing timing)
    : descriptor_(make_descriptor()), link_(device_, timing) {
  device_.match = kFm1PortMatch;
}

Fm1Instance::~Fm1Instance() { link_.stop(); }

bool Fm1Instance::activate(double, uint32_t) {
  link_.start();  // idempotent: a reactivation keeps the conversation
  return true;
}

void Fm1Instance::process(const float* const* inputs, float* const* outputs, uint32_t frames) {
  for (int ch = 0; ch < channels_; ++ch) {
    if (outputs[ch] == nullptr) continue;
    const float* in = inputs != nullptr ? inputs[ch] : nullptr;
    if (in == nullptr) std::fill_n(outputs[ch], frames, 0.0f);
    else if (in != outputs[ch]) std::copy_n(in, frames, outputs[ch]);
  }
}

namespace {

constexpr uint8_t kMagic[4] = {'N', 'F', 'M', '1'};
constexpr uint8_t kVersion = 1;
constexpr uint8_t kSectionProject = 1;
constexpr uint8_t kSectionSounds = 2;

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

}  // namespace

std::vector<uint8_t> Fm1Instance::pack_state(const std::vector<uint8_t>& project,
                                              const std::vector<std::vector<uint8_t>>& sounds) {
  std::vector<uint8_t> blob(kMagic, kMagic + 4);
  blob.push_back(kVersion);
  if (!project.empty()) {
    blob.push_back(kSectionProject);
    put32(blob, static_cast<uint32_t>(project.size()));
    blob.insert(blob.end(), project.begin(), project.end());
  }
  if (!sounds.empty()) {
    std::vector<uint8_t> table;
    table.push_back(static_cast<uint8_t>(sounds.size() & 0xff));
    table.push_back(static_cast<uint8_t>(sounds.size() >> 8));
    for (const auto& sound : sounds) {
      put32(table, static_cast<uint32_t>(sound.size()));
      table.insert(table.end(), sound.begin(), sound.end());
    }
    blob.push_back(kSectionSounds);
    put32(blob, static_cast<uint32_t>(table.size()));
    blob.insert(blob.end(), table.begin(), table.end());
  }
  return blob;
}

// A session file is bytes on disk: anything short, unknown or cut off is
// refused whole rather than handed to the device in part. Unknown sections
// (a later Nirbija's) are skipped.
bool Fm1Instance::unpack_state(const std::vector<uint8_t>& blob, std::vector<uint8_t>* project,
                               std::vector<std::vector<uint8_t>>* sounds) {
  project->clear();
  if (sounds != nullptr) sounds->clear();
  if (blob.size() < 5 || !std::equal(kMagic, kMagic + 4, blob.begin()) || blob[4] < 1) return false;
  size_t at = 5;
  while (at < blob.size()) {
    if (blob.size() - at < 5) return false;
    const uint8_t tag = blob[at];
    const uint32_t length = get32(blob.data() + at + 1);
    at += 5;
    if (length > blob.size() - at) return false;
    if (tag == kSectionProject) project->assign(blob.begin() + at, blob.begin() + at + length);
    if (tag == kSectionSounds && sounds != nullptr) {
      const uint8_t* p = blob.data() + at;
      if (length < 2) return false;
      const size_t count = p[0] | p[1] << 8;
      size_t in = 2;
      for (size_t i = 0; i < count; ++i) {
        if (length - in < 4) return false;
        const uint32_t size = get32(p + in);
        in += 4;
        if (size > length - in) return false;
        sounds->emplace_back(p + in, p + in + size);
        in += size;
      }
      if (sounds->size() > kMaxSounds) sounds->resize(kMaxSounds);
    }
    at += length;
  }
  return true;
}

std::vector<uint8_t> Fm1Instance::save_state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (project_.empty() && sounds_.empty()) return {};  // nothing of the device's to keep
  return pack_state(project_, sounds_);
}

bool Fm1Instance::load_state(const std::vector<uint8_t>& blob) {
  if (blob.empty()) return true;
  std::vector<uint8_t> project;
  std::vector<std::vector<uint8_t>> sounds;
  if (!unpack_state(blob, &project, &sounds)) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sounds_ = sounds;
    if (!project.empty()) project_ = project;
  }
  link_.set_sound_table(std::move(sounds));
  if (!project.empty()) link_.put_project(std::move(project));
  return true;
}

int Fm1Instance::capture_sound(int track) {
  if (track < 0 || track >= kSoundTracks) return 0;
  auto reply = link_.call(96, {static_cast<uint8_t>(track)});  // TRACK_SOUND get
  if (!reply || reply->size() < 4 || (*reply)[0] != track) return 0;
  std::vector<uint8_t> sound(reply->begin() + 1, reply->end());
  std::vector<std::vector<uint8_t>> table;
  int index = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < sounds_.size(); ++i)
      if (sounds_[i] == sound) index = static_cast<int>(i) + 1;
    if (index == 0) {
      if (sounds_.size() >= kMaxSounds) return 0;
      sounds_.push_back(std::move(sound));
      index = static_cast<int>(sounds_.size());
      sounds_dirty_ = true;
    }
    table = sounds_;
  }
  link_.set_sound_table(std::move(table));
  return index;
}

std::vector<std::string> Fm1Instance::sound_labels() const {
  const Fm1Info info = link_.info();
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  for (const auto& sound : sounds_) {
    const int engine = sound.empty() ? -1 : sound[0];
    out.push_back(engine >= 0 && engine < static_cast<int>(info.engine_names.size())
                      ? info.engine_names[static_cast<size_t>(engine)]
                      : "?");
  }
  return out;
}

bool Fm1Instance::take_state_dirty() {
  bool sounds = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sounds = sounds_dirty_;
    sounds_dirty_ = false;
  }
  auto project = link_.take_project();
  if (!project) return sounds;
  read_at_.store(static_cast<int64_t>(std::time(nullptr)), std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mutex_);
  if (*project == project_) return sounds;
  project_ = std::move(*project);
  return true;
}

void Fm1Instance::send_project_again() {
  std::vector<uint8_t> project;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    project = project_;
  }
  if (!project.empty()) link_.put_project(std::move(project));
}

size_t Fm1Instance::project_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return project_.size();
}

std::vector<ParameterInfo> Fm1Instance::parameters() const {
  std::vector<ParameterInfo> out;
  // The sounds first and always, even before the device was read: a scene
  // saved with them names them whatever the device holds today.
  for (int t = 0; t < kSoundTracks; ++t) {
    ParameterInfo info;
    info.id = param_id(t, kSoundKey);
    info.name = "T" + std::to_string(t + 1) + " SOUND";
    info.min_value = 0;
    info.max_value = static_cast<double>(kMaxSounds);
    info.default_value = 0;
    info.stepped = true;
    out.push_back(std::move(info));
  }
  const auto board = link_.board();
  for (size_t t = 0; t < board.size(); ++t) {
    for (const Fm1Param& p : board[t].params) {
      ParameterInfo info;
      info.id = param_id(static_cast<int>(t), p.key);
      info.name = "T" + std::to_string(t + 1) + " " + p.name;
      info.min_value = p.min;
      info.max_value = p.max;
      info.default_value = p.def;
      // An enum (a wave, a filter type) jumps on the bar line; a level walks.
      info.stepped = !p.names.empty();
      out.push_back(std::move(info));
    }
  }
  return out;
}

double Fm1Instance::parameter_value(uint32_t id) const {
  if ((id & 0xff) == kSoundKey) return link_.wanted_sound(static_cast<uint8_t>(id >> 8));
  return link_.value(static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id & 0xff));
}

void Fm1Instance::set_parameter(uint32_t id, double value) {
  if ((id & 0xff) == kSoundKey) {
    link_.want_sound_rt(static_cast<uint8_t>(id >> 8), static_cast<int>(std::lround(value)));
    return;
  }
  link_.set_param(static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id & 0xff),
                  static_cast<int>(std::lround(value)));
}

void Fm1Instance::set_parameter_rt(uint32_t id, double value) {
  if ((id & 0xff) == kSoundKey) {
    link_.want_sound_rt(static_cast<uint8_t>(id >> 8), static_cast<int>(std::lround(value)));
    return;
  }
  link_.set_param_rt(static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id & 0xff),
                     static_cast<int>(std::lround(value)));
}

size_t Fm1Instance::take_touched(TouchedParam* out, size_t capacity) {
  for (const Fm1Link::Touch& t : link_.take_touches())
    touched_.push_back({param_id(t.track, t.key), static_cast<double>(t.value)});
  const size_t n = std::min(capacity, touched_.size());
  std::copy_n(touched_.begin(), n, out);
  touched_.erase(touched_.begin(), touched_.begin() + static_cast<std::ptrdiff_t>(n));
  return n;
}

void Fm1Instance::queue_midi(const MidiEvent& event) {
  if (device_.serviced.load(std::memory_order_acquire)) device_.notes.push(event);
}

}  // namespace nirbija
