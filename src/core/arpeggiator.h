#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// Hold a chord above a synth and this walks it in time. The other half of the
// hole ECOSYSTEM.md counted: the step sequencer writes a line by hand, this one
// turns notes you are already playing into one.
//
// Unlike the sequencer it has an input to keep track of. The held chord is
// touched only from the audio thread - queue_midi runs there, just before
// process - so it needs no atomics and no lock, only a fixed array it never
// grows out of.
class ArpeggiatorInstance : public PluginInstance {
 public:
  ArpeggiatorInstance();

  static PluginDescriptor make_descriptor();

  // More than a hand can hold, and small enough to sort on the audio thread
  // without thinking about it.
  static constexpr size_t kMaxHeld = 16;

  enum Mode {
    Up = 0,
    Down,
    UpDown,     // turns without repeating the ends
    DownUp,
    AsPlayed,
    Random,
    Chord,      // every held note at once, in time: not an arpeggio, but the
                // same clock and the same gate, and people reach for it
    ModeCount,
  };

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int) override {}
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override;

  void set_transport(const TransportInfo& transport) override {
    transport_ = transport;
  }
  void queue_midi(const MidiEvent& event) override;
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;
  size_t take_midi_output(MidiEvent* out, size_t capacity) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  // How far along the figure it is, for a UI to light. -1 when nothing is held.
  int playhead() const override { return playhead_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kMaxEvents = 64;

  struct Held {
    uint8_t note = 0;
    uint8_t velocity = 0;
  };

  double beats_per_step() const;
  void emit(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2);
  void release_all(uint32_t frame);
  void hold(uint8_t note, uint8_t velocity);
  void drop(uint8_t note);
  // Fills `out` with the held notes in playing order and returns how many.
  size_t figure(std::array<Held, kMaxHeld>& out) const;
  void play_position(size_t position, uint32_t frame);

  PluginDescriptor descriptor_;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  // --- set by the UI thread, read by the audio thread ------------------------
  std::atomic<int> division_{2};
  std::atomic<int> mode_{Up};
  std::atomic<int> octaves_{1};
  std::atomic<double> gate_{0.5};
  std::atomic<int> transpose_{0};
  std::atomic<bool> latch_{false};
  std::atomic<bool> thru_{false};
  std::atomic<int> playhead_{-1};

  // --- audio thread only -----------------------------------------------------
  std::array<Held, kMaxHeld> held_{};
  size_t held_count_ = 0;
  // Latch keeps a chord after the keys come up; the next key pressed starts a
  // new one rather than adding to the old.
  bool keys_down_ = false;
  bool restart_on_next_ = false;

  std::array<uint8_t, kMaxHeld> sounding_{};
  size_t sounding_count_ = 0;
  double sounding_off_ = 0.0;

  size_t position_ = 0;
  double last_step_beat_ = -1.0;
  uint32_t random_state_ = 0x9e3779b9u;

  std::array<MidiEvent, kMaxEvents> events_{};
  size_t event_count_ = 0;
};

}  // namespace nirbija
