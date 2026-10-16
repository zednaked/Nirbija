// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The FM-1 Link (core/fm1_link.h) against a simulated FM-1 (fake_fm1.h), with
// the two rings the engine would move to and from the USB port and nothing
// else: no JACK, no audio thread. The handshake and WATCH, a firmware too old,
// a request and its reply, pushes landing in the middle of a reply, replies
// cut into small events with a clock byte inside, a lost reply sent again, a
// cable pulled and put back, the keepalive, and parameter moves from the
// audio thread arriving coalesced in TRACK_PARAMS. And the byte ring itself.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "core/external_midi.h"
#include "core/fm1.h"
#include "core/fm1_link.h"
#include "fake_fm1.h"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// Polls until `done` or `ms` pass; the Link and the fake run on their own.
bool within(int ms, const std::function<bool()>& done) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

// Short times so a test of a minute's worth of cable trouble takes a second.
nirbija::Fm1Link::Timing quick() {
  nirbija::Fm1Link::Timing t;
  t.reply_ms = 40;
  t.retries = 1;
  t.ping_ms = 100;
  t.reconnect_ms = 100;
  t.missed_pings = 2;
  return t;
}

using State = nirbija::Fm1Link::State;

void byte_ring() {
  nirbija::RtByteRing<64> ring;
  uint8_t a[20], out[40];
  for (int i = 0; i < 20; ++i) a[i] = static_cast<uint8_t>(i + 1);
  expect(ring.front() == 0 && ring.pop(out, sizeof out) == 0, "ring: empty at first");
  expect(ring.push(a, 20) && ring.push(a, 20) && !ring.push(a, 20),
         "ring: a third frame of 20 does not fit 64 (2 + 20 each), and is not half written");
  expect(ring.front() == 20 && ring.pop(out, sizeof out) == 20 && out[19] == 20,
         "ring: a frame comes out whole");
  expect(!ring.push(a, 0) && !ring.push(a, 33), "ring: an empty frame and one over half the ring are refused");
  ring.pop(out, sizeof out);
  bool same = true;
  for (int k = 0; k < 500; ++k) {  // round and round the wrap, one frame in, one out
    const size_t size = 1 + k % 19;
    a[0] = static_cast<uint8_t>(k);
    a[size - 1] = static_cast<uint8_t>(k ^ 0x55);
    if (!ring.push(a, size) || ring.pop(out, sizeof out) != size || out[0] != a[0] ||
        out[size - 1] != a[size - 1])
      same = false;
  }
  expect(same, "ring: 500 frames of changing sizes across the wrap, each intact");
  ring.push(a, 20);
  expect(ring.pop(out, 10) == 0 && ring.front() == 0, "ring: a frame too large for the reader is dropped, not stuck");
}

void handshake() {
  nirbija::ExternalMidi device;
  FakeFm1 fm1(device);
  nirbija::Fm1Link link(device, quick());
  fm1.start();
  link.start();
  expect(within(1000, [&] { return link.state() == State::Connected; }), "handshake: connected");
  const nirbija::Fm1Info info = link.info();
  expect(info.version == "JANGADA 1.0.1" && info.protocol == 8 && info.tracks == 4 &&
             info.param_count == 105 && info.engine_names.size() == 2 && info.engine_names[1] == "FM6",
         "handshake: INFO read (version, protocol 8, 4 tracks, P_COUNT, the engine names)");
  expect(fm1.watch() == 3, "handshake: WATCH 3 (the other tracks' mix too)");
  const int pings = fm1.received(25);
  std::this_thread::sleep_for(std::chrono::milliseconds(450));
  expect(fm1.received(25) >= pings + 3, "keepalive: a PING every ping_ms while idle");
  link.stop();
  fm1.stop();
}

void outdated() {
  nirbija::ExternalMidi device;
  FakeFm1 fm1(device);
  fm1.protocol = 7;
  nirbija::Fm1Link link(device, quick());
  fm1.start();
  link.start();
  expect(within(1000, [&] { return link.state() == State::Outdated; }), "outdated: protocol 7 is not connected");
  expect(!link.call(25, {}) && fm1.received(22) == 0, "outdated: no calls, and no WATCH sent");
  link.stop();
  fm1.stop();
}

