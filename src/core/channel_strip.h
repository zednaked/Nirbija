#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/plugin.h"

namespace nirbija {

// Insert slots per strip. AUM grows its slot list on demand; the cap here is
// what the audio thread walks, so it is fixed and generous rather than dynamic.
inline constexpr size_t kMaxInserts = 16;

// One mixer channel: input -> insert chain -> gain/pan -> destination bus.
// Owned by the graph and only ever touched by the audio thread once attached;
// the UI mutates it through the engine's command queue.
class ChannelStrip {
 public:
  ChannelStrip(std::string name, int channel_count);
  ~ChannelStrip();

  void prepare(double sample_rate, uint32_t max_block_frames);

  // In-place on `buffers`, which holds channel_count() pointers. Any MIDI for
  // this block is handed to the inserts first, so a synth sitting in the chain
  // hears it in the same block.
  void process(float* const* buffers, uint32_t frames,
               const MidiEvent* midi = nullptr, size_t midi_count = 0,
               const TransportInfo* transport = nullptr);

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

  // Insert slots, safe to edit while the audio thread is rendering. The plugin
  // is fully activated before it becomes visible, and a removed one is retired
  // rather than freed, so the audio thread never touches a dead pointer.
  bool add_insert(std::unique_ptr<PluginInstance> plugin);
  void remove_insert(size_t index);
  size_t insert_count() const { return insert_count_.load(std::memory_order_acquire); }
  PluginInstance* insert_at(size_t index) const;

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

  // Published to the audio thread. A null slot is a hole left by a removal and
  // is skipped, which keeps the indices of the surviving inserts stable.
  std::array<std::atomic<PluginInstance*>, kMaxInserts> insert_slots_{};
  std::atomic<size_t> insert_count_{0};

  // UI thread only. `retired_` holds inserts pulled out of the chain: the audio
  // thread may still be inside one when it is removed, so they are kept alive
  // until the strip itself dies.
  // TODO(phase-8): reclaim these once the audio thread has confirmed a pass.
  std::vector<std::unique_ptr<PluginInstance>> owned_inserts_;
  std::vector<std::unique_ptr<PluginInstance>> retired_;

  // Scratch pointers handed to plugins, sized in prepare() so process() never
  // allocates.
  std::vector<float*> plugin_io_;

  // MIDI in flight down the insert chain: what came in from the channel port,
  // plus whatever the inserts upstream produced. Fixed size so nothing
  // allocates mid-block.
  std::array<MidiEvent, 256> midi_chain_{};
  size_t midi_chain_count_ = 0;
};

}  // namespace nirbija
