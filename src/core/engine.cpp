#include "core/engine.h"

#include <jack/midiport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

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
      if (ports_[ch] == nullptr) {
        std::fill_n(dest[ch], frames, 0.0f);
        continue;
      }
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
class TapSource : public AudioSource {
 public:
  TapSource(AudioGraph* graph, size_t source, int pair)
      : graph_(graph), source_(source), pair_(pair) {}

  // Sized once here rather than on the stack: this runs on the JACK realtime
  // thread, whose stack is far smaller than the two 32 KB arrays this used to
  // put on it.
  void prepare(uint32_t max_block_frames) override {
    if (max_block_frames <= left_.size()) return;  // grow only, never shrink
    left_.assign(max_block_frames, 0.0f);
    right_.assign(max_block_frames, 0.0f);
  }

  void read(float* const* dest, int channels, uint32_t frames) override {
    const uint32_t n =
        std::min(frames, static_cast<uint32_t>(left_.size()));
    graph_->copy_tap(source_, pair_, left_.data(), right_.data(), n);
    for (uint32_t i = 0; i < n; ++i) {
      dest[0][i] = left_[i];
      if (channels > 1) dest[1][i] = right_[i];
    }
    if (n < frames) {
      std::fill_n(dest[0] + n, frames - n, 0.0f);
      if (channels > 1) std::fill_n(dest[1] + n, frames - n, 0.0f);
    }
  }

 private:
  AudioGraph* graph_;
  size_t source_;
  int pair_;
  std::vector<float> left_, right_;
};

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

  control_in_ = jack_port_register(client_, "control_in", JACK_DEFAULT_MIDI_TYPE,
                                   JackPortIsInput, 0);
  clock_out_ = jack_port_register(client_, "clock_out", JACK_DEFAULT_MIDI_TYPE,
                                  JackPortIsOutput, 0);

  jack_set_process_callback(client_, jack_process_trampoline, this);
  jack_set_buffer_size_callback(client_, jack_buffer_size_trampoline, this);
  jack_set_sample_rate_callback(client_, jack_sample_rate_trampoline, this);
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
  control_in_ = nullptr;
  clock_out_ = nullptr;
}

