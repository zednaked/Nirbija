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

// A strip is mono or stereo, and nothing else: the graph's scratch, the mix
// bus and the pan law are all two channels wide. Anything wider arriving from
// a session file or a caller is clamped here rather than indexing past them.
inline constexpr int kMaxStripChannels = 2;

// One mixer channel: input -> insert chain -> gain/pan -> destination bus.
// Owned by the graph and only ever touched by the audio thread once attached;
// the UI mutates it through the engine's command queue.
class ChannelStrip {
 public:
  ChannelStrip(std::string name, int channel_count);
  ~ChannelStrip();

  void prepare(double sample_rate, uint32_t max_block_frames,
               bool force_reactivate = false);

  // In-place on `buffers`, which holds channel_count() pointers. Any MIDI for
  // this block is handed to the inserts first, so a synth sitting in the chain
  // hears it in the same block.
  void process(float* const* buffers, uint32_t frames,
               const MidiEvent* midi = nullptr, size_t midi_count = 0,
               const TransportInfo* transport = nullptr);

  const std::string& name() const { return name_; }
  // UI thread only. The recorder reads this when a take starts, so a renamed
  // channel records to a file with the name you gave it.
  void set_name(std::string name) { name_ = std::move(name); }
  int channel_count() const { return channel_count_; }

  // Audio-thread setters: plain stores, no allocation.
  void set_gain(float linear) { gain_.store(linear, std::memory_order_relaxed); }
  void set_pan(float pan) { pan_.store(pan, std::memory_order_relaxed); }
  float pan() const { return pan_.load(std::memory_order_relaxed); }
  // Where the pan actually is this block, audio thread only: the graph pans
  // a mono strip with this so the spread moves with the fader, not ahead
  // of it.
  float smoothed_pan() const { return smoothed_pan_; }
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

  // Peak since the last read, for the UI meters. Reading resets it. A channel
  // outside the strip's own width reads zero: the UI's idea of the width and
  // the strip's can differ across a session load, and that must not index off
  // the end of the meter array.
  float read_peak(int channel);

  // Insert slots, safe to edit while the audio thread is rendering. The plugin
  // is fully activated before it becomes visible, and a removed one is retired
  // rather than freed, so the audio thread never touches a dead pointer.
  // Fills the first hole left by a removal, or appends when there is none.
  // `placed_at` reports where it landed, for the UI's parallel label list.
  bool add_insert(std::unique_ptr<PluginInstance> plugin,
                  size_t* placed_at = nullptr);
  void remove_insert(size_t index);
  // Frees inserts retired by remove_insert(), once the audio thread can no
  // longer be inside them. Pass whether the audio thread exists at all: with
  // none, the generation gate never opens and nothing is ever freed.
  void reclaim(bool audio_running = true);

  // Swaps two slots. The audio thread takes the whole chain as one snapshot
  // (see chain_seq_), so a block renders either the old order or the new one,
  // never a mix that runs one of the pair twice.
  void swap_inserts(size_t a, size_t b);
  size_t insert_count() const { return insert_count_.load(std::memory_order_acquire); }
  PluginInstance* insert_at(size_t index) const;

  void set_insert_bypassed(size_t index, bool on);
  bool insert_bypassed(size_t index) const;
  void set_insert_post_fader(size_t index, bool on);
  bool insert_post_fader(size_t index) const;

  // Bit N set = MIDI channel N (0-15) is allowed. 0xFFFF is all.
  void set_midi_mask(uint16_t mask) { midi_mask_.store(mask, std::memory_order_relaxed); }
  uint16_t midi_mask() const { return midi_mask_.load(std::memory_order_relaxed); }

  void set_sidechain_slot(int slot) {
    sidechain_slot_.store(slot, std::memory_order_relaxed);
  }
  int sidechain_slot() const { return sidechain_slot_.load(std::memory_order_relaxed); }

  uint32_t latency_samples() const;
  int extra_output_pairs() const;
  void copy_extra_output(int pair, float* left, float* right, uint32_t frames) const;

  // Last processed audio, after fader and mute, for hardware outs / sidechain.
  const float* output_cache(int channel) const;

  void set_pdc_delay(uint32_t samples) {
    pdc_delay_.store(samples, std::memory_order_relaxed);
  }
  void apply_pdc(float* const* buffers, uint32_t frames);
  void feed_sidechain(const float* const* buffers, int channels, uint32_t frames);

