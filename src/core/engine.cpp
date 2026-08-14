#include "core/engine.h"

#include <algorithm>

namespace nirbija {
namespace {

// Reads a channel's audio straight out of its JACK input ports. Only ever used
// from inside the process callback, where jack_port_get_buffer is realtime-safe.
class JackInputSource : public AudioSource {
 public:
  JackInputSource(jack_port_t* left, jack_port_t* right)
      : ports_{left, right} {}

  void read(float* const* dest, int channels, uint32_t frames) override {
    for (int ch = 0; ch < channels; ++ch) {
      const auto* input = static_cast<const float*>(
          jack_port_get_buffer(ports_[ch], frames));
      if (input != nullptr) {
        std::copy_n(input, frames, dest[ch]);
      } else {
        std::fill_n(dest[ch], frames, 0.0f);
      }
    }
  }

 private:
  jack_port_t* ports_[2];
};

}  // namespace

Engine::Engine() : graph_(std::make_unique<AudioGraph>()) {}

Engine::~Engine() { stop(); }

bool Engine::start(const std::string& client_name) {
  jack_status_t status;
  client_ = jack_client_open(client_name.c_str(), JackNoStartServer, &status);
  if (client_ == nullptr) return false;

  sample_rate_ = jack_get_sample_rate(client_);
  block_frames_ = jack_get_buffer_size(client_);
  graph_->prepare(sample_rate_, block_frames_);

  master_out_[0] = jack_port_register(client_, "master_out_l",
                                      JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  master_out_[1] = jack_port_register(client_, "master_out_r",
                                      JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  if (master_out_[0] == nullptr || master_out_[1] == nullptr) {
    stop();
    return false;
  }

  jack_set_process_callback(client_, jack_process_trampoline, this);
  jack_set_buffer_size_callback(client_, jack_buffer_size_trampoline, this);
  if (jack_activate(client_) != 0) {
    stop();
    return false;
  }
  return true;
}

void Engine::stop() {
  if (client_ == nullptr) return;
  jack_deactivate(client_);
  jack_client_close(client_);
  client_ = nullptr;
  master_out_[0] = master_out_[1] = nullptr;
}

size_t Engine::add_channel(const std::string& name, int channel_count) {
  if (client_ == nullptr) return kMaxChannels;

  // Ports are named by index, not by the channel's display name: two channels
  // may share a name, but JACK port names must be unique.
  const size_t index = graph_->channel_count();
  jack_port_t* ports[2] = {nullptr, nullptr};
  for (int ch = 0; ch < channel_count; ++ch) {
    const std::string port_name =
        std::to_string(index + 1) +
        (channel_count == 1 ? "_in" : (ch == 0 ? "_in_l" : "_in_r"));
    ports[ch] = jack_port_register(client_, port_name.c_str(),
                                   JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    if (ports[ch] == nullptr) {
      // Roll back the ports already claimed so a failed add leaves no stragglers.
      for (int done = 0; done < ch; ++done) jack_port_unregister(client_, ports[done]);
      return kMaxChannels;
    }
  }

  return graph_->add_channel(name, channel_count,
                             std::make_unique<JackInputSource>(ports[0], ports[1]));
}

bool Engine::post(const EngineCommand& command) { return commands_.push(command); }

int Engine::jack_process_trampoline(jack_nframes_t frames, void* arg) {
  return static_cast<Engine*>(arg)->process(frames);
}

int Engine::jack_buffer_size_trampoline(jack_nframes_t frames, void* arg) {
  // JACK guarantees this runs with the process callback stopped, so reallocating
  // the graph's scratch here is safe.
  auto* engine = static_cast<Engine*>(arg);
  engine->block_frames_ = frames;
  engine->graph_->prepare(engine->sample_rate_, frames);
  return 0;
}

void Engine::drain_commands() {
  EngineCommand command;
  while (commands_.pop(command)) {
    if (command.kind == EngineCommand::Kind::SetMasterGain) {
      graph_->set_master_gain(command.value);
      continue;
    }
    if (command.channel >= graph_->channel_count()) continue;
    ChannelStrip& strip = graph_->channel(command.channel);
    switch (command.kind) {
      case EngineCommand::Kind::SetGain: strip.set_gain(command.value); break;
      case EngineCommand::Kind::SetPan: strip.set_pan(command.value); break;
      case EngineCommand::Kind::SetMute: strip.set_muted(command.value != 0.0f); break;
      case EngineCommand::Kind::SetSolo: strip.set_soloed(command.value != 0.0f); break;
      case EngineCommand::Kind::SetMasterGain:
      case EngineCommand::Kind::None:
        break;
    }
  }
}

int Engine::process(jack_nframes_t frames) {
  drain_commands();

  float* master[2];
  for (int ch = 0; ch < 2; ++ch)
    master[ch] = static_cast<float*>(jack_port_get_buffer(master_out_[ch], frames));

  graph_->render(master, frames);
  return 0;
}

}  // namespace nirbija
