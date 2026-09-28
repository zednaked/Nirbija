// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// The insert chain is read under a sequence counter. When the UI thread is
// caught mid-edit for a whole block, the block runs the last chain that was
// read whole rather than no inserts at all - for one block, since a plugin
// removed after that snapshot is reclaimed two generations later. And the
// bypass crossfade state follows the plugin when the UI swaps slots, without
// the UI writing the audio thread's state.

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/channel_strip.h"

namespace {

constexpr uint32_t kBlock = 256;
int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

class ConstantInsert : public nirbija::PluginInstance {
 public:
  explicit ConstantInsert(float level) : level_(level) {}
  void set_channel_layout(int) override {}
  bool activate(double, uint32_t) override { return true; }
  void deactivate() override {}
  void process(const float* const*, float* const* outputs, uint32_t frames) override {
    ++runs;
    for (int ch = 0; ch < 2; ++ch) std::fill_n(outputs[ch], frames, level_);
  }
  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}
  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }
  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }
  int runs = 0;

 private:
  float level_;
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.constant",
                                  .name = "Constant",
                                  .kind = nirbija::PluginKind::Effect,
                                  .audio_inputs = 2,
                                  .audio_outputs = 2};
};

// Runs one block of DC 0.5 through the strip; returns the level it ended on
// and the largest deviation from it within the block.
struct Block {
  float last;
  float wobble;
};

Block run(nirbija::ChannelStrip& strip) {
  std::vector<float> left(kBlock, 0.5f), right(kBlock, 0.5f);
  float* buffers[2] = {left.data(), right.data()};
  strip.process(buffers, kBlock);
  Block b{left[kBlock - 1], 0.0f};
  for (float v : left) b.wobble = std::max(b.wobble, std::fabs(v - b.last));
  return b;
}

void settle(nirbija::ChannelStrip& strip, int blocks = 8) {
  for (int i = 0; i < blocks; ++i) run(strip);
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

}  // namespace

int main() {
  nirbija::ChannelStrip strip("s", 2);
  strip.prepare(48000.0, kBlock);

  // --- the fallback -----------------------------------------------------------
  auto first = std::make_unique<ConstantInsert>(0.1f);
  ConstantInsert* a = first.get();
  size_t slot_a = 0;
  expect(strip.add_insert(std::move(first), &slot_a), "could not add insert a");
  settle(strip);
  expect(near(run(strip).last, 0.1f), "the insert does not replace the dry");

  strip.begin_chain_edit_for_test();
  const int runs_before = a->runs;
  Block torn = run(strip);
  expect(near(torn.last, 0.1f) && a->runs == runs_before + 1,
         "the first torn block did not reuse the last good chain (" +
             std::to_string(torn.last) + ")");
  Block dry = run(strip);
  expect(near(dry.last, 0.5f) && a->runs == runs_before + 1,
         "the second torn block still ran a stale chain (" +
             std::to_string(dry.last) + ")");
  strip.end_chain_edit_for_test();
  expect(near(run(strip).last, 0.1f), "the chain did not come back after the edit");

  // --- the bypass mix follows a swap ---------------------------------------------
  auto second = std::make_unique<ConstantInsert>(0.2f);
  ConstantInsert* b = second.get();
  size_t slot_b = 0;
  expect(strip.add_insert(std::move(second), &slot_b), "could not add insert b");
  expect(slot_a == 0 && slot_b == 1, "unexpected slots");
  settle(strip);
  expect(near(run(strip).last, 0.2f), "the last insert does not win");

  strip.set_insert_bypassed(slot_b, true);
  settle(strip, 16);  // 10 ms crossfade, well within
  expect(near(run(strip).last, 0.1f), "bypassing b did not expose a");

  // Swap: b (bypassed, mix settled at dry) moves to slot 0, a to slot 1. The
  // output is a's constant with no crossfade re-run in either slot: the mix
  // travelled with each plugin.
  strip.swap_inserts(slot_a, slot_b);
  Block swapped = run(strip);
  expect(near(swapped.last, 0.1f) && swapped.wobble < 1e-6f,
         "the swap re-ran a crossfade: level " + std::to_string(swapped.last) +
             " wobble " + std::to_string(swapped.wobble));
  expect(strip.insert_bypassed(0) && !strip.insert_bypassed(1),
         "the bypass flag did not travel with b");

  // Un-bypass b in its new slot 0: it is now before a, so a still wins, and
  // nothing steps.
  strip.set_insert_bypassed(0, false);
  Block unbypass = run(strip);
  expect(unbypass.wobble < 1e-6f && near(unbypass.last, 0.1f),
         "un-bypassing the earlier slot disturbed the later one");
  expect(b->runs > 0, "b never ran");

  // A new plugin dropped into a hole starts wet, whatever the previous
  // occupant's mix was.
  strip.set_insert_bypassed(1, true);  // a, bypassed: output is b's 0.2
  settle(strip, 16);
  expect(near(run(strip).last, 0.2f), "bypassing a did not expose b");
  strip.remove_insert(1);
  strip.reclaim(false);
  size_t slot_c = 0;
  expect(strip.add_insert(std::make_unique<ConstantInsert>(0.3f), &slot_c),
         "could not add insert c");
  expect(slot_c == 1, "c did not fill the hole");
  Block fresh = run(strip);
  expect(near(fresh.last, 0.3f) && fresh.wobble < 1e-6f,
         "a new plugin in a hole inherited the old bypass mix: level " +
             std::to_string(fresh.last) + " wobble " + std::to_string(fresh.wobble));

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
