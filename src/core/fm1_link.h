// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/external_midi.h"
#include "core/rt_queue.h"

namespace nirbija {

// What the FM-1 said about itself in INFO (Jangada's editor protocol,
// web/EDITOR_PROTOCOL.md in the Jangada repository).
struct Fm1Info {
  std::string version;  // "JANGADA 1.0.1"
  int engines = 0;
  int param_count = 0;  // P_COUNT
  int first_engine_param = 0;  // P_E0
  std::vector<std::string> engine_names;
  int tracks = 0;
  int protocol = 0;  // 8 and up: TRACK_SOUND, TRACK_PARAMS, ENG_DESC
};

// Something the FM-1 told us without being asked: CHANGED (23), RELOAD (24),
// STEP_CHANGED (26), TRACK_CHANGED (32). `args` are the bytes after the cmd.
struct Fm1Push {
  uint8_t cmd = 0;
  std::vector<uint8_t> args;
};

// The conversation with one FM-1 (design/fm1.md, "O Link"). A thread of its
// own reads `device.from_device`, writes `device.to_device`, and keeps the
// protocol's one rule: one request at a time, each waiting for its reply.
//
// Disconnected is a normal state, not an error: no port, no answer to INFO or
// to two PINGs in a row. The thread asks again every `reconnect_ms`.
class Fm1Link {
 public:
  static constexpr int kMinProtocol = 8;
  static constexpr size_t kMaxTracks = 4;
  static constexpr size_t kMaxParams = 128;

  struct Timing {
    int reply_ms = 200;      // a reply later than this is a miss
    int retries = 1;         // a miss is sent again this many times
    int ping_ms = 1000;      // the device drops WATCH after 3 s of silence
    int reconnect_ms = 2000;
    int missed_pings = 2;    // this many in a row: disconnected
    int project_quiet_ms = 30000;  // changed, then quiet this long: read the project
    int put_retry_ms = 1000;       // a project waiting for the song to stop asks again
  };

  // Where giving the device a project stands (put_project).
  enum class PutState { None, Pending, WaitingForStop, Done, Refused };

  enum class State { Disconnected, Outdated, Connected };

  explicit Fm1Link(ExternalMidi& device);
  Fm1Link(ExternalMidi& device, Timing timing);
  ~Fm1Link();

  void start();
  void stop();

  State state() const { return state_.load(std::memory_order_acquire); }
  Fm1Info info() const;

  // Any thread but the audio one. Sends F0 7D 46 4C cmd args F7 and waits for
  // the reply with the same cmd; returns the bytes after it. Nothing when
  // not connected, or when the device never answered.
  std::optional<std::vector<uint8_t>> call(uint8_t cmd, const std::vector<uint8_t>& args);

  // Audio thread. A track's parameter (its index on the device) moves. Only
  // the latest value of each goes out, in one TRACK_PARAMS with every other
  // that moved, as soon as the previous request is answered. False when the
  // queue is full, which the next block's value makes good.
  bool set_param_rt(uint8_t track, uint8_t id, int value);

  // UI thread. The pushes since the last call, oldest first.
  std::vector<Fm1Push> take_pushes();

  // --- the project (design/fm1.md, "A sessão") ---------------------------------
  // The FM-1's working project, object 0 of Jangada's backup ("JNG2"). The
  // Link reads it on connecting and after the device changed and went quiet
  // for project_quiet_ms; each read lands here, newest only. Any thread.
  std::optional<std::vector<uint8_t>> take_project();
  // Any thread. Gives the device this project (BK_PUT), as soon as it is
  // connected and its song is stopped, before any read: a session opened with
  // the FM-1 away must not be overwritten by what the FM-1 held. A newer put
  // replaces one not yet done.
  void put_project(std::vector<uint8_t> project);
  PutState put_state() const { return put_state_.load(std::memory_order_acquire); }

  // For tests and the strip's status line.
  uint64_t params_frames_sent() const { return params_frames_.load(std::memory_order_relaxed); }
  uint64_t projects_read() const { return projects_read_.load(std::memory_order_relaxed); }

 private:
  struct Request {
    uint8_t cmd = 0;
    std::vector<uint8_t> args;
    std::optional<std::vector<uint8_t>> reply;
    bool done = false;
  };
  struct ParamMove {
    uint8_t track = 0;
    uint8_t id = 0;
    int16_t value = 0;
  };

  void thread_main();
  // One request on the wire, its retries and its wait. The reply, or nothing.
  std::optional<std::vector<uint8_t>> exchange(uint8_t cmd, const std::vector<uint8_t>& args);
  bool send_frame(uint8_t cmd, const std::vector<uint8_t>& args);
  // Reads what came in; a frame that is a push is queued, one that is the
  // reply to `waiting` (0: none) is returned.
  std::optional<std::vector<uint8_t>> read_incoming(uint8_t waiting);
  bool try_connect();
  void go_disconnected();
  void fail_requests();
  void gather_params();
  std::vector<uint8_t> take_params_frame();
  std::optional<std::vector<uint8_t>> read_project();
  // 0 done, 3 the song plays, anything else refused or lost.
  int write_project(const std::vector<uint8_t>& project);

  ExternalMidi& device_;
  const Timing timing_;
  std::atomic<State> state_{State::Disconnected};
  std::atomic<bool> running_{false};
  std::thread thread_;

  mutable std::mutex mutex_;  // info_, requests_, pushes_
  std::condition_variable wake_;
  Fm1Info info_;
  std::deque<Request*> requests_;
  std::vector<Fm1Push> pushes_;

  RtQueue<ParamMove, 1024> param_moves_;
  // Link thread only: the latest value of each parameter not yet sent.
  std::array<std::array<int16_t, kMaxParams>, kMaxTracks> pending_{};
  std::array<std::array<bool, kMaxParams>, kMaxTracks> dirty_{};
  std::atomic<uint64_t> params_frames_{0};

  // The project. `put_` and `got_` under mutex_; the rest Link thread only.
  std::optional<std::vector<uint8_t>> put_;
  std::optional<std::vector<uint8_t>> got_;
  std::atomic<PutState> put_state_{PutState::None};
  std::atomic<uint64_t> projects_read_{0};
  bool project_stale_ = true;  // changed (or never read) since the last read
  std::chrono::steady_clock::time_point changed_at_{};
  std::chrono::steady_clock::time_point put_after_{};

  // Link thread only: the SysEx being assembled from the incoming chunks.
  std::vector<uint8_t> partial_;
  bool in_sysex_ = false;
};

}  // namespace nirbija
