#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// A MIDI plugin you write yourself, in Lua, without compiling anything. The
// free equivalent of Mozaic and StreamByter, which ECOSYSTEM.md counted as the
// second half of the Tier 1 hole.
//
// The shape of it is the whole design, and design/scripting.md argues it at
// length: **the script never runs on the audio thread**. MIDI is processed
// under a realtime deadline, and a Lua interpreter allocates and collects
// garbage - the first pause is an audible dropout. So the script is a compiler
// of tables, not a processor of events. It runs on the UI thread whenever it or
// a knob changes, and hands back lookup tables that the audio thread only
// indexes.
//
// What that buys: the realtime side is a few array reads and cannot stall.
// What it costs: a script cannot decide something in reaction to the note that
// just arrived. Scales, chords, velocity curves and channel routing are all
// expressible; "transpose the next note if the last one was low" is not.
class ScriptInstance : public PluginInstance {
 public:
  ScriptInstance();
  ~ScriptInstance() override;

  static PluginDescriptor make_descriptor();

  // The tables a script produces. Plain arrays, no indirection: this is the
  // only thing the audio thread ever looks at.
  struct Tables {
    // Where each incoming note goes. -1 drops it.
    std::array<int, 128> note{};
    // What each incoming velocity becomes. 0 drops the note.
    std::array<int, 128> velocity{};
    // Which channel each incoming channel is sent out on.
    std::array<int, 16> channel{};
  };

  static constexpr int kKnobs = 4;

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int) override {}
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override {}

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

  // --- the UI thread's side --------------------------------------------------
  // Compiles and runs the script, publishing what it returns. False means it
  // did not compile or did not run; `error()` says why, in Lua's own words.
  bool set_script(const std::string& source);
  std::string script() const;
  std::string error() const;

  // What a fresh instance carries, and a reasonable thing to show someone who
  // has never seen the API.
  static const char* default_script();

 private:
  struct Lua;

  // Publishes a table set and retires the one it replaces. Nothing is freed
  // until the audio thread has been seen past it.
  void publish(std::unique_ptr<Tables> tables);
  void collect();

  PluginDescriptor descriptor_;

  std::atomic<Tables*> live_{nullptr};
  // Retired sets, each with the block count at which it stopped being current.
  std::vector<std::pair<uint64_t, std::unique_ptr<Tables>>> retired_;
  std::atomic<uint64_t> blocks_{0};

  std::unique_ptr<Lua> lua_;
  mutable std::mutex text_mutex_;  // UI thread only: guards the two strings
  std::string source_;
  std::string error_;

  std::array<std::atomic<double>, kKnobs> knobs_{};

  static constexpr size_t kMaxEvents = 64;
  std::array<MidiEvent, kMaxEvents> events_{};
  size_t event_count_ = 0;

  // Incoming (note, channel) -> the pitch and channel that actually went
  // out, so a knob rebuild cannot send the matching note-off to a new
  // pitch and leave the old one hanging.
  struct Sounding {
    uint8_t in_note = 0;
    uint8_t in_channel = 0;
    uint8_t out_note = 0;
    uint8_t out_channel = 0;
  };
  std::array<Sounding, kMaxEvents> sounding_{};
  size_t sounding_count_ = 0;
};

}  // namespace nirbija
