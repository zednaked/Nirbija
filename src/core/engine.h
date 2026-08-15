#pragma once

#include <jack/jack.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/audio_graph.h"
#include "core/recorder.h"
#include "core/rt_queue.h"

namespace nirbija {

// A parameter edit travelling from the UI thread to the audio thread. Anything
// that would allocate is built before the message is pushed.
struct EngineCommand {
  enum class Kind {
    None,
    SetGain,
    SetPan,
    SetMute,
    SetSolo,
    SetMasterGain,
    SetPlaying,
    SetTempo,
    Rewind,
    SetMetronome,
    SetTimeSig,
    InjectMidi,
  } kind = Kind::None;
  size_t channel = 0;
  // Buses live in their own list in the graph, so the index alone is ambiguous.
  bool bus = false;
  float value = 0.0f;
};

// Owns the JACK client and the live graph.
class Engine {
 public:
  Engine();
  ~Engine();

  bool start(const std::string& client_name);
  void stop();
  bool running() const { return client_ != nullptr; }

  double sample_rate() const { return sample_rate_; }
  uint32_t block_frames() const { return block_frames_; }

  AudioGraph& graph() { return *graph_; }
  const AudioGraph& graph() const { return *graph_; }

  // For wiring ports from outside, which is how a session restores its
  // connections and how the tests plug a sender in.
  jack_client_t* client() const { return client_; }

  // --- routing ------------------------------------------------------------
  // Everything a channel can be fed from, as JACK port names. `midi` picks
  // between MIDI sources and audio ones; `physical_only` narrows it to hardware.
  std::vector<std::string> available_sources(bool midi, bool physical_only = false) const;

  // Everything the master bus can be sent to.
  std::vector<std::string> available_sinks(bool physical_only = false) const;

  // Replaces whatever the channel was listening to. An empty port name just
  // disconnects. A stereo channel takes two consecutive source ports when the
  // source has them, which is what picking "an input" means to a person.
  bool connect_source(size_t channel, const std::string& port, bool midi);

  // What a channel is currently fed from, empty when nothing is connected.
  std::string current_source(size_t channel, bool midi) const;
  std::vector<std::string> current_sources(size_t channel, bool midi) const;

  // One matrix cell: adds or removes a single MIDI connection without touching
  // the channel's other sources — many-to-many, unlike connect_source.
  bool set_midi_link(size_t channel, const std::string& port, bool on);

  bool connect_master(const std::string& left, const std::string& right);
  std::string current_master_sink() const;

  // Direct hardware out for a channel, independent of its mix destination.
  bool connect_channel_sink(size_t channel, const std::string& port);
  std::string current_channel_sink(size_t channel) const;

  // A later strip that reads extra stereo pair `pair` of the plugin on
  // `source` (0 = first pair beyond the source strip's own width).
  size_t add_tap_channel(size_t source, int pair);

  void inject_midi(size_t channel, const MidiEvent& event);
  void set_midi_clock(bool on) { clock_enabled_.store(on, std::memory_order_relaxed); }
  bool midi_clock() const { return clock_enabled_.load(std::memory_order_relaxed); }
  void set_time_signature(int num, int den);

  // Wires the master to the system's default output, the way a mixer that just
  // opened is expected to already be audible.
  bool connect_master_to_default_output();

  // --- control surface ------------------------------------------------------
  // A dedicated MIDI port for controllers driving the mixer itself, separate
  // from the channels' note inputs. The audio thread queues what arrives; the
  // UI drains it on its poll and applies the mappings there, so a mapping can
  // reach anything the UI can without the audio thread touching it.
  size_t poll_control(MidiEvent* out, size_t capacity);

  // Wires every MIDI source into the control port. Called when learning
  // starts, so "move a knob" works without a routing step first.
  void connect_all_midi_to_control();

