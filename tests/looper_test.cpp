// Drives the looper the way a player would: record a phrase, close the loop,
// hear it repeat, overdub on top, clear it away.

#include <cmath>
#include <cstdio>
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

// Runs one block. `level` is the input fed in; the return is the output peak.
float run_block(nirbija::LooperInstance& looper, float level) {
  std::vector<float> in_l(kBlock, level), in_r(kBlock, level);
  std::vector<float> out_l(kBlock), out_r(kBlock);
  const float* ins[2] = {in_l.data(), in_r.data()};
  float* outs[2] = {out_l.data(), out_r.data()};

  nirbija::TransportInfo transport;
  transport.playing = true;
  looper.set_transport(transport);
  looper.process(ins, outs, kBlock);

  float peak = 0.0f;
  for (float sample : out_l) peak = std::max(peak, std::fabs(sample));
  return peak;
}

}  // namespace

int main() {
  nirbija::LooperInstance looper;
  looper.set_channel_layout(2);
  looper.activate(kRate, kBlock);
  looper.set_parameter(3, 0.0);  // quantise off: the test is its own clock

  // Live input passes through even with nothing recorded.
  if (run_block(looper, 0.5f) < 0.45f) fail("input does not pass through");

  // Record 8 blocks of tone, then close the loop.
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 8; ++i) run_block(looper, 0.5f);
  looper.set_parameter(0, 0.0);

  // Input now silent: whatever comes out is the loop.
  float loop_peak = 0.0f;
  for (int i = 0; i < 16; ++i) loop_peak = std::max(loop_peak, run_block(looper, 0.0f));
  if (loop_peak < 0.45f) fail("the closed loop does not play back");

  // Overdub doubles the layer.
  looper.set_parameter(0, 1.0);
  for (int i = 0; i < 8; ++i) run_block(looper, 0.5f);
  looper.set_parameter(0, 0.0);
  float dubbed = 0.0f;
  for (int i = 0; i < 16; ++i) dubbed = std::max(dubbed, run_block(looper, 0.0f));
  if (dubbed < loop_peak + 0.3f) fail("overdub did not add a layer");

  // Play off mutes the loop without touching the live input.
  looper.set_parameter(1, 0.0);
  if (run_block(looper, 0.0f) > 1e-6f) fail("play off still sounds the loop");
  if (run_block(looper, 0.5f) < 0.45f) fail("play off swallowed the live input");
  looper.set_parameter(1, 1.0);

  // Clear empties it.
  looper.set_parameter(2, 1.0);
  float cleared = 0.0f;
  for (int i = 0; i < 8; ++i) cleared = std::max(cleared, run_block(looper, 0.0f));
  if (cleared > 1e-6f) fail("clear left audio behind");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
