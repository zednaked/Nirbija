#include "core/engine.h"

#include <jack/midiport.h>

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

// Reads a channel's MIDI straight out of its JACK port. Like the audio source,
// it only runs inside the process callback.
class JackMidiSource : public MidiSource {
 public:
  explicit JackMidiSource(jack_port_t* port) : port_(port) {}

  size_t read(MidiEvent* out, size_t capacity, uint32_t frames) override {
    void* buffer = jack_port_get_buffer(port_, frames);
    if (buffer == nullptr) return 0;

    const jack_nframes_t count = jack_midi_get_event_count(buffer);
    size_t written = 0;
    for (jack_nframes_t i = 0; i < count && written < capacity; ++i) {
      jack_midi_event_t event;
      if (jack_midi_event_get(&event, buffer, i) != 0) continue;
      // Anything longer than three bytes is SysEx, which nothing downstream
      // takes yet; dropping it beats truncating it into a bogus message.
      if (event.size == 0 || event.size > 3) continue;

      MidiEvent& target = out[written++];
      target.frame = event.time;
      target.size = static_cast<uint8_t>(event.size);
      std::copy_n(event.buffer, event.size, target.data);
    }
    return written;
  }

 private:
  jack_port_t* port_;
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

  // Every channel gets a MIDI input too, so a synth can be dropped into any
  // strip without rebuilding it. AUM does the same: MIDI is routed to a
  // channel, not to a special kind of channel.
  const std::string midi_name = std::to_string(index + 1) + "_midi_in";
  jack_port_t* midi_port = jack_port_register(client_, midi_name.c_str(),
                                              JACK_DEFAULT_MIDI_TYPE,
                                              JackPortIsInput, 0);
  if (midi_port == nullptr) {
    for (int ch = 0; ch < channel_count; ++ch)
      jack_port_unregister(client_, ports[ch]);
    return kMaxChannels;
  }

  ChannelPorts record;
  record.audio[0] = ports[0];
  record.audio[1] = ports[1];
  record.midi = midi_port;
  channel_ports_.push_back(record);

  return graph_->add_channel(name, channel_count,
                             std::make_unique<JackInputSource>(ports[0], ports[1]),
                             std::make_unique<JackMidiSource>(midi_port));
}

void Engine::remove_channel(size_t channel) {
  if (client_ == nullptr || channel >= channel_ports_.size()) return;

  const ChannelPorts& record = channel_ports_[channel];
  for (jack_port_t* port : record.audio)
    if (port != nullptr) jack_port_disconnect(client_, port);
  if (record.midi != nullptr) jack_port_disconnect(client_, record.midi);

  graph_->remove_channel(channel);
}

std::vector<std::string> Engine::ports_matching(unsigned long flags,
                                                const char* type,
                                                bool physical_only) const {
  if (client_ == nullptr) return {};

  if (physical_only) flags |= JackPortIsPhysical;
  const char** ports = jack_get_ports(client_, nullptr, type, flags);

  std::vector<std::string> found;
  for (const char** port = ports; port != nullptr && *port != nullptr; ++port) {
    const std::string name = *port;
    // Our own ports are never worth offering: connecting the mixer to itself
    // is either a no-op or a feedback loop.
    if (name.rfind(jack_get_client_name(client_), 0) == 0) continue;
    found.push_back(name);
  }
  if (ports != nullptr) jack_free(ports);
  return found;
}

std::vector<std::string> Engine::available_sources(bool midi,
                                                   bool physical_only) const {
  return ports_matching(JackPortIsOutput,
                        midi ? JACK_DEFAULT_MIDI_TYPE : JACK_DEFAULT_AUDIO_TYPE,
                        physical_only);
}

std::vector<std::string> Engine::available_sinks(bool physical_only) const {
  return ports_matching(JackPortIsInput, JACK_DEFAULT_AUDIO_TYPE, physical_only);
}

