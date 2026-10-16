// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

// An FM-1 with Jangada, as far as the Link can tell: it reads the frames the
// Link puts in `to_device` and answers into `from_device`, the two rings the
// engine would otherwise move to and from the USB port. The protocol is the
// part of Jangada's editor protocol (web/EDITOR_PROTOCOL.md there) the Link
// uses, with the same rules: one frame at a time, a reply per request, no
// reply to what it does not understand, pushes whenever.
//
// The knobs make it misbehave the way a real one on a real cable does.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/external_midi.h"

class FakeFm1 {
 public:
  static constexpr int kTracks = 4;
  static constexpr int kParams = 105;

  explicit FakeFm1(nirbija::ExternalMidi& device) : device_(device) {}
  ~FakeFm1() { stop(); }

  void start() {
    running_ = true;
    thread_ = std::thread([this] { run(); });
  }
  void stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
  }

  // --- knobs ------------------------------------------------------------------
  std::atomic<int> protocol{8};
  std::atomic<bool> unplugged{false};    // hears nothing, says nothing
  std::atomic<int> drop_replies{0};      // the next n replies never leave
  std::atomic<bool> push_first{false};   // a CHANGED push before every reply
  std::atomic<int> chunk{0};             // replies cut into events of n bytes (0: whole)
  std::atomic<bool> clock_inside{false}; // a MIDI clock byte in the middle of each reply
  std::atomic<int> reply_delay_us{0};    // the USB round trip and the device's main loop

  // --- what it saw ------------------------------------------------------------
  int received(uint8_t cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_[cmd];
  }
  int watch() const { return watch_.load(); }
  int param(int track, int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return params_[track][id];
  }
  int largest_params_frame() const { return largest_params_.load(); }

  // A push of its own, as when a knob turns on the device. It leaves from the
  // device's own thread, the one producer of `from_device`.
  void push_changed_soon(int id, int value) {
    push_value_ = value;
    push_id_ = id;
  }

 private:
  void push_changed(int id, int value) {
    std::vector<uint8_t> a{0, static_cast<uint8_t>(id)};
    v14(a, value);
    send(23, a);
  }

  static void v14(std::vector<uint8_t>& out, int v) {
    v += 8192;
    out.push_back(static_cast<uint8_t>(v & 0x7f));
    out.push_back(static_cast<uint8_t>((v >> 7) & 0x7f));
  }
  static int rv(const uint8_t* p) { return (p[0] | p[1] << 7) - 8192; }

  void run() {
    uint8_t in[4096];
    while (running_) {
      const int id = push_id_.exchange(-1);
      if (id >= 0 && !unplugged) push_changed(id, push_value_);
      size_t n = device_.to_device.pop(in, sizeof in);
      if (n == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        continue;
      }
      if (unplugged) continue;
      if (n < 6 || in[0] != 0xf0 || in[n - 1] != 0xf7 || in[1] != 0x7d || in[2] != 0x46 ||
          in[3] != 0x4c)
        continue;
      handle(in[4], in + 5, n - 6);
    }
  }

  void handle(uint8_t cmd, const uint8_t* a, size_t na) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counts_[cmd];
    }
    std::vector<uint8_t> r;
    switch (cmd) {
      case 1: {  // INFO
        for (char c : std::string("JANGADA 1.0.1")) r.push_back(static_cast<uint8_t>(c));
        r.push_back(0);
        r.push_back(2);         // NENGINES
        r.push_back(kParams);   // P_COUNT
        r.push_back(30);        // G_COUNT
        r.push_back(64);        // NSTEP
        r.push_back(64);        // P_E0
        for (const char* name : {"ANALOG", "FM6"}) {
          for (const char* c = name; *c; ++c) r.push_back(static_cast<uint8_t>(*c));
          r.push_back(0);
        }
        r.push_back(kTracks);
        r.push_back(static_cast<uint8_t>(protocol.load()));
        r.insert(r.end(), {0x46, 3, 8, 64});
        break;
      }
      case 22:  // WATCH
        if (na < 1) return;
        watch_ = a[0] & 3;
        r.push_back(a[0] & 3);
        break;
      case 25:  // PING
        r.push_back(0);
        break;
      case 31:  // TRACK_PARAM
        if (na < 2 || a[0] >= kTracks || a[1] >= kParams) return;
        if (na >= 4) set(a[0], a[1], rv(a + 2));
        r = {a[0], a[1]};
        v14(r, param(a[0], a[1]));
        break;
      case 97: {  // TRACK_PARAMS
        if (protocol < 8 || na < 1 || na != 1u + 4u * a[0]) return;
        for (size_t k = 0; k < a[0]; ++k)
          if (a[1 + 4 * k] >= kTracks || a[2 + 4 * k] >= kParams) return;
        if (a[0] > largest_params_) largest_params_ = a[0];
        r.push_back(a[0]);
        for (size_t k = 0; k < a[0]; ++k) {
          const uint8_t* q = a + 1 + 4 * k;
          set(q[0], q[1], rv(q + 2));
          r.push_back(q[0]);
          r.push_back(q[1]);
          v14(r, param(q[0], q[1]));
        }
        break;
      }
      default:
        return;  // not understood: no reply, as the device does
    }
    if (reply_delay_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(reply_delay_us.load()));
    if (push_first) push_changed(7, 42);
    if (drop_replies > 0) {
      --drop_replies;
      return;
    }
    send(cmd, r);
  }

  void set(int track, int id, int value) {
    std::lock_guard<std::mutex> lock(mutex_);
    params_[track][id] = value < 0 ? 0 : value > 127 ? 127 : value;
  }

  void send(uint8_t cmd, const std::vector<uint8_t>& args) {
    std::vector<uint8_t> f{0xf0, 0x7d, 0x46, 0x4c, cmd};
    f.insert(f.end(), args.begin(), args.end());
    if (clock_inside && f.size() > 6) f.insert(f.begin() + 6, 0xf8);
    f.push_back(0xf7);
    const size_t step = chunk > 0 ? static_cast<size_t>(chunk.load()) : f.size();
    for (size_t at = 0; at < f.size(); at += step) {
      const size_t n = std::min(step, f.size() - at);
      while (running_ && !device_.from_device.push(f.data() + at, n))
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  nirbija::ExternalMidi& device_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::map<uint8_t, int> counts_;
  int params_[kTracks][kParams] = {};
  std::atomic<int> watch_{0};
  std::atomic<int> largest_params_{0};
  std::atomic<int> push_id_{-1};
  std::atomic<int> push_value_{0};
};
