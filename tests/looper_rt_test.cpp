// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The looper as the audio thread sees it: undo and redo carried out inside
// process() with nothing copied on the UI thread and nothing allocated on
// the audio thread; a Rec press landing on the bar to the sample; and a
// loop whose wrap does not click.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "core/looper.h"

namespace {

constexpr uint32_t kBlock = 256;
constexpr double kRate = 48000.0;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

// Every allocation while this is set is a failure: process() must not
// touch the heap, whatever request is pending.
bool g_in_process = false;
long g_allocations_in_process = 0;

}  // namespace

void* operator new(std::size_t size) {
  if (g_in_process) ++g_allocations_in_process;
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  if (g_in_process) ++g_allocations_in_process;
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

// A block driven from a caller-owned input, returning the output left
// channel in `out` (sized kBlock by the caller). All buffers are the
// caller's so the allocation guard only sees the looper.
void run(nirbija::LooperInstance& looper, const float* in_l, const float* in_r,
         float* out_l, float* out_r, const nirbija::TransportInfo& transport) {
  const float* ins[2] = {in_l, in_r};
  float* outs[2] = {out_l, out_r};
  looper.set_transport(transport);
  g_in_process = true;
  looper.process(ins, outs, kBlock);
  g_in_process = false;
}

nirbija::TransportInfo free_transport() {
  nirbija::TransportInfo t;
  t.playing = true;
  t.tempo_bpm = 120.0;
  t.numerator = 4;
  return t;
}

// Peak of the wet signal over `blocks` blocks of silent input, skipping the
// first block so the ramps have settled.
float settled_peak(nirbija::LooperInstance& looper, int blocks) {
  std::vector<float> zero(kBlock, 0.0f), out_l(kBlock), out_r(kBlock);
  float peak = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    run(looper, zero.data(), zero.data(), out_l.data(), out_r.data(),
        free_transport());
    if (b == 0) continue;
    for (float s : out_l) peak = std::max(peak, std::fabs(s));
  }
  return peak;
}

void undo_redo_through_process() {
  nirbija::LooperInstance looper;
  looper.set_channel_layout(2);
  looper.activate(kRate, kBlock);
  looper.set_parameter(3, 0.0);  // free length: the test is its own clock

  std::vector<float> level(kBlock), zero(kBlock, 0.0f), out_l(kBlock),
      out_r(kBlock);
  auto record = [&](float value, int blocks) {
    std::fill(level.begin(), level.end(), value);
    looper.set_parameter(0, 1.0);
    for (int b = 0; b < blocks; ++b)
      run(looper, level.data(), level.data(), out_l.data(), out_r.data(),
          free_transport());
    looper.set_parameter(0, 0.0);
    // The punch-out lands at the next block; one more of silence closes it
    // and lets the record ramp finish.
    run(looper, zero.data(), zero.data(), out_l.data(), out_r.data(),
        free_transport());
  };

  record(0.5f, 8);
  if (!looper.loop_closed()) fail("rt: first take did not close");
  const float base = settled_peak(looper, 4);
  if (std::fabs(base - 0.5f) > 0.02f)
    fail("rt: base take plays at " + std::to_string(base) + ", wanted 0.5");

  record(0.25f, 8);
  const float dubbed = settled_peak(looper, 4);
  if (std::fabs(dubbed - 0.75f) > 0.03f)
    fail("rt: overdub plays at " + std::to_string(dubbed) + ", wanted 0.75");

  if (!looper.can_undo()) fail("rt: overdub left nothing to undo");
  looper.request_undo();
  const float undone = settled_peak(looper, 4);
  if (std::fabs(undone - 0.5f) > 0.03f)
    fail("rt: undo left the loop at " + std::to_string(undone) + ", wanted 0.5");

  if (!looper.can_redo()) fail("rt: undo did not arm redo");
  looper.request_redo();
  const float redone = settled_peak(looper, 4);
  if (std::fabs(redone - 0.75f) > 0.03f)
    fail("rt: redo left the loop at " + std::to_string(redone) + ", wanted 0.75");

  // Clear, then undo the clear: the whole stack comes back by swapping
  // which tape is live, no copy.
  looper.request_clear();
  const float cleared = settled_peak(looper, 2);
  if (cleared > 1e-6f) fail("rt: clear left audio playing");
  if (looper.loop_closed()) fail("rt: clear left the loop closed");
  if (!looper.can_undo()) fail("rt: clear left nothing to undo");
  looper.request_undo();
  // The undo waits for the shadow sweep to finish; a couple of blocks.
  const float restored = settled_peak(looper, 6);
  if (!looper.loop_closed()) fail("rt: undo did not bring the cleared loop back");
  if (std::fabs(restored - 0.75f) > 0.03f)
    fail("rt: undo of clear plays at " + std::to_string(restored) +
         ", wanted 0.75");

  // Multiply doubles the loop through the same request path.
  const double beats = looper.loop_beats();
  looper.request_multiply();
  settled_peak(looper, 2);
  if (looper.loop_beats() < beats * 1.9) fail("rt: multiply did not double");
  looper.request_undo();
  settled_peak(looper, 6);
  if (std::fabs(looper.loop_beats() - beats) > 1e-6)
    fail("rt: undo of multiply did not restore the length");

  if (g_allocations_in_process != 0)
    fail("rt: process() allocated " + std::to_string(g_allocations_in_process) +
         " time(s)");
}

