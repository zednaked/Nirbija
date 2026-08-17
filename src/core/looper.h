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

  // Knobs and the tape travel with the session. A first pass that is still
  // open is written as a closed loop so closing the app mid-take does not
  // throw the recording away.
  std::vector<uint8_t> save_state() const override;
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

  bool recording() const {
    return record_request_.load(std::memory_order_relaxed);
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

  // The state before the last Rec or Clear, plus each phrase recorded
  // since — a phrase is a run of input between silences. Undo peels the
  // last of those, not the whole pass, so a held Rec with two licks in it
  // does not throw the first one away. Called from the UI thread with the
  // graph parked — the copy is the loop, not something process() can afford.
  void capture_undo();
  void capture_undo_empty();
  bool can_undo() const;
  bool can_redo() const;
  void undo();
  void redo();

  // Doubles the tape by appending a copy of itself, so the next pass can
  // write into the new half. UI thread, graph parked — the copy is the
  // loop, not something process() can afford.
  bool can_multiply() const;
  void multiply();

 private:
  // What the audio thread is doing right now with the loop.
  enum class Stage { Empty, Defining, Playing, Overdubbing, Stopped };

  // 0 free, 1 beat, 2 one bar, 3 two, 4 four, 5 eight, 6 = kQuantizeSync.
  static constexpr int kQuantizeMax = kQuantizeSync;

  bool at_boundary(uint32_t frames) const;
  void apply_requests(uint32_t frames);
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
  void fire_count_click(bool downbeat);
  float tone_sample(int channel, float sample);
  // Walks play_pos_ around [start, end) after a step; true if it wrapped.
  bool wrap_play_pos(double start, double end, bool reverse);
  // Stereo frames currently on the tape: the closed length, or the open
  // take if the loop has not been punched out yet.
  uint64_t tape_frames() const;
  // Copies src into buffer_, resampling when src_rate is not this instance's
  // rate. Returns how many frames landed, already clamped to capacity.
  uint64_t import_audio(const float* src, uint64_t src_frames, double src_rate);

  PluginDescriptor descriptor_;
  int channels_ = 2;
  double sample_rate_ = 48000.0;
  TransportInfo transport_;

  // Interleaved stereo, sized once in activate() for the longest loop allowed;
  // nothing ever grows on the audio thread.
  std::vector<float> buffer_;
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

  // Count-in, audio thread only. Own clock so Rec can count with Play off.
  bool counting_ = false;
  double count_phase_ = 0.0;
  int count_total_ = 0;
  uint32_t click_remaining_ = 0;
  uint32_t click_length_ = 0;
  double click_phase_ = 0.0;
  double click_step_ = 0.0;

  // One-pole lowpass on the wet loop, audio thread only.
  float tone_lpf_[2] = {0.0f, 0.0f};

  // The record state the audio thread last acted on, to spot edges.
  bool record_active_ = false;

  // Trim window and fade shape, as fractions of the closed loop. Written from
  // the UI thread while dragging a handle, read every sample on the audio
  // thread - relaxed is enough because nothing else has to happen-before a
  // trim edit landing a block late; the loop just keeps playing the old
  // window for one more block.
  std::atomic<double> trim_start_{0.0};
  std::atomic<double> trim_end_{1.0};
  std::atomic<double> fade_in_{0.0};
  std::atomic<double> fade_out_{0.0};

  // Updated once a block, for the UI to read - see position_fraction().
  std::atomic<double> position_fraction_{-1.0};

  // Frame bounds of the current trim window, clamped to the loop length
  // passed in - always length_'s current value, but process() only wants to
  // load that atomic once a sample, not three times.
  uint64_t trim_start_frames(uint64_t length) const;
  uint64_t trim_end_frames(uint64_t length, uint64_t start) const;
  // The fade multiplier at a frame already known to be inside [start, end).
  float envelope_at(uint64_t position, uint64_t start, uint64_t end) const;

  struct UndoLayer {
    std::vector<float> audio;
    uint64_t length = 0;
    double beats = 0.0;
    double trim_start = 0.0;
    double trim_end = 1.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
    bool valid = false;
    bool undone = false;
  };
  UndoLayer undo_;
  void swap_undo();

  // One byte per frame, set when Rec heard something this pass. Sized in
  // activate() with the tape so the audio thread never grows it.
  std::vector<uint8_t> recorded_;
  struct Peel {
    uint64_t start = 0;
    uint64_t end = 0;
    std::vector<float> audio;
    uint64_t dropped_length = 0;
    double dropped_beats = 0.0;
  };
  std::vector<Peel> peels_;
  bool last_burst(uint64_t* start, uint64_t* end) const;
  bool peel_last_burst();
  void restore_peel();
  void clear_recorded();
};


}  // namespace nirbija
