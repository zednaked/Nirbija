// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/fm1_link.h"

#include <algorithm>
#include <chrono>

namespace nirbija {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint8_t kHeader[3] = {0x7d, 0x46, 0x4c};  // non-commercial ID, "FL"
constexpr uint8_t kInfo = 1;
constexpr uint8_t kWatch = 22;
constexpr uint8_t kPing = 25;
constexpr uint8_t kTrackParams = 97;
constexpr uint8_t kBackupList = 34;
constexpr uint8_t kBackupGet = 35;
constexpr uint8_t kBackupPut = 36;
constexpr size_t kChunk = 256;  // the most one BK_GET / BK_PUT carries
constexpr size_t kMaxFrame = 1024;  // the device's largest reply is under 900
constexpr size_t kMaxPushes = 1024;

bool is_push(uint8_t cmd) { return cmd == 23 || cmd == 24 || cmd == 26 || cmd == 32; }

// CRC-32 as zlib and the device (storage.c st_crc32) count it.
uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xffffffffu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// Numbers as five 7-bit bytes, LSB first (u35).
void put35(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 5; ++i) out.push_back(static_cast<uint8_t>((v >> (7 * i)) & 0x7f));
}
uint32_t get35(const uint8_t* a) {
  return static_cast<uint32_t>(a[0]) | static_cast<uint32_t>(a[1]) << 7 |
         static_cast<uint32_t>(a[2]) << 14 | static_cast<uint32_t>(a[3]) << 21 |
         static_cast<uint32_t>(a[4]) << 28;
}

// pack7: groups of up to 7 bytes, each behind a byte holding their top bits.
void pack7(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
  while (n > 0) {
    const size_t k = std::min<size_t>(n, 7);
    uint8_t top = 0;
    for (size_t i = 0; i < k; ++i) top |= static_cast<uint8_t>((p[i] >> 7) << i);
    out.push_back(top);
    for (size_t i = 0; i < k; ++i) out.push_back(p[i] & 0x7f);
    p += k;
    n -= k;
  }
}
std::vector<uint8_t> unpack7(const uint8_t* a, size_t n) {
  std::vector<uint8_t> out;
  while (n > 0) {
    const uint8_t top = *a++;
    --n;
    for (size_t j = 0; j < 7 && n > 0; ++j, --n)
      out.push_back(static_cast<uint8_t>(*a++ | ((top >> j) & 1u) << 7));
  }
  return out;
}

// INFO's reply: version, NENGINES, P_COUNT, G_COUNT, NSTEP, P_E0, the engine
// names, NTRK, the protocol version (and more, which is not needed here).
std::optional<Fm1Info> parse_info(const std::vector<uint8_t>& a) {
  Fm1Info info;
  size_t i = 0;
  auto text = [&](std::string& out) {
    while (i < a.size() && a[i] != 0) out.push_back(static_cast<char>(a[i++]));
    if (i >= a.size()) return false;
    ++i;
    return true;
  };
  if (!text(info.version) || i + 5 > a.size()) return std::nullopt;
  info.engines = a[i++];
  info.param_count = a[i++];
  ++i;  // G_COUNT
  ++i;  // NSTEP
  info.first_engine_param = a[i++];
  for (int e = 0; e < info.engines; ++e) {
    std::string name;
    if (!text(name)) return std::nullopt;
    info.engine_names.push_back(name);
  }
  info.tracks = i < a.size() ? a[i++] : 1;
  info.protocol = i < a.size() ? a[i++] : 0;
  return info;
}

}  // namespace

Fm1Link::Fm1Link(ExternalMidi& device) : Fm1Link(device, Timing{}) {}
Fm1Link::Fm1Link(ExternalMidi& device, Timing timing) : device_(device), timing_(timing) {}
Fm1Link::~Fm1Link() { stop(); }

void Fm1Link::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { thread_main(); });
}

void Fm1Link::stop() {
  if (!running_.exchange(false)) return;
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  fail_requests();
  state_.store(State::Disconnected, std::memory_order_release);
}

Fm1Info Fm1Link::info() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return info_;
}

std::optional<std::vector<uint8_t>> Fm1Link::call(uint8_t cmd,
                                                  const std::vector<uint8_t>& args) {
  if (state() != State::Connected || !running_.load()) return std::nullopt;
  Request request;
  request.cmd = cmd;
  request.args = args;
  std::unique_lock<std::mutex> lock(mutex_);
  requests_.push_back(&request);
  wake_.notify_all();
  wake_.wait(lock, [&] { return request.done; });
  return request.reply;
}

