// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// How long a block takes. The graph's promise is that render() costs the same
// every block - no allocation, no lock, no work that depends on what the UI
// did - so what this measures is the worst block, not the average: one slow
// block in a thousand is one dropout in five seconds.
//
// Sixteen stereo strips each with a cheap insert, sends into two buses, the
// limiter on, gain and pan moving every block the way a live set moves them.
// The report is printed every run; the check only fails when
// NIRBIJA_BENCH_STRICT is set, since under ASan in Debug the numbers mean
// nothing and the pre-push hook would fail on a slow laptop. Run it from the
// Release tree to see the real figure:
//
//     ./build/tests/nirbija_render_bench
//     NIRBIJA_BENCH_STRICT=1 ./build/tests/nirbija_render_bench

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/audio_graph.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr int kStrips = 16;
constexpr int kBlocks = 4000;  // ~21 s of audio

class NoiseSource : public nirbija::AudioSource {
 public:
  void read(float* const* dest, int channels, uint32_t frames) override {
    for (int ch = 0; ch < channels; ++ch) {
      for (uint32_t i = 0; i < frames; ++i) {
        state_ = state_ * 1664525u + 1013904223u;
        dest[ch][i] = (static_cast<float>(state_ >> 8) / 8388608.0f - 1.0f) * 0.1f;
      }
    }
  }

 private:
  uint32_t state_ = 12345;
};

// A one-pole low-pass per channel: the cheapest thing that is still a real
// per-sample plugin, so the insert path is measured and not skipped.
class FilterInsert : public nirbija::PluginInstance {
 public:
  void set_channel_layout(int) override {}
  bool activate(double, uint32_t) override { return true; }
  void deactivate() override {}
  void process(const float* const* inputs, float* const* outputs,
               uint32_t frames) override {
    for (int ch = 0; ch < 2; ++ch) {
      float z = z_[ch];
      for (uint32_t i = 0; i < frames; ++i) {
        z += 0.1f * (inputs[ch][i] - z);
        outputs[ch][i] = z;
      }
      z_[ch] = z;
    }
  }
  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}
  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }
  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

 private:
  float z_[2] = {0.0f, 0.0f};
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.filter",
                                  .name = "Filter",
                                  .kind = nirbija::PluginKind::Effect,
                                  .audio_inputs = 2,
                                  .audio_outputs = 2};
};

}  // namespace

int main() {
  nirbija::AudioGraph graph;
  graph.prepare(kSampleRate, kBlock);
  graph.set_master_limiter(true);

  const size_t bus_a = graph.add_bus("a");
  const size_t bus_b = graph.add_bus("b");
  for (int i = 0; i < kStrips; ++i) {
    const size_t index =
        graph.add_channel("s" + std::to_string(i), 2, std::make_unique<NoiseSource>());
    if (index >= nirbija::kMaxChannels) {
      std::fprintf(stderr, "FAIL graph full at %d\n", i);
      return 1;
    }
    nirbija::ChannelStrip& strip = graph.channel(index);
    strip.add_insert(std::make_unique<FilterInsert>());
    strip.set_send(0, static_cast<int>(bus_a), 0.3f);
    strip.set_send(1, static_cast<int>(bus_b), 0.2f);
  }

  std::vector<float> left(kBlock), right(kBlock);
  float* master[2] = {left.data(), right.data()};

  // Warm up: first blocks pay for cold caches and page faults that a running
  // engine paid at start.
  for (int i = 0; i < 200; ++i) graph.render(master, kBlock);

  using clock = std::chrono::steady_clock;
  std::vector<double> micros;
  micros.reserve(kBlocks);
  for (int b = 0; b < kBlocks; ++b) {
    // The set moves: a fader and a pan every block, on a different strip.
    nirbija::ChannelStrip& strip = graph.channel(static_cast<size_t>(b % kStrips));
    strip.set_gain(0.5f + 0.4f * std::sin(b * 0.01f));
    strip.set_pan(0.5f * std::sin(b * 0.013f));
    if (b % 500 == 0) graph.set_master_gain(b % 1000 == 0 ? 1.0f : 0.7f);

    const auto t0 = clock::now();
    graph.render(master, kBlock);
    const auto t1 = clock::now();
    micros.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
  }

  std::sort(micros.begin(), micros.end());
  const double median = micros[micros.size() / 2];
  const double p99 = micros[static_cast<size_t>(micros.size() * 0.99)];
  const double worst = micros.back();
  const double budget = 1e6 * kBlock / kSampleRate;  // one block's time

  std::printf("render: %d strips, %u frames @ %.0f Hz, %d blocks\n", kStrips, kBlock,
              kSampleRate, kBlocks);
  std::printf("  median %.1f us   p99 %.1f us   worst %.1f us   budget %.0f us\n",
              median, p99, worst, budget);
  std::printf("  worst block used %.1f%% of its time\n", 100.0 * worst / budget);

  if (std::getenv("NIRBIJA_BENCH_STRICT") == nullptr) return 0;

  // Half the block on the worst block, in Release, on any machine that can run
  // the app: past that the same graph at 128 frames is already dropping out.
  int failures = 0;
  if (worst > budget * 0.5) {
    std::fprintf(stderr, "FAIL worst block %.1f us is over half the budget of %.0f us\n",
                 worst, budget);
    ++failures;
  }
  // A worst block far above the median is the fingerprint of something that
  // is not constant-time: an allocation, a lock, a page fault.
  if (worst > median * 20.0 && worst > 50.0) {
    std::fprintf(stderr, "FAIL worst block %.1f us is %.0fx the median %.1f us\n", worst,
                 worst / median, median);
    ++failures;
  }
  return failures == 0 ? 0 : 1;
}
