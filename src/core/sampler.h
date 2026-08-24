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
  void set_pad(int pad, int note, bool one_shot, float volume, float pan,
               float pitch);

  std::string pad_name(int pad) const;
  int pad_note(int pad) const;
  bool pad_one_shot(int pad) const;
  float pad_volume(int pad) const;
  float pad_pan(int pad) const;
  float pad_pitch(int pad) const;
  double pad_start(int pad) const;
  double pad_end(int pad) const;
  bool pad_has_audio(int pad) const;

  bool recording() const {
    return recording_.load(std::memory_order_relaxed);
  }
  int rec_pad() const { return rec_pad_.load(std::memory_order_relaxed); }
  int focused() const { return focused_.load(std::memory_order_relaxed); }
  uint32_t sounding_mask() const {
    return sounding_mask_.load(std::memory_order_relaxed);
  }

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
    std::string name;
    std::string path;  // UI thread; empty when the pad is a Rec take
  };

  struct Voice {
    int pad = -1;
    Buffer* buffer = nullptr;
    double position = 0.0;
    double step = 1.0;
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
