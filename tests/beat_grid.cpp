// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The grid everything punches on: dsp::boundary_frame, walked block by block
// the way the engine walks it - the beat count summed one block at a time,
// so it drifts a hair either side of where it should be. Every line of the
// grid has to land in exactly one block, on the frame nearest to it. The
// metronome used to lose every beat that fell on a block edge (125 BPM at
// 256 frames is all of them), and the sampler waited a beat longer to punch.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/dsp.h"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    if (failures < 20) std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

// Walks `blocks` blocks and checks each line of `unit` is found once, on the
// frame the exact arithmetic puts it (within one: a line half a frame off
// may round either way).
void walk(double rate, uint32_t frames, double tempo, double unit, int blocks) {
  const double per_frame = tempo / 60.0 / rate;
  const std::string where = std::to_string(tempo) + " BPM, " +
                            std::to_string(frames) + " frames at " +
                            std::to_string(static_cast<int>(rate)) + ", unit " +
                            std::to_string(unit);
  double beats = 0.0;
  int64_t found = 0;
  for (int b = 0; b < blocks; ++b) {
    const uint32_t at = nirbija::dsp::boundary_frame(beats, tempo, rate, unit, 0, frames);
    if (at < frames) {
      const double line = static_cast<double>(found) * unit;
      const double exact = line / per_frame;
      const double got = static_cast<double>(b) * frames + at;
      expect(std::fabs(got - exact) <= 1.0,
             where + ": line " + std::to_string(found) + " at frame " +
                 std::to_string(got) + ", should be " + std::to_string(exact));
      ++found;
      expect(nirbija::dsp::boundary_frame(beats, tempo, rate, unit, at + 1, frames) == frames,
             where + ": two lines in one block");
    }
    beats += static_cast<double>(frames) / rate * tempo / 60.0;
  }
  // Lines before the end of the walk; one on its last half frame rounds to
  // the next block, which was not walked.
  const double end = beats - 0.5 * per_frame;
  const auto expected = static_cast<int64_t>(std::ceil(end / unit));
  expect(found == expected, where + ": " + std::to_string(found) + " lines, expected " +
                                std::to_string(expected));
}

}  // namespace

int main() {
  const double rates[] = {44100.0, 48000.0, 96000.0};
  const uint32_t sizes[] = {64, 128, 256, 512, 1024};
  // 125 and 93.75 put every beat exactly on a block edge at some of these
  // sizes; the rest drift across them.
  const double tempos[] = {60.0, 90.0, 93.75, 100.0, 120.0, 125.0, 140.0, 150.0, 187.5};
  for (double rate : rates)
    for (uint32_t frames : sizes)
      for (double tempo : tempos) {
        walk(rate, frames, tempo, 1.0, 20000);
        walk(rate, frames, tempo, 4.0, 20000);
      }

  // Quantisation off: from is where it happens.
  expect(nirbija::dsp::boundary_frame(1.3, 120.0, 48000.0, 0.0, 17, 256) == 17,
         "no grid should punch at `from`");

  // From the top the downbeat is frame 0 - nobody has to ask for it apart.
  expect(nirbija::dsp::boundary_frame(0.0, 120.0, 48000.0, 1.0, 0, 256) == 0,
         "the first downbeat is not on frame 0");

  // Several lines in one block, walked with `from` the way a sampler that
  // punches mid-block asks for the next one.
  {
    const double rate = 48000.0, tempo = 120.0, unit = 0.01;  // 240 frames
    uint32_t from = 0;
    int count = 0;
    uint32_t last = 0;
    for (;;) {
      const uint32_t at = nirbija::dsp::boundary_frame(3.0, tempo, rate, unit, from, 1024);
      if (at >= 1024) break;
      expect(count == 0 || at - last == 240, "lines inside a block are not 240 apart");
      last = at;
      from = at + 1;
      ++count;
    }
    expect(count == 5, "a 1024-frame block should hold 5 lines of 240, got " +
                           std::to_string(count));
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("beat grid: every line in exactly one block\n");
  return 0;
}
