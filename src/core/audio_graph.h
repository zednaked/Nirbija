#pragma once

#include <memory>
#include <vector>

#include "core/channel_strip.h"

namespace nirbija {

// The set of channels the audio thread is currently rendering. It is treated as
// immutable while live: the UI thread builds a new graph, hands the pointer
// over, and the old one is destroyed back on the UI thread.
class AudioGraph {
 public:
  AudioGraph();
  ~AudioGraph();

  void prepare(double sample_rate, uint32_t max_block_frames);

  // Sums every unmuted strip into `master`, which holds two pointers.
  void render(float* const* master, uint32_t frames);

  ChannelStrip& add_channel(std::string name, int channel_count);
  size_t channel_count() const { return channels_.size(); }
  ChannelStrip& channel(size_t index) { return *channels_[index]; }

 private:
  bool any_soloed() const;

  std::vector<std::unique_ptr<ChannelStrip>> channels_;
  double sample_rate_ = 0.0;
  uint32_t max_block_frames_ = 0;

  // Per-strip scratch, allocated in prepare() and reused every block.
  std::vector<std::vector<float>> scratch_;
  std::vector<float*> scratch_ptrs_;
};

}  // namespace nirbija
