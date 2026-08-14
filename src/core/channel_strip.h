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

// Sends per strip. A send is a scaled copy of the strip's output going to a bus
// while the strip keeps feeding its own destination — how a reverb is shared
// without losing the dry signal.
inline constexpr size_t kMaxSends = 4;

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
  // Where this strip sends its output: -1 is the master bus, anything else is
  // the index of a mix bus.
  void set_destination(int destination) {
    destination_.store(destination, std::memory_order_relaxed);
  }
  int destination() const { return destination_.load(std::memory_order_relaxed); }

  // Sends are plain atomics: a send level is one number, and the audio thread
  // reading a slightly stale one for a block is inaudible.
  void set_send(size_t index, int bus, float level) {
    if (index >= kMaxSends) return;
    sends_[index].level.store(level, std::memory_order_relaxed);
    sends_[index].bus.store(bus, std::memory_order_release);
  }
  int send_bus(size_t index) const {
    return index < kMaxSends ? sends_[index].bus.load(std::memory_order_acquire) : -1;
  }
  float send_level(size_t index) const {
    return index < kMaxSends ? sends_[index].level.load(std::memory_order_relaxed)
                             : 0.0f;
  }

  void set_armed(bool armed) { armed_.store(armed, std::memory_order_relaxed); }
  bool armed() const { return armed_.load(std::memory_order_relaxed); }

  // Which recorder track this strip feeds, or -1 while not recording. Set by
  // the engine when a recording starts.
  void set_record_track(int track) { record_track_.store(track, std::memory_order_release); }
  int record_track() const { return record_track_.load(std::memory_order_acquire); }

  // Peak since the last read, for the UI meters. Reading resets it.
  float read_peak(int channel);

  // Insert slots, safe to edit while the audio thread is rendering. The plugin
  // is fully activated before it becomes visible, and a removed one is retired
  // rather than freed, so the audio thread never touches a dead pointer.
  bool add_insert(std::unique_ptr<PluginInstance> plugin);
  void remove_insert(size_t index);

  // Swaps two slots. The audio thread may be part way through the chain when
  // this lands, so a single block can render the pair in either order; nothing
  // is dropped or freed, and the block after is correct.
  void swap_inserts(size_t a, size_t b);
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
  std::atomic<bool> armed_{false};
  std::atomic<int> destination_{-1};

  struct Send {
    std::atomic<int> bus{-1};
    std::atomic<float> level{0.0f};
  };
  std::array<Send, kMaxSends> sends_;
  std::atomic<int> record_track_{-1};

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
