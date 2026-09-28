// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/dsp.h"
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

  // Knobs and the tape travel with the session. A first pass that is still
  // open is written as a closed loop so closing the app mid-take does not
  // throw the recording away.
  std::vector<uint8_t> save_state() const override;
  // The tape is being written while Rec is down, and for the few blocks an
  // undo, redo or multiply request takes to land; a save copies the whole
  // tape. Between takes the audio thread only reads it, and the copy can
  // happen underneath.
  bool save_needs_quiet() const override {
    return record_request_.load(std::memory_order_relaxed) ||
           undo_request_.load(std::memory_order_relaxed) ||
           redo_request_.load(std::memory_order_relaxed) ||
           multiply_request_.load(std::memory_order_relaxed);
  }
  bool load_state(const std::vector<uint8_t>& blob) override;

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  // --- editing the closed loop ---------------------------------------------
  // Where playback starts and stops within it, and how long the fade in and
  // out either side of that window run - all as a fraction of the loop's own
  // length, so they still mean the same thing after a re-record changes what
  // that length is. Not run through the numbered set_parameter() above: these
  // are shaped by dragging a waveform, not a value worth binding a MIDI knob
  // to.
  double trim_start() const { return trim_start_.load(std::memory_order_relaxed); }
  double trim_end() const { return trim_end_.load(std::memory_order_relaxed); }
  double fade_in() const { return fade_in_.load(std::memory_order_relaxed); }
  double fade_out() const { return fade_out_.load(std::memory_order_relaxed); }
  void set_trim(double start, double end);
  void set_fades(double fade_in, double fade_out);

  // What the UI asked for: Rec is down. The head may not be writing yet -
  // a quantised punch-in waits for the grid - see writing() for that.
  bool recording() const {
    return record_request_.load(std::memory_order_relaxed);
  }
  // The head is actually writing the tape this block. Differs from
  // recording() for up to a Length unit either side of a press: armed and
  // waiting for the bar before, still writing up to the bar after. The
  // editor counts those beats down for the player - that gap is exactly
  // when they need to know whether to play.
  bool writing() const { return writing_.load(std::memory_order_relaxed); }
  // Beats to the next quantise boundary - the moment a pending Rec press
  // lands - from the transport at the last block. -1 when there is no
  // grid to wait for (Length free, or the beat grid not moving), in which
  // case a press lands on the next block.
  double beats_to_boundary() const {
    return beats_to_boundary_.load(std::memory_order_relaxed);
  }
  bool playing() const { return play_request_.load(std::memory_order_relaxed); }
  bool count_in() const { return count_in_.load(std::memory_order_relaxed); }
  void set_count_in(bool on);
  // Beats still to go before Rec starts, 0 if we are not counting.
  int count_in_beats() const {
    return count_beats_left_.load(std::memory_order_relaxed);
  }
  bool counting_in() const { return count_in_beats() > 0; }
  // True once anything has been written this pass, or a loop is already closed.
  // A silent take still counts: the editor has to show the empty waveform
  // rather than "nothing recorded".
  bool has_audio() const {
    return length_.load(std::memory_order_relaxed) > 0 ||
           written_.load(std::memory_order_relaxed) > 0;
  }
  bool loop_closed() const {
    return length_.load(std::memory_order_relaxed) > 0;
  }

  // Length's own setting, 0 free .. 5 = 8 bars, or kQuantizeSync to follow
  // another Looper's cycle instead. Plain enough to poll from the host at
  // UI rate to drive the sync push in set_sync_beats().
  int quantize() const { return quantize_.load(std::memory_order_relaxed); }
  // Length reading "Sync": follow another Looper's closed length instead of
  // a fixed beat/bar count. The host (MixerModel) resolves which instance
  // that is from sync_target_row/slot() and pushes its loop_beats() in here
  // once a poll - this instance never reaches for another one itself, so it
  // stays as ignorant of the graph around it as every other knob here.
  static constexpr int kQuantizeSync = 6;
  double sync_beats() const { return sync_beats_.load(std::memory_order_relaxed); }
  void set_sync_beats(double beats) {
    sync_beats_.store(std::max(0.0, beats), std::memory_order_relaxed);
  }
  // Which other insert to follow when Length reads Sync - opaque row/slot
  // coordinates this instance never dereferences itself. -1 means none
  // picked yet. Saved and restored with the rest of the state so the choice
  // survives a reload.
  int sync_target_row() const {
    return sync_target_row_.load(std::memory_order_relaxed);
  }
  int sync_target_slot() const {
    return sync_target_slot_.load(std::memory_order_relaxed);
  }
  void set_sync_target(int row, int slot) {
    sync_target_row_.store(row, std::memory_order_relaxed);
    sync_target_slot_.store(slot, std::memory_order_relaxed);
  }

  // One peak per bucket, buckets spanning the closed loop start to end nose
  // to tail - an overview to draw, not the audio itself. Called from the UI
  // thread while the audio thread may still be writing into the same buffer;
  // reading it unguarded is the standard, accepted tradeoff for a waveform
  // overview - worst case one refresh reads a torn sample and draws a single
  // stray peak, never a crash, and the next refresh corrects it.
  std::vector<float> waveform(int buckets) const;

  // Which overdub pass most recently touched each bucket, 0 for the base
  // take counting up once per Rec press after that - so the editor can tint
  // fresh material differently from what has sat there since the first pass.
  // Best-effort like waveform() above: Undo does not rewind this, so a peel
  // can leave a bucket showing a newer layer than what is actually playing
  // until the next overdub touches it.
  std::vector<int> layer_map(int buckets) const;

  // 0..1 through the closed loop, or -1 while there is nothing playing to
  // show one for. A mirror updated once a block, the same pattern as
  // `playhead()` on step_sequencer and arpeggiator - safe to read from the
  // UI thread on a timer without touching the per-sample position the audio
  // thread actually plays from.
  double position_fraction() const {
    return position_fraction_.load(std::memory_order_relaxed);
  }

  // Peak of the loop's own wet signal in the last block processed - the tape
  // played back plus whatever is being overdubbed onto it, not the dry
  // pass-through. Lets the editor show whether the stack is getting hot
  // separately from the channel's own meter, which is the sum of both.
  float loop_peak() const { return loop_peak_.load(std::memory_order_relaxed); }

  // Beats in the closed loop at the tempo it was closed at, 0 if unknown.
  // The editor draws a bar grid from this and the time signature.
  double loop_beats() const { return loop_beats_.load(std::memory_order_relaxed); }

  // --- undo ------------------------------------------------------------------
  // One layer of undo: the state before the last Rec, Clear or Multiply,
  // plus each phrase recorded since — a phrase is a run of input between
  // silences. Undo peels the last of those, not the whole pass, so a held
  // Rec with two licks in it does not throw the first one away.
  //
  // Everything here is a request the audio thread carries out at the start
  // of a block: the snapshot is taken by the head itself as it overwrites
  // the tape (copy-before-write into a second, equally sized buffer), and
  // undo swaps which of the two is live. Nothing is copied on the UI thread
  // and the graph never has to be parked — a park fades the master to
  // silence, which is a hole in the music on every Rec press. can_undo() and
  // can_redo() are mirrors updated once a block.
  bool can_undo() const { return can_undo_.load(std::memory_order_relaxed); }
  bool can_redo() const { return can_redo_.load(std::memory_order_relaxed); }
  void request_undo() { undo_request_.store(true, std::memory_order_release); }
  void request_redo() { redo_request_.store(true, std::memory_order_release); }
  void request_clear() { clear_request_.store(true, std::memory_order_release); }

  // Doubles the tape by appending a copy of itself, so the next pass can
  // write into the new half. Same request pattern as undo; the copy is at
  // most half the tape and runs once, on the audio thread, at a block start.
  bool can_multiply() const;
  void request_multiply() {
    multiply_request_.store(true, std::memory_order_release);
  }

 private:
  // What the audio thread is doing right now with the loop.
  enum class Stage { Empty, Defining, Playing, Overdubbing };

  // 0 free, 1 beat, 2 one bar, 3 two, 4 four, 5 eight, 6 = kQuantizeSync.
  static constexpr int kQuantizeMax = kQuantizeSync;

  // Frame inside this block where a pending Rec change lands: 0 when the
  // grid is off or the block starts on the line, `frames` when the line is
  // beyond this block.
  uint32_t punch_frame(uint32_t frames) const;
  double beats_to_boundary(const TransportInfo& transport) const;
  // Handles the clear/undo/redo/multiply requests and the count-in, then
  // returns the frame at which the record edge (if any) applies.
  uint32_t apply_requests(uint32_t frames);
  void apply_record_edge();
  void punch_out();
  double unit_beats() const;
  uint64_t snap_length(uint64_t written) const;
  // One Length unit in frames, 0 when Length is free. The first take closes
  // here on its own so leaving Rec down does not keep eating silence onto
  // the end of the phrase — and then keep playing that growing tape.
  uint64_t grid_frames() const;
  void close_loop(uint64_t frames, Stage next);
  void start_count_in();
  void stop_count_in();
  void begin_record();
  // Walks play_pos_ around [start, end) after a step; true if it wrapped.
  bool wrap_play_pos(double start, double end, bool reverse);
  // Stereo frames currently on the tape: the closed length, or the open
  // take if the loop has not been punched out yet.
  uint64_t tape_frames() const;
  // Copies src into both tape buffers, resampling when src_rate is not this
  // instance's rate. Returns how many frames landed, already clamped to
  // capacity. UI thread only (activate / load_state).
  uint64_t import_audio(const float* src, uint64_t src_frames, double src_rate);

  // The live tape and its shadow (the undo layer) - see the undo section
  // above. Both are sized once in activate(); the audio thread only ever
  // flips which one is live.
  float* tape() { return tapes_[live_.load(std::memory_order_relaxed)].data(); }
  const float* tape() const {
    return tapes_[live_.load(std::memory_order_relaxed)].data();
  }
  float* shadow() {
    return tapes_[live_.load(std::memory_order_relaxed) ^ 1].data();
  }
  // Copy-before-write: the first time a pass touches a frame, its old value
  // goes into the shadow. Frames past the snapshot's length were silent in
  // the old state, so the shadow gets silence for them.
  void snapshot(uint64_t frame);
  void snapshot_range(uint64_t lo, uint64_t hi);
  // Frames the previous undo layer changed have to be copied into the shadow
  // before a new layer can be swapped in; this does a bounded chunk of that
  // work per block.
  void sweep_shadow();
  bool sweep_done() const { return stale_pos_ >= stale_hi_; }
  // Writes `in` onto the tape at `frame` with weight `ramp` (0..1), the way
  // the current stage and knobs say: fresh tape while defining, add or
  // replace over an existing loop.
  void write_frame(uint64_t frame, const float in[2], float ramp);
  // 4-point read at a fractional position, indices wrapped within [lo, hi).
  void read_frame(const float* buffer, double position, uint64_t lo,
                  uint64_t hi, float out[2]) const;

  PluginDescriptor descriptor_;
  int channels_ = 2;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  // Interleaved stereo, two of them, sized once in activate() for the
  // longest loop allowed; nothing ever grows on the audio thread.
  std::vector<float> tapes_[2];
  std::atomic<int> live_{0};
  uint64_t capacity_frames_ = 0;
  // Frames in the closed loop, 0 while empty. Atomic because waveform() and
  // has_audio() read it from the UI thread; it only ever changes at a loop
  // closing or clearing, not once a sample.
  std::atomic<uint64_t> length_{0};
  // Fractional so pitch can walk the buffer at a rate that is not 1.
  double play_pos_ = 0.0;
  // Atomic because waveform() and has_audio() read it from the UI thread
  // while the first pass is still being written.
  std::atomic<uint64_t> written_{0};
  std::atomic<double> loop_beats_{0.0};

  Stage stage_ = Stage::Empty;

  // What the UI asked for, applied at the next quantise boundary.
  std::atomic<bool> record_request_{false};
  std::atomic<bool> play_request_{true};
  std::atomic<bool> clear_request_{false};
  std::atomic<bool> undo_request_{false};
  std::atomic<bool> redo_request_{false};
  std::atomic<bool> multiply_request_{false};
  std::atomic<int> quantize_{2};  // 0 free, 1 beat, 2..5 = 1/2/4/8 bars, 6 sync
  std::atomic<float> gain_{1.0f};
  std::atomic<float> pitch_{0.0f};  // semitones, -12..12
  std::atomic<float> tone_{1.0f};   // 0 dark .. 1 open
  std::atomic<bool> reverse_{false};
  std::atomic<bool> once_{false};
  std::atomic<bool> replace_{false};
  std::atomic<float> speed_{1.0f};      // 0.25..4, stacked on pitch
  std::atomic<float> feedback_{1.0f};   // 0..1, applied on overdub
  std::atomic<bool> count_in_{false};
  std::atomic<int> count_beats_left_{0};

  // Length reading Sync - see quantize()/kQuantizeSync above. sync_beats_ is
  // pushed in from outside once a poll; sync_target_* is just carried for
  // the host to read back, never touched by this instance itself.
  std::atomic<double> sync_beats_{0.0};
  std::atomic<int> sync_target_row_{-1};
  std::atomic<int> sync_target_slot_{-1};

  // Peak of the last block's wet loop signal - see loop_peak() above.
  std::atomic<float> loop_peak_{0.0f};

  // Which overdub pass is currently being written, audio thread only - see
  // layer_map() above. 0 is the base take; a fresh Rec press past Empty
  // starts a new one.
  int current_layer_ = 0;
  // One byte per frame, the layer that most recently wrote there. Sized in
  // activate() with the tape so the audio thread never grows it.
  std::vector<uint8_t> layer_;
  // One byte per frame: how much of a full record pass the frame has had so
  // far, 255 = a whole one. The ~5 ms ramp in and the ramp out of a pass
  // that is exactly one loop long land on the same frames, and each scales
  // the old layer by only part of the feedback; summing the weights lets the
  // second touch finish what the first started, so a one-cycle wipe with
  // feedback 0 (or Replace) really wipes. Reset the first time a layer
  // touches the frame, in snapshot().
  std::vector<uint8_t> rec_weight_;

  // Count-in, audio thread only. Own clock so Rec can count with Play off.
  bool counting_ = false;
  double count_phase_ = 0.0;
  int count_total_ = 0;
  dsp::ClickTone click_;

  // One-pole lowpass on the wet loop, audio thread only. The coefficient is
  // worked out once a block from the Tone knob, not once a sample.
  float tone_lpf_[2] = {0.0f, 0.0f};
  float tone_coeff_ = 1.0f;
  bool tone_bypass_ = true;
  // Playback rate for this block: pitch and speed folded together.
  double rate_ = 1.0;

  // Per-sample smoothing, all set once a block and stepped once a sample:
  // the loop gain (a mapped fader must not zipper), Play on/off (a hard cut
  // clicks) and the weight of the input going onto the tape at the edges of
  // a record pass (a hard punch clicks on the tape, every time round).
  dsp::LinearRamp gain_ramp_;
  dsp::LinearRamp play_ramp_;
  dsp::LinearRamp rec_ramp_;
  // ~5 ms in frames: the record edge ramps, the Play ramp, and the wrap
  // crossfade all use this one length.
  uint32_t edge_frames_ = 240;

  // The record state the audio thread last acted on, to spot edges; and the
  // request it is about to act on at this block's punch frame.
  bool record_active_ = false;
  bool pending_record_ = false;

  // Trim window and fade shape, as fractions of the closed loop. Written from
  // the UI thread while dragging a handle, read every sample on the audio
  // thread - relaxed is enough because nothing else has to happen-before a
  // trim edit landing a block late; the loop just keeps playing the old
  // window for one more block.
  std::atomic<double> trim_start_{0.0};
  std::atomic<double> trim_end_{1.0};
  std::atomic<double> fade_in_{0.0};
  std::atomic<double> fade_out_{0.0};

  // Updated once a block, for the UI to read - see position_fraction(),
  // writing(), beats_to_boundary(), can_undo() and can_redo().
  std::atomic<double> position_fraction_{-1.0};
  std::atomic<bool> writing_{false};
  std::atomic<double> beats_to_boundary_{-1.0};
  std::atomic<bool> can_undo_{false};
  std::atomic<bool> can_redo_{false};

  // Frame bounds of the current trim window, clamped to the loop length
  // passed in - always length_'s current value, but process() only wants to
  // load that atomic once a sample, not three times.
  uint64_t trim_start_frames(uint64_t length) const;
  uint64_t trim_end_frames(uint64_t length, uint64_t start) const;
  // The fade multiplier at a frame already known to be inside [start, end).
  float envelope_at(uint64_t position, uint64_t start, uint64_t end) const;

  // --- undo machinery, audio thread only -------------------------------------
  // The metadata half of the undo layer; the audio half is the shadow tape.
  struct UndoMeta {
    uint64_t length = 0;
    double beats = 0.0;
    double trim_start = 0.0;
    double trim_end = 1.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
    bool valid = false;
    bool undone = false;
  };
  UndoMeta undo_meta_;
  // One byte per frame: 0 the shadow equals the tape here, 1 the shadow is
  // stale (a finished layer changed the tape and the sweep has not caught
  // up), 2 the shadow holds this layer's pre-pass value. Sized in activate().
  std::vector<uint8_t> shadow_state_;
  static constexpr uint8_t kShadowSynced = 0;
  static constexpr uint8_t kShadowStale = 1;
  static constexpr uint8_t kShadowSnapped = 2;
  // Range of frames snapshotted by the current layer, and the range the
  // sweep still has to sync. Both empty when lo >= hi.
  uint64_t touched_lo_ = 0;
  uint64_t touched_hi_ = 0;
  uint64_t stale_pos_ = 0;
  uint64_t stale_hi_ = 0;
  // Takes the current state as the new undo layer: the previous layer's
  // differences become permanent (queued for the sweep) and the phrase
  // list starts over.
  void capture_undo();
  void swap_undo();
  void apply_undo();
  void apply_redo();
  void apply_multiply();
  void apply_clear();

  // One byte per frame, set when Rec heard something this pass, plus one
  // past the highest frame set so the scans stay short. Sized in activate()
  // with the tape so the audio thread never grows it.
  std::vector<uint8_t> recorded_;
  // recorded_hi_ only ever grows until the next clear, so a clear knows how
  // far to wipe; recorded_count_ is how many bytes are set, so "is there a
  // phrase to peel" is a comparison, not a scan, once a block.
  uint64_t recorded_hi_ = 0;
  uint64_t recorded_count_ = 0;
  void mark_recorded(uint64_t frame);
  void clear_recorded();
  bool has_burst() const {
    return recorded_count_ > 0 && tape_frames() > 0;
  }
  // A peeled phrase lives in the shadow tape (the peel swaps the range
  // between tape and shadow), so a peel is just its bounds.
  struct Peel {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t dropped_length = 0;
    double dropped_beats = 0.0;
  };
  static constexpr int kMaxPeels = 32;
  Peel peels_[kMaxPeels];
  int peel_count_ = 0;
  bool last_burst(uint64_t* start, uint64_t* end) const;
  bool peel_last_burst();
  void restore_peel();
  void swap_range(uint64_t lo, uint64_t hi);
  bool undo_possible() const;
  bool redo_possible() const;
};


}  // namespace nirbija
