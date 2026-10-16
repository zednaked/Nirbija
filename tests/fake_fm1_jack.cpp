// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// Manual tool: the simulated FM-1 (fake_fm1.h) on the JACK bus, with the
// ports the real one shows over USB - "Jangada MIDI in", "Jangada MIDI out"
// and two capture ports carrying a quiet tone - so the whole mixer can be
// pointed at it: an FM-1 strip finds it, the Link talks to it, the editor
// draws its tracks. Not a ctest target.
//
//   nirbija_fake_fm1_jack [seconds]

#include <jack/jack.h>
#include <jack/midiport.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "core/external_midi.h"
#include "fake_fm1.h"

namespace {

nirbija::ExternalMidi wire;  // the USB cable: the same two rings the engine uses
jack_port_t* midi_in = nullptr;
jack_port_t* midi_out = nullptr;
jack_port_t* audio[2] = {nullptr, nullptr};
double phase = 0.0;
double rate = 48000.0;

int process(jack_nframes_t frames, void*) {
  void* in = jack_port_get_buffer(midi_in, frames);
  for (uint32_t i = 0; i < jack_midi_get_event_count(in); ++i) {
    jack_midi_event_t event;
    if (jack_midi_event_get(&event, in, i) == 0 && event.size > 0 && event.buffer[0] == 0xf0)
      wire.to_device.push(event.buffer, event.size);
  }
  void* out = jack_port_get_buffer(midi_out, frames);
  jack_midi_clear_buffer(out);
  static uint8_t frame[4096];
  while (size_t n = wire.from_device.front()) {
    if (n > jack_midi_max_event_size(out)) break;
    n = wire.from_device.pop(frame, sizeof frame);
    if (n > 0) jack_midi_event_write(out, frames - 1, frame, n);
  }
  auto* l = static_cast<float*>(jack_port_get_buffer(audio[0], frames));
  auto* r = static_cast<float*>(jack_port_get_buffer(audio[1], frames));
  for (uint32_t i = 0; i < frames; ++i) {
    phase += 220.0 / rate;
    l[i] = r[i] = 0.05f * static_cast<float>(std::sin(2.0 * M_PI * phase));
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 30;
  jack_status_t status;
  jack_client_t* client = jack_client_open("fake-fm1", JackNoStartServer, &status);
  if (client == nullptr) {
    std::fprintf(stderr, "no JACK server\n");
    return 1;
  }
  rate = jack_get_sample_rate(client);
  midi_in = jack_port_register(client, "Jangada MIDI in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
  midi_out = jack_port_register(client, "Jangada MIDI out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
  audio[0] = jack_port_register(client, "Jangada capture_FL", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  audio[1] = jack_port_register(client, "Jangada capture_FR", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

  FakeFm1 device(wire);
  device.reply_delay_us = 3000;
  // A sound worth looking at: an acid ANALOG on track 1, an FM6 piano on 2,
  // ANALOG pads on 3, the drums with some room.
  const int t1[][2] = {{49, 0}, {50, 10}, {51, 64}, {52, 0}, {53, 50}, {54, 100}, {55, 25}, {56, 64},
                       {1, 0}, {2, 55}, {3, 20}, {4, 30}, {33, 20}, {35, 45}, {36, 15}, {9, 60}, {10, 1}};
  for (const auto& kv : t1) device.set_param_key(0, kv[0], kv[1]);
  const int t2[][2] = {{49, 5}, {50, 2}, {53, -12}, {54, 1}, {55, 18}, {56, 10}, {1, 2}, {2, 80},
                       {3, 40}, {4, 70}, {34, 30}, {35, 25}, {36, 50}, {9, 42}};
  for (const auto& kv : t2) device.set_param_key(1, kv[0], kv[1]);
  const int t3[][2] = {{49, 2}, {53, 90}, {54, 30}, {1, 90}, {2, 90}, {3, 110}, {4, 100}, {34, 40}, {36, 100}};
  for (const auto& kv : t3) device.set_param_key(2, kv[0], kv[1]);
  const int t4[][2] = {{33, 10}, {35, 20}, {36, 25}};
  for (const auto& kv : t4) device.set_param_key(3, kv[0], kv[1]);
  std::vector<uint8_t> project(3200);
  for (size_t i = 0; i < project.size(); ++i) project[i] = static_cast<uint8_t>(i * 7);
  device.set_project(project);
  device.start();

  jack_set_process_callback(client, process, nullptr);
  if (jack_activate(client) != 0) return 1;
  std::printf("fake FM-1 on the bus for %d s\n", seconds);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  jack_deactivate(client);
  jack_client_close(client);
  device.stop();
  return 0;
}
