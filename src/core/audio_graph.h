// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/audio_source.h"
#include "core/channel_strip.h"
#include "core/dsp.h"
#include "core/recorder.h"

namespace nirbija {

// kMaxChannels, kMaxBuses and the destination constants live in
// channel_strip.h, where a destination is validated before it is published.

class AudioGraph {
 public:
  AudioGraph();
  ~AudioGraph();

  void prepare(double sample_rate, uint32_t max_block_frames);

  // Realtime thread. Sums every audible strip into `master`, two pointers wide.
  void render(float* const* master, uint32_t frames);

  // UI thread. Parks the graph: the master fades to silence over a few
  // milliseconds, and from the first block rendered entirely at zero no
  // plugin runs until unpark(), which fades it back in. wait_quiescent()
  // proves such a block has passed, which is what a state load needs. Used
  // around plugin state load, around saves that a plugin says need it, and
  // around recorder teardown.
  void park();
  void unpark();
  bool parked() const { return parked_.load(std::memory_order_acquire); }
  uint64_t render_generation() const {
    return render_generation_.load(std::memory_order_acquire);
  }
  // Blocks rendered with nothing running. A save that parks nothing leaves
  // this where it was, which is what the session tests check.
  uint64_t quiet_generation() const {
    return quiet_generation_.load(std::memory_order_acquire);
  }
  // False when the blocks never arrived — a stalled or absent audio thread.
  // The caller has then *not* been given the guarantee it asked for and must
  // not treat the graph as quiescent.
  [[nodiscard]] bool wait_renders(int blocks);
  // Waits for one whole block rendered with the graph parked and the fade
  // already at zero: no plugin was inside process() during it, and none will
  // be until unpark(). Same false as wait_renders() when it never comes.
  [[nodiscard]] bool wait_quiescent();
  // Realtime thread, after render(). Applies to a buffer that copies a
  // strip's output straight to a port the same fade the master got this
  // block, and silence when the block was parked and the copy is stale.
  void apply_park_ramp(float* buffer, uint32_t frames) const;

  // A brickwall on the master, after the fader: a millisecond and a half of
  // lookahead, so what leaves this box never passes the ceiling and never
  // clips the converter. Transparent below the ceiling; costs that much
  // latency on the master when on.
  void set_master_limiter(bool on) {
    master_limiter_.store(on, std::memory_order_relaxed);
  }
  bool master_limiter() const {
    return master_limiter_.load(std::memory_order_relaxed);
  }
  // Deepest gain reduction since the last read, in linear gain (1 = none).
  // Reading resets it.
  float read_limiter_floor() {
    return limiter_floor_.exchange(1.0f, std::memory_order_relaxed);
  }
  uint32_t master_latency_samples() const;
  // Everything this box adds between its inputs and its outputs: the deepest
  // insert chain (every other strip is delayed to match it) plus the
  // limiter's lookahead. What the JACK latency callback reports. Any thread.
  uint32_t internal_latency_samples() const;

  // The metronome click, rendered inside the graph so it goes through the
  // limiter and the park fade like everything else that reaches the master,
  // while still sitting after the master fader: pulling the mix down for a
  // break should not take the count with it. Realtime thread, before
  // render(): a click starting at `frame` of the coming block. A block is
  // assumed shorter than a beat, so one click per block is enough.
  void schedule_click(uint32_t frame, bool accent);

  // UI thread. Drops retired strips the audio thread has now left.
  // Frees strips and sources retired by removal, once the audio thread can no
  // longer be inside them. Pass whether the audio thread exists at all.
  void reclaim(bool audio_running = true);

  // Realtime thread. Handed to every insert before it processes.
  void set_transport(const TransportInfo& transport) { transport_ = transport; }

  // The recorder is borrowed, not owned: the engine outlives the graph's use of
  // it and is what starts and stops it.
  void set_recorder(Recorder* recorder, int master_track) {
    master_track_.store(master_track, std::memory_order_release);
    recorder_.store(recorder, std::memory_order_release);
  }

  // UI thread. The strip is fully built and prepared before it becomes visible
  // to the audio thread, so no half-initialised channel is ever rendered.
  // Returns the channel index, or kMaxChannels if the graph is full.
  size_t add_channel(std::string name, int channel_count,
                     std::unique_ptr<AudioSource> source,
                     std::unique_ptr<MidiSource> midi = nullptr);

  // Slot the next add_channel will take, so the engine can reuse JACK ports
  // sitting in a hole.
  size_t next_channel_slot() const;

  // How many slots have ever been used. Removed slots keep their index so a
  // removal never renumbers the channels around it.
  size_t channel_count() const { return active_.load(std::memory_order_acquire); }
  bool channel_alive(size_t index) const;
  // UI thread only: dereferences the owning pointer, which add_channel and
  // remove_channel move under it.
  ChannelStrip& channel(size_t index) { return *channels_[index]; }
  // Realtime thread. The published pointer, null for a removed or never
  // used slot. The only way the audio thread may reach a strip: the owning
  // pointers above are moved by the UI thread without any coordination.
  ChannelStrip* live_channel(size_t index) const {
    return index < kMaxChannels ? live_[index].load(std::memory_order_acquire)
                                : nullptr;
  }

