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

// One parameter of a track as the editor shows it, from ENG_DESC.
struct Fm1Param {
  uint8_t key = 0;  // its stable key (Jangada's keys.h): what ids and sessions use
  std::string name;
  int min = 0;
  int max = 127;
  int def = 0;
  std::vector<std::string> names;  // an enum's value names, else empty
};

// What the editor shows of one track: its engine and its parameters, in the
// order they are drawn. Empty `params` until the device was read.
struct Fm1Track {
  int engine = -1;  // NENGINES (info().engines) = the drum track
  std::string engine_name;
  std::vector<Fm1Param> params;
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

  // Audio thread. A track's parameter, named by its stable key, moves. Only
  // the latest value of each goes out, in one TRACK_PARAMS with every other
  // that moved, as soon as the previous request is answered (the Link turns
  // the key into the device's index). False when the queue is full, which the
  // next block's value makes good.
  bool set_param_rt(uint8_t track, uint8_t key, int value);
  // The same from any other thread (the UI).
  void set_param(uint8_t track, uint8_t key, int value);

  // --- the tracks as the editor shows them -----------------------------------
  // Read on connecting and after every RELOAD (an engine, a preset or a
  // project loaded on the device): each track's engine and parameters.
  // `board_generation()` moves each time the list changed. Any thread.
  std::array<Fm1Track, kMaxTracks> board() const;
  uint64_t board_generation() const { return board_gen_.load(std::memory_order_acquire); }
  // The track selected on the device (its CHANGED pushes are about it).
  int selected_track() const { return selected_.load(std::memory_order_relaxed); }
  // A value as last known: read from the device, pushed by it, or set here.
  // Any thread, the audio one too. 0 for a key never seen.
  int value(uint8_t track, uint8_t key) const;
  // Values the player turned on the device since the last call (CHANGED
  // pushes), as (track, key, value). UI thread.
  struct Touch {
    uint8_t track, key;
    int value;
  };
  std::vector<Touch> take_touches();

  // --- whole sounds (design/fm1.md, "As cenas") -------------------------------
  // A sound is a TRACK_SOUND frame without its track byte: engine, count,
  // the values by key, an FM6 track's patch. The table is the session's.
  // UI thread.
  void set_sound_table(std::vector<std::vector<uint8_t>> table);
  // Audio thread (a scene on its bar line) or any other: track `track` is to
  // play sound `index` of the table (1-based; 0 leaves the track alone).
  // Every call counts, the same index again included, so a scene entered
  // again restores its sound over whatever the hand did meanwhile. Sent
  // before any other request: it is the one with a bar line to meet.
  void want_sound_rt(uint8_t track, int index);
  int wanted_sound(uint8_t track) const {
    return track < kMaxTracks ? sound_want_[track].load(std::memory_order_relaxed) : 0;
  }
  uint64_t sounds_sent() const { return sounds_sent_.load(std::memory_order_relaxed); }

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
  // Any thread. Read the project now instead of waiting for a quiet spell
  // (the editor's "Read from FM-1").
  void read_project_now() {
    read_now_.store(true, std::memory_order_release);
    wake_.notify_all();
  }

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
  bool read_board();
  bool send_wanted_sounds();
  void note_push(uint8_t cmd, const std::vector<uint8_t>& args);
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
  std::vector<ParamMove> ui_moves_;  // under mutex_
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
  std::atomic<bool> read_now_{false};

  // The board. `board_` and `touches_` under mutex_; values atomic; the maps
  // and the description cache Link thread only.
  std::array<Fm1Track, kMaxTracks> board_{};
  std::atomic<uint64_t> board_gen_{0};
  std::atomic<int> selected_{0};
  std::array<std::array<std::atomic<int16_t>, kMaxParams>, kMaxTracks> values_{};
  std::vector<Touch> touches_;
  std::array<int16_t, kMaxParams> key_to_index_{};  // -1: unknown
  std::array<int16_t, 256> index_to_key_{};
  std::vector<std::pair<int, std::vector<Fm1Param>>> descs_;  // per engine
  bool board_stale_ = true;

  std::vector<std::vector<uint8_t>> sounds_;  // under mutex_
  std::array<std::atomic<int>, kMaxTracks> sound_want_{};
  std::array<std::atomic<uint32_t>, kMaxTracks> sound_seq_{};
  std::array<uint32_t, kMaxTracks> sound_done_seq_{};  // Link thread only
  std::atomic<uint64_t> sounds_sent_{0};
  std::chrono::steady_clock::time_point changed_at_{};
  std::chrono::steady_clock::time_point put_after_{};

  // Link thread only: the SysEx being assembled from the incoming chunks.
  std::vector<uint8_t> partial_;
  bool in_sysex_ = false;
};

}  // namespace nirbija