  // --- recording ----------------------------------------------------------
  // Records every armed channel plus the master, one file each, into a new
  // folder under `directory`. Returns the folder, or an empty string if the
  // recording could not be started.
  std::string start_recording(const std::string& directory);
  void stop_recording();
  bool recording() const { return recorder_.recording(); }
  bool recording_overran() const { return recorder_.overran(); }
  double recorded_seconds() const;

  // --- transport ----------------------------------------------------------
  // Read from the UI thread; the audio thread owns the writing.
  bool playing() const { return playing_.load(std::memory_order_relaxed); }
  double tempo() const { return tempo_.load(std::memory_order_relaxed); }
  bool metronome() const { return metronome_.load(std::memory_order_relaxed); }
  uint64_t transport_frame() const { return transport_frame_.load(std::memory_order_relaxed); }
  int time_numerator() const { return time_num_.load(std::memory_order_relaxed); }
  int time_denominator() const { return time_den_.load(std::memory_order_relaxed); }

  // UI thread. Registers this channel's JACK input ports and adds the strip.
  // Returns the channel index, or kMaxChannels if the graph is full or the
  // ports could not be registered.
  size_t add_channel(const std::string& name, int channel_count);

  // UI thread. Stops rendering the channel and disconnects its ports. The ports
  // stay registered: a render pass already inside the channel would still be
  // reading them, and an unused port is cheaper than that risk.
  void remove_channel(size_t channel);

  // A bus needs no ports: it is fed by the channels that point at it.
  size_t add_bus(const std::string& name) { return graph_->add_bus(name); }
  void remove_bus(size_t bus) { graph_->remove_bus(bus); }

  // Park the graph (silence + two observed blocks) so state I/O and recorder
  // teardown cannot race process().
  void park_graph();
  void unpark_graph();

  // UI thread. Returns false if the queue is full, meaning the audio thread has
  // stalled — the caller should surface that, not silently retry.
  bool post(const EngineCommand& command);

 private:
  static int jack_process_trampoline(jack_nframes_t frames, void* arg);
  static int jack_buffer_size_trampoline(jack_nframes_t frames, void* arg);
  static int jack_sample_rate_trampoline(jack_nframes_t rate, void* arg);
  int process(jack_nframes_t frames);
  void drain_commands();

  // Ports are kept per channel so routing can be changed later; the sources
  // inside the graph only ever read from them.
  struct ChannelPorts {
    jack_port_t* audio[2] = {nullptr, nullptr};
    jack_port_t* audio_out[2] = {nullptr, nullptr};
    jack_port_t* midi = nullptr;
  };

  struct InjectedMidi {
    size_t channel = 0;
    MidiEvent event;
  };

  std::vector<std::string> ports_matching(unsigned long flags, const char* type,
                                          bool physical_only) const;

  jack_client_t* client_ = nullptr;
  jack_port_t* master_out_[2] = {nullptr, nullptr};
  jack_port_t* control_in_ = nullptr;
  jack_port_t* clock_out_ = nullptr;
  RtQueue<MidiEvent, 256> control_events_;
  RtQueue<InjectedMidi, 256> injected_midi_;
  std::vector<ChannelPorts> channel_ports_;

  double sample_rate_ = 0.0;
  uint32_t block_frames_ = 0;

  std::unique_ptr<AudioGraph> graph_;
  Recorder recorder_;

  std::atomic<bool> playing_{false};
  std::atomic<double> tempo_{120.0};
  std::atomic<bool> metronome_{false};
  std::atomic<bool> clock_enabled_{false};
  std::atomic<int> time_num_{4};
  std::atomic<int> time_den_{4};
  std::atomic<uint64_t> transport_frame_{0};

  // Click synthesis state, audio thread only.
  uint32_t click_remaining_ = 0;
  uint32_t click_length_ = 0;
  double click_phase_ = 0.0;
  double click_step_ = 0.0;

  void render_metronome(float* const* master, uint32_t frames, bool playing,
                        double tempo, double start_beats);
  double clock_phase_ = 0.0;
  bool transport_changed_ = true;
  RtQueue<EngineCommand, 1024> commands_;
};

}  // namespace nirbija