bool Fm1Link::set_param_rt(uint8_t track, uint8_t id, int value) {
  ParamMove move;
  move.track = track;
  move.id = id;
  move.value = static_cast<int16_t>(value < -8192 ? -8192 : value > 8191 ? 8191 : value);
  return param_moves_.push(move);
}

std::optional<std::vector<uint8_t>> Fm1Link::take_project() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<std::vector<uint8_t>> out;
  out.swap(got_);
  return out;
}

void Fm1Link::put_project(std::vector<uint8_t> project) {
  std::lock_guard<std::mutex> lock(mutex_);
  put_ = std::move(project);
  put_after_ = {};
  put_state_.store(PutState::Pending, std::memory_order_release);
  wake_.notify_all();
}

// BK_LIST (which takes the snapshot of the working project), then BK_GET of
// object 0 in pieces, checked against the CRC the list gave.
std::optional<std::vector<uint8_t>> Fm1Link::read_project() {
  auto list = exchange(kBackupList, {});
  if (!list || list->size() < 2 || (*list)[0] != 0) return std::nullopt;
  uint32_t length = 0, crc = 0;
  bool found = false;
  for (size_t i = 0; i < (*list)[1] && 2 + 11 * i + 11 <= list->size(); ++i) {
    const uint8_t* entry = list->data() + 2 + 11 * i;
    if (entry[0] != 0) continue;
    length = get35(entry + 1);
    crc = get35(entry + 6);
    found = true;
  }
  if (!found || length == 0 || length > 65536) return std::nullopt;
  std::vector<uint8_t> project;
  while (project.size() < length) {
    const uint32_t at = static_cast<uint32_t>(project.size());
    const uint32_t count = std::min<uint32_t>(kChunk, length - at);
    std::vector<uint8_t> args{0};
    put35(args, at);
    args.push_back(static_cast<uint8_t>(count & 0x7f));
    args.push_back(static_cast<uint8_t>(count >> 7));
    auto got = exchange(kBackupGet, args);
    // id, rc, offset, count, the bytes
    if (!got || got->size() < 9 || (*got)[0] != 0 || (*got)[1] != 0 || get35(got->data() + 2) != at)
      return std::nullopt;
    std::vector<uint8_t> bytes = unpack7(got->data() + 9, got->size() - 9);
    if (bytes.size() != count) return std::nullopt;
    project.insert(project.end(), bytes.begin(), bytes.end());
  }
  if (crc32(project.data(), project.size()) != crc) return std::nullopt;  // changed while read
  return project;
}

// BK_PUT of object 0: begin, the bytes in order, commit (the device checks the
// CRC and the project, then loads it). A put lost part way is aborted, so the
// device does not hold its buffer for the 15 s a staging lasts.
int Fm1Link::write_project(const std::vector<uint8_t>& project) {
  auto rc_of = [](const std::optional<std::vector<uint8_t>>& r) {
    return r && r->size() >= 3 ? static_cast<int>((*r)[2]) : -1;
  };
  std::vector<uint8_t> begin{0, 0};
  put35(begin, static_cast<uint32_t>(project.size()));
  put35(begin, crc32(project.data(), project.size()));
  int rc = rc_of(exchange(kBackupPut, begin));
  if (rc != 0) return rc;
  for (size_t at = 0; at < project.size() && rc == 0; at += kChunk) {
    std::vector<uint8_t> data{1, 0};
    put35(data, static_cast<uint32_t>(at));
    pack7(data, project.data() + at, std::min(kChunk, project.size() - at));
    rc = rc_of(exchange(kBackupPut, data));
  }
  if (rc == 0) return rc_of(exchange(kBackupPut, {2, 0}));
  exchange(kBackupPut, {3, 0});
  return rc;
}

std::vector<Fm1Push> Fm1Link::take_pushes() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Fm1Push> out;
  out.swap(pushes_);
  return out;
}

void Fm1Link::fail_requests() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Request* request : requests_) {
    request->reply.reset();
    request->done = true;
  }
  requests_.clear();
  wake_.notify_all();
}