void requests() {
  nirbija::ExternalMidi device;
  FakeFm1 fm1(device);
  nirbija::Fm1Link link(device, quick());
  fm1.start();
  link.start();
  within(1000, [&] { return link.state() == State::Connected; });

  auto r = link.call(31, {1, 9, 20 + 0, 64});  // TRACK_PARAM track 2, id 9 = 20
  expect(r && r->size() == 4 && (*r)[0] == 1 && (*r)[1] == 9 && fm1.param(1, 9) == 20,
         "call: TRACK_PARAM set, its reply back");

  fm1.push_first = true;
  r = link.call(31, {2, 3});
  const auto pushes = link.take_pushes();
  expect(r && (*r)[0] == 2 && (*r)[1] == 3 && !pushes.empty() && pushes.back().cmd == 23 &&
             pushes.back().args.size() == 4 && pushes.back().args[1] == 7,
         "pushes: a CHANGED landing before the reply is queued as a push, the reply still answers");
  fm1.push_first = false;

  fm1.chunk = 3;
  fm1.clock_inside = true;
  r = link.call(31, {0, 5, 100 & 127, 64});
  expect(r && (*r)[1] == 5, "chunks: a reply cut into 3-byte events, a clock byte inside, read whole");
  fm1.chunk = 0;
  fm1.clock_inside = false;

  fm1.drop_replies = 1;
  r = link.call(25, {});
  expect(r.has_value() && link.state() == State::Connected, "retry: one lost reply, sent again, answered");

  expect(!link.call(120, {}) && link.state() == State::Connected,
         "unknown: a command the device ignores gives nothing, and one miss is not a disconnect");

  fm1.push_changed_soon(12, 99);
  expect(within(500, [&] {
           for (const auto& p : link.take_pushes())
             if (p.cmd == 23 && p.args.size() == 4 && p.args[1] == 12) return true;
           return false;
         }),
         "pushes: a knob turned on the device while idle reaches the UI");
  link.stop();
  fm1.stop();
}

void cable() {
  nirbija::ExternalMidi device;
  FakeFm1 fm1(device);
  nirbija::Fm1Link link(device, quick());
  fm1.start();
  link.start();
  within(1000, [&] { return link.state() == State::Connected; });
  fm1.unplugged = true;
  expect(within(1000, [&] { return link.state() == State::Disconnected; }),
         "cable: pulled, disconnected after two missed pings");
  expect(!link.call(25, {}), "cable: a call while disconnected fails at once");
  const int infos = fm1.received(1);
  fm1.unplugged = false;
  expect(within(1500, [&] { return link.state() == State::Connected; }) && fm1.received(1) > infos,
         "cable: put back, INFO again, connected again");
  expect(fm1.watch() == 3, "cable: WATCH asked for again (the device dropped it)");
  link.stop();
  fm1.stop();
}

void params() {
  nirbija::ExternalMidi device;
  FakeFm1 fm1(device);
  nirbija::Fm1Link link(device, quick());
  fm1.start();
  link.start();
  within(1000, [&] { return link.state() == State::Connected; });
  fm1.reply_delay_us = 5000;  // a request answered in ~5 ms, as over USB
  // An audio thread: a block every ~1 ms, three parameters fading, for 300 blocks.
  std::thread audio([&] {
    for (int block = 0; block < 300; ++block) {
      link.set_param_rt(0, 64, block * 127 / 299);
      link.set_param_rt(1, 65, 127 - block * 127 / 299);
      link.set_param_rt(3, 70, block % 128);
      std::this_thread::sleep_for(std::chrono::microseconds(1000));
    }
  });
  audio.join();
  expect(within(1000, [&] {
           return fm1.param_key(0, 64) == 127 && fm1.param_key(1, 65) == 0 &&
                  fm1.param_key(3, 70) == 299 % 128;
         }),
         "params: the last value of each fade arrives, on the index the device gave each key");
  const uint64_t frames = link.params_frames_sent();
  expect(frames > 3 && frames < 150,
         "params: 900 moves over 300 blocks, ~5 ms a reply, went out coalesced in " +
             std::to_string(frames) + " TRACK_PARAMS");
  // 200 parameters at once: two frames, none over 127.
  for (int t = 0; t < 2; ++t)
    for (int id = 0; id < 100; ++id) link.set_param_rt(static_cast<uint8_t>(t), static_cast<uint8_t>(id), 50);
  expect(within(1000, [&] { return fm1.param_key(1, 99) == 50 && fm1.param_key(0, 0) == 50; }) &&
             fm1.largest_params_frame() <= 127,
         "params: 200 at once arrive, no frame over 127 entries");
  link.stop();
  fm1.stop();
}

