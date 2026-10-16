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

Fm1Instance::Fm1Instance() : descriptor_(make_descriptor()) { device_.match = kFm1PortMatch; }

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

void Fm1Instance::queue_midi(const MidiEvent& event) {
  if (device_.serviced.load(std::memory_order_acquire)) device_.notes.push(event);
}

}  // namespace nirbija
