#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"
#include "core/rt_queue.h"

namespace nirbija {

// The computer's own keyboard, played as an instrument: no MIDI hardware
// required to get notes into a strip. A press or release from the UI thread
// is queued rather than touched directly — the same key can be typed again
// before the audio thread has caught up with the last one, and a lock here
// is not an option on that thread.
//
// No transport dependency, unlike the step sequencer: a key does what a real
// MIDI keyboard's would, on or off the moment it moves, whether or not
// anything is playing. Notes arriving from earlier in the chain still pass
// straight through, so this can share a strip with another source the same
// way the sequencer does.
class KeyboardInstrumentInstance : public PluginInstance {
 public:
  KeyboardInstrumentInstance();

  static PluginDescriptor make_descriptor();

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int) override {}
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override;

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

  // UI thread: a physical key going down or up. Octave and velocity are the
  // caller's own business — by the time a note number gets here it is
  // already absolute, 0-127.
  void key_down(int note, int velocity);
  void key_up(int note);

 private:
  static constexpr size_t kMaxEvents = 64;
  static constexpr size_t kQueueCapacity = 256;  // power of two, RtQueue needs it

  struct KeyEvent {
    uint8_t note = 0;
    uint8_t velocity = 0;
    bool down = false;
  };

  void emit_event(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2);

  PluginDescriptor descriptor_;
  double sample_rate_ = 48000.0;

  std::atomic<int> channel_{0};  // 0-based on the wire, 1-based to the user

  // UI thread pushes, audio thread pops in process() — see class comment.
  RtQueue<KeyEvent, kQueueCapacity> incoming_;

  // --- audio thread only -----------------------------------------------------
  // Which notes this instance itself is currently holding down, so a queued
  // key-up that lost its race with a key-down (or a stray repeat) never
  // double-fires or releases a note it never sounded.
  std::array<bool, 128> held_{};
  std::array<MidiEvent, kMaxEvents> events_{};
  size_t event_count_ = 0;
};

}  // namespace nirbija