void instance() {
  bool listed = false;
  for (auto& backend : nirbija::make_all_backends())
    for (const auto& d : backend->scan())
      if (d.uid == "nirbija.fm1" && d.kind == nirbija::PluginKind::Instrument && d.has_midi_input)
        listed = backend->instantiate(d) != nullptr;
  expect(listed, "instance: FM-1 in the picker, an instrument with MIDI in");

  nirbija::Fm1Instance fm1;
  fm1.set_channel_layout(2);
  fm1.activate(48000.0, 256);
  float in_l[4] = {0.1f, 0.2f, 0.3f, 0.4f}, in_r[4] = {-0.1f, -0.2f, -0.3f, -0.4f};
  float out_l[4] = {}, out_r[4] = {};
  const float* ins[2] = {in_l, in_r};
  float* outs[2] = {out_l, out_r};
  fm1.process(ins, outs, 4);
  expect(out_l[3] == 0.4f && out_r[0] == -0.1f, "instance: the strip's input (the FM-1's USB audio) passes");
  float shared[2] = {0.5f, 0.6f};
  const float* same_in[2] = {shared, shared};
  float* same_out[2] = {shared, shared};
  fm1.process(same_in, same_out, 2);
  expect(shared[1] == 0.6f, "instance: in place too");

  nirbija::ExternalMidi* device = fm1.external_midi();
  nirbija::MidiEvent note;
  note.size = 3;
  note.data[0] = 0x91;
  note.data[1] = 60;
  note.data[2] = 100;
  fm1.queue_midi(note);
  expect(device != nullptr && device->match == "Jangada" && device->notes.empty(),
         "instance: matches the ports by \"Jangada\"; a note while unattached is not kept");
  device->serviced = true;
  fm1.queue_midi(note);
  expect(device->notes.count() == 1 && device->notes.begin()->data[0] == 0x91,
         "instance: attached, the note goes to the FM-1 on its own channel (2: track 2)");
}

nirbija::Fm1Link::Timing session_timing() {
  nirbija::Fm1Link::Timing t = quick();
  t.project_quiet_ms = 150;
  t.put_retry_ms = 50;
  return t;
}

std::vector<uint8_t> some_project(size_t n, uint32_t seed) {  // every byte value, top bits too
  std::vector<uint8_t> p(n);
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    p[i] = static_cast<uint8_t>(seed >> 24);
  }
  return p;
}

std::vector<uint8_t> saved_project(const nirbija::Fm1Instance& fm1) {
  std::vector<uint8_t> project;
  nirbija::Fm1Instance::unpack_state(fm1.save_state(), &project);
  return project;
}

void session_read() {
  nirbija::Fm1Instance fm1(session_timing());
  FakeFm1 device(*fm1.external_midi());
  const auto first = some_project(3000, 1);
  device.set_project(first);
  device.start();
  fm1.activate(48000.0, 256);
  expect(fm1.save_state().empty(), "session: nothing to save before the device was ever read");
  expect(within(2000, [&] { return fm1.take_state_dirty(); }) && saved_project(fm1) == first,
         "session: read on connecting, 3000 bytes in 12 pieces, every byte value, the session dirty");

  const auto second = some_project(3100, 2);
  device.set_project(second);
  device.push_changed_soon(5, 1);
  expect(within(2000, [&] { return fm1.take_state_dirty(); }) && saved_project(fm1) == second,
         "session: changed on the device, quiet, read again");
  const uint64_t reads = fm1.link().projects_read();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  expect(fm1.link().projects_read() == reads && !fm1.take_state_dirty(),
         "session: nothing changed, nothing read");

  const auto third = some_project(2900, 3);
  device.set_project(third);
  device.corrupt_next_get = true;
  device.push_changed_soon(5, 2);
  expect(within(3000, [&] { return fm1.take_state_dirty(); }) && saved_project(fm1) == third &&
             fm1.link().projects_read() == reads + 1,
         "session: a piece that changed on the way fails the CRC; read again, whole");
  device.stop();
}

