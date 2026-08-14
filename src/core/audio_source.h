#pragma once

#include <cstdint>

namespace nirbija {

// Where a channel's audio comes from. Keeps the graph free of JACK so it can be
// driven from a test with a synthetic source.
class AudioSource {
 public:
  virtual ~AudioSource() = default;

  // Realtime thread. Fills `dest` (one pointer per channel) with `frames`
  // samples. Must write every sample, including silence.
  virtual void read(float* const* dest, int channels, uint32_t frames) = 0;
};

}  // namespace nirbija
