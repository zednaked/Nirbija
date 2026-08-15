#pragma once

#include <cstdint>

#include "core/plugin.h"

namespace nirbija {

// Where a channel's audio comes from. Keeps the graph free of JACK so it can be
// driven from a test with a synthetic source.
class AudioSource {
 public:
  virtual ~AudioSource() = default;

  // Realtime thread. Fills `dest` (one pointer per channel) with `frames`
  // samples. Must write every sample, including silence.
  virtual void read(float* const* dest, int channels, uint32_t frames) = 0;

  // UI thread, with the graph's process stopped. Sources that need scratch of
  // their own size it here; one that reads straight from a port needs nothing.
  virtual void prepare(uint32_t max_block_frames) { (void)max_block_frames; }
};

// Where a channel's MIDI comes from. Split from AudioSource because a channel
// can have one, the other, or both.
class MidiSource {
 public:
  virtual ~MidiSource() = default;

  // Realtime thread. Writes at most `capacity` events for this block into
  // `out`, in time order, and returns how many.
  virtual size_t read(MidiEvent* out, size_t capacity, uint32_t frames) = 0;
};

}  // namespace nirbija