void session_restore() {
  const auto session = some_project(3500, 7);
  const auto stale = some_project(3000, 8);
  nirbija::Fm1Instance fm1(session_timing());
  FakeFm1 device(*fm1.external_midi());
  device.set_project(stale);
  device.unplugged = true;
  expect(fm1.load_state(nirbija::Fm1Instance::pack_state(session)), "restore: the session's blob taken");
  device.start();
  fm1.activate(48000.0, 256);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  expect(saved_project(fm1) == session && fm1.link().put_state() == nirbija::Fm1Link::PutState::Pending,
         "restore: the FM-1 away; the session keeps its project, the put waits");
  device.unplugged = false;
  expect(within(2000, [&] { return fm1.link().put_state() == nirbija::Fm1Link::PutState::Done; }) &&
             device.project() == session && device.commits() == 1,
         "restore: the FM-1 back; it gets the session's project");
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  fm1.take_state_dirty();
  expect(saved_project(fm1) == session, "restore: the FM-1's old project never replaced the session's");

  const auto next = some_project(1000, 9);
  device.playing = true;
  fm1.load_state(nirbija::Fm1Instance::pack_state(next));
  expect(within(1000, [&] {
           return fm1.link().put_state() == nirbija::Fm1Link::PutState::WaitingForStop;
         }) && device.project() == session,
         "restore: the song plays; the put waits for it to stop");
  device.playing = false;
  expect(within(1000, [&] { return fm1.link().put_state() == nirbija::Fm1Link::PutState::Done; }) &&
             device.project() == next,
         "restore: stopped; the project goes in");
  device.stop();
}

void board() {
  nirbija::Fm1Instance fm1(session_timing());
  FakeFm1 device(*fm1.external_midi());
  device.set_param_key(0, 53, 99);  // track 1's CUT
  device.set_param_key(1, 50, -3);  // track 2's FB (FM6)
  device.start();
  fm1.activate(48000.0, 256);
  nirbija::Fm1Link& link = fm1.link();
  expect(within(2000, [&] { return link.board_generation() >= 1; }), "board: read on connecting");
  auto b = link.board();
  expect(b[0].engine_name == "ANALOG" && b[0].params.size() == 18 && b[0].params[0].name == "WAVE" &&
             b[0].params[0].names.size() == 5 && b[0].params[4].name == "CUT",
         "board: track 1 ANALOG, 18 shown, WAVE an enum with its five names, CUT fifth");
  expect(b[1].engine_name == "FM6" && b[1].params[1].name == "FB" && b[1].params[1].min == -7,
         "board: track 2 FM6, its own EDIT (FB -7..7)");
  expect(b[3].engine_name == "DRUMS" && b[3].params.size() == 4 && b[3].params[0].name == "DST",
         "board: the drum track, its four sends only");
  expect(link.value(0, 53) == 99 && link.value(1, 50) == -3,
         "board: values by key, from the index the device gave each");
  expect(device.desc_requests() == 36, "board: two engines described, 18 each (the drum track's sends reuse ANALOG's)");

  const auto params = fm1.parameters();
  bool ids = params.size() == 3 + 18 * 3 + 4;
  for (const auto& p : params)
    if ((p.id >> 8) > 3 || ((p.id & 0xff) >= 128 && (p.id & 0xff) != 200)) ids = false;
  const auto wave = std::find_if(params.begin(), params.end(),
                                 [](const nirbija::ParameterInfo& p) { return p.id == (0u << 8 | 49u); });
  expect(ids && wave != params.end() && wave->stepped && wave->name == "T1 WAVE" &&
             fm1.parameter_value(0u << 8 | 53u) == 99.0,
         "instance: 61 parameters (3 SOUND, 18 x 3, 4), id = track << 8 | key, enums stepped, values read");

  fm1.set_parameter(2u << 8 | 36u, 77);  // track 3's REV from the editor
  expect(within(1000, [&] { return device.param_key(2, 36) == 77; }), "instance: a bar moved here reaches the device");

  device.turn_soon(53, 12);  // CUT turned on the device (track 1 is selected)
  nirbija::TouchedParam touched[8];
  size_t n = 0;
  within(1000, [&] { return (n = fm1.take_touched(touched, 8)) > 0; });
  expect(n == 1 && touched[0].id == (0u << 8 | 53u) && touched[0].value == 12.0 && link.value(0, 53) == 12,
         "instance: a knob turned on the FM-1 is the player's hand, for the scenes");

  const uint64_t gen = link.board_generation();
  device.reload_soon(2, 1);  // track 3 loads an FM6 preset on the device
  expect(within(2000, [&] { return link.board_generation() > gen; }) && link.board()[2].engine_name == "FM6" &&
             link.board()[2].params[0].name == "ALG" && device.desc_requests() == 36 && link.selected_track() == 2,
         "board: RELOAD after an engine change, read again; FM6 already described, no new ENG_DESC");
  device.stop();
}

