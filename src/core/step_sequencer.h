#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// A step sequencer that lives in an insert slot and feeds whatever comes after
// it in the chain. Eight lanes of sixty-four steps, sixteen patterns, snapped
// to the host's transport — put it above a synth in the same strip and the
// strip plays itself.
//
// Each native head is monophonic — a new step cuts the one before it, unless
// a tie holds the same pitch — so eight notes can sound at once, one per lane.
//
// Everything the audio thread reads is an atomic scalar in a fixed array. No
// step is ever added or removed, so there is nothing here to allocate.
//
// Record turns queued input into the pattern instead of merely passing it
// through: each incoming note snaps to the nearest step, its velocity comes
// along, and a note still held when the next step arrives becomes a tie
// across the steps it spanned instead of a retrigger. The sequencer's own
// steps go quiet while armed, so there is only ever one source of truth for
// what is sounding — what you played passes straight through underneath,
// unchanged, the same as always.
class StepSequencerInstance : public PluginInstance {
 public:
  StepSequencerInstance();

  static PluginDescriptor make_descriptor();

  static constexpr int kLanes = 8;
  static constexpr int kMaxSteps = 64;
  static constexpr int kVisibleSteps = 16;  // shim + old parameter IDs 16–191
  static constexpr int kPatterns = 16;
  static constexpr int kExtraHeads = 4;
  // Old name for the ID-block width. Must stay 16: a loop to kMaxSteps
  // writing 80+i lands in the probability block.
  static constexpr int kSteps = kVisibleSteps;
  static constexpr int kUnlockedNote = 255;

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
  enum Cond : uint8_t {
    Always = 0,
    Fill,
    NotFill,
    Pre,
    NotPre,
    Nei,
    AOverB
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

  // Mixer (and tests) write without parameter IDs. Out of range is a no-op.
  void set_cell(int pattern, int lane, int step, int note, int velocity, bool on,
                float probability);
  void set_cell(int pattern, int lane, int step, int note, int velocity, bool on,
                float probability, bool accent, bool tie);
  void set_trig(int pattern, int lane, int step, float micro, int ratchet,
                int cond, int cond_arg);
  int cell_note(int pattern, int lane, int step) const;
  int cell_velocity(int pattern, int lane, int step) const;
  bool cell_active(int pattern, int lane, int step) const;
  float cell_probability(int pattern, int lane, int step) const;
  bool cell_accent(int pattern, int lane, int step) const;
  bool cell_tie(int pattern, int lane, int step) const;
  float cell_microtiming(int pattern, int lane, int step) const;
  int cell_ratchet(int pattern, int lane, int step) const;
  int cell_condition(int pattern, int lane, int step) const;
  int cell_cond_arg(int pattern, int lane, int step) const;
  bool lane_muted(int lane) const;
  void set_lane_mute(int lane, bool mute);
  int lane_note(int lane) const;
  int lane_length(int lane) const;
  int lane_division(int lane) const;
  int lane_direction(int lane) const;
  int lane_channel(int lane) const;
  double lane_gate(int lane) const;
  int lane_euclid(int lane) const;
  // Euclid is a bang — mute/channel here must not repaint Toussaint.
  void set_lane(int lane, int note, int length, int division, int direction,
                int channel, bool mute, double gate);
  void set_lane_euclid(int lane, int pulses);
  void set_extra_head(int extra, int lane, int rate, int direction, int start,
                      int length, int transpose, bool mute);
  int extra_head_lane(int extra) const;
  int extra_head_rate(int extra) const;
  int extra_head_direction(int extra) const;
  int extra_head_start(int extra) const;
  int extra_head_length(int extra) const;
  int extra_head_transpose(int extra) const;
  bool extra_head_muted(int extra) const;
  int extra_head_step(int extra) const;
  int native_head_step(int lane) const;
  void set_focus(int lane);
  int focus() const;
  int pattern() const;
  int next_pattern() const;
  bool fill() const;
  bool recording() const;
  int view() const;
  int transpose() const;
  float swing() const;
  int scale() const;
  int root() const;
  float macro(int index) const;

 private:
  static constexpr size_t kMaxEvents = 128;
  static constexpr int kVoiceSlots = kLanes + kExtraHeads;

  struct StepCell {
    std::atomic<int> note{kUnlockedNote};
    std::atomic<int> velocity{100};
    std::atomic<bool> active{false};
    std::atomic<float> probability{1.0f};
    std::atomic<bool> accent{false};
    std::atomic<bool> tie{false};
    std::atomic<float> microtiming{0.0f};
    std::atomic<int> ratchet{1};
    std::atomic<uint8_t> condition{Always};
    std::atomic<uint8_t> cond_arg{0};
  };

