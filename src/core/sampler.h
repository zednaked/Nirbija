#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/plugin.h"
#include "core/rt_queue.h"

namespace nirbija {

// A 16-pad sampler that lives in an insert slot. The first built-in
// instrument: MIDI in, audio out, and the strip's own input is what Rec
// writes onto a pad. Load a file the same way the File Player does, or
// punch Rec and play whatever just came in.
//
// This is Koala's gesture, not sfizz's. A pad is a take, not a zone in an
// SFZ. `note_names()` publishes the sixteen, so a step sequencer sitting
// above this chip labels its lanes Kick/Snare instead of MIDI 36.
//
// Rec writes into a preallocated ring on the audio thread. The take is
// committed on the UI thread (`commit_take()`), because publishing a new
// buffer allocates. The mixer's idle poll does that; tests call it
// themselves after Rec drops.
class SamplerInstance : public PluginInstance {
 public:
  static constexpr int kPads = 16;
  static constexpr double kMaxSeconds = 8.0;

  SamplerInstance();
  ~SamplerInstance() override;

  static PluginDescriptor make_descriptor();

  // UI thread. Decodes the file into the pad, stereo, keeping the file's
  // own rate so playback resamples. False leaves the pad as it was.
  bool load(int pad, const std::string& path);

  // UI thread. If Rec has just closed a take, copy it onto that pad.
  // False means there was nothing waiting.
  bool commit_take();

  // A pad loaded from a file remembers the path, like the File Player, so a
  // session is a list of files rather than megabytes of floats. Rec takes
  // have no path and travel inside the blob. Relative paths are resolved
  // against `base_dir` — the folder the session file lives in.
  std::string pad_path(int pad) const;
  void resolve_paths(std::string_view base_dir);

  void clear_pad(int pad);
  void set_pad_name(int pad, std::string name);
  void set_trim(int pad, double start, double end);
  // Fractions of the trim window, each capped at half of it — the same
  // shape as LooperInstance's fade, dragged the same way in the editor.
  void set_fades(int pad, double fade_in, double fade_out);
  void set_pad(int pad, int note, bool one_shot, float volume, float pan,
               float pitch);
  // Give this pad a MIDI note. If another pad already has it, they swap —
  // a controller's sixteen pads can be wired onto the sixteen without two
  // of them sharing a key and the first one swallowing every hit.
  void assign_note(int pad, int note);

  // UI thread. Puts back whatever was on the pad before its last Rec, Load
  // or Clear. A second call swaps forward again — a toggle, not a history,
  // because what a pad wants after a bad take is "let me hear the old one",
  // not a stack to dig through.
  bool undo_pad(int pad);
  bool pad_can_undo(int pad) const;

  // A bar of clicks before Rec actually punches in, so the take does not
  // start with the button being pressed. Beats still to go, 0 when no
  // count is running — the editor polls this the way it polls `recording()`.
  int count_in_beats_left() const {
    return count_in_left_.load(std::memory_order_relaxed);
  }
  bool counting_in() const { return count_in_beats_left() > 0; }

  std::string pad_name(int pad) const;
  int pad_note(int pad) const;
  bool pad_one_shot(int pad) const;
  float pad_volume(int pad) const;
  float pad_pan(int pad) const;
  float pad_pitch(int pad) const;
  double pad_start(int pad) const;
  double pad_end(int pad) const;
  double pad_fade_in(int pad) const;
  double pad_fade_out(int pad) const;
  bool pad_has_audio(int pad) const;

