// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/engine.h"
#include "core/ble_midi.h"

#include <jack/midiport.h>
#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#if defined(__SSE__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace nirbija {
namespace {

int midi_data_bytes(uint8_t status) {
  const uint8_t high = status & 0xf0;
  if (high == 0xc0 || high == 0xd0) return 1;  // program change, pressure
  if (status >= 0xf8) return 0;                // realtime
  if (status >= 0xf0) return 1;                // coarse: SysEx handled elsewhere
  return 2;
}

// JACK / PipeWire may hand us a clean 3-byte note, a 4-byte UMP MIDI 1.0
// packet, or a BLE MIDI blob with timestamps. Turn whatever that is into
// one or more 1–3 byte channel-voice messages.
size_t unpack_jack_midi(const uint8_t* src, size_t len, uint32_t frame,
                        MidiEvent* out, size_t cap, uint8_t* running) {
  if (src == nullptr || len == 0 || cap == 0) return 0;

  size_t written = 0;
  auto push = [&](uint8_t status, uint8_t d1, uint8_t d2, uint8_t bytes) {
    if (written >= cap || bytes < 1) return;
    MidiEvent& event = out[written++];
    event.frame = frame;
    event.size = bytes;
    event.data[0] = status;
    event.data[1] = d1;
    event.data[2] = d2;
  };

  if (len <= 3) {
    uint8_t status = src[0];
    if (status < 0x80) {
      if (*running < 0x80) return 0;
      const int need = midi_data_bytes(*running);
      if (need == 1)
        push(*running, src[0], 0, 2);
      else if (len >= 2)
        push(*running, src[0], src[1], 3);
      return written;
    }
    if (status < 0xf8) *running = status;
    push(src[0], len > 1 ? src[1] : 0, len > 2 ? src[2] : 0,
         static_cast<uint8_t>(len));
    return written;
  }

  // UMP MIDI 1.0 channel voice: 0x2n status d1 d2
  if (len == 4 && (src[0] & 0xf0) == 0x20 && src[1] >= 0x80) {
    *running = src[1];
    push(src[1], src[2], src[3], 3);
    return written;
  }

  uint8_t status = *running;
  for (size_t i = 0; i < len && written < cap;) {
    const uint8_t byte = src[i++];
    if (byte >= 0xf8) continue;  // clock, etc.
    if (byte >= 0x80) {
      status = byte;
      *running = byte;
      continue;
    }
    if (status < 0x80 || status >= 0xf0) continue;
    const int need = midi_data_bytes(status);
    uint8_t d1 = byte;
    uint8_t d2 = 0;
    if (need == 2) {
      if (i >= len) break;
      if (src[i] >= 0x80) continue;
      d2 = src[i++];
    }
    push(status, d1, d2, static_cast<uint8_t>(need + 1));
  }
  return written;
}

size_t drain_jack_midi_port(jack_port_t* port, uint32_t frames, MidiEvent* out,
                            size_t cap, uint8_t* running) {
  if (port == nullptr || cap == 0) return 0;
  void* buffer = jack_port_get_buffer(port, frames);
  if (buffer == nullptr) return 0;
  const jack_nframes_t count = jack_midi_get_event_count(buffer);
  size_t written = 0;
  for (jack_nframes_t i = 0; i < count && written < cap; ++i) {
    jack_midi_event_t event;
    if (jack_midi_event_get(&event, buffer, i) != 0) continue;
    if (event.size == 0 || event.buffer == nullptr) continue;
    written += unpack_jack_midi(event.buffer, event.size, event.time,
                                out + written, cap - written, running);
  }
  return written;
}

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
    return drain_jack_midi_port(port_, frames, out, capacity, &running_);
  }

 private:
  jack_port_t* port_;
  uint8_t running_{0};
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

  // Everything mapped now and later stays resident: a page fault inside
  // process() is a disk read on the audio thread. Failure (no
  // RLIMIT_MEMLOCK, typically) is not fatal, only slower under pressure, so
  // it is said once and the engine carries on.
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    static bool said = false;
    if (!said) {
      said = true;
      std::perror(
          "nirbija: mlockall failed; memory may page under load (raise "
          "RLIMIT_MEMLOCK: put the user in the audio group, or set memlock "
          "unlimited in /etc/security/limits.d/)");
    }
  }

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
  clock_in_ = jack_port_register(client_, "clock_in", JACK_DEFAULT_MIDI_TYPE,
                                 JackPortIsInput, 0);

  jack_set_process_callback(client_, jack_process_trampoline, this);
  jack_set_xrun_callback(client_, jack_xrun_trampoline, this);
  jack_set_buffer_size_callback(client_, jack_buffer_size_trampoline, this);
  jack_set_sample_rate_callback(client_, jack_sample_rate_trampoline, this);
  // No jack_set_port_connect_callback: with one registered, pipewire-jack
  // (1.6) leaves a jack_port_disconnect() unreflected in
  // jack_port_get_all_connections() when it returns, and the UI, which
  // reads the connections straight after, shows the old source. The direct
  // outs' listener flags are refreshed from the UI thread instead.
  jack_set_latency_callback(client_, jack_latency_trampoline, this);

  // Before the process callback can run: process() reads ble_midi_, and a
  // pointer that appears after activation is a pointer it could read half
  // set. PipeWire lists the SMC-PAD as a MIDI port but never AcquireNotify's
  // the BLE characteristic, so the port stays silent; we take the notify FD
  // ourselves.
  ble_midi_ = std::make_unique<BleMidi>();
  ble_midi_->start();

  if (jack_activate(client_) != 0) {
    stop();
    return false;
  }
  return true;
}