void Fm1Link::go_disconnected() {
  state_.store(State::Disconnected, std::memory_order_release);
  fail_requests();
  project_stale_ = true;  // read again on the way back, unless a put waits
  changed_at_ = {};
}

bool Fm1Link::send_frame(uint8_t cmd, const std::vector<uint8_t>& args) {
  read_incoming(0);  // a late reply to an earlier request must not answer this one
  std::vector<uint8_t> frame;
  frame.reserve(args.size() + 6);
  frame.push_back(0xf0);
  frame.insert(frame.end(), kHeader, kHeader + 3);
  frame.push_back(cmd & 0x7f);
  for (uint8_t b : args) frame.push_back(b & 0x7f);
  frame.push_back(0xf7);
  return device_.to_device.push(frame.data(), frame.size());
}

std::optional<std::vector<uint8_t>> Fm1Link::read_incoming(uint8_t waiting) {
  std::optional<std::vector<uint8_t>> reply;
  uint8_t chunk[ExternalMidi::kRingBytes / 2];
  while (size_t n = device_.from_device.pop(chunk, sizeof chunk)) {
    for (size_t i = 0; i < n; ++i) {
      const uint8_t b = chunk[i];
      if (b >= 0xf8) continue;  // clock and the like may land inside a SysEx
      if (b == 0xf0) {
        partial_.clear();
        in_sysex_ = true;
        continue;
      }
      if (!in_sysex_) continue;  // notes, the device's keys: not ours to read
      if (b == 0xf7) {
        in_sysex_ = false;
        if (partial_.size() < 4 || partial_[0] != kHeader[0] || partial_[1] != kHeader[1] ||
            partial_[2] != kHeader[2])
          continue;  // DX7 SysEx and the update protocol share the port
        const uint8_t cmd = partial_[3];
        std::vector<uint8_t> args(partial_.begin() + 4, partial_.end());
        if (is_push(cmd)) {
          project_stale_ = true;  // something changed on the device
          changed_at_ = Clock::now();
          std::lock_guard<std::mutex> lock(mutex_);
          if (pushes_.size() >= kMaxPushes) pushes_.erase(pushes_.begin());
          pushes_.push_back(Fm1Push{cmd, std::move(args)});
        } else if (waiting != 0 && cmd == waiting && !reply) {
          reply = std::move(args);
        }
        continue;
      }
      if (b >= 0x80) {  // any other status ends a SysEx that never closed
        in_sysex_ = false;
        continue;
      }
      if (partial_.size() < kMaxFrame) partial_.push_back(b);
      else in_sysex_ = false;
    }
  }
  return reply;
}

std::optional<std::vector<uint8_t>> Fm1Link::exchange(uint8_t cmd,
                                                      const std::vector<uint8_t>& args) {
  for (int attempt = 0; attempt <= timing_.retries; ++attempt) {
    if (!send_frame(cmd, args)) continue;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timing_.reply_ms);
    while (running_.load(std::memory_order_relaxed)) {
      if (auto reply = read_incoming(cmd)) return reply;
      if (Clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!running_.load(std::memory_order_relaxed)) break;
  }
  return std::nullopt;
}

bool Fm1Link::try_connect() {
  auto reply = exchange(kInfo, {});
  if (!reply) return false;
  auto parsed = parse_info(*reply);
  if (!parsed) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    info_ = *parsed;
  }
  if (parsed->protocol < kMinProtocol) {
    state_.store(State::Outdated, std::memory_order_release);
    return false;
  }
  // Bit 1: TRACK_CHANGED too, the mix of the tracks that are not selected.
  if (!exchange(kWatch, {3})) return false;
  state_.store(State::Connected, std::memory_order_release);
  return true;
}

void Fm1Link::gather_params() {
  ParamMove move;
  while (param_moves_.pop(move)) {
    if (move.track >= kMaxTracks || move.id >= kMaxParams) continue;
    pending_[move.track][move.id] = move.value;
    dirty_[move.track][move.id] = true;
  }
}