  bool recording() const {
    return recording_.load(std::memory_order_relaxed);
  }
  int rec_pad() const { return rec_pad_.load(std::memory_order_relaxed); }
  int focused() const { return focused_.load(std::memory_order_relaxed); }
  uint32_t sounding_mask() const {
    return sounding_mask_.load(std::memory_order_relaxed);
  }
  // A brief flash per pad on any note-on that names it, whether or not the
  // pad has audio to actually play — the editor's confirmation that a MIDI
  // controller's note reached the right pad, for wiring one up by ear
  // (or by eye) rather than by the manual. Unlike sounding_mask(), an
  // empty pad flashes too.
  uint32_t hit_flash_mask() const {
    return hit_flash_mask_.load(std::memory_order_relaxed);
  }
  // Last note-on that reached this chip, whether or not a pad owns it.
  // -1 if nothing has arrived yet, or the last message was a CC.
  int last_midi_note() const {
    return last_midi_note_.load(std::memory_order_relaxed);
  }
  int last_midi_cc() const {
    return last_midi_cc_.load(std::memory_order_relaxed);
  }

  // UI thread. A controller note that did not come in through the strip —
  // the editor listens on the mixer's control port so a pad still lights
  // (and plays) when the SMC-PAD is wired to a different channel.
  void hear_note(int note, int velocity, bool down);
  void hear_cc(int cc);

  // One peak per bucket across the pad's audio, for the editor waveform.
  // Same unguarded-read tradeoff as LooperInstance::waveform().
  std::vector<float> waveform(int pad, int buckets) const;

  // UI thread: tap a pad in the editor. Goes through a queue, not a lock.
  void preview_down(int pad, int velocity);
  void preview_up(int pad);

  // PluginInstance ------------------------------------------------------------
  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double sample_rate, uint32_t max_block_frames) override;
  void deactivate() override;

  void set_transport(const TransportInfo& transport) override {
    transport_ = transport;
  }
  void queue_midi(const MidiEvent& event) override;
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override;

  std::vector<ParameterInfo> parameters() const override;
  double parameter_value(uint32_t id) const override;
  void set_parameter(uint32_t id, double value) override;

  std::vector<NoteName> note_names() const override;

  std::vector<uint8_t> save_state() const override;
  bool load_state(const std::vector<uint8_t>& blob) override;

  // O host diz se existe thread de audio: sem ela o portao de geracao nunca
  // abre e nada aposentado seria liberado. Ver PluginInstance.
  void reclaim_retired(bool audio_running) override;

  // Quantos buffers trocados ainda esperam para ser liberados. Existe para o
  // teste poder afirmar o contrato em si - "nada ficou esperando" - em vez de
  // um substituto como RSS, que nao cai sob o AddressSanitizer porque ele
  // mantem em quarentena o que foi liberado.
  size_t retired_count() const { return retired_.size(); }

  // Just the pads: names, tuning and audio, not the instrument's own gain,
  // quantize or count-in — what "swap the kit" means without also resetting
  // how Rec behaves. A different shape from save_state()/load_state(), which
  // speak for the whole instrument the way a session wants it to.
  std::vector<uint8_t> save_pads() const;
  bool load_pads(const std::vector<uint8_t>& blob);

  const PluginDescriptor& descriptor() const override { return descriptor_; }

  // Interleaved stereo at the file's (or the rec take's) own rate. Public so
  // the decoder in the .cpp can name it without being a member.
  struct Buffer {
    std::vector<float> samples;
    uint64_t frames = 0;
    double sample_rate = 48000.0;
  };

 private:
  static constexpr size_t kMaxEvents = 64;
  static constexpr size_t kQueueCapacity = 256;
  static constexpr int kFade = 64;

  struct RetiredBuffer {
    std::shared_ptr<Buffer> buffer;
    uint64_t generation = 0;
  };

  struct Pad {
    std::shared_ptr<Buffer> owned;
    std::atomic<Buffer*> live{nullptr};
    std::atomic<int> note{36};
    std::atomic<bool> one_shot{true};
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> pitch{0.0f};
    std::atomic<double> start{0.0};
    std::atomic<double> end{1.0};
    std::atomic<double> fade_in{0.0};
    std::atomic<double> fade_out{0.0};
    std::string name;
    std::string path;  // UI thread; empty when the pad is a Rec take