// 120 bpm, 48 kHz, 256-frame blocks, Length one bar: a bar is 96000
// frames. Rec is pressed from a transport position that puts the bar line
// 91 frames into a block, with an impulse every 1000 frames of absolute
// time. If the head starts on the bar to the sample, and the loop is exactly
// a bar, the looped impulses keep landing on multiples of 1000 for ever.
void punch_on_the_sample() {
  nirbija::LooperInstance looper;
  looper.set_channel_layout(2);
  looper.activate(kRate, kBlock);
  looper.set_parameter(3, 2.0);  // one bar

  constexpr uint64_t kBar = 96000;
  constexpr uint64_t kImpulseEvery = 1000;
  constexpr uint64_t kStart = 48037;  // (96000 - 48037) % 256 == 91
  const double beats_per_frame = 120.0 / 60.0 / kRate;

  uint64_t frame = kStart;
  std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock), out_r(kBlock);
  auto step = [&]() {
    for (uint32_t i = 0; i < kBlock; ++i)
      in_l[i] = in_r[i] = ((frame + i) % kImpulseEvery == 0) ? 1.0f : 0.0f;
    nirbija::TransportInfo t;
    t.playing = true;
    t.rolling = true;
    t.tempo_bpm = 120.0;
    t.numerator = 4;
    t.beats = static_cast<double>(frame) * beats_per_frame;
    run(looper, in_l.data(), in_r.data(), out_l.data(), out_r.data(), t);
    frame += kBlock;
  };

  looper.set_parameter(0, 1.0);
  while (frame < kBar + kBar / 2) step();  // across the first bar line
  if (!looper.writing()) fail("punch: rec did not start writing on the bar");
  looper.set_parameter(0, 0.0);
  while (frame < 2 * kBar + kBar / 2) step();  // across the second: closes
  if (!looper.loop_closed()) fail("punch: the loop did not close on the bar");
  if (std::fabs(looper.loop_beats() - 4.0) > 1e-6)
    fail("punch: the loop is " + std::to_string(looper.loop_beats()) +
         " beats, wanted exactly 4");

  // Silence in; the loop's impulses out. Each one must sit on the grid.
  std::fill(in_l.begin(), in_l.end(), 0.0f);
  std::fill(in_r.begin(), in_r.end(), 0.0f);
  int seen = 0;
  int misplaced = 0;
  for (int b = 0; b < 800; ++b) {
    nirbija::TransportInfo t;
    t.playing = true;
    t.rolling = true;
    t.tempo_bpm = 120.0;
    t.numerator = 4;
    t.beats = static_cast<double>(frame) * beats_per_frame;
    run(looper, in_l.data(), in_r.data(), out_l.data(), out_r.data(), t);
    for (uint32_t i = 0; i < kBlock; ++i) {
      if (std::fabs(out_l[i]) > 0.5f) {
        ++seen;
        if ((frame + i) % kImpulseEvery != 0) ++misplaced;
      }
    }
    frame += kBlock;
  }
  if (seen < 100) fail("punch: too few impulses came back from the loop");
  if (misplaced > 0)
    fail("punch: " + std::to_string(misplaced) + " of " + std::to_string(seen) +
         " looped impulses were off the grid");
}

// A 100 Hz tone recorded into a free-length loop that is not a whole number
// of periods: the wrap used to step from the last sample to the first. With
// the record edges ramped onto the tape, the largest sample-to-sample jump
// in playback stays within the tone's own slope.
void wrap_does_not_click() {
  nirbija::LooperInstance looper;
  looper.set_channel_layout(2);
  looper.activate(kRate, kBlock);
  looper.set_parameter(3, 0.0);

  constexpr double kHz = 100.0;
  constexpr float kAmp = 0.5f;
  double phase = 0.7;  // start mid-cycle
  std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock), out_r(kBlock);
  auto tone_block = [&]() {
    for (uint32_t i = 0; i < kBlock; ++i) {
      in_l[i] = in_r[i] = kAmp * static_cast<float>(std::sin(phase));
      phase += 2.0 * 3.14159265358979323846 * kHz / kRate;
    }
    run(looper, in_l.data(), in_r.data(), out_l.data(), out_r.data(),
        free_transport());
  };

  tone_block();
  tone_block();
  looper.set_parameter(0, 1.0);
  for (int b = 0; b < 8; ++b) tone_block();  // 2048 frames: 4.27 periods
  looper.set_parameter(0, 0.0);
  tone_block();  // closes the loop and ramps the tail onto the head
  if (!looper.loop_closed()) fail("wrap: the take did not close");

  std::fill(in_l.begin(), in_l.end(), 0.0f);
  std::fill(in_r.begin(), in_r.end(), 0.0f);
  const float slope = static_cast<float>(
      2.0 * 3.14159265358979323846 * kHz / kRate * kAmp);
  float worst = 0.0f;
  float previous = 0.0f;
  bool have_previous = false;
  for (int b = 0; b < 40; ++b) {
    run(looper, in_l.data(), in_r.data(), out_l.data(), out_r.data(),
        free_transport());
    if (b < 2) {
      // The record ramp is still landing on the tape in the first block.
      previous = out_l[kBlock - 1];
      have_previous = true;
      continue;
    }
    for (uint32_t i = 0; i < kBlock; ++i) {
      if (have_previous) worst = std::max(worst, std::fabs(out_l[i] - previous));
      previous = out_l[i];
      have_previous = true;
    }
  }
  // Twice the tone's slope: the crossfade adds at most the difference of
  // two tone samples spread over ~240 frames.
  if (worst > slope * 2.0f)
    fail("wrap: largest jump " + std::to_string(worst) + " vs tone slope " +
         std::to_string(slope));
  if (worst == 0.0f) fail("wrap: the loop came back silent");
}

}  // namespace

int main() {
  undo_redo_through_process();
  punch_on_the_sample();
  wrap_does_not_click();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
