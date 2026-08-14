#pragma once

#include <jack/jack.h>

#include <memory>
#include <string>
#include <vector>

#include "core/audio_graph.h"
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
  } kind = Kind::None;
  size_t channel = 0;
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

  bool connect_master(const std::string& left, const std::string& right);
  std::string current_master_sink() const;

  // Wires the master to the system's default output, the way a mixer that just
  // opened is expected to already be audible.
  bool connect_master_to_default_output();

  // UI thread. Registers this channel's JACK input ports and adds the strip.
  // Returns the channel index, or kMaxChannels if the graph is full or the
  // ports could not be registered.
  size_t add_channel(const std::string& name, int channel_count);

  // UI thread. Stops rendering the channel and disconnects its ports. The ports
  // stay registered: a render pass already inside the channel would still be
  // reading them, and an unused port is cheaper than that risk.
  void remove_channel(size_t channel);

  // UI thread. Returns false if the queue is full, meaning the audio thread has
  // stalled — the caller should surface that, not silently retry.
  bool post(const EngineCommand& command);

 private:
  static int jack_process_trampoline(jack_nframes_t frames, void* arg);
  static int jack_buffer_size_trampoline(jack_nframes_t frames, void* arg);
  int process(jack_nframes_t frames);
  void drain_commands();

  // Ports are kept per channel so routing can be changed later; the sources
  // inside the graph only ever read from them.
  struct ChannelPorts {
    jack_port_t* audio[2] = {nullptr, nullptr};
    jack_port_t* midi = nullptr;
  };

  std::vector<std::string> ports_matching(unsigned long flags, const char* type,
                                          bool physical_only) const;

  jack_client_t* client_ = nullptr;
  jack_port_t* master_out_[2] = {nullptr, nullptr};
  std::vector<ChannelPorts> channel_ports_;

  double sample_rate_ = 0.0;
  uint32_t block_frames_ = 0;

  std::unique_ptr<AudioGraph> graph_;
  RtQueue<EngineCommand, 1024> commands_;
};

}  // namespace nirbija
