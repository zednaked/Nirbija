// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/fm1.h"

#include <algorithm>

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

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

}  // namespace

std::vector<uint8_t> Fm1Instance::pack_state(const std::vector<uint8_t>& project) {
  std::vector<uint8_t> blob(kMagic, kMagic + 4);
  blob.push_back(kVersion);
  if (!project.empty()) {
    blob.push_back(kSectionProject);
    put32(blob, static_cast<uint32_t>(project.size()));
    blob.insert(blob.end(), project.begin(), project.end());
  }
  return blob;
}

// A session file is bytes on disk: anything short, unknown or cut off is
// refused whole rather than handed to the device in part. Unknown sections
// (a later Nirbija's) are skipped.
bool Fm1Instance::unpack_state(const std::vector<uint8_t>& blob, std::vector<uint8_t>* project) {
  project->clear();
  if (blob.size() < 5 || !std::equal(kMagic, kMagic + 4, blob.begin()) || blob[4] < 1) return false;
  size_t at = 5;
  while (at < blob.size()) {
    if (blob.size() - at < 5) return false;
    const uint8_t tag = blob[at];
    const uint32_t length = get32(blob.data() + at + 1);
    at += 5;
    if (length > blob.size() - at) return false;
    if (tag == kSectionProject) project->assign(blob.begin() + at, blob.begin() + at + length);
    at += length;
  }
  return true;
}

std::vector<uint8_t> Fm1Instance::save_state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (project_.empty()) return {};  // never read: nothing of the device's to keep
  return pack_state(project_);
}

bool Fm1Instance::load_state(const std::vector<uint8_t>& blob) {
  if (blob.empty()) return true;
  std::vector<uint8_t> project;
  if (!unpack_state(blob, &project)) return false;
  if (project.empty()) return true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    project_ = project;
  }
  link_.put_project(std::move(project));
  return true;
}

bool Fm1Instance::take_state_dirty() {
  auto project = link_.take_project();
  if (!project) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (*project == project_) return false;
  project_ = std::move(*project);
  return true;
}

void Fm1Instance::queue_midi(const MidiEvent& event) {
  if (device_.serviced.load(std::memory_order_acquire)) device_.notes.push(event);
}

}  // namespace nirbija
