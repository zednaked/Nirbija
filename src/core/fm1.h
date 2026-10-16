// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <string>
#include <vector>

#include "core/external_midi.h"
#include "core/fm1_link.h"
#include "core/plugin.h"

namespace nirbija {

// What the FM-1's USB ports are called with Jangada (its USB product name).
inline constexpr char kFm1PortMatch[] = "Jangada";

// The FM-1 with Jangada as an instrument of the mixer (design/fm1.md). The
// sound is made in the box: the strip's input is its USB audio, and this
// passes it through. What the strip's MIDI plays goes to the FM-1 as it
// comes, channel n to its track n. The engine's ports and the Link carry the
// rest; the mixer attaches this instance's device (external_midi()) to the
// engine while it sits in a chain.
class Fm1Instance : public PluginInstance {
 public:
  static PluginDescriptor make_descriptor();

  Fm1Instance();
  ~Fm1Instance() override;

  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override {}
  void process(const float* const* inputs, float* const* outputs, uint32_t frames) override;
  void queue_midi(const MidiEvent& event) override;

  std::vector<ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}

  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }

  const PluginDescriptor& descriptor() const override { return descriptor_; }
  ExternalMidi* external_midi() override { return &device_; }

  Fm1Link& link() { return link_; }

 private:
  PluginDescriptor descriptor_;
  ExternalMidi device_;
  Fm1Link link_{device_};
  int channels_ = 2;
};

}  // namespace nirbija
