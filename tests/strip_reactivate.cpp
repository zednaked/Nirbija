// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// A JACK period that grows past what the inserts were activated with has to
// activate them again: CLAP and VST3 plugins size their buffers from that
// number and would overflow. A period that shrinks changes nothing for them.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/channel_strip.h"

namespace {

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

// Records every activation's block size and every deactivation, and scales
// the audio by a half so the block that runs can be told apart from dry.
class RecordingPlugin : public nirbija::PluginInstance {
 public:
  void set_channel_layout(int channels) override { channels_ = channels; }
  bool activate(double, uint32_t max_block_frames) override {
    activations.push_back(max_block_frames);
    active = true;
    return true;
  }
  void deactivate() override {
    ++deactivations;
    active = false;
  }
  void process(const float* const*, float* const* outputs, uint32_t frames) override {
    processed_while_inactive = processed_while_inactive || !active;
    largest_block = std::max(largest_block, frames);
    for (int ch = 0; ch < channels_; ++ch)
      for (uint32_t f = 0; f < frames; ++f) outputs[ch][f] *= 0.5f;
  }
  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}
  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }
  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

  std::vector<uint32_t> activations;
  int deactivations = 0;
  bool active = false;
  bool processed_while_inactive = false;
  uint32_t largest_block = 0;

 private:
  int channels_ = 2;
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.recording",
                                  .name = "Recording",
                                  .kind = nirbija::PluginKind::Effect,
                                  .audio_inputs = 2,
                                  .audio_outputs = 2};
};

}  // namespace

int main() {
  nirbija::ChannelStrip strip("s", 2);
  strip.prepare(48000.0, 256);

  auto plugin = std::make_unique<RecordingPlugin>();
  RecordingPlugin* raw = plugin.get();
  expect(strip.add_insert(std::move(plugin)), "could not add the insert");
  expect(raw->activations.size() == 1 && raw->activations[0] == 256,
         "the insert was not activated with the prepared block");
  expect(strip.activated_block_frames() == 256, "activated size not recorded");

  // Smaller: nothing happens to the plugin.
  strip.prepare(48000.0, 128);
  expect(raw->activations.size() == 1, "a smaller period re-activated the insert");
  expect(raw->deactivations == 0, "a smaller period deactivated the insert");

  // Same size again: still nothing.
  strip.prepare(48000.0, 256);
  expect(raw->activations.size() == 1, "an unchanged period re-activated the insert");

  // Larger: deactivated once, activated once, with the new maximum.
  strip.prepare(48000.0, 1024);
  expect(raw->deactivations == 1,
         "a larger period did not deactivate first (" +
             std::to_string(raw->deactivations) + ")");
  expect(raw->activations.size() == 2 && raw->activations.back() == 1024,
         "a larger period did not re-activate with the new maximum");
  expect(strip.activated_block_frames() == 1024, "activated size not updated");
  expect(raw->active, "the insert was left deactivated");

  // And the strip's own scratch grew with it: a full block renders through
  // the insert with no overflow and the expected level.
  std::vector<float> left(1024, 0.8f), right(1024, 0.8f);
  float* buffers[2] = {left.data(), right.data()};
  strip.process(buffers, 1024);
  expect(raw->largest_block == 1024, "the insert did not see the full block");
  expect(!raw->processed_while_inactive, "the insert ran while deactivated");
  bool level_ok = true;
  for (uint32_t f = 0; f < 1024; ++f)
    if (std::abs(left[f] - 0.4f) > 1e-5f) level_ok = false;
  expect(level_ok, "the block did not come out at half level");
  expect(strip.output_cache(0) != nullptr && strip.output_cache(0)[1023] == left[1023],
         "the output cache did not hold the whole block");

  // A second insert added after the growth is activated with the grown size.
  auto later = std::make_unique<RecordingPlugin>();
  RecordingPlugin* later_raw = later.get();
  expect(strip.add_insert(std::move(later)), "could not add the second insert");
  expect(later_raw->activations.size() == 1 && later_raw->activations[0] == 1024,
         "a later insert was not activated with the grown maximum");

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
