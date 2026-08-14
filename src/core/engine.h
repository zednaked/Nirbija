#pragma once

#include <jack/jack.h>

#include <memory>
#include <string>

#include "core/audio_graph.h"
#include "core/rt_queue.h"

namespace nirbija {

// A structural or parameter edit travelling from the UI thread to the audio
// thread. Anything that would allocate is built before the message is pushed.
struct EngineCommand {
  enum class Kind { None, SetGain, SetPan, SetMute, SetSolo } kind = Kind::None;
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

  // UI thread. Returns false if the queue is full, meaning the audio thread has
  // stalled — the caller should surface that, not silently retry.
  bool post(const EngineCommand& command);

 private:
  static int jack_process_trampoline(jack_nframes_t frames, void* arg);
  int process(jack_nframes_t frames);
  void drain_commands();

  jack_client_t* client_ = nullptr;
  jack_port_t* master_out_[2] = {nullptr, nullptr};

  double sample_rate_ = 0.0;
  uint32_t block_frames_ = 0;

  std::unique_ptr<AudioGraph> graph_;
  RtQueue<EngineCommand, 1024> commands_;
};

}  // namespace nirbija