size_t Engine::add_channel(const std::string& name, int channel_count) {
  if (client_ == nullptr) return kMaxChannels;

  // Mono or stereo. A session file is just bytes on disk, and a width of five
  // arriving from one used to run off the end of the port array below.
  channel_count = std::clamp(channel_count, 1, kMaxStripChannels);

  const size_t index = graph_->next_channel_slot();
  if (index >= kMaxChannels) return kMaxChannels;

  // Revive a hole: ports from the previous occupant are still registered.
  if (index < channel_ports_.size() && channel_ports_[index].audio[0] != nullptr) {
    return graph_->add_channel(
        name, channel_count,
        std::make_unique<JackInputSource>(channel_ports_[index].audio[0],
                                          channel_ports_[index].audio[1]),
        std::make_unique<JackMidiSource>(channel_ports_[index].midi));
  }

  // Ports are named by index, not by the channel's display name: two channels
  // may share a name, but JACK port names must be unique.
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
  for (int ch = 0; ch < channel_count; ++ch) {
    const std::string out_name =
        std::to_string(index + 1) +
        (channel_count == 1 ? "_out" : (ch == 0 ? "_out_l" : "_out_r"));
    record.audio_out[ch] = jack_port_register(
        client_, out_name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  }
  // By index, not appended: the slot handed back can be a hole in the middle
  // (a tap channel that was removed leaves one with no ports of its own), and
  // pushing there would file this channel's ports under someone else's number.
  if (channel_ports_.size() <= index) channel_ports_.resize(index + 1);
  channel_ports_[index] = record;

  return graph_->add_channel(name, channel_count,
                             std::make_unique<JackInputSource>(ports[0], ports[1]),
                             std::make_unique<JackMidiSource>(midi_port));
}

std::string Engine::start_recording(const std::string& directory) {
  if (client_ == nullptr || recorder_.recording()) return {};

  // Armed channels first, master last, so the file numbers read in strip order.
  std::vector<std::string> names;
  std::vector<size_t> armed;
  const size_t count = graph_->channel_count();
  for (size_t i = 0; i < count; ++i) {
    if (!graph_->channel_alive(i)) continue;
    ChannelStrip& strip = graph_->channel(i);
    if (!strip.armed()) continue;
    armed.push_back(i);
    names.push_back(strip.name());
  }
  names.push_back("master");

  // One folder per take, named by the clock, so takes never overwrite.
  const std::time_t now = std::time(nullptr);
  char stamp[32] = {};
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", std::localtime(&now));
  const std::string take = directory + "/" + stamp;

  if (!recorder_.start(take, sample_rate_, names)) return {};

  for (size_t track = 0; track < armed.size(); ++track)
    graph_->channel(armed[track]).set_record_track(static_cast<int>(track));

  graph_->set_recorder(&recorder_, static_cast<int>(names.size()) - 1);
  return take;
}

void Engine::stop_recording() {
  if (!recorder_.recording()) return;

  // Publish the unhook first, then wait until the audio thread has left any
  // write() that already held the pointer, then join the writer.
  graph_->set_recorder(nullptr, -1);
  const size_t count = graph_->channel_count();
  for (size_t i = 0; i < count; ++i)
    if (graph_->channel_alive(i)) graph_->channel(i).set_record_track(-1);
  // Best effort only. What actually makes the teardown safe is the in-flight
  // guard inside Recorder::stop(), since a write() can already be past its
  // recording() check when this returns.
  if (client_ != nullptr) (void)graph_->wait_renders(2);

  recorder_.stop();
}

bool Engine::park_graph() {
  graph_->park();
  // With no client there is no audio thread to wait for, and waiting anyway
  // cost 200 ms of nothing on every save and every plugin restore.
  if (client_ == nullptr) return true;
  if (graph_->wait_renders(2)) return true;
  std::fprintf(stderr,
               "nirbija: the audio thread did not report two blocks; the graph "
               "may not be quiescent\n");
  return false;
}

void Engine::unpark_graph() { graph_->unpark(); }

double Engine::recorded_seconds() const {
  if (sample_rate_ <= 0.0) return 0.0;
  return static_cast<double>(recorder_.frames_written()) / sample_rate_;
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
  std::vector<std::string> found =
      ports_matching(JackPortIsOutput,
                     midi ? JACK_DEFAULT_MIDI_TYPE : JACK_DEFAULT_AUDIO_TYPE,
                     physical_only);

  // Real inputs first, monitors last. A device's capture and its output's
  // monitor carry the same friendly name, and picking the monitor by accident
  // connects a guitar channel to silence.
  if (!midi) {
    std::stable_sort(found.begin(), found.end(),
                     [](const std::string& a, const std::string& b) {
                       const bool a_monitor = a.find(":monitor_") != std::string::npos;
                       const bool b_monitor = b.find(":monitor_") != std::string::npos;
                       return a_monitor < b_monitor;
                     });
  }
  return found;
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

std::vector<std::string> Engine::current_sources(size_t channel, bool midi) const {
  std::vector<std::string> found;
  if (client_ == nullptr || channel >= channel_ports_.size()) return found;
  jack_port_t* port = midi ? channel_ports_[channel].midi
                           : channel_ports_[channel].audio[0];
  if (port == nullptr) return found;

  const char** connections = jack_port_get_all_connections(client_, port);
  for (const char** c = connections; c != nullptr && *c != nullptr; ++c)
    found.emplace_back(*c);
  if (connections != nullptr) jack_free(connections);
  return found;
}

bool Engine::set_midi_link(size_t channel, const std::string& port, bool on) {
  if (client_ == nullptr || channel >= channel_ports_.size()) return false;
  jack_port_t* target = channel_ports_[channel].midi;
  if (target == nullptr) return false;

  if (on)
    return jack_connect(client_, port.c_str(), jack_port_name(target)) == 0;
  return jack_disconnect(client_, port.c_str(), jack_port_name(target)) == 0;
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

bool Engine::connect_channel_sink(size_t channel, const std::string& port) {
  if (client_ == nullptr || channel >= channel_ports_.size()) return false;
  for (jack_port_t* out : channel_ports_[channel].audio_out)
    if (out != nullptr) jack_port_disconnect(client_, out);
  if (port.empty() || channel_ports_[channel].audio_out[0] == nullptr)
    return true;
  bool ok = jack_connect(client_, jack_port_name(channel_ports_[channel].audio_out[0]),
                         port.c_str()) == 0;
  const std::vector<std::string> all = available_sinks(false);
  const auto it = std::find(all.begin(), all.end(), port);
  if (channel_ports_[channel].audio_out[1] != nullptr && it != all.end() &&
      std::next(it) != all.end()) {
    const std::string& next = *std::next(it);
    if (next.rfind(port.substr(0, port.find(':')), 0) == 0)
      ok = jack_connect(client_,
                        jack_port_name(channel_ports_[channel].audio_out[1]),
                        next.c_str()) == 0 &&
           ok;
  }
  return ok;
}

std::string Engine::current_channel_sink(size_t channel) const {
  if (client_ == nullptr || channel >= channel_ports_.size()) return {};
  jack_port_t* port = channel_ports_[channel].audio_out[0];
  if (port == nullptr) return {};
  const char** connections = jack_port_get_all_connections(client_, port);
  std::string found;
  if (connections != nullptr && connections[0] != nullptr) found = connections[0];
  if (connections != nullptr) jack_free(connections);
  return found;
}

size_t Engine::add_tap_channel(size_t source, int pair) {
  if (client_ == nullptr) return kMaxChannels;
  const size_t index = graph_->next_channel_slot();
  if (index >= kMaxChannels) return kMaxChannels;
  if (channel_ports_.size() <= index) channel_ports_.resize(index + 1);

  auto tap = std::make_unique<TapSource>(graph_.get(), source, pair);
  tap->prepare(block_frames_);
  return graph_->add_channel("tap", 2, std::move(tap), nullptr);
}

void Engine::inject_midi(size_t channel, const MidiEvent& event) {
  injected_midi_.push({channel, event});
}

void Engine::set_time_signature(int num, int den) {
  EngineCommand command;
  command.kind = EngineCommand::Kind::SetTimeSig;
  command.channel = static_cast<size_t>(std::max(1, num));
  command.value = static_cast<float>(std::max(1, den));
  post(command);
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
  // Process is stopped. Scratch grows if needed and is never shrunk; plugins
  // are not re-activated for a period change.
  auto* engine = static_cast<Engine*>(arg);
  engine->block_frames_ = frames;
  engine->graph_->prepare(engine->sample_rate_, frames);
  return 0;
}

int Engine::jack_sample_rate_trampoline(jack_nframes_t rate, void* arg) {
  auto* engine = static_cast<Engine*>(arg);
  engine->sample_rate_ = rate;
  engine->graph_->prepare(rate, engine->block_frames_);
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
      const bool next = command.value != 0.0f;
      const bool was = playing_.load(std::memory_order_relaxed);
      playing_.store(next, std::memory_order_relaxed);
      // A start is a discontinuity; a pause is not a seek.
      if (next && !was) transport_changed_ = true;
      continue;
    }
    if (command.kind == EngineCommand::Kind::SetTempo) {
      if (command.value > 0.0f) tempo_.store(command.value, std::memory_order_relaxed);
      continue;
    }
    if (command.kind == EngineCommand::Kind::Rewind) {
      transport_frame_.store(0, std::memory_order_relaxed);
      clock_phase_ = 0.0;
      transport_changed_ = true;
      continue;
    }
    if (command.kind == EngineCommand::Kind::SetTimeSig) {
      const int num = static_cast<int>(command.channel);
      const int den = static_cast<int>(command.value);
      if (num > 0) time_num_.store(num, std::memory_order_relaxed);
      if (den > 0) time_den_.store(den, std::memory_order_relaxed);
      continue;
    }
    if (command.kind == EngineCommand::Kind::SetMetronome) {
      metronome_.store(command.value != 0.0f, std::memory_order_relaxed);
      continue;
    }
    if (command.bus) {
      if (command.channel >= graph_->bus_count()) continue;
      if (!graph_->bus_alive(command.channel)) continue;
    } else {
      if (command.channel >= graph_->channel_count()) continue;
      if (!graph_->channel_alive(command.channel)) continue;
    }
    ChannelStrip& strip = command.bus ? graph_->bus(command.channel)
                                      : graph_->channel(command.channel);
    switch (command.kind) {
      case EngineCommand::Kind::SetGain: strip.set_gain(command.value); break;
      case EngineCommand::Kind::SetPan: strip.set_pan(command.value); break;
      case EngineCommand::Kind::SetMute: strip.set_muted(command.value != 0.0f); break;
      case EngineCommand::Kind::SetSolo: strip.set_soloed(command.value != 0.0f); break;
      case EngineCommand::Kind::SetMasterGain:
      case EngineCommand::Kind::SetPlaying:
      case EngineCommand::Kind::SetTempo:
      case EngineCommand::Kind::Rewind:
      case EngineCommand::Kind::SetMetronome:
      case EngineCommand::Kind::SetTimeSig:
      case EngineCommand::Kind::InjectMidi:
      case EngineCommand::Kind::None:
        break;
    }
  }
}

size_t Engine::poll_control(MidiEvent* out, size_t capacity) {
  size_t drained = 0;
  MidiEvent event;
  while (drained < capacity && control_events_.pop(event)) out[drained++] = event;
  return drained;
}

void Engine::connect_all_midi_to_control() {
  if (client_ == nullptr || control_in_ == nullptr) return;
  for (const std::string& source : available_sources(true))
    jack_connect(client_, source.c_str(), jack_port_name(control_in_));
}

int Engine::process(jack_nframes_t frames) {
  drain_commands();

  // Controller traffic is handed to the UI thread whole; the mappings live
  // there.
  if (control_in_ != nullptr) {
    void* buffer = jack_port_get_buffer(control_in_, frames);
    const jack_nframes_t count =
        buffer != nullptr ? jack_midi_get_event_count(buffer) : 0;
    for (jack_nframes_t i = 0; i < count; ++i) {
      jack_midi_event_t event;
      if (jack_midi_event_get(&event, buffer, i) != 0) continue;
      if (event.size == 0 || event.size > 3) continue;
      MidiEvent forwarded;
      forwarded.frame = event.time;
      forwarded.size = static_cast<uint8_t>(event.size);
      std::copy_n(event.buffer, event.size, forwarded.data);
      control_events_.push(forwarded);
    }
  }

  InjectedMidi injected;
  while (injected_midi_.pop(injected))
    graph_->push_injected_midi(injected.channel, injected.event);

  const bool playing = playing_.load(std::memory_order_relaxed);
  const double tempo = tempo_.load(std::memory_order_relaxed);
  const uint64_t frame = transport_frame_.load(std::memory_order_relaxed);

  TransportInfo transport;
  transport.playing = playing;
  transport.tempo_bpm = tempo;
  transport.numerator = time_num_.load(std::memory_order_relaxed);
  transport.denominator = time_den_.load(std::memory_order_relaxed);
  transport.frame = frame;
  transport.seconds = sample_rate_ > 0.0
                          ? static_cast<double>(frame) / sample_rate_
                          : 0.0;
  transport.beats = transport.seconds * tempo / 60.0;
  transport.changed = transport_changed_;
  transport_changed_ = false;

  graph_->set_transport(transport);

  float* master[2];
  for (int ch = 0; ch < 2; ++ch)
    master[ch] = static_cast<float*>(jack_port_get_buffer(master_out_[ch], frames));

  graph_->render(master, frames);
  render_metronome(master, frames, playing, tempo, transport.beats);

  for (size_t i = 0; i < channel_ports_.size(); ++i) {
    if (!graph_->channel_alive(i)) continue;
    ChannelStrip& strip = graph_->channel(i);
    for (int ch = 0; ch < 2; ++ch) {
      jack_port_t* port = channel_ports_[i].audio_out[ch];
      if (port == nullptr) continue;
      auto* dest = static_cast<float*>(jack_port_get_buffer(port, frames));
      const float* src = strip.output_cache(std::min(ch, strip.channel_count() - 1));
      if (dest == nullptr) continue;
      if (src != nullptr) std::copy_n(src, frames, dest);
      else std::fill_n(dest, frames, 0.0f);
    }
  }

  if (clock_out_ != nullptr) {
    void* buffer = jack_port_get_buffer(clock_out_, frames);
    if (buffer != nullptr) {
      jack_midi_clear_buffer(buffer);
      if (clock_enabled_.load(std::memory_order_relaxed) && playing &&
          sample_rate_ > 0.0 && tempo > 0.0) {
        const double frames_per_clock =
            sample_rate_ / (tempo / 60.0 * 24.0);
        for (uint32_t i = 0; i < frames; ++i) {
          clock_phase_ += 1.0;
          if (clock_phase_ >= frames_per_clock) {
            clock_phase_ -= frames_per_clock;
            uint8_t msg = 0xf8;
            jack_midi_event_write(buffer, i, &msg, 1);
          }
        }
      } else if (!playing) {
        clock_phase_ = 0.0;
      }
    }
  }

  if (playing) transport_frame_.store(frame + frames, std::memory_order_relaxed);
  return 0;
}

// A short sine tick on every beat, a fifth higher on the downbeat. Added after
// the master fader on purpose: pulling the mix down for a break should not
// take the count with it.
void Engine::render_metronome(float* const* master, uint32_t frames, bool playing,
                              double tempo, double start_beats) {
  const bool wanted = metronome_.load(std::memory_order_relaxed) && playing;
  if (!wanted && click_remaining_ == 0) return;

  const double beats_per_frame = tempo / 60.0 / sample_rate_;

  for (uint32_t i = 0; i < frames; ++i) {
    if (wanted) {
      const double beat_now = start_beats + beats_per_frame * i;
      const double beat_next = beat_now + beats_per_frame;
      if (std::floor(beat_now) != std::floor(beat_next) || beat_now == 0.0) {
        const long long beat = static_cast<long long>(
            beat_now == 0.0 ? 0 : std::floor(beat_next));
        const int bar = std::max(1, time_num_.load(std::memory_order_relaxed));
        const bool downbeat = beat % bar == 0;
        click_length_ = static_cast<uint32_t>(sample_rate_ * 0.03);
        click_remaining_ = click_length_;
        click_phase_ = 0.0;
        click_step_ = 2.0 * 3.14159265358979 * (downbeat ? 1568.0 : 1046.5) /
                      sample_rate_;
      }
    }

    if (click_remaining_ > 0) {
      const float envelope =
          static_cast<float>(click_remaining_) / static_cast<float>(click_length_);
      const float sample =
          static_cast<float>(std::sin(click_phase_)) * envelope * envelope * 0.4f;
      click_phase_ += click_step_;
      --click_remaining_;
      master[0][i] += sample;
      master[1][i] += sample;
    }
  }
}

}  // namespace nirbija