bool Engine::connect_source(size_t channel, const std::string& port, bool midi) {
  if (client_ == nullptr || channel >= channel_ports_.size()) return false;
  const ChannelPorts& record = channel_ports_[channel];

  // Picking a source replaces the old one rather than stacking on top of it,
  // which is what choosing an input in a mixer means.
  const int count = midi ? 1 : (record.audio[1] != nullptr ? 2 : 1);
  for (int i = 0; i < count; ++i) {
    jack_port_t* target = midi ? record.midi : record.audio[i];
    if (target != nullptr) jack_port_disconnect(client_, target);
  }
  if (port.empty()) return true;

  if (midi) return jack_connect(client_, port.c_str(), jack_port_name(record.midi)) == 0;

  // A stereo channel fed from a stereo source should take both sides. The
  // sibling is the next port of the same client, which is how JACK names them.
  std::vector<std::string> siblings = available_sources(false);
  const auto chosen = std::find(siblings.begin(), siblings.end(), port);

  bool ok = jack_connect(client_, port.c_str(), jack_port_name(record.audio[0])) == 0;
  if (count == 2 && chosen != siblings.end() && std::next(chosen) != siblings.end()) {
    const std::string& next = *std::next(chosen);
    const std::string client_of = port.substr(0, port.find(':'));
    if (next.rfind(client_of, 0) == 0)
      ok = jack_connect(client_, next.c_str(), jack_port_name(record.audio[1])) == 0 && ok;
  }
  return ok;
}

std::string Engine::current_source(size_t channel, bool midi) const {
  if (client_ == nullptr || channel >= channel_ports_.size()) return {};
  jack_port_t* port = midi ? channel_ports_[channel].midi
                           : channel_ports_[channel].audio[0];
  if (port == nullptr) return {};

  const char** connections = jack_port_get_all_connections(client_, port);
  std::string found;
  if (connections != nullptr && connections[0] != nullptr) found = connections[0];
  if (connections != nullptr) jack_free(connections);
  return found;
}

bool Engine::connect_master(const std::string& left, const std::string& right) {
  if (client_ == nullptr) return false;
  for (jack_port_t* port : master_out_)
    if (port != nullptr) jack_port_disconnect(client_, port);
  if (left.empty()) return true;

  bool ok = jack_connect(client_, jack_port_name(master_out_[0]), left.c_str()) == 0;
  if (!right.empty())
    ok = jack_connect(client_, jack_port_name(master_out_[1]), right.c_str()) == 0 && ok;
  return ok;
}

std::string Engine::current_master_sink() const {
  if (client_ == nullptr || master_out_[0] == nullptr) return {};
  const char** connections = jack_port_get_all_connections(client_, master_out_[0]);
  std::string found;
  if (connections != nullptr && connections[0] != nullptr) found = connections[0];
  if (connections != nullptr) jack_free(connections);
  return found;
}

bool Engine::connect_master_to_default_output() {
  // Physical playback ports come back in the server's own order, so the first
  // pair is the default output.
  const std::vector<std::string> sinks = available_sinks(true);
  if (sinks.empty()) return false;
  return connect_master(sinks[0], sinks.size() > 1 ? sinks[1] : std::string());
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
    // Transport commands are not about any one channel, and each of them makes
    // the next block a discontinuity the plugins have to be told about.
    if (command.kind == EngineCommand::Kind::SetPlaying) {
      playing_.store(command.value != 0.0f, std::memory_order_relaxed);
      transport_changed_ = true;
      continue;
    }
    if (command.kind == EngineCommand::Kind::SetTempo) {
      if (command.value > 0.0f) tempo_.store(command.value, std::memory_order_relaxed);
      transport_changed_ = true;
      continue;
    }
    if (command.kind == EngineCommand::Kind::Rewind) {
      transport_frame_ = 0;
      transport_changed_ = true;
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
      case EngineCommand::Kind::SetPlaying:
      case EngineCommand::Kind::SetTempo:
      case EngineCommand::Kind::Rewind:
      case EngineCommand::Kind::None:
        break;
    }
  }
}

int Engine::process(jack_nframes_t frames) {
  drain_commands();

  const bool playing = playing_.load(std::memory_order_relaxed);
  const double tempo = tempo_.load(std::memory_order_relaxed);

  TransportInfo transport;
  transport.playing = playing;
  transport.tempo_bpm = tempo;
  transport.frame = transport_frame_;
  transport.seconds = sample_rate_ > 0.0
                          ? static_cast<double>(transport_frame_) / sample_rate_
                          : 0.0;
  transport.beats = transport.seconds * tempo / 60.0;
  transport.changed = transport_changed_;
  transport_changed_ = false;

  graph_->set_transport(transport);

  float* master[2];
  for (int ch = 0; ch < 2; ++ch)
    master[ch] = static_cast<float*>(jack_port_get_buffer(master_out_[ch], frames));

  graph_->render(master, frames);

  // The clock only moves while playing; stopped means parked, not paused
  // somewhere the plugins cannot see.
  if (playing) transport_frame_ += frames;
  return 0;
}

}  // namespace nirbija