void Engine::stop() {
  // The process callback stops before anything it reads is torn down: the
  // BLE queue was reset with process() still running and popping from it.
  if (client_ != nullptr) jack_deactivate(client_);
  if (ble_midi_) {
    ble_midi_->stop();
    ble_midi_.reset();
  }
  if (client_ == nullptr) return;
  jack_client_close(client_);
  client_ = nullptr;
  master_out_[0] = master_out_[1] = nullptr;
  control_in_ = nullptr;
  clock_out_ = nullptr;
  clock_in_ = nullptr;
}

size_t Engine::add_channel(const std::string& name, int channel_count) {
  if (client_ == nullptr) return kMaxChannels;

  // Mono or stereo. A session file is just bytes on disk, and a width of five
  // arriving from one used to run off the end of the port array below.
  channel_count = std::clamp(channel_count, 1, kMaxStripChannels);

  const size_t index = graph_->next_channel_slot();
  if (index >= kMaxChannels) return kMaxChannels;

  // Revive a hole: ports from the previous occupant are still registered.
  ChannelPorts& record = channel_ports_[index];
  if (record.audio[0].load(std::memory_order_acquire) != nullptr) {
    // A mono occupant only registered the left half. A stereo strip in
    // the same hole needs the right-hand ports or its right side is
    // permanently silent.
    if (channel_count > 1 && record.audio[1].load(std::memory_order_relaxed) == nullptr) {
      const std::string in_name = std::to_string(index + 1) + "_in_r";
      record.audio[1].store(jack_port_register(client_, in_name.c_str(),
                                               JACK_DEFAULT_AUDIO_TYPE,
                                               JackPortIsInput, 0),
                            std::memory_order_release);
      const std::string out_name = std::to_string(index + 1) + "_out_r";
      record.audio_out[1].store(jack_port_register(client_, out_name.c_str(),
                                                   JACK_DEFAULT_AUDIO_TYPE,
                                                   JackPortIsOutput, 0),
                                std::memory_order_release);
    }
    record.tap.store(false, std::memory_order_release);
    refresh_out_connected(index);
    // The ports are all published above before the graph publishes the strip,
    // so a pass that can see the channel can see its ports.
    return graph_->add_channel(
        name, channel_count,
        std::make_unique<JackInputSource>(
            record.audio[0].load(std::memory_order_relaxed),
            record.audio[1].load(std::memory_order_relaxed)),
        std::make_unique<JackMidiSource>(record.midi.load(std::memory_order_relaxed)));
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

  // Every port is published here, before graph_->add_channel() publishes
  // the strip, so process() never finds a live channel with half a record.
  record.audio[0].store(ports[0], std::memory_order_release);
  record.audio[1].store(ports[1], std::memory_order_release);
  record.midi.store(midi_port, std::memory_order_release);
  for (int ch = 0; ch < channel_count; ++ch) {
    const std::string out_name =
        std::to_string(index + 1) +
        (channel_count == 1 ? "_out" : (ch == 0 ? "_out_l" : "_out_r"));
    record.audio_out[ch].store(
        jack_port_register(client_, out_name.c_str(), JACK_DEFAULT_AUDIO_TYPE,
                           JackPortIsOutput, 0),
        std::memory_order_release);
  }
  record.tap.store(false, std::memory_order_release);
  refresh_out_connected(index);

  return graph_->add_channel(name, channel_count,
                             std::make_unique<JackInputSource>(ports[0], ports[1]),
                             std::make_unique<JackMidiSource>(midi_port));
}

std::string Engine::start_recording(const std::string& directory) {
  if (client_ == nullptr || recorder_.recording()) return {};

  // Armed channels first, then armed buses, master last, so the file
  // numbers read in strip order.
  std::vector<std::string> names;
  std::vector<size_t> armed;
  std::vector<size_t> armed_buses;
  const size_t count = graph_->channel_count();
  for (size_t i = 0; i < count; ++i) {
    if (!graph_->channel_alive(i)) continue;
    ChannelStrip& strip = graph_->channel(i);
    if (!strip.armed()) continue;
    armed.push_back(i);
    names.push_back(strip.name());
  }
  const size_t buses = graph_->bus_count();
  for (size_t i = 0; i < buses; ++i) {
    if (!graph_->bus_alive(i)) continue;
    ChannelStrip& strip = graph_->bus(i);
    if (!strip.armed()) continue;
    armed_buses.push_back(i);
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
  for (size_t i = 0; i < armed_buses.size(); ++i)
    graph_->bus(armed_buses[i])
        .set_record_track(static_cast<int>(armed.size() + i));

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
  const size_t buses = graph_->bus_count();
  for (size_t i = 0; i < buses; ++i)
    if (graph_->bus_alive(i)) graph_->bus(i).set_record_track(-1);
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
  if (graph_->wait_quiescent()) return true;
  std::fprintf(stderr,
               "nirbija: the audio thread did not report a parked block; the "
               "graph may not be quiescent\n");
  return false;
}

void Engine::unpark_graph() { graph_->unpark(); }

double Engine::recorded_seconds() const {
  if (sample_rate_ <= 0.0) return 0.0;
  return static_cast<double>(recorder_.frames_written()) / sample_rate_;
}

void Engine::remove_channel(size_t channel) {
  if (client_ == nullptr || channel >= kMaxChannels) return;

  const ChannelPorts& record = channel_ports_[channel];
  for (const auto& port : record.audio)
    if (jack_port_t* p = port.load(std::memory_order_acquire)) jack_port_disconnect(client_, p);
  if (jack_port_t* midi = record.midi.load(std::memory_order_acquire))
    jack_port_disconnect(client_, midi);

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
  if (client_ == nullptr || channel >= kMaxChannels) return false;
  const ChannelPorts& record = channel_ports_[channel];
  jack_port_t* const audio[2] = {record.audio[0].load(std::memory_order_acquire),
                                 record.audio[1].load(std::memory_order_acquire)};
  jack_port_t* const midi_port = record.midi.load(std::memory_order_acquire);

  // Picking a source replaces the old one rather than stacking on top of it,
  // which is what choosing an input in a mixer means.
  const int count = midi ? 1 : (audio[1] != nullptr ? 2 : 1);
  for (int i = 0; i < count; ++i) {
    jack_port_t* target = midi ? midi_port : audio[i];
    if (target != nullptr) jack_port_disconnect(client_, target);
  }
  if (port.empty()) return true;
  if (record.tap.load(std::memory_order_acquire)) return false;

  if (midi) {
    if (midi_port == nullptr) return false;
    return jack_connect(client_, port.c_str(), jack_port_name(midi_port)) == 0;
  }
  if (audio[0] == nullptr) return false;

  // A stereo channel fed from a stereo source should take both sides. The
  // sibling is the next port of the same client, which is how JACK names them.
  std::vector<std::string> siblings = available_sources(false);
  const auto chosen = std::find(siblings.begin(), siblings.end(), port);

  bool ok = jack_connect(client_, port.c_str(), jack_port_name(audio[0])) == 0;
  if (count == 2 && chosen != siblings.end() && std::next(chosen) != siblings.end()) {
    const std::string& next = *std::next(chosen);
    const std::string client_of = port.substr(0, port.find(':'));
    if (next.rfind(client_of, 0) == 0)
      ok = jack_connect(client_, next.c_str(), jack_port_name(audio[1])) == 0 && ok;
  }
  return ok;
}

std::vector<std::string> Engine::current_sources(size_t channel, bool midi) const {
  std::vector<std::string> found;
  if (client_ == nullptr || channel >= kMaxChannels) return found;
  jack_port_t* port = midi ? channel_ports_[channel].midi.load(std::memory_order_acquire)
                           : channel_ports_[channel].audio[0].load(std::memory_order_acquire);
  if (port == nullptr) return found;

  const char** connections = jack_port_get_all_connections(client_, port);
  for (const char** c = connections; c != nullptr && *c != nullptr; ++c)
    found.emplace_back(*c);
  if (connections != nullptr) jack_free(connections);
  return found;
}

bool Engine::set_midi_link(size_t channel, const std::string& port, bool on) {
  if (client_ == nullptr || channel >= kMaxChannels) return false;
  jack_port_t* target = channel_ports_[channel].midi.load(std::memory_order_acquire);
  if (target == nullptr) return false;

  if (on)
    return jack_connect(client_, port.c_str(), jack_port_name(target)) == 0;
  return jack_disconnect(client_, port.c_str(), jack_port_name(target)) == 0;
}

std::string Engine::current_source(size_t channel, bool midi) const {
  if (client_ == nullptr || channel >= kMaxChannels) return {};
  jack_port_t* port = midi ? channel_ports_[channel].midi.load(std::memory_order_acquire)
                           : channel_ports_[channel].audio[0].load(std::memory_order_acquire);
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
  if (client_ == nullptr || channel >= kMaxChannels) return false;
  ChannelPorts& record = channel_ports_[channel];
  jack_port_t* const outs[2] = {record.audio_out[0].load(std::memory_order_acquire),
                                record.audio_out[1].load(std::memory_order_acquire)};
  for (jack_port_t* out : outs)
    if (out != nullptr) jack_port_disconnect(client_, out);
  if (port.empty() || outs[0] == nullptr) {
    refresh_out_connected(channel);
    return true;
  }
  // Marked connected before the wire goes in, so the block the connection
  // lands in already carries fresh audio rather than whatever the buffer
  // last held.
  record.out_connected.store(true, std::memory_order_release);
  bool ok = jack_connect(client_, jack_port_name(outs[0]), port.c_str()) == 0;
  const std::vector<std::string> all = available_sinks(false);
  const auto it = std::find(all.begin(), all.end(), port);
  if (outs[1] != nullptr && it != all.end() && std::next(it) != all.end()) {
    const std::string& next = *std::next(it);
    if (next.rfind(port.substr(0, port.find(':')), 0) == 0)
      ok = jack_connect(client_, jack_port_name(outs[1]), next.c_str()) == 0 && ok;
  }
  refresh_out_connected(channel);
  return ok;
}

void Engine::refresh_out_connected(size_t channel) {
  if (client_ == nullptr || channel >= kMaxChannels) return;
  ChannelPorts& record = channel_ports_[channel];
  bool connected = false;
  for (const auto& out : record.audio_out) {
    jack_port_t* port = out.load(std::memory_order_acquire);
    if (port != nullptr && jack_port_connected(port) > 0) connected = true;
  }
  record.out_connected.store(connected, std::memory_order_release);
}

std::string Engine::current_channel_sink(size_t channel) const {
  if (client_ == nullptr || channel >= kMaxChannels) return {};
  jack_port_t* port = channel_ports_[channel].audio_out[0].load(std::memory_order_acquire);
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

  ChannelPorts& record = channel_ports_[index];
  // A removed strip leaves its ports registered. Disconnect them so the
  // tap does not leak onto the previous occupant's hardware outs, and
  // mark the slot so process() will not write them either.
  for (const auto& port : record.audio)
    if (jack_port_t* p = port.load(std::memory_order_acquire)) jack_port_disconnect(client_, p);
  for (const auto& port : record.audio_out)
    if (jack_port_t* p = port.load(std::memory_order_acquire)) jack_port_disconnect(client_, p);
  if (jack_port_t* midi = record.midi.load(std::memory_order_acquire))
    jack_port_disconnect(client_, midi);
  record.tap.store(true, std::memory_order_release);
  refresh_out_connected(index);

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
  // Process is stopped. Scratch grows if needed and is never shrunk, and a
  // period that grows past what the plugins were activated with has every
  // insert deactivated and activated again with the new maximum (see
  // ChannelStrip::prepare); a smaller one changes nothing for them.
  auto* engine = static_cast<Engine*>(arg);
  engine->block_frames_ = frames;
  engine->graph_->prepare(engine->sample_rate_, frames);
  return 0;
}

void Engine::jack_latency_trampoline(jack_latency_callback_mode_t mode, void* arg) {
  static_cast<Engine*>(arg)->report_latency(mode);
}

// JACK's model: in capture mode a client sets, on each output port, the
// capture latency of the inputs feeding it plus its own; in playback mode it
// sets, on each input port, the playback latency of the outputs it feeds
// plus its own. The graph mixes every input to every output, so the deepest
// upstream number is taken and the graph's own (the compensation everything
// is delayed to, plus the limiter's lookahead) is added on top.
void Engine::report_latency(jack_latency_callback_mode_t mode) {
  if (client_ == nullptr) return;
  const jack_nframes_t own = graph_->internal_latency_samples();

  jack_port_t* inputs[kMaxChannels * 2 + 2];
  jack_port_t* outputs[kMaxChannels * 2 + 2];
  size_t n_in = 0;
  size_t n_out = 0;
  for (size_t i = 0; i < kMaxChannels; ++i) {
    const ChannelPorts& record = channel_ports_[i];
    for (const auto& port : record.audio)
      if (jack_port_t* p = port.load(std::memory_order_acquire)) inputs[n_in++] = p;
    for (const auto& port : record.audio_out)
      if (jack_port_t* p = port.load(std::memory_order_acquire)) outputs[n_out++] = p;
  }
  for (jack_port_t* port : master_out_)
    if (port != nullptr) outputs[n_out++] = port;

  jack_port_t** from = mode == JackCaptureLatency ? inputs : outputs;
  const size_t n_from = mode == JackCaptureLatency ? n_in : n_out;
  jack_port_t** to = mode == JackCaptureLatency ? outputs : inputs;
  const size_t n_to = mode == JackCaptureLatency ? n_out : n_in;

  jack_latency_range_t upstream{0, 0};
  for (size_t i = 0; i < n_from; ++i) {
    jack_latency_range_t range;
    jack_port_get_latency_range(from[i], mode, &range);
    upstream.min = std::max(upstream.min, range.min);
    upstream.max = std::max(upstream.max, range.max);
  }
  jack_latency_range_t reported{upstream.min + own, upstream.max + own};
  for (size_t i = 0; i < n_to; ++i)
    jack_port_set_latency_range(to[i], mode, &reported);
}

void Engine::latency_changed() {
  if (client_ != nullptr) jack_recompute_total_latencies(client_);
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
      transport_beats_ = 0.0;
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
    if (command.kind == EngineCommand::Kind::SceneArm) {
      scenes_.arm(static_cast<int>(command.value));
      continue;
    }
    if (command.kind == EngineCommand::Kind::SceneHand) {
      scenes_.hand(command.bus, command.channel,
                   static_cast<SceneTarget::What>(static_cast<int>(command.value)),
                   command.tag, command.param);
      continue;
    }
    if (command.bus) {
      if (command.channel >= graph_->bus_count()) continue;
      if (!graph_->bus_alive(command.channel)) continue;
    } else {
      if (command.channel >= graph_->channel_count()) continue;
      if (!graph_->channel_alive(command.channel)) continue;
    }
    // The published pointer, never the owning one: the UI thread moves the
    // owning pointer around under us in add_channel and remove_channel.
    ChannelStrip* live = command.bus ? graph_->live_bus(command.channel)
                                     : graph_->live_channel(command.channel);
    if (live == nullptr) continue;
    ChannelStrip& strip = *live;
    switch (command.kind) {
      case EngineCommand::Kind::SetGain: strip.set_gain(command.value); break;
      case EngineCommand::Kind::SetPan: strip.set_pan(command.value); break;
      case EngineCommand::Kind::SetMute: strip.set_muted(command.value != 0.0f); break;
      case EngineCommand::Kind::SetSolo: strip.set_soloed(command.value != 0.0f); break;
      case EngineCommand::Kind::SetSceneGate: strip.set_scene_on(command.value != 0.0f); break;
      case EngineCommand::Kind::SetMasterGain:
      case EngineCommand::Kind::SetPlaying:
      case EngineCommand::Kind::SetTempo:
      case EngineCommand::Kind::Rewind:
      case EngineCommand::Kind::SetMetronome:
      case EngineCommand::Kind::SetTimeSig:
      case EngineCommand::Kind::InjectMidi:
      case EngineCommand::Kind::SceneArm:
      case EngineCommand::Kind::SceneHand:
      case EngineCommand::Kind::None:
        break;
    }
  }
}

size_t Engine::poll_control(MidiEvent* out, size_t capacity) {
  // The UI's poll is also when the direct outs learn whether anyone listens:
  // a connection made from outside (a patchbay) shows up here within a
  // tick, and until it does the out holds silence, not a stale block. Our
  // own connect_channel_sink() marks the flag on the spot.
  if (client_ != nullptr) {
    const size_t count = graph_->channel_count();
    for (size_t i = 0; i < count; ++i)
      if (graph_->channel_alive(i) &&
          !channel_ports_[i].tap.load(std::memory_order_acquire))
        refresh_out_connected(i);
  }
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

void Engine::connect_all_midi_to_channel(size_t channel) {
  if (client_ == nullptr || channel >= kMaxChannels) return;
  jack_port_t* midi = channel_ports_[channel].midi.load(std::memory_order_acquire);
  if (midi == nullptr) return;
  for (const std::string& source : available_sources(true))
    jack_connect(client_, source.c_str(), jack_port_name(midi));
}

int Engine::jack_xrun_trampoline(void* arg) {
  static_cast<Engine*>(arg)->xruns_.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

// Denormals off for this thread. A reverb tail or a filter decaying towards
// nothing spends its last seconds in numbers so small the FPU takes a
// hundred times longer on each of them, which is a dropout timed exactly
// where the music went quiet. PipeWire leaves the flags as it found them, and
// not every plugin sets them for itself, so the host does, every block: the
// cost is a register write.
static void disable_denormals() {
#if defined(__SSE__)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
  uint64_t fpcr;
  asm volatile("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= 1u << 24;  // FZ
  asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

int Engine::process(jack_nframes_t frames) {
  disable_denormals();
  drain_commands();

  // Controller traffic is handed to the UI thread whole; the mappings live
  // there.
  MidiEvent midi_block[64];
  const size_t midi_n = drain_jack_midi_port(
      control_in_, frames, midi_block, 64, &midi_running_status_);
  for (size_t i = 0; i < midi_n; ++i) control_events_.push(midi_block[i]);

  if (ble_midi_ != nullptr) {
    MidiEvent ble[32];
    const size_t ble_n = ble_midi_->pop(ble, 32);
    const size_t channels = graph_->channel_count();
    for (size_t i = 0; i < ble_n; ++i) {
      control_events_.push(ble[i]);
      for (size_t ch = 0; ch < channels; ++ch) {
        if (graph_->channel_alive(ch))
          graph_->push_injected_midi(ch, ble[i]);
      }
    }
  }

  InjectedMidi injected;
  while (injected_midi_.pop(injected))
    graph_->push_injected_midi(injected.channel, injected.event);

  // Before anything reads the transport: while following, start, stop, tempo
  // and position all arrive on a wire rather than from our own counter.
  read_external_clock(frames);

  const bool playing = playing_.load(std::memory_order_relaxed);
  const bool metronome = metronome_.load(std::memory_order_relaxed);
  // One clock. Play unmutes sequenced plugins; the metronome alone is
  // enough to walk the grid so a looper can punch to the click.
  const bool rolling = playing || metronome;
  const double tempo = tempo_.load(std::memory_order_relaxed);
  const uint64_t frame = transport_frame_.load(std::memory_order_relaxed);

  TransportInfo transport;
  transport.playing = playing;
  transport.rolling = rolling;
  transport.tempo_bpm = tempo;
  transport.numerator = time_num_.load(std::memory_order_relaxed);
  transport.denominator = time_den_.load(std::memory_order_relaxed);
  transport.frame = frame;
  transport.seconds = sample_rate_ > 0.0
                          ? static_cast<double>(frame) / sample_rate_
                          : 0.0;
  // Beats are counted, not derived from the frame: seconds times tempo would
  // move the song every time the tempo moved, by more the further in it was.
  transport.beats = transport_beats_;
  transport.changed = transport_changed_;
  transport_changed_ = false;

  graph_->set_transport(transport);
  // Before the render, so a scene that starts on a bar line inside this
  // block starts on its frame.
  scenes_.run(*graph_, transport, frames, sample_rate_);

  float* master[2];
  for (int ch = 0; ch < 2; ++ch)
    master[ch] = static_cast<float*>(jack_port_get_buffer(master_out_[ch], frames));

  // The click is scheduled before the render and rendered inside it, so it
  // goes through the limiter and the park fade with everything else.
  schedule_metronome(frames, tempo, transport.beats);
  graph_->render(master, frames);

  const size_t channels = graph_->channel_count();
  for (size_t i = 0; i < channels; ++i) {
    // The published pointer, not graph_->channel(): the UI thread moves the
    // owning pointer in add_channel and remove_channel.
    ChannelStrip* strip = graph_->live_channel(i);
    if (strip == nullptr) continue;
    const ChannelPorts& record = channel_ports_[i];
    if (record.tap.load(std::memory_order_acquire)) continue;
    // Nobody listening: nothing to copy. The block after the listener left
    // gets silence written once, so the buffer does not hold a stale block
    // for the next listener to hear.
    const bool connected = record.out_connected.load(std::memory_order_acquire);
    if (!connected && !out_written_[i]) continue;
    out_written_[i] = connected;
    for (int ch = 0; ch < 2; ++ch) {
      jack_port_t* port = record.audio_out[ch].load(std::memory_order_acquire);
      if (port == nullptr) continue;
      auto* dest = static_cast<float*>(jack_port_get_buffer(port, frames));
      if (dest == nullptr) continue;
      if (!connected) {
        std::fill_n(dest, frames, 0.0f);
        continue;
      }
      const float* src = strip->output_cache(std::min(ch, strip->channel_count() - 1));
      if (src != nullptr) std::copy_n(src, frames, dest);
      else std::fill_n(dest, frames, 0.0f);
      // The same fade the master got. Without it a parked block copied the
      // strip's last block out again, every block, for as long as the park
      // lasted: a buzz on the direct outs while the master was silent.
      graph_->apply_park_ramp(dest, frames);
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

  if (rolling && !follow_clock_.load(std::memory_order_relaxed)) {
    transport_frame_.store(frame + frames, std::memory_order_relaxed);
    if (sample_rate_ > 0.0)
      transport_beats_ += static_cast<double>(frames) / sample_rate_ * tempo / 60.0;
  }
  published_beats_.store(transport_beats_, std::memory_order_relaxed);
  return 0;
}

// Reads MIDI beat clock off `clock_in` and makes the transport follow it.
//
// The wire carries twenty-four ticks to the quarter note and nothing else: no
// tempo, no position. Tempo is inferred from how far apart the ticks land, and
// position is simply how many have gone by. Counting ticks rather than frames
// is what keeps the two from fighting - a tempo estimate that wobbles must not
// make the song position wobble with it.
void Engine::read_external_clock(uint32_t frames) {
  if (!follow_clock_.load(std::memory_order_relaxed) || clock_in_ == nullptr)
    return;

  void* buffer = jack_port_get_buffer(clock_in_, frames);
  if (buffer == nullptr) return;

  const uint32_t count = jack_midi_get_event_count(buffer);
  double consumed = 0.0;  // frames of this block already charged to a gap

  for (uint32_t i = 0; i < count; ++i) {
    jack_midi_event_t event;
    if (jack_midi_event_get(&event, buffer, i) != 0 || event.size < 1) continue;

    switch (event.buffer[0]) {
      case 0xf8: {  // timing clock
        const double gap = frames_since_tick_ + event.time - consumed;
        consumed = event.time;
        frames_since_tick_ = 0.0;

        // A gap under a millisecond or over a second is a sender starting up,
        // a dropout, or a port being connected mid-stream - none of which is a
        // tempo. Smoothed, because one late tick is jitter, not a rallentando.
        if (gap > sample_rate_ * 0.001 && gap < sample_rate_) {
          tick_frames_ = tick_frames_ > 0.0 ? tick_frames_ * 0.8 + gap * 0.2 : gap;
          const double bpm = 60.0 * sample_rate_ / (tick_frames_ * 24.0);
          if (bpm > 20.0 && bpm < 400.0)
            tempo_.store(bpm, std::memory_order_relaxed);
        }
        if (external_running_) ++clock_ticks_;
        break;
      }
      case 0xfa:  // start: from the top
        clock_ticks_ = 0;
        frames_since_tick_ = 0.0;
        external_running_ = true;
        playing_.store(true, std::memory_order_relaxed);
        transport_changed_ = true;
        break;
      case 0xfb:  // continue: from where it stopped
        external_running_ = true;
        playing_.store(true, std::memory_order_relaxed);
        transport_changed_ = true;
        break;
      case 0xfc:  // stop
        external_running_ = false;
        playing_.store(false, std::memory_order_relaxed);
        transport_changed_ = true;
        break;
      case 0xf2:  // song position, in sixteenths, fourteen bits little end first
        if (event.size >= 3) {
          const uint32_t sixteenths =
              static_cast<uint32_t>(event.buffer[1] & 0x7f) |
              (static_cast<uint32_t>(event.buffer[2] & 0x7f) << 7);
          clock_ticks_ = static_cast<uint64_t>(sixteenths) * 6;
          frames_since_tick_ = 0.0;
          transport_changed_ = true;
        }
        break;
      default:
        break;
    }
  }
  frames_since_tick_ += frames - consumed;

  // Ticks to beats, with the fraction of a tick since the last one so the
  // position moves smoothly between them rather than in twenty-fourths.
  double beats = static_cast<double>(clock_ticks_) / 24.0;
  if (external_running_ && tick_frames_ > 0.0)
    beats += frames_since_tick_ / tick_frames_ / 24.0;

  // The rest of the engine reads a frame count and multiplies it by the tempo
  // to get beats, so the frame written here is the one that gives back exactly
  // the position the clock asked for - and it is recomputed every block, so a
  // change in the tempo estimate moves the tempo without moving the song.
  const double tempo = tempo_.load(std::memory_order_relaxed);
  if (tempo > 0.0 && sample_rate_ > 0.0) {
    transport_frame_.store(
        static_cast<uint64_t>(beats * 60.0 / tempo * sample_rate_),
        std::memory_order_relaxed);
  }
  transport_beats_ = beats;
}

// A short tick on every beat, higher on the downbeat, rendered by the graph
// after the master fader (pulling the mix down for a break should not take
// the count with it) and before the limiter and the park fade, which it used
// to skip. The beat's frame is worked out once for the block rather than by
// flooring the beat count at every sample.
void Engine::schedule_metronome(uint32_t frames, double tempo, double start_beats) {
  if (!metronome_.load(std::memory_order_relaxed)) return;
  if (sample_rate_ <= 0.0 || tempo <= 0.0) return;

  const uint32_t at = dsp::boundary_frame(start_beats, tempo, sample_rate_, 1.0, 0, frames);
  if (at >= frames) return;

  const double beats_per_frame = tempo / 60.0 / sample_rate_;
  const long long beat = std::llround(start_beats + beats_per_frame * at);
  const int bar = std::max(1, time_num_.load(std::memory_order_relaxed));
  graph_->schedule_click(at, beat % bar == 0);
}

}  // namespace nirbija