    // UI thread only: what the pad held before its last Rec, Load or
    // Clear, for undo_pad() to swap back in. Not part of the session —
    // a safety net for the take you are on, not something worth saving.
    std::shared_ptr<Buffer> undo_owned;
    std::string undo_path;
    double undo_start = 0.0;
    double undo_end = 1.0;
    double undo_fade_in = 0.0;
    double undo_fade_out = 0.0;
    bool has_undo = false;
  };

  struct Voice {
    int pad = -1;
    Buffer* buffer = nullptr;
    double position = 0.0;
    double step = 1.0;
    uint64_t start_frame = 0;
    uint64_t end_frame = 0;
    float gain_l = 1.0f;
    float gain_r = 1.0f;
    int attack = 0;
    int release = kFade;
    bool releasing = false;
  };

  struct Preview {
    uint8_t pad = 0;
    uint8_t velocity = 100;
    bool down = false;
  };

  void publish(int pad, std::shared_ptr<Buffer> buffer);
  void start_voice(int pad, int velocity, uint32_t frame);
  void release_voice(int pad);
  void chase_pad(int pad);
  void handle_midi(const MidiEvent& event);
  int pad_for_note(int note) const;
  void seed_defaults();
  void apply_focused_param(uint32_t id, double value);
  void stash_undo(int pad);
  void fire_count_click(bool downbeat);
  void flash_pad(int pad);
  float fade_gain(int pad, uint64_t position, uint64_t start,
                  uint64_t end) const;
  void write_pads_to(std::vector<uint8_t>& out) const;
  bool read_pads_from(const uint8_t*& cursor, const uint8_t* end,
                      int pad_count, bool has_path_field);

  PluginDescriptor descriptor_;
  std::array<Pad, kPads> pads_{};
  std::vector<RetiredBuffer> retired_;
  std::atomic<uint64_t> process_generation_{0};

  double engine_rate_ = 48000.0;
  int channels_ = 2;
  TransportInfo transport_{};

  std::atomic<float> gain_{1.0f};
  std::atomic<int> focused_{0};
  std::atomic<int> quantize_{0};  // 0 off, 1 beat, 2 bar
  std::atomic<bool> rec_request_{false};
  std::atomic<bool> recording_{false};
  std::atomic<int> rec_pad_{0};
  std::atomic<uint32_t> stop_mask_{0};
  std::atomic<uint32_t> sounding_mask_{0};

  // Hit-flash: audio thread only except the published mask. One countdown
  // per pad, set to a fixed duration on any matching note-on and ticked
  // down by the block size each process() call.
  std::array<int, kPads> hit_flash_frames_{};
  std::atomic<uint32_t> hit_flash_mask_{0};
  std::atomic<uint32_t> ping_mask_{0};
  std::atomic<int> last_midi_note_{-1};
  std::atomic<int> last_midi_cc_{-1};

  // Count-in ahead of Rec. counting_ and the click generator are audio
  // thread only, driven each block by the count_in_ atomic the UI writes;
  // count_in_left_ mirrors the beats still to go for the editor to poll.
  std::atomic<bool> count_in_{false};
  bool counting_ = false;
  double count_phase_ = 0.0;
  int count_total_ = 0;
  std::atomic<int> count_in_left_{0};
  uint32_t click_remaining_ = 0;
  uint32_t click_length_ = 0;
  double click_phase_ = 0.0;
  double click_step_ = 0.0;

  // Audio thread writes, UI thread copies after Rec drops. Sized in activate.
  std::vector<float> rec_buffer_;
  uint64_t rec_capacity_ = 0;
  uint64_t rec_written_ = 0;  // audio thread while recording_
  std::atomic<uint64_t> rec_ready_frames_{0};
  std::atomic<int> rec_ready_pad_{-1};
  bool rec_waiting_ = false;  // audio: quantised start not yet crossed

  std::array<MidiEvent, kMaxEvents> incoming_{};
  size_t incoming_count_ = 0;
  RtQueue<Preview, kQueueCapacity> preview_;

  std::array<Voice, kPads> voices_{};  // audio thread only
};
}  // namespace nirbija
