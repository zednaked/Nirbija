// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// A change of compensation delay is a slope, not a jump. A slow sine goes
// through a strip; the delay changes to half its period (the worst case, the
// old and new read positions in antiphase), and the output is watched sample
// by sample for a jump larger than the sine's own slope plus the crossfade's
// allowance. Then the delayed output is checked to be the input, that many
// samples late, and a prepare() of the same size is checked not to throw the
// delay line away.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/channel_strip.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr double kHz = 100.0;  // period 480 samples
constexpr float kAmplitude = 0.5f;

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

struct Feed {
  nirbija::ChannelStrip* strip = nullptr;
  uint64_t n = 0;              // samples generated so far
  std::vector<float> history;  // every input sample, for the delay check
  std::vector<float> output;   // every output sample
  float previous = 0.0f;
  bool have_previous = false;
  float max_jump = 0.0f;

  float sample(uint64_t index) const {
    return kAmplitude * static_cast<float>(
        std::sin(2.0 * 3.14159265358979 * kHz * static_cast<double>(index) / kSampleRate));
  }

  void run(int blocks) {
    std::vector<float> buffer(kBlock);
    float* buffers[1] = {buffer.data()};
    for (int b = 0; b < blocks; ++b) {
      for (uint32_t f = 0; f < kBlock; ++f) {
        buffer[f] = sample(n + f);
        history.push_back(buffer[f]);
      }
      n += kBlock;
      strip->process(buffers, kBlock);
      for (uint32_t f = 0; f < kBlock; ++f) {
        if (have_previous)
          max_jump = std::max(max_jump, std::fabs(buffer[f] - previous));
        previous = buffer[f];
        have_previous = true;
        output.push_back(buffer[f]);
      }
    }
  }

  // Over the last `samples`, the output is the input `delay` samples late.
  float delay_error(uint32_t delay, size_t samples) const {
    float worst = 0.0f;
    for (size_t i = output.size() - samples; i < output.size(); ++i)
      worst = std::max(worst, std::fabs(output[i] - history[i - delay]));
    return worst;
  }
};

// The sine's own slope per sample plus what a linear cross of two antiphase
// copies adds over the shortest fade the strip uses (one block here).
const float kSlope = kAmplitude * 2.0f * 3.14159265f * static_cast<float>(kHz / kSampleRate);
const float kAllowed = (kSlope + 2.0f * kAmplitude / static_cast<float>(kBlock)) * 1.2f;

}  // namespace

int main() {
  nirbija::ChannelStrip strip("m", 1);
  strip.prepare(kSampleRate, kBlock);
  Feed feed;
  feed.strip = &strip;

  feed.run(20);
  expect(feed.max_jump <= kSlope * 1.05f, "the plain sine itself stepped");
  expect(feed.delay_error(0, kBlock * 4) < 1e-5f, "no delay did not pass the sine through");

  // Half a period: the two read positions are in antiphase, the largest
  // possible jump without the crossfade would be the full peak-to-peak.
  feed.max_jump = 0.0f;
  strip.set_pdc_delay(240);
  feed.run(40);
  expect(feed.max_jump <= kAllowed,
         "the delay change stepped: jump " + std::to_string(feed.max_jump) +
             " allowed " + std::to_string(kAllowed));
  expect(feed.max_jump > kSlope * 0.5f, "the delay never actually moved");
  expect(feed.delay_error(240, kBlock * 8) < 1e-4f,
         "after the fade the output is not the input 240 samples late");

  // Two changes in a row, the second landing mid-fade: it waits, no step.
  feed.max_jump = 0.0f;
  strip.set_pdc_delay(480);
  feed.run(1);
  strip.set_pdc_delay(120);
  feed.run(40);
  expect(feed.max_jump <= kAllowed,
         "a change during a fade stepped: jump " + std::to_string(feed.max_jump));
  expect(feed.delay_error(120, kBlock * 8) < 1e-4f,
         "the second change did not land at 120 samples");

  // Back to none, the same way.
  feed.max_jump = 0.0f;
  strip.set_pdc_delay(0);
  feed.run(40);
  expect(feed.max_jump <= kAllowed,
         "removing the delay stepped: jump " + std::to_string(feed.max_jump));
  expect(feed.delay_error(0, kBlock * 8) < 1e-5f, "the delay did not come back to none");

  // prepare() of the same size keeps the delay line: the delayed output stays
  // continuous across it, which it would not if the line were zeroed.
  strip.set_pdc_delay(240);
  feed.run(40);
  feed.max_jump = 0.0f;
  strip.prepare(kSampleRate, kBlock);
  feed.run(4);
  expect(feed.max_jump <= kSlope * 1.05f,
         "prepare() of the same size disturbed the delay line: jump " +
             std::to_string(feed.max_jump));
  expect(feed.delay_error(240, kBlock * 4) < 1e-4f, "the delay was lost across prepare()");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
