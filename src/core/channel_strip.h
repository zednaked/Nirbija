#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// One mixer channel: input -> insert chain -> gain/pan -> destination bus.
// Owned by the graph and only ever touched by the audio thread once attached;
// the UI mutates it through the engine's command queue.
class ChannelStrip {
 public:
  ChannelStrip(std::string name, int channel_count);
  ~ChannelStrip();

  void prepare(double sample_rate, uint32_t max_block_frames);

  // In-place on `buffers`, which holds channel_count() pointers.
  void process(float* const* buffers, uint32_t frames);

  const std::string& name() const { return name_; }
  int channel_count() const { return channel_count_; }

  // Audio-thread setters: plain stores, no allocation.
  void set_gain(float linear) { gain_.store(linear, std::memory_order_relaxed); }
  void set_pan(float pan) { pan_.store(pan, std::memory_order_relaxed); }
  float pan() const { return pan_.load(std::memory_order_relaxed); }
  void set_muted(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
  void set_soloed(bool soloed) { soloed_.store(soloed, std::memory_order_relaxed); }
  bool soloed() const { return soloed_.load(std::memory_order_relaxed); }

  // Peak since the last read, for the UI meters. Reading resets it.
  float read_peak(int channel);

  // Structural changes: caller must guarantee the strip is detached from the
  // running graph, which the engine does by swapping the graph under a message.
  void add_insert(std::unique_ptr<PluginInstance> plugin);
  void remove_insert(size_t index);
  size_t insert_count() const { return inserts_.size(); }

 private:
  std::string name_;
  int channel_count_;
  double sample_rate_ = 0.0;
  uint32_t max_block_frames_ = 0;

  std::atomic<float> gain_{1.0f};
  std::atomic<float> pan_{0.0f};   // -1 left, +1 right
  std::atomic<bool> muted_{false};
  std::atomic<bool> soloed_{false};

  // One smoothed value per parameter so a fader move does not click.
  float smoothed_gain_ = 1.0f;
  float smoothed_pan_ = 0.0f;
  float smoothing_coeff_ = 0.0f;

  std::vector<std::atomic<float>> peaks_;
  std::vector<std::unique_ptr<PluginInstance>> inserts_;

  // Scratch pointers handed to plugins, sized in prepare() so process() never
  // allocates.
  std::vector<float*> plugin_io_;
};

}  // namespace nirbija
