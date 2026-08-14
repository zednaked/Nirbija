#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "core/audio_source.h"
#include "core/channel_strip.h"

namespace nirbija {

// Fixed channel capacity. Slots are pre-allocated so adding a channel never
// resizes anything the audio thread is walking; the audio thread only reads
// `active_` and stops there.
inline constexpr size_t kMaxChannels = 64;

class AudioGraph {
 public:
  AudioGraph();
  ~AudioGraph();

  void prepare(double sample_rate, uint32_t max_block_frames);

  // Realtime thread. Sums every audible strip into `master`, two pointers wide.
  void render(float* const* master, uint32_t frames);

  // UI thread. The strip is fully built and prepared before it becomes visible
  // to the audio thread, so no half-initialised channel is ever rendered.
  // Returns the channel index, or kMaxChannels if the graph is full.
  size_t add_channel(std::string name, int channel_count,
                     std::unique_ptr<AudioSource> source);

  size_t channel_count() const { return active_.load(std::memory_order_acquire); }
  ChannelStrip& channel(size_t index) { return *channels_[index]; }

  void set_master_gain(float linear) {
    master_gain_.store(linear, std::memory_order_relaxed);
  }
  // Peak since the last read, per master channel. Reading resets it.
  float read_master_peak(int channel);

 private:
  bool any_soloed(size_t count) const;

  std::array<std::unique_ptr<ChannelStrip>, kMaxChannels> channels_;
  std::array<std::unique_ptr<AudioSource>, kMaxChannels> sources_;
  std::atomic<size_t> active_{0};

  double sample_rate_ = 0.0;
  uint32_t max_block_frames_ = 0;

  std::atomic<float> master_gain_{1.0f};
  std::atomic<float> master_peaks_[2]{{0.0f}, {0.0f}};

  // Scratch reused every block, sized in prepare() so render() never allocates.
  std::vector<std::vector<float>> scratch_;
  std::vector<float*> scratch_ptrs_;
};

}  // namespace nirbija