void sounds() {
  nirbija::Fm1Instance fm1(session_timing());
  FakeFm1 device(*fm1.external_midi());
  device.set_param_key(0, 53, 30);  // track 1: ANALOG, CUT 30
  device.start();
  fm1.activate(48000.0, 256);
  within(2000, [&] { return fm1.link().board_generation() >= 1; });
  const auto params = fm1.parameters();
  const bool listed = std::count_if(params.begin(), params.end(), [](const nirbija::ParameterInfo& p) {
                        return (p.id & 0xff) == 200 && p.stepped && p.max_value == 64.0;
                      }) == 3;
  expect(listed, "sound: one stepped SOUND for each synth track (1..3), 0..64");

  const int dark = fm1.capture_sound(0);
  device.set_param_key(0, 53, 110);  // the hand opens the filter
  const int bright = fm1.capture_sound(0);
  expect(dark == 1 && bright == 2 && fm1.capture_sound(0) == 2 && fm1.sound_labels().size() == 2 &&
             fm1.sound_labels()[0] == "ANALOG" && fm1.take_state_dirty(),
         "sound: captured into the table (the same sound once), the session dirty");

  const uint64_t sent = fm1.link().sounds_sent();
  fm1.set_parameter_rt(0u << 8 | 200u, 1);  // a scene on its bar line: the dark one
  // The device takes the values before it answers: wait for the answer too.
  expect(within(1000, [&] { return device.param_key(0, 53) == 30 && fm1.link().sounds_sent() == sent + 1; }) &&
             fm1.parameter_value(0u << 8 | 200u) == 1.0,
         "sound: SOUND 1 on the bar line, the device plays it whole (CUT back to 30)");
  device.set_param_key(0, 53, 77);  // the hand again, between scenes
  fm1.set_parameter_rt(0u << 8 | 200u, 1);  // the same scene entered again
  expect(within(1000, [&] { return device.param_key(0, 53) == 30 && fm1.link().sounds_sent() == sent + 2; }),
         "sound: the same SOUND again is sent again, over the hand");
  const int puts = device.sound_puts();
  fm1.set_parameter_rt(0u << 8 | 200u, 0);
  fm1.set_parameter_rt(1u << 8 | 200u, 9);  // past the table
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  expect(device.sound_puts() == puts, "sound: 0, or a number past the table, sends nothing");

  // Carried in the session, and sent from it.
  const auto blob = fm1.save_state();
  std::vector<uint8_t> project;
  std::vector<std::vector<uint8_t>> table;
  expect(nirbija::Fm1Instance::unpack_state(blob, &project, &table) && table.size() == 2,
         "sound: the table is in the session");
  nirbija::Fm1Instance again(session_timing());
  expect(again.load_state(blob) && again.sound_labels().size() == 2, "sound: and back from it");
  device.stop();
}

void session_blobs() {
  const auto p = some_project(100, 4);
  auto blob = nirbija::Fm1Instance::pack_state(p);
  std::vector<uint8_t> out;
  expect(nirbija::Fm1Instance::unpack_state(blob, &out) && out == p, "blob: round trip");
  auto later = blob;
  later.insert(later.end(), {9, 3, 0, 0, 0, 1, 2, 3});  // a section from a later Nirbija
  expect(nirbija::Fm1Instance::unpack_state(later, &out) && out == p, "blob: an unknown section skipped");
  auto cut = blob;
  cut.resize(cut.size() - 1);
  auto bad = blob;
  bad[0] = 'X';
  nirbija::Fm1Instance fm1(session_timing());
  expect(!fm1.load_state(cut) && !fm1.load_state(bad) && fm1.save_state().empty() &&
             fm1.link().put_state() == nirbija::Fm1Link::PutState::None,
         "blob: cut short or not ours, refused whole; nothing sent");
}

}  // namespace

int main() {
  byte_ring();
  handshake();
  outdated();
  requests();
  cable();
  params();
  instance();
  board();
  sounds();
  session_blobs();
  session_read();
  session_restore();
  std::printf("%s\n", failures ? "FAILED" : "all ok");
  return failures ? 1 : 0;
}