  struct LaneState {
    std::atomic<int> note{60};
    std::atomic<int> length{kVisibleSteps};
    std::atomic<int> division{2};
    std::atomic<int> direction{Forward};
    std::atomic<int> channel{0};  // 0-based on the wire, 1-based to the user
    std::atomic<bool> mute{false};
    std::atomic<double> gate{0.5};
    std::atomic<int> euclid{0};
  };

  struct ExtraHead {
    std::atomic<int> lane{0};
    std::atomic<int> rate{0};
    std::atomic<int> direction{Forward};
    std::atomic<int> start{0};
    std::atomic<int> length{kVisibleSteps};
    std::atomic<bool> mute{true};
    std::atomic<int> transpose{0};
  };

  struct HeadVoice {
    int pitch = -1;
    uint8_t channel = 0;  // channel of the ON, not the live lane atomic
    double off_beat = 0;
    // Ratchet scheduler — last_step_beat_ is the *step* guard, not this.
    int ratchet_left = 0;
    double next_pulse_beat = 0;
    double pulse_spacing = 0;  // step_beats / N
    double pulse_dur = 0;      // gate * spacing
    double step_end_beat = 0;
    int pulse_vel = 100;
  };

  struct Capture {
    bool open = false;
    int64_t index = 0;
    int pitch = 0;
    int velocity = 100;
    int lane = 0;
    int pattern = 0;
    bool locked = false;
  };

  double beat_of(double index, double step_beats) const;
  int map_step(int index, int length, int direction);
  void emit(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2);
  void stop_sounding(int head, uint32_t frame);
  int focused_index() const;
  uint32_t next_rng();      // audio thread
  uint32_t next_ui_rng();   // UI thread — pattern ops, not process()
  int current_pattern() const;
  void rotate_steps(int delta);
  void randomize_hits();
  void randomize_notes();
  void clear_hits();
  void fill_euclidean(int pulses);
  void mutate_pattern();
  void capture_events(size_t incoming_count, double start_beat,
                      double block_beats, uint32_t frames);
  void close_capture(Capture& cap, int64_t release_index, int length);
  void cancel_scheduler(int head, uint32_t frame);
  void reset_blank();
  void paint_constructor_pattern();
  bool cell_in_range(int pattern, int lane, int step) const;
  StepCell& cell_at(int pattern, int lane, int step);
  const StepCell& cell_at(int pattern, int lane, int step) const;

  PluginDescriptor descriptor_;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  // --- what the UI thread writes and the audio thread reads ------------------
  std::array<std::array<std::array<StepCell, kMaxSteps>, kLanes>, kPatterns>
      cells_{};
  std::array<LaneState, kLanes> lanes_{};
  std::array<ExtraHead, kExtraHeads> extra_heads_{};
  std::atomic<float> swing_{0.0f};
  std::atomic<int> scale_{Chromatic};
  std::atomic<int> root_{0};
  std::atomic<int> transpose_{0};
  std::atomic<int> pattern_{0};
  std::atomic<int> next_pattern_{-1};
  std::atomic<bool> fill_{false};
  std::atomic<int> view_{0};
  std::atomic<int> focus_{0};
  std::array<std::atomic<float>, 4> macros_{};
  std::atomic<bool> record_armed_{false};

  // --- audio thread only -----------------------------------------------------
  std::array<HeadVoice, kVoiceSlots> voices_{};
  std::array<bool, kVoiceSlots> last_tied_{};
  // Last step boundary already played, per head, so a block that crosses none
  // does not replay the one it starts on.
  std::array<double, kVoiceSlots> last_step_beat_{};
  uint32_t rng_ = 0xC0FFEEu;
  uint32_t ui_rng_ = 0xBADC0DEu;

  // The armed state process() last acted on, to spot the edge where a fresh
  // arm has to cut off whatever the pattern itself was sounding.
  bool record_active_ = false;
  std::array<Capture, kLanes> captures_{};

  // Pre / Nei: last *visit* (miss included), audio-thread only.
  std::array<bool, kVoiceSlots> last_fired_{};
  std::array<std::array<bool, kMaxSteps>, kLanes> last_on_{};
  int last_bar_ = -1;

  std::atomic<int> playhead_{-1};
  // Last mapped step per native head, so the snapshot can light all eight.
  // −1 when nothing is walking (stopped, or an extra that is muted).
  std::array<std::atomic<int>, kLanes> head_steps_{};
  std::array<std::atomic<int>, kExtraHeads> extra_head_steps_{};

  std::array<MidiEvent, kMaxEvents> events_{};
  size_t event_count_ = 0;
};

}  // namespace nirbija