// TRACK_PARAMS: n, n x (track, id, value + 8192 as two 7-bit bytes). At most
// 127 a frame; the rest wait for the next.
std::vector<uint8_t> Fm1Link::take_params_frame() {
  std::vector<uint8_t> args{0};
  for (size_t t = 0; t < kMaxTracks; ++t) {
    for (size_t id = 0; id < kMaxParams && args[0] < 127; ++id) {
      if (!dirty_[t][id]) continue;
      dirty_[t][id] = false;
      const int v = pending_[t][id] + 8192;
      args.push_back(static_cast<uint8_t>(t));
      args.push_back(static_cast<uint8_t>(id));
      args.push_back(static_cast<uint8_t>(v & 0x7f));
      args.push_back(static_cast<uint8_t>((v >> 7) & 0x7f));
      ++args[0];
    }
  }
  return args;
}

void Fm1Link::thread_main() {
  auto next_connect = Clock::now();
  auto last_sent = Clock::now();
  int misses = 0;
  while (running_.load(std::memory_order_relaxed)) {
    const auto now = Clock::now();
    if (state() != State::Connected) {
      fail_requests();  // nothing to send them to
      gather_params();  // and nothing to send these to: drop them
      for (auto& row : dirty_) row.fill(false);
      if (now >= next_connect) {
        misses = 0;
        try_connect();
        last_sent = Clock::now();
        next_connect = Clock::now() + std::chrono::milliseconds(timing_.reconnect_ms);
      } else {
        read_incoming(0);
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(5));
      }
      continue;
    }

    // The project first: a put waiting, else a read when one is due. Each
    // counts as a request for the keepalive and the miss count.
    std::optional<std::vector<uint8_t>> to_put;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (put_ && now >= put_after_) to_put = put_;
    }
    if (to_put) {
      const int rc = write_project(*to_put);
      std::lock_guard<std::mutex> lock(mutex_);
      const bool same = put_ && *put_ == *to_put;  // not replaced meanwhile
      // 3: the song plays; 5: the staging lapsed (a USB reset); -1: lost. Again later.
      if (rc == 0 || rc == 1 || rc == 2 || rc == 4) {
        if (same) {
          put_.reset();
          put_state_.store(rc == 0 ? PutState::Done : PutState::Refused, std::memory_order_release);
        }
        if (rc == 0) {
          project_stale_ = false;  // the device holds what the session holds
          changed_at_ = Clock::now();
        }
      } else {
        if (same) {
          put_after_ = Clock::now() + std::chrono::milliseconds(timing_.put_retry_ms);
          if (rc == 3) put_state_.store(PutState::WaitingForStop, std::memory_order_release);
        }
      }
      last_sent = Clock::now();
      misses = rc >= 0 ? 0 : misses + 1;
      if (misses >= timing_.missed_pings) {
        go_disconnected();
        next_connect = Clock::now() + std::chrono::milliseconds(timing_.reconnect_ms);
      }
      continue;
    }
    bool put_waiting;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      put_waiting = put_.has_value();
    }
    if (project_stale_ && !put_waiting &&
        now - changed_at_ >= std::chrono::milliseconds(timing_.project_quiet_ms)) {
      auto project = read_project();
      if (project) {
        std::lock_guard<std::mutex> lock(mutex_);
        got_ = std::move(project);
        projects_read_.fetch_add(1, std::memory_order_relaxed);
        project_stale_ = false;
      } else {
        changed_at_ = Clock::now();  // try again after another quiet spell
      }
      last_sent = Clock::now();
      continue;
    }

    std::optional<std::vector<uint8_t>> reply;
    bool sent = true;
    gather_params();
    std::vector<uint8_t> params = take_params_frame();
    Request* request = nullptr;
    if (params[0] > 0) {
      reply = exchange(kTrackParams, params);
      params_frames_.fetch_add(1, std::memory_order_relaxed);
    } else {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!requests_.empty()) {
          request = requests_.front();
          requests_.pop_front();
        }
      }
      if (request != nullptr) {
        reply = exchange(request->cmd, request->args);
        std::lock_guard<std::mutex> lock(mutex_);
        request->reply = reply;
        request->done = true;
        wake_.notify_all();
      } else if (now - last_sent >= std::chrono::milliseconds(timing_.ping_ms)) {
        reply = exchange(kPing, {});
      } else {
        sent = false;
        read_incoming(0);
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(1));
      }
    }
    if (!sent) continue;
    last_sent = Clock::now();
    misses = reply ? 0 : misses + 1;
    if (misses >= timing_.missed_pings) {
      go_disconnected();
      next_connect = Clock::now() + std::chrono::milliseconds(timing_.reconnect_ms);
    }
  }
}

}  // namespace nirbija