  // UI thread. The strip stops being rendered immediately, but is kept alive:
  // the audio thread may be inside it at this very moment.
  void remove_channel(size_t index);

  // --- mix buses ----------------------------------------------------------
  // Returns the bus index, or kMaxBuses when there is no room.
  size_t add_bus(std::string name);
  size_t bus_count() const { return bus_active_.load(std::memory_order_acquire); }
  bool bus_alive(size_t index) const;
  // UI thread only, like channel().
  ChannelStrip& bus(size_t index) { return *buses_[index]; }
  // Realtime thread, like live_channel().
  ChannelStrip* live_bus(size_t index) const {
    return index < kMaxBuses ? live_buses_[index].load(std::memory_order_acquire)
                             : nullptr;
  }
  void remove_bus(size_t index);

  void set_master_gain(float linear) {
    master_gain_.store(linear, std::memory_order_relaxed);
  }
  void set_master_dim(bool on) { master_dim_.store(on, std::memory_order_relaxed); }
  void set_master_mute(bool on) { master_mute_.store(on, std::memory_order_relaxed); }
  void set_master_mono(bool on) { master_mono_.store(on, std::memory_order_relaxed); }
  bool master_dim() const { return master_dim_.load(std::memory_order_relaxed); }
  bool master_mute() const { return master_mute_.load(std::memory_order_relaxed); }
  bool master_mono() const { return master_mono_.load(std::memory_order_relaxed); }
  float read_master_peak(int channel);
  float read_master_clip() { return master_clip_.exchange(0.0f, std::memory_order_relaxed); }

  // Extra stereo pairs published by the last multi-out insert on `source`.
  void copy_tap(size_t source, int pair, float* left, float* right,
                uint32_t frames) const;
  void push_injected_midi(size_t channel, const MidiEvent& event);

 private:
  bool any_channel_soloed(size_t count) const;
  bool any_bus_soloed(size_t count) const;
  void record_master(float* const* master, uint32_t frames);
  size_t take_channel_slot();
  size_t take_bus_slot();

  // Sums a strip's output into wherever it is pointed, widening a mono strip on
  // the way. `gain` is where this block wants to end up; `state` is where the
  // last one left off, per output channel, and the block walks between the
  // two - so a solo, a send level or a mono pan never steps. A block that
  // starts and ends at zero costs nothing.
  void mix_into(float* const* target, const ChannelStrip& strip, int width,
                uint32_t frames, float gain, float* state);
  // The most any smoothed amount may move in one block: a full swing takes at
  // least kRampSeconds however short the block.
  float slew(float from, float to, uint32_t frames) const;

  // Where a strip's output goes. `rendered_channels` and `rendered_buses` are
  // how far the pass has got, since a strip can only feed something still
  // ahead of it — which is what makes a loop unrepresentable rather than
  // something to detect.
  float* const* destination_for(int destination, float* const* master,
                                long long rendered_channels,
                                long long rendered_buses);

  // Sends run after the strip has been processed, so what they carry is what
  // the strip is actually putting out, fader included.
  // `slot` picks the ramp state; a bus uses kMaxChannels + its index. With
  // `audible` false every send ramps to nothing rather than dropping out.
  void apply_sends(const ChannelStrip& strip, int width, uint32_t frames,
                   long long rendered_buses, size_t slot, bool audible);
  void run_limiter(float* const* master, uint32_t frames);
  void render_click(float* const* master, uint32_t frames);
  // Zeroes the channel and bus scratch that was written last block, and only
  // that: with 64 channels and 16 buses, clearing them all every block was
  // most of an idle graph's work.
  void clear_dirty_scratch();


  std::array<std::unique_ptr<ChannelStrip>, kMaxChannels> channels_;
  std::array<std::unique_ptr<AudioSource>, kMaxChannels> sources_;
  std::array<std::unique_ptr<MidiSource>, kMaxChannels> midi_sources_;

  // What the audio thread reads, published the same way the strips are. The
  // unique_ptrs above only own; reading one of those from the audio thread
  // while add_channel reassigns it would be a race on the pointer itself, not
  // merely on what it points at.
  std::array<std::atomic<AudioSource*>, kMaxChannels> live_sources_{};
  std::array<std::atomic<MidiSource*>, kMaxChannels> live_midi_sources_{};

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

  // Which of the buffers above something was mixed into this block, so the
  // next block zeroes only those. Audio thread only.
  std::array<bool, kMaxBuses> bus_dirty_{};
  std::array<bool, kMaxChannels> channel_dirty_{};

