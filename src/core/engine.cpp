#include "core/engine.h"

namespace nirbija {

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

bool Engine::post(const EngineCommand& command) { return commands_.push(command); }

int Engine::jack_process_trampoline(jack_nframes_t frames, void* arg) {
  return static_cast<Engine*>(arg)->process(frames);
}

void Engine::drain_commands() {
  EngineCommand command;
  while (commands_.pop(command)) {
    if (command.channel >= graph_->channel_count()) continue;
    ChannelStrip& strip = graph_->channel(command.channel);
    switch (command.kind) {
      case EngineCommand::Kind::SetGain: strip.set_gain(command.value); break;
      case EngineCommand::Kind::SetPan: strip.set_pan(command.value); break;
      case EngineCommand::Kind::SetMute: strip.set_muted(command.value != 0.0f); break;
      case EngineCommand::Kind::SetSolo: strip.set_soloed(command.value != 0.0f); break;
      case EngineCommand::Kind::None: break;
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
