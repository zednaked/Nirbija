// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <mutex>
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
  explicit Fm1Instance(Fm1Link::Timing timing);  // tests: short times
  ~Fm1Instance() override;

  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override {}
  void process(const float* const* inputs, float* const* outputs, uint32_t frames) override;
  void queue_midi(const MidiEvent& event) override;

  std::vector<ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}

  // The session keeps the FM-1's working project (design/fm1.md, "A
  // sessão"): the copy the Link last read, never read on the spot. Loading
  // gives it to the device as soon as the device can take it.
  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;
  // A newer read of the device's project arrived: the session is worth
  // writing again.
  bool take_state_dirty() override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }
  ExternalMidi* external_midi() override { return &device_; }

  Fm1Link& link() { return link_; }

  // The blob: "NFM1", a version byte, then sections (tag, length u32 LE,
  // bytes). Section 1 is the project, as the device gave it ("JNG2").
  static std::vector<uint8_t> pack_state(const std::vector<uint8_t>& project);
  static bool unpack_state(const std::vector<uint8_t>& blob, std::vector<uint8_t>* project);

 private:
  mutable std::mutex mutex_;
  std::vector<uint8_t> project_;  // what the session gets; empty until first read

  PluginDescriptor descriptor_;
  ExternalMidi device_;
  Fm1Link link_;
  int channels_ = 2;
};

}  // namespace nirbija
