#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// A loop pedal that lives in an insert slot. The channel's input passes
// through it unchanged, and on top of that it records, loops and overdubs
// what came in — with the start and end of the loop snapped to the beat or
// the bar, so a loop closed a little early or late still lands in time.
class LooperInstance : public PluginInstance {
 public:
  LooperInstance();

  static PluginDescriptor make_descriptor();

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override {}

  void set_transport(const TransportInfo& transport) override { transport_ = transport; }
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  // Loop audio is not part of the session: it is performance, not
  // configuration. Only the knobs survive.
  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

 private:
  // What the audio thread is doing right now with the loop.
  enum class Stage { Empty, Defining, Playing, Overdubbing, Stopped };

  bool at_boundary(uint32_t frames) const;
  void apply_requests(uint32_t frames);

  PluginDescriptor descriptor_;
  int channels_ = 2;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  // Interleaved stereo, sized once in activate() for the longest loop allowed;
  // nothing ever grows on the audio thread.
  std::vector<float> buffer_;
  uint64_t capacity_frames_ = 0;
  uint64_t length_ = 0;    // frames in the closed loop, 0 while empty
  uint64_t position_ = 0;  // play/overdub position within length_
  uint64_t written_ = 0;   // frames recorded while defining

  Stage stage_ = Stage::Empty;

  // What the UI asked for, applied at the next quantise boundary.
  std::atomic<bool> record_request_{false};
  std::atomic<bool> play_request_{true};
  std::atomic<bool> clear_request_{false};
  std::atomic<int> quantize_{2};  // 0 free, 1 beat, 2 bar
  std::atomic<float> gain_{1.0f};

  // The record state the audio thread last acted on, to spot edges.
  bool record_active_ = false;
};

}  // namespace nirbija