 private:
  void run_insert(PluginInstance* insert, float* const* buffers, uint32_t frames,
                  const TransportInfo* transport, bool filter_midi);
  // Runs the plugin so it can consume MIDI (and not hang a voice) but puts
  // the dry audio back and drops anything it emitted.
  void run_bypassed(PluginInstance* insert, float* const* buffers,
                    uint32_t frames, const TransportInfo* transport);
  // The blocks between: the plugin runs, and what leaves the slot walks
  // from wet to dry (or back) across the block. `mix` is how much dry was in
  // it at the end of the last block and is left at where this one ends.
  void run_crossfade(PluginInstance* insert, float* const* buffers,
                     uint32_t frames, const TransportInfo* transport,
                     float* mix, bool to_bypass);
  static bool midi_allowed(const MidiEvent& event, uint16_t mask);

  // One block's view of the insert chain, taken whole before anything runs.
  // Not named `slots`: Qt defines that as a macro, and this header is included
  // from the UI.
  struct ChainSnapshot {
    PluginInstance* inserts[kMaxInserts] = {};
    uint8_t flags[kMaxInserts] = {};
    size_t count = 0;
  };
  // Reads the chain under the sequence counter so the pointers and their flags
  // all belong to the same instant. False when the UI thread was mid-edit for
  // every attempt, which costs this block its inserts rather than risking a
  // chain that runs one plugin twice.
  bool snapshot_chain(ChainSnapshot* out) const;

  // Brackets a chain edit. The odd value in between is what tells a reader its
  // snapshot is torn.
  void begin_chain_edit() { chain_seq_.fetch_add(1, std::memory_order_acq_rel); }
  void end_chain_edit() { chain_seq_.fetch_add(1, std::memory_order_release); }

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
  std::atomic<uint16_t> midi_mask_{0xFFFFu};
  std::atomic<int> sidechain_slot_{-1};
  std::array<std::atomic<uint8_t>, kMaxInserts> insert_flags_{};
  static constexpr uint8_t kBypass = 1;
  static constexpr uint8_t kPostFader = 2;

  // One smoothed value per parameter so a fader move does not click.
  float smoothed_gain_ = 1.0f;
  float smoothed_pan_ = 0.0f;
  float smoothing_coeff_ = 0.0f;
  // Mute is a straight line to zero and back over a few milliseconds,
  // separate from the fader's own smoothing so it feels like a switch.
  float mute_gain_ = 1.0f;
  float mute_step_ = 1.0f;
  // How much dry each slot is putting out, 0 wet to 1 bypassed. Owned by the
  // audio thread except that add_insert() zeroes the slot it fills, so a new
  // plugin starts wet like its flags say.
  std::array<std::atomic<float>, kMaxInserts> bypass_mix_{};

  std::vector<std::atomic<float>> peaks_;

  // Published to the audio thread. A null slot is a hole left by a removal and
  // is skipped, which keeps the indices of the surviving inserts stable.
  std::array<std::atomic<PluginInstance*>, kMaxInserts> insert_slots_{};
  std::atomic<size_t> insert_count_{0};

  // Even while the chain is settled, odd while the UI thread is rewriting it.
  std::atomic<uint32_t> chain_seq_{0};

  // UI thread only. `retired_` holds inserts pulled out of the chain: the audio
  // thread may still be inside one when it is removed, so they are kept alive
  // until two process() calls have passed.
  struct RetiredInsert {
    std::unique_ptr<PluginInstance> plugin;
    uint64_t generation = 0;
  };
  std::vector<std::unique_ptr<PluginInstance>> owned_inserts_;
  std::vector<RetiredInsert> retired_;
  std::atomic<uint64_t> process_generation_{0};

  // Scratch pointers handed to plugins, sized in prepare() so process() never
  // allocates.
  std::vector<float*> plugin_io_;

  // MIDI in flight down the insert chain: what came in from the channel port,
  // plus whatever the inserts upstream produced. Fixed size so nothing
  // allocates mid-block; deep enough for a sequencer rolling ratchets on
  // every head in one period.
  std::array<MidiEvent, 1024> midi_chain_{};
  size_t midi_chain_count_ = 0;

  std::vector<std::vector<float>> output_cache_;
  std::vector<std::vector<float>> delay_line_;
  std::vector<size_t> delay_write_;
  std::atomic<uint32_t> pdc_delay_{0};
};

}  // namespace nirbija
