#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace nirbija {

// Single-producer single-consumer ring buffer. The UI thread pushes, the audio
// thread pops; neither ever blocks. Capacity is fixed at compile time so no
// allocation happens on either side after construction.
template <typename T, size_t Capacity>
class RtQueue {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two so the wrap is a mask");

 public:
  // Producer side only.
  bool push(T value) {
    const size_t write = write_.load(std::memory_order_relaxed);
    const size_t next = (write + 1) & kMask;
    if (next == read_.load(std::memory_order_acquire)) return false;  // full
    slots_[write] = std::move(value);
    write_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side only.
  bool pop(T& out) {
    const size_t read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) return false;  // empty
    out = std::move(slots_[read]);
    read_.store((read + 1) & kMask, std::memory_order_release);
    return true;
  }

 private:
  static constexpr size_t kMask = Capacity - 1;
  std::array<T, Capacity> slots_{};
  std::atomic<size_t> write_{0};
  std::atomic<size_t> read_{0};
};

}  // namespace nirbija
