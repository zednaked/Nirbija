#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "core/audio_source.h"
#include "core/channel_strip.h"
#include "core/recorder.h"

namespace nirbija {

// Fixed channel capacity. Slots are pre-allocated so adding a channel never
// resizes anything the audio thread is walking; the audio thread only reads
// `active_` and stops there.
inline constexpr size_t kMaxChannels = 64;

// Mix buses. A bus is a strip like any other, fed by whatever channels point at
// it rather than by a port.
inline constexpr size_t kMaxBuses = 16;

// How a strip names where it sends: -1 is the master, anything below
// kChannelDestination is a bus index, and above it a channel slot. One number
// so it can live in a single atomic the audio thread reads per block.
inline constexpr int kMasterDestination = -1;
inline constexpr int kChannelDestination = 1000;

inline int channel_destination(size_t slot) {
  return kChannelDestination + static_cast<int>(slot);
}

class AudioGraph {
 public:
  AudioGraph();
  ~AudioGraph();

  void prepare(double sample_rate, uint32_t max_block_frames);

  // Realtime thread. Sums every audible strip into `master`, two pointers wide.
  void render(float* const* master, uint32_t frames);

  // Realtime thread. Handed to every insert before it processes.
  void set_transport(const TransportInfo& transport) { transport_ = transport; }

  // The recorder is borrowed, not owned: the engine outlives the graph's use of
  // it and is what starts and stops it.
  void set_recorder(Recorder* recorder, int master_track) {
    master_track_ = master_track;
    recorder_.store(recorder, std::memory_order_release);
  }

  // UI thread. The strip is fully built and prepared before it becomes visible
  // to the audio thread, so no half-initialised channel is ever rendered.
  // Returns the channel index, or kMaxChannels if the graph is full.
  size_t add_channel(std::string name, int channel_count,
                     std::unique_ptr<AudioSource> source,
                     std::unique_ptr<MidiSource> midi = nullptr);

  // How many slots have ever been used. Removed slots keep their index so a
  // removal never renumbers the channels around it.
  size_t channel_count() const { return active_.load(std::memory_order_acquire); }
  bool channel_alive(size_t index) const;
  ChannelStrip& channel(size_t index) { return *channels_[index]; }

  // UI thread. The strip stops being rendered immediately, but is kept alive:
  // the audio thread may be inside it at this very moment.
  void remove_channel(size_t index);

  // --- mix buses ----------------------------------------------------------
  // Returns the bus index, or kMaxBuses when there is no room.
  size_t add_bus(std::string name);
  size_t bus_count() const { return bus_active_.load(std::memory_order_acquire); }
  bool bus_alive(size_t index) const;
  ChannelStrip& bus(size_t index) { return *buses_[index]; }
  void remove_bus(size_t index);

  void set_master_gain(float linear) {
    master_gain_.store(linear, std::memory_order_relaxed);
  }
  // Peak since the last read, per master channel. Reading resets it.
  float read_master_peak(int channel);

 private:
  bool any_soloed(size_t count) const;
  void record_master(float* const* master, uint32_t frames);

  // Sums a strip's output into wherever it is pointed, widening a mono strip on
  // the way.
  void mix_into(float* const* target, const ChannelStrip& strip, int width,
                uint32_t frames, float gain = 1.0f);

  // Where a strip's output goes. `rendered_channels` and `rendered_buses` are
  // how far the pass has got, since a strip can only feed something still
  // ahead of it — which is what makes a loop unrepresentable rather than
  // something to detect.
  float* const* destination_for(int destination, float* const* master,
                                long long rendered_channels,
                                long long rendered_buses);

  // Sends run after the strip has been processed, so what they carry is what
  // the strip is actually putting out, fader included.
  void apply_sends(const ChannelStrip& strip, int width, uint32_t frames,
                   long long rendered_buses);


  std::array<std::unique_ptr<ChannelStrip>, kMaxChannels> channels_;
  std::array<std::unique_ptr<AudioSource>, kMaxChannels> sources_;
  std::array<std::unique_ptr<MidiSource>, kMaxChannels> midi_sources_;

  // What the audio thread actually walks. A null slot was removed and is
  // skipped; the owning pointers above are what keep the object alive.
  std::array<std::atomic<ChannelStrip*>, kMaxChannels> live_{};
  std::atomic<size_t> active_{0};

  std::array<std::unique_ptr<ChannelStrip>, kMaxBuses> buses_;
  std::array<std::atomic<ChannelStrip*>, kMaxBuses> live_buses_{};
  std::atomic<size_t> bus_active_{0};

  // What the channels sum into before their bus processes them. Sized in
  // prepare() and reused every block.
  std::array<std::vector<std::vector<float>>, kMaxBuses> bus_buffers_;
  std::array<std::vector<float*>, kMaxBuses> bus_ptrs_;

  // The same idea for channels: a channel can be fed by an earlier channel, so
  // it needs somewhere for that to land before it renders.
  std::array<std::vector<std::vector<float>>, kMaxChannels> channel_buffers_;
  std::array<std::vector<float*>, kMaxChannels> channel_ptrs_;

  // UI thread only. Removed strips wait here: freeing one while the audio
  // thread is inside it would be a use-after-free.
  // TODO: reclaim these once the audio thread has confirmed a pass.
  std::vector<std::unique_ptr<ChannelStrip>> retired_;

  double sample_rate_ = 0.0;
  uint32_t max_block_frames_ = 0;

  TransportInfo transport_;
  std::atomic<Recorder*> recorder_{nullptr};
  int master_track_ = -1;
  std::atomic<float> master_gain_{1.0f};
  std::atomic<float> master_peaks_[2]{{0.0f}, {0.0f}};

  // One block's worth of MIDI, reused per channel. Deep enough for anything a
  // sequencer sends in a single period.
  std::array<MidiEvent, 128> midi_scratch_{};

  // Scratch reused every block, sized in prepare() so render() never allocates.
  std::vector<std::vector<float>> scratch_;
  std::vector<float*> scratch_ptrs_;
};

}  // namespace nirbija
