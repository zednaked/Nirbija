#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace nirbija {

// Records channels and the master bus to disk while the mixer runs.
//
// The audio thread only ever writes into a preallocated ring per track and
// bumps an index; a writer thread drains the rings and does the file I/O. That
// split is the whole design: opening a file, allocating, or blocking on a disk
// that has gone away must never happen where a missed deadline is an audible
// dropout.
class Recorder {
 public:
  Recorder();
  ~Recorder();

  // UI thread. `directory` receives one file per armed track. Returns false if
  // the directory cannot be written to, which is worth telling someone about
  // before they play a take.
  bool start(const std::string& directory, double sample_rate,
             const std::vector<std::string>& track_names);
  void stop();

  bool recording() const { return recording_.load(std::memory_order_acquire); }

  // How many frames have been captured, for a running time display.
  uint64_t frames_written() const {
    return frames_written_.load(std::memory_order_relaxed);
  }

  // True when the writer could not keep up and samples were lost. Sticky: a
  // dropout that only showed for a moment still ruins the take, so it stays
  // until the next start().
  bool overran() const { return overran_.load(std::memory_order_relaxed); }

  // Realtime thread. `track` indexes the names passed to start(). Silently does
  // nothing when not recording, so callers need no branch of their own.
  void write(size_t track, const float* const* channels, int channel_count,
             uint32_t frames);

  // Realtime thread. Called once per block after every track has been written.
  void advance(uint32_t frames);

 private:
  struct Track;

  void writer_loop();

  std::vector<std::unique_ptr<Track>> tracks_;
  std::thread writer_;
  // How many audio-thread write() calls are inside the tracks right now. stop()
  // waits for this to fall to zero before freeing them: clearing the vector
  // under a write() that had already passed its recording() check is a
  // use-after-free, and the check alone cannot close that window.
  std::atomic<int> writers_in_flight_{0};
  std::atomic<bool> recording_{false};
  std::atomic<bool> writer_running_{false};
  std::atomic<bool> overran_{false};
  std::atomic<uint64_t> frames_written_{0};
  double sample_rate_ = 0.0;
};

}  // namespace nirbija
