// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nirbija {

// Single-producer single-consumer ring of byte frames, for MIDI that does not
// fit a MidiEvent: a SysEx of a few hundred bytes going to a device, or the
// chunks a device sends back. Each frame is stored whole behind a two-byte
// length, and a push either fits whole or does not happen, so the consumer
// never sees half a message. Neither side blocks or allocates; one is the
// audio thread, the other a worker.
template <size_t Capacity>
class RtByteRing {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two so the wrap is a mask");
  static_assert(Capacity <= 65536, "frame lengths are stored in two bytes");

 public:
  static constexpr size_t kMaxFrame = Capacity / 2;

  // Producer side only. False when the frame is empty, larger than half the
  // ring, or does not fit right now.
  bool push(const uint8_t* data, size_t n) {
    if (n == 0 || n > kMaxFrame) return false;
    const size_t write = write_.load(std::memory_order_relaxed);
    const size_t read = read_.load(std::memory_order_acquire);
    if (Capacity - (write - read) < n + 2) return false;
    put(write, static_cast<uint8_t>(n & 0xff));
    put(write + 1, static_cast<uint8_t>(n >> 8));
    for (size_t i = 0; i < n; ++i) put(write + 2 + i, data[i]);
    write_.store(write + 2 + n, std::memory_order_release);
    return true;
  }

  // Consumer side only. The size of the next frame, 0 when there is none.
  size_t front() const {
    const size_t read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) return 0;
    return static_cast<size_t>(bytes_[read & kMask]) |
           static_cast<size_t>(bytes_[(read + 1) & kMask]) << 8;
  }

  // Consumer side only. Copies the next frame out and drops it; returns its
  // size. A frame larger than `capacity` is dropped and 0 returned, so one bad
  // frame cannot wedge the ring.
  size_t pop(uint8_t* out, size_t capacity) {
    const size_t n = front();
    if (n == 0) return 0;
    const size_t read = read_.load(std::memory_order_relaxed);
    if (n <= capacity)
      for (size_t i = 0; i < n; ++i) out[i] = bytes_[(read + 2 + i) & kMask];
    read_.store(read + 2 + n, std::memory_order_release);
    return n <= capacity ? n : 0;
  }

  // Consumer side only.
  void clear() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  static constexpr size_t kMask = Capacity - 1;
  void put(size_t at, uint8_t byte) { bytes_[at & kMask] = byte; }

  uint8_t bytes_[Capacity] = {};
  // Free-running counters, masked on use: full and empty differ by Capacity,
  // so no byte has to be kept free to tell them apart.
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
};

}  // namespace nirbija