  // Each strip's latency this block, read once per block: latency_samples()
  // walks the insert chain, and it used to be asked three times per strip.
  std::array<uint32_t, kMaxChannels> channel_latency_{};
  std::array<uint32_t, kMaxBuses> bus_latency_{};

  // UI thread only. Removed strips wait here: freeing one while the audio
  // thread is inside it would be a use-after-free. Tagged with the render
  // generation at retire so reclaim() can drop them two blocks later.
  struct RetiredStrip {
    std::unique_ptr<ChannelStrip> strip;
    uint64_t generation = 0;
  };
  std::vector<RetiredStrip> retired_;

  // The sources need exactly the same treatment. A removal leaves them in
  // place so a render pass already inside the slot keeps its ground, which
  // means the *next* add_channel to reuse that slot is what would free them —
  // while that pass may still be in read(). So they are retired too.
  struct RetiredSource {
    std::unique_ptr<AudioSource> audio;
    std::unique_ptr<MidiSource> midi;
    uint64_t generation = 0;
  };
  std::vector<RetiredSource> retired_sources_;

  double sample_rate_ = 0.0;
  uint32_t max_block_frames_ = 0;

  TransportInfo transport_;
  std::atomic<Recorder*> recorder_{nullptr};
  std::atomic<int> master_track_{-1};
  std::atomic<bool> parked_{false};
  std::atomic<uint64_t> render_generation_{0};
  // Counts only blocks rendered with the graph parked and the fade already
  // at zero - the blocks nothing was running in.
  std::atomic<uint64_t> quiet_generation_{0};
  // The park fade, audio thread only. park_ramp_ holds this block's curve so
  // the engine can give a strip's direct out the same shape.
  float park_gain_ = 1.0f;
  std::vector<float> park_ramp_;
  bool park_ramp_active_ = false;
  bool last_render_quiet_ = false;

  std::atomic<float> master_gain_{1.0f};
  std::atomic<bool> master_dim_{false};
  std::atomic<bool> master_mute_{false};
  std::atomic<bool> master_mono_{false};
  std::atomic<bool> master_limiter_{false};
  std::atomic<float> master_peaks_[2]{{0.0f}, {0.0f}};
  std::atomic<float> master_clip_{0.0f};
  std::atomic<float> limiter_floor_{1.0f};

  // Where every smoothed amount was at the end of the last block, audio
  // thread only. Two per strip for its destination (left, right), two per
  // send, and the master's own fader and mono blend.
  std::array<std::array<float, 2>, kMaxChannels> channel_mix_{};
  std::array<std::array<float, 2>, kMaxBuses> bus_mix_{};
  std::array<std::array<std::array<float, 2>, kMaxSends>,
             kMaxChannels + kMaxBuses>
      send_mix_{};
  // The destination each strip last mixed into, so a reroute fades in at the
  // new place instead of arriving at full level.
  std::array<int, kMaxChannels> channel_last_dest_{};
  std::array<int, kMaxBuses> bus_last_dest_{};
  float master_mix_ = 1.0f;
  float master_mono_mix_ = 0.0f;

  // The limiter's delay line and gain smoother, sized in prepare().
  struct Limiter {
    std::vector<float> delay[2];
    std::vector<float> gains;  // the instant gain, one per delayed sample
    size_t write = 0;
    size_t lookahead = 0;
    float envelope = 1.0f;     // instant attack, held, then released
    uint32_t hold_left = 0;
    double window_sum = 0.0;   // running sum over `gains` for the average
    float release_coeff = 0.0f;
    // A block with the limiter off still runs the delay so switching it on
    // or off is a fade, not a jump in time.
    float blend = 0.0f;
  };
  Limiter limiter_;

  // The click, audio thread only. `click_at_` is the frame of the coming
  // block a click starts on, or UINT32_MAX for none.
  dsp::ClickTone click_;
  uint32_t click_at_ = UINT32_MAX;
  bool click_accent_ = false;

  static constexpr int kMaxTapPairs = 8;
  std::array<std::array<std::vector<float>, kMaxTapPairs>, kMaxChannels> tap_l_{};
  std::array<std::array<std::vector<float>, kMaxTapPairs>, kMaxChannels> tap_r_{};
  // How many pairs the last block actually wrote, so a bypassed or removed
  // multi-out insert can have the leftover block zeroed once rather than
  // looping forever into any tap that is still listening.
  std::array<int, kMaxChannels> tap_written_{};

  // One block's worth of MIDI, reused per channel. Deep enough for a
  // sequencer rolling ratchets on every head in one period.
  std::array<MidiEvent, 1024> midi_scratch_{};
  std::array<std::array<MidiEvent, 32>, kMaxChannels> injected_midi_{};
  std::array<size_t, kMaxChannels> injected_midi_n_{};

  // Scratch reused every block, sized in prepare() so render() never allocates.
  std::vector<std::vector<float>> scratch_;
  std::vector<float*> scratch_ptrs_;
};

}  // namespace nirbija
