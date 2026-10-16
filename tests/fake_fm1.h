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

#include <algorithm>
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
  std::atomic<bool> playing{false};      // the song plays: a project commit answers 3
  std::atomic<bool> corrupt_next_get{false};  // one BK_GET answers with a byte changed

  // The working project, object 0 of the backup. The test sets it and reads
  // it back; a commit replaces it.
  void set_project(const std::vector<uint8_t>& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    project_ = p;
  }
  std::vector<uint8_t> project() {
    std::lock_guard<std::mutex> lock(mutex_);
    return project_;
  }
  int commits() const { return commits_.load(); }

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
  static void put35(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 5; ++i) out.push_back(static_cast<uint8_t>((v >> (7 * i)) & 0x7f));
  }
  static uint32_t get35(const uint8_t* a) {
    return a[0] | a[1] << 7 | a[2] << 14 | static_cast<uint32_t>(a[3]) << 21 |
           static_cast<uint32_t>(a[4]) << 28;
  }
  static uint32_t crc32(const std::vector<uint8_t>& v) {
    uint32_t c = 0xffffffffu;
    for (uint8_t b : v) {
      c ^= b;
      for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
  }
  static void pack7(std::vector<uint8_t>& out, const std::vector<uint8_t>& in) {
    for (size_t at = 0; at < in.size(); at += 7) {
      const size_t k = std::min<size_t>(7, in.size() - at);
      uint8_t top = 0;
      for (size_t i = 0; i < k; ++i) top |= static_cast<uint8_t>((in[at + i] >> 7) << i);
      out.push_back(top);
      for (size_t i = 0; i < k; ++i) out.push_back(in[at + i] & 0x7f);
    }
  }
  static std::vector<uint8_t> unpack7(const uint8_t* a, size_t n) {
    std::vector<uint8_t> out;
    while (n > 0) {
      const uint8_t top = *a++;
      --n;
      for (size_t j = 0; j < 7 && n > 0; ++j, --n) out.push_back(static_cast<uint8_t>(*a++ | ((top >> j) & 1) << 7));
    }
    return out;
  }

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
      case 34: {  // BK_LIST: rc, count, per object: id, length u35, CRC u35 (only object 0 here)
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = project_;
        r = {0, 1, 0};
        put35(r, static_cast<uint32_t>(snapshot_.size()));
        put35(r, crc32(snapshot_));
        break;
      }
      case 35: {  // BK_GET: id, offset u35, count -> id, rc, offset, count, pack7
        if (na != 8) return;
        const uint32_t off = get35(a + 1), count = a[6] | a[7] << 7;
        std::vector<uint8_t> bytes;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (a[0] != 0 || count == 0 || count > 256 || off > snapshot_.size() ||
              count > snapshot_.size() - off) {
            r = {a[0], 1};
            put35(r, off);
            r.insert(r.end(), {0, 0});
            break;
          }
          bytes.assign(snapshot_.begin() + off, snapshot_.begin() + off + count);
        }
        if (corrupt_next_get.exchange(false)) bytes[0] ^= 1;
        r = {0, 0};
        put35(r, off);
        r.push_back(static_cast<uint8_t>(count & 127));
        r.push_back(static_cast<uint8_t>(count >> 7));
        pack7(r, bytes);
        break;
      }
      case 36: {  // BK_PUT: op, id, ... -> op, id, rc
        if (na < 2) return;
        const uint8_t op = a[0];
        uint8_t rc = 1;
        std::lock_guard<std::mutex> lock(mutex_);
        if (op == 0 && na == 12 && a[1] == 0) {
          staging_.clear();
          staged_len_ = get35(a + 2);
          staged_crc_ = get35(a + 7);
          staging_on_ = true;
          rc = 0;
        } else if (!staging_on_) {
          rc = 5;
        } else if (op == 1 && na >= 9 && get35(a + 2) == staging_.size()) {
          const std::vector<uint8_t> bytes = unpack7(a + 7, na - 7);
          staging_.insert(staging_.end(), bytes.begin(), bytes.end());
          rc = staging_.size() <= staged_len_ ? 0 : 1;
        } else if (op == 2 && na == 2) {
          staging_on_ = false;
          if (staging_.size() != staged_len_ || crc32(staging_) != staged_crc_) rc = 2;
          else if (playing) rc = 3;
          else {
            project_ = staging_;
            ++commits_;
            rc = 0;
          }
        } else if (op == 3) {
          staging_on_ = false;
          rc = 0;
        }
        r = {a[0], a[1], rc};
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
  std::vector<uint8_t> project_, snapshot_, staging_;
  uint32_t staged_len_ = 0, staged_crc_ = 0;
  bool staging_on_ = false;
  std::atomic<int> commits_{0};
  std::atomic<int> push_value_{0};
};
