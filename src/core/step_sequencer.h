#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// A step sequencer that lives in an insert slot and feeds whatever comes after
// it in the chain. Sixteen steps, one note each, snapped to the host's
// transport — put it above a synth in the same strip and the strip plays
// itself.
//
// Monophonic on purpose: one note sounds at a time and a new step cuts the one
// before it, unless a tie holds the same pitch across. That is the instrument
// this imitates, and it means the note that has to be chased off at the end
// is always exactly one.
//
// Everything the audio thread reads is an atomic scalar in a fixed array. No
// step is ever added or removed, so there is nothing here to allocate.
class StepSequencerInstance : public PluginInstance {
 public:
  StepSequencerInstance();

  static PluginDescriptor make_descriptor();

  static constexpr int kSteps = 16;

  enum Direction : int { Forward = 0, Reverse, Pendulum, Random, DirectionCount };
  enum Scale : int {
    Chromatic = 0,
    Major,
    Minor,
    Dorian,
    Mixolydian,
    PentaMinor,
    PentaMajor,
    Blues,
    ScaleCount
  };

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int) override {}
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override;

  void set_transport(const TransportInfo& transport) override {
    transport_ = transport;
  }
  // Notes arriving from earlier in the chain pass straight through, so a
  // keyboard and the sequencer can share one strip.
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

  // The step the audio thread last started, for the grid to light up.
  int playhead() const override {
    return playhead_.load(std::memory_order_relaxed);
  }

  static int snap_to_scale(int note, int scale, int root);

 private:
  static constexpr size_t kMaxEvents = 64;

  double beats_per_step() const;
  double beat_of(double index) const;
  int map_step(int index, int length);
  void emit(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2);
  void stop_sounding(uint32_t frame);
  uint32_t next_rng();      // audio thread
  uint32_t next_ui_rng();   // UI thread — pattern ops, not process()
  void rotate_steps(int delta);
  void randomize_hits();
  void randomize_notes();
  void clear_hits();
  void fill_euclidean(int pulses);

  PluginDescriptor descriptor_;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  // --- what the UI thread writes and the audio thread reads ------------------
  std::array<std::atomic<int>, kSteps> note_;
  std::array<std::atomic<int>, kSteps> velocity_;
  std::array<std::atomic<bool>, kSteps> active_;
  std::array<std::atomic<float>, kSteps> probability_;
  std::array<std::atomic<bool>, kSteps> accent_;
  std::array<std::atomic<bool>, kSteps> tie_;
  std::atomic<int> division_{2};   // index into the table in the .cpp
  std::atomic<int> length_{kSteps};
  std::atomic<double> gate_{0.5};  // fraction of a step the note is held
  std::atomic<int> transpose_{0};
  std::atomic<int> channel_{0};  // 0-based on the wire, 1-based to the user
  std::atomic<float> swing_{0.0f};
  std::atomic<int> direction_{Forward};
  std::atomic<int> scale_{Chromatic};
  std::atomic<int> root_{0};
  std::atomic<int> euclid_{0};

  // --- audio thread only -----------------------------------------------------
  int sounding_note_ = -1;    // -1 when nothing is held
  double sounding_off_ = 0.0; // song position, in beats, the note is due off
  bool last_tied_ = false;
  // The last step boundary already played, so a block that crosses none does
  // not replay the one it starts on.
  double last_step_beat_ = -1.0;
  uint32_t rng_ = 0xC0FFEEu;
  uint32_t ui_rng_ = 0xBADC0DEu;

  std::atomic<int> playhead_{-1};

  std::array<MidiEvent, kMaxEvents> events_{};
  size_t event_count_ = 0;
};

}  // namespace nirbija
