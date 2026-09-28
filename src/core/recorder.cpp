// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#include "core/recorder.h"

#include <sndfile.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

namespace nirbija {
namespace {

namespace fs = std::filesystem;

// Eight seconds of stereo at 48 kHz. Generous on purpose: the writer only wakes
// every few milliseconds, and a disk that stalls briefly should cost nothing.
constexpr size_t kRingFrames = 8 * 48000;

// Characters that are legal in a channel name but a nuisance in a filename.
std::string sanitise(const std::string& name) {
  std::string clean;
  clean.reserve(name.size());
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') {
      clean.push_back('_');
    } else {
      clean.push_back(c);
    }
  }
  if (clean.empty()) clean = "track";
  return clean;
}

}  // namespace

// One recorded track: a ring the audio thread fills and the writer drains.
struct Recorder::Track {
  std::string name;
  int channels = 2;
  SNDFILE* file = nullptr;

  // Interleaved, so a frame is `channels` consecutive floats.
  std::vector<float> ring;
  std::atomic<size_t> write_frame{0};
  size_t read_frame = 0;

  size_t ring_frames() const { return ring.size() / static_cast<size_t>(channels); }
};

Recorder::Recorder() = default;

Recorder::~Recorder() { stop(); }

bool Recorder::start(const std::string& directory, double sample_rate,
                     const std::vector<std::string>& track_names) {
  if (recording()) return true;
  if (track_names.empty()) return false;

  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec) return false;

  sample_rate_ = sample_rate;
  tracks_.clear();
  overran_.store(false, std::memory_order_relaxed);
  frames_written_.store(0, std::memory_order_relaxed);

  for (size_t i = 0; i < track_names.size(); ++i) {
    auto track = std::make_unique<Track>();
    track->name = track_names[i];
    track->channels = 2;
    track->ring.assign(kRingFrames * 2, 0.0f);

    SF_INFO info{};
    info.samplerate = static_cast<int>(sample_rate);
    info.channels = track->channels;
    // Float rather than 24-bit: a mixer bus can exceed full scale, and a take
    // that clipped on the way to disk cannot be brought back.
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    const fs::path path =
        fs::path(directory) /
        (std::to_string(i + 1) + " " + sanitise(track->name) + ".wav");
    track->file = sf_open(path.c_str(), SFM_WRITE, &info);
    if (track->file == nullptr) {
      // Half a recording is worse than none: unwind and report the failure.
      for (auto& opened : tracks_)
        if (opened->file != nullptr) sf_close(opened->file);
      tracks_.clear();
      return false;
    }
    tracks_.push_back(std::move(track));
  }

  writer_running_.store(true, std::memory_order_release);
  recording_.store(true, std::memory_order_release);
  writer_ = std::thread([this] { writer_loop(); });
  return true;
}

void Recorder::stop() {
  if (!recording()) return;

  // The audio thread stops first, then the writer drains what is left.
  //
  // This store and the in-flight load below, against write()'s increment and
  // its load of recording_, are a store-then-load on each side (Dekker's
  // pattern). With release/acquire both sides can read the stale value at
  // once - this thread sees no writer in flight while write() still sees
  // recording - so all four are sequentially consistent.
  recording_.store(false, std::memory_order_seq_cst);

  // Any write() already past its check still holds a Track. Freeing them now
  // would pull the ring out from under it, so wait for the last one out. This
  // is the UI thread; a block is the longest it can possibly take.
  while (writers_in_flight_.load(std::memory_order_seq_cst) != 0)
    std::this_thread::yield();

  writer_running_.store(false, std::memory_order_release);
  if (writer_.joinable()) writer_.join();

  for (auto& track : tracks_) {
    if (track->file == nullptr) continue;
    sf_close(track->file);
    track->file = nullptr;
  }
  tracks_.clear();
}

void Recorder::write(size_t track_index, const float* const* channels,
                     int channel_count, uint32_t frames) {
  // Announce first, check second. The other order leaves a window where stop()
  // sees no writers, frees the tracks, and this call then walks into them.
  // Sequentially consistent for the same reason as in stop(): see there.
  writers_in_flight_.fetch_add(1, std::memory_order_seq_cst);
  if (recording_.load(std::memory_order_seq_cst) && track_index < tracks_.size() &&
      channel_count > 0) {
    Track& track = *tracks_[track_index];
    const size_t capacity = track.ring_frames();
    const size_t write_frame = track.write_frame.load(std::memory_order_relaxed);
    size_t slot = (write_frame % capacity) * static_cast<size_t>(track.channels);
    const size_t wrap = track.ring.size();
    float* ring = track.ring.data();

    // The source of each side chosen once, outside the loop. A mono channel
    // is written to both sides rather than to half a file.
    const int width = std::min(track.channels, 2);
    const float* source[2] = {channels[0],
                              channels[std::min(1, channel_count - 1)]};

    for (uint32_t f = 0; f < frames; ++f) {
      for (int ch = 0; ch < width; ++ch) ring[slot + ch] = source[ch][f];
      // Walked rather than recomputed: the modulo per sample per channel was
      // a division in the innermost loop of the audio thread.
      slot += static_cast<size_t>(track.channels);
      if (slot >= wrap) slot = 0;
    }

    track.write_frame.store(write_frame + frames, std::memory_order_release);
  }
  writers_in_flight_.fetch_sub(1, std::memory_order_release);
}

void Recorder::advance(uint32_t frames) {
  if (!recording()) return;
  frames_written_.fetch_add(frames, std::memory_order_relaxed);
}

void Recorder::writer_loop() {
  std::vector<float> chunk;

  while (true) {
    const bool running = writer_running_.load(std::memory_order_acquire);

    bool wrote_anything = false;
    for (auto& track : tracks_) {
      const size_t write_frame = track->write_frame.load(std::memory_order_acquire);
      const size_t capacity = track->ring_frames();

      size_t available = write_frame - track->read_frame;
      if (available == 0) continue;

      // More than a ring's worth means the audio thread lapped the writer and
      // the oldest samples are already overwritten. The take is compromised
      // either way; skipping to what is still intact keeps it in sync rather
      // than writing a scrambled mix of old and new.
      if (available > capacity) {
        overran_.store(true, std::memory_order_relaxed);
        track->read_frame = write_frame - capacity;
        available = capacity;
      }

      chunk.resize(available * static_cast<size_t>(track->channels));
      for (size_t f = 0; f < available; ++f) {
        const size_t slot = ((track->read_frame + f) % capacity) * track->channels;
        std::copy_n(track->ring.data() + slot, track->channels,
                    chunk.data() + f * track->channels);
      }

      sf_writef_float(track->file, chunk.data(), static_cast<sf_count_t>(available));
      track->read_frame += available;
      wrote_anything = true;
    }

    if (!running && !wrote_anything) break;
    if (!wrote_anything)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  for (auto& track : tracks_)
    if (track->file != nullptr) sf_write_sync(track->file);
}

}  // namespace nirbija
