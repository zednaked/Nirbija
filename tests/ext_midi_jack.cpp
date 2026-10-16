// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The engine's ports for a hardware instrument (Engine::attach_external,
// core/external_midi.h) through JACK itself. A second client stands for the
// FM-1's USB MIDI ports: the engine finds them by name and wires both ways, a
// SysEx of 500 bytes from the worker's ring reaches the device as one event,
// byte for byte, what the device sends lands in the ring, and after detaching
// nothing more goes out.

#include <jack/jack.h>
#include <jack/midiport.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/engine.h"
#include "core/external_midi.h"
#include "core/fm1.h"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// The device's two ports, as a USB MIDI bridge shows them.
class DevicePorts {
 public:
  bool start() {
    jack_status_t status;
    client_ = jack_client_open("nirbija-ext-felucca", JackNoStartServer, &status);
    if (client_ == nullptr) return false;
    in_ = jack_port_register(client_, "Felucca MIDI in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    out_ = jack_port_register(client_, "Felucca MIDI out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    if (in_ == nullptr || out_ == nullptr) return false;
    jack_set_process_callback(client_, &DevicePorts::trampoline, this);
    return jack_activate(client_) == 0;
  }
  void stop() {
    if (client_ == nullptr) return;
    jack_deactivate(client_);
    jack_client_close(client_);
    client_ = nullptr;
  }
  std::string name() const { return jack_get_client_name(client_); }
  int connections() const { return jack_port_connected(in_) + jack_port_connected(out_); }

  // Test thread -> process(): one SysEx to send.
  void send(const std::vector<uint8_t>& bytes) {
    outgoing_ = bytes;
    pending_.store(true, std::memory_order_release);
  }
  nirbija::RtByteRing<8192> heard;  // process() -> test thread: each event as it came

 private:
  static int trampoline(jack_nframes_t frames, void* arg) {
    return static_cast<DevicePorts*>(arg)->process(frames);
  }
  int process(jack_nframes_t frames) {
    void* in = jack_port_get_buffer(in_, frames);
    for (uint32_t i = 0; i < jack_midi_get_event_count(in); ++i) {
      jack_midi_event_t event;
      if (jack_midi_event_get(&event, in, i) == 0) heard.push(event.buffer, event.size);
    }
    void* out = jack_port_get_buffer(out_, frames);
    jack_midi_clear_buffer(out);
    if (pending_.exchange(false, std::memory_order_acquire))
      jack_midi_event_write(out, 0, outgoing_.data(), outgoing_.size());
    return 0;
  }

  jack_client_t* client_ = nullptr;
  jack_port_t* in_ = nullptr;
  jack_port_t* out_ = nullptr;
  std::vector<uint8_t> outgoing_;
  std::atomic<bool> pending_{false};
};

template <typename Done>
bool within(int ms, Done done) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

}  // namespace

// CTest le 77 como "pulado": sem servidor de audio este teste nao exercita nada.
constexpr int kSkip = 77;

int main() {
  nirbija::Engine engine;
  if (!engine.start("nirbija-exttest")) {
    std::printf("no JACK server available, skipping\n");
    return kSkip;
  }
  DevicePorts device_ports;
  if (!device_ports.start()) {
    std::printf("could not open a second JACK client, skipping\n");
    engine.stop();
    return kSkip;
  }

  nirbija::ExternalMidi device;
  // The server may rename a client to keep names unique: match on the name it got.
  device.match = device_ports.name() + ":Felucca";
  const size_t slot = engine.attach_external(&device);
  expect(slot < nirbija::kMaxExternal && !engine.external_port_name(slot, true).empty(),
         "attach: a slot and its two ports");
  expect(device_ports.connections() == 3,
         "attach: wired both ways to the device's ports, found by name, the clock along");

  std::vector<uint8_t> sysex{0xf0, 0x7d, 0x46, 0x4c, 96};
  for (int i = 0; i < 494; ++i) sysex.push_back(static_cast<uint8_t>(i & 0x7f));
  sysex.push_back(0xf7);
  device.to_device.push(sysex.data(), sysex.size());
  std::vector<uint8_t> got(1024);
  size_t n = 0;
  within(2000, [&] { return (n = device_ports.heard.pop(got.data(), got.size())) > 0; });
  got.resize(n);
  expect(got == sysex, "out: a 500-byte SysEx reaches the device as one event, byte for byte");

  const std::vector<uint8_t> reply{0xf0, 0x7d, 0x46, 0x4c, 25, 0, 0xf7};
  device_ports.send(reply);
  std::vector<uint8_t> back;
  within(2000, [&] {
    uint8_t chunk[256];
    while (size_t k = device.from_device.pop(chunk, sizeof chunk)) back.insert(back.end(), chunk, chunk + k);
    return !back.empty() && back.back() == 0xf7;
  });
  expect(back == reply, "in: what the device sends lands in the worker's ring");

  const auto before = std::chrono::steady_clock::now();
  engine.detach_external(slot);
  const auto took = std::chrono::steady_clock::now() - before;
  expect(took < std::chrono::milliseconds(500) && device_ports.connections() == 0,
         "detach: returns within a few periods, the device's ports unwired");
  device.to_device.push(sysex.data(), sysex.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  expect(device_ports.heard.front() == 0 && device.to_device.front() == sysex.size(),
         "detach: nothing goes out any more, the frame stays in the ring");

  expect(engine.attach_external(&device) == slot, "attach again: the same slot, its ports reused");
  engine.detach_external(slot);

  // A strip with the FM-1 in it: what the strip's MIDI plays reaches the device.
  const size_t channel = engine.add_channel("FM-1", 2);
  auto fm1 = std::make_unique<nirbija::Fm1Instance>();
  nirbija::Fm1Instance* raw = fm1.get();
  raw->external_midi()->match = device.match;
  expect(channel < nirbija::kMaxChannels && engine.graph().channel(channel).add_insert(std::move(fm1)),
         "strip: the FM-1 in a strip");
  const size_t fm1_slot = engine.attach_external(raw->external_midi());
  nirbija::MidiEvent note;
  note.size = 3;
  note.data[0] = 0x92;
  note.data[1] = 64;
  note.data[2] = 90;
  engine.inject_midi(channel, note);
  std::vector<uint8_t> heard(64);
  size_t k = 0;
  within(2000, [&] {
    while ((k = device_ports.heard.pop(heard.data(), heard.size())) > 0)
      if (k == 3 && heard[0] == 0x92) return true;
    return false;
  });
  expect(k == 3 && heard[0] == 0x92 && heard[1] == 64 && heard[2] == 90,
         "strip: a note into the strip comes out to the FM-1, channel 3 as sent");
  engine.detach_external(fm1_slot);

  device_ports.stop();
  engine.stop();
  std::printf("%s\n", failures ? "FAILED" : "all ok");
  return failures ? 1 : 0;
}
