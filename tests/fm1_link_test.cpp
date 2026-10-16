// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The FM-1 Link (core/fm1_link.h) against a simulated FM-1 (fake_fm1.h), with
// the two rings the engine would move to and from the USB port and nothing
// else: no JACK, no audio thread. The handshake and WATCH, a firmware too old,
// a request and its reply, pushes landing in the middle of a reply, replies
// cut into small events with a clock byte inside, a lost reply sent again, a
// cable pulled and put back, the keepalive, and parameter moves from the
// audio thread arriving coalesced in TRACK_PARAMS. And the byte ring itself.

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "core/external_midi.h"
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
           return fm1.param(0, 64) == 127 && fm1.param(1, 65) == 0 && fm1.param(3, 70) == 299 % 128;
         }),
         "params: the last value of each fade arrives");
  const uint64_t frames = link.params_frames_sent();
  expect(frames > 3 && frames < 150,
         "params: 900 moves over 300 blocks, ~5 ms a reply, went out coalesced in " +
             std::to_string(frames) + " TRACK_PARAMS");
  // 200 parameters at once: two frames, none over 127.
  for (int t = 0; t < 2; ++t)
    for (int id = 0; id < 100; ++id) link.set_param_rt(static_cast<uint8_t>(t), static_cast<uint8_t>(id), 50);
  expect(within(1000, [&] { return fm1.param(1, 99) == 50 && fm1.param(0, 0) == 50; }) &&
             fm1.largest_params_frame() <= 127,
         "params: 200 at once arrive, no frame over 127 entries");
  link.stop();
  fm1.stop();
}

}  // namespace

int main() {
  byte_ring();
  handshake();
  outdated();
  requests();
  cable();
  params();
  std::printf("%s\n", failures ? "FAILED" : "all ok");
  return failures ? 1 : 0;
}
