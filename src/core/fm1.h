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

  // The shown parameters of the four tracks (design/fm1.md, "Os parâmetros"):
  // id = track << 8 | the stable key, so a session or a scene names the same
  // parameter after a firmware update. The list follows the device: it is
  // empty before the first read and changes with a track's engine.
  static uint32_t param_id(int track, int key) { return static_cast<uint32_t>(track) << 8 | static_cast<uint32_t>(key); }
  // SOUND: one stepped parameter per synth track (1..3), past every Jangada
  // key: which sound of the table the track plays (1-based; 0 leaves it).
  // A scene sets it on its bar line and the Link sends that sound whole.
  static constexpr int kSoundKey = 200;
  static constexpr int kSoundTracks = 3;
  static constexpr size_t kMaxSounds = 64;
  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;
  void set_parameter_rt(uint32_t id, double value) override;
  // Knobs turned on the device itself: the player's hand, for the scenes.
  size_t take_touched(TouchedParam* out, size_t capacity) override;

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
  const Fm1Link& link() const { return link_; }

  // Takes the sound track `track` plays now on the device into the table and
  // returns its number (1-based), the one already there when it is the same;
  // 0 when the device did not answer or the table is full. UI thread: it
  // waits for the device, a few milliseconds.
  int capture_sound(int track);
  // The table, for the editor: each sound's engine name, in order.
  std::vector<std::string> sound_labels() const;

  // The editor's two buttons: read the device's project now; give it the
  // session's project again.
  void read_project_now() { link_.read_project_now(); }
  void send_project_again();
  size_t project_bytes() const;
  // When the session's copy was last read from the device (seconds since the
  // epoch), 0 for never.
  int64_t read_at() const { return read_at_.load(std::memory_order_relaxed); }

  // The blob: "NFM1", a version byte, then sections (tag, length u32 LE,
  // bytes). Section 1 is the project, as the device gave it ("JNG2").
  // Section 2 is the table of sounds: a count (u16 LE), then each sound
  // (length u32 LE, bytes).
  static std::vector<uint8_t> pack_state(const std::vector<uint8_t>& project,
                                         const std::vector<std::vector<uint8_t>>& sounds = {});
  static bool unpack_state(const std::vector<uint8_t>& blob, std::vector<uint8_t>* project,
                           std::vector<std::vector<uint8_t>>* sounds = nullptr);

 private:
  mutable std::mutex mutex_;
  std::vector<uint8_t> project_;  // what the session gets; empty until first read
  std::vector<TouchedParam> touched_;  // UI thread: device touches not taken yet
  std::vector<std::vector<uint8_t>> sounds_;  // the table, under mutex_
  bool sounds_dirty_ = false;                 // under mutex_
  std::atomic<int64_t> read_at_{0};

  PluginDescriptor descriptor_;
  ExternalMidi device_;
  Fm1Link link_;
  int channels_ = 2;
};

}  // namespace nirbija
