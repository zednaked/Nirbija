// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// How long a block takes. The graph's promise is that render() costs the same
// every block - no allocation, no lock, no work that depends on what the UI
// did - so what this measures is the worst block, not the average: one slow
// block in a thousand is one dropout in five seconds.
//
// Sixteen stereo strips each with a cheap insert, sends into two buses, the
// limiter on, gain and pan moving every block the way a live set moves them.
// Then the same again with a scene playing: 64 targets (a gate, a fader and
// two plugin knobs on every strip) and a new scene on every bar line with a
// fade of a bar, so the conductor is always in the middle of walking all of
// them - the most a scene can cost a block.
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
#include "core/scene_conductor.h"

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
  // Two knobs for a scene to walk: plain stores, like the built-in plugins.
  double parameter_value(uint32_t id) const override { return id < 2 ? knob_[id] : 0.0; }
  void set_parameter(uint32_t id, double value) override {
    if (id < 2) knob_[id] = value;
  }
  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }
  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

 private:
  float z_[2] = {0.0f, 0.0f};
  double knob_[2] = {0.0, 0.0};
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

  // Two scenes that each take every strip somewhere else, a bar long, with a
  // fade of a bar: from the first line on, every block is mid-fade.
  nirbija::SceneConductor scenes;
  {
    auto table = std::make_shared<nirbija::SceneTable>();
    for (uint32_t id = 1; id <= 2; ++id) {
      nirbija::SceneTable::Scene scene;
      scene.id = id;
      scene.bars = 1;
      scene.fade_bars = 1;
      scene.first = static_cast<uint32_t>(table->targets.size());
      for (int i = 0; i < kStrips; ++i) {
        nirbija::SceneTarget target;
        target.strip = static_cast<uint16_t>(i);
        target.what = nirbija::SceneTarget::What::Gate;
        target.value = 1.0f;
        table->targets.push_back(target);
        target.what = nirbija::SceneTarget::What::Level;
        target.value = id == 1 ? 0.2f : 0.9f;
        table->targets.push_back(target);
        target.what = nirbija::SceneTarget::What::Param;
        target.insert_tag = graph.channel(static_cast<size_t>(i)).insert_tag(0);
        for (uint32_t knob = 0; knob < 2; ++knob) {
          target.param = knob;
          target.value = id == 1 ? 0.1f : 0.8f;
          table->targets.push_back(target);
        }
      }
      scene.count = static_cast<uint32_t>(table->targets.size()) - scene.first;
      table->scenes.push_back(scene);
    }
    scenes.publish(table);
  }
  constexpr uint32_t kSceneTargets = 2 * 2 * kStrips;

  using clock = std::chrono::steady_clock;
  struct Result {
    double median, p99, worst;
  };
  // One pass: the set moving, and with `with_scene` the conductor running
  // ahead of every render, as the engine runs it.
  auto measure = [&](bool with_scene) {
    nirbija::TransportInfo transport;
    transport.playing = transport.rolling = with_scene;
    transport.tempo_bpm = 120.0;
    transport.numerator = 4;
    transport.denominator = 4;
    transport.changed = true;
    auto one = [&](int b) {
      // The set moves: a fader and a pan every block, on a different strip.
      nirbija::ChannelStrip& strip = graph.channel(static_cast<size_t>(b % kStrips));
      if (!with_scene) strip.set_gain(0.5f + 0.4f * std::sin(b * 0.01f));
      strip.set_pan(0.5f * std::sin(b * 0.013f));
      if (b % 500 == 0) graph.set_master_gain(b % 1000 == 0 ? 1.0f : 0.7f);
      const auto t0 = clock::now();
      if (with_scene) {
        graph.set_transport(transport);
        scenes.run(graph, transport, kBlock, kSampleRate);
      }
      graph.render(master, kBlock);
      const auto t1 = clock::now();
      transport.changed = false;
      transport.beats += kBlock / kSampleRate * transport.tempo_bpm / 60.0;
      return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };
    // Warm up: first blocks pay for cold caches and page faults that a
    // running engine paid at start - and, with a scene, reach the first line.
    for (int i = 0; i < 400; ++i) one(i);
    std::vector<double> micros;
    micros.reserve(kBlocks);
    for (int b = 0; b < kBlocks; ++b) micros.push_back(one(b));
    std::sort(micros.begin(), micros.end());
    return Result{micros[micros.size() / 2],
                  micros[static_cast<size_t>(micros.size() * 0.99)], micros.back()};
  };

  const Result plain = measure(false);
  const Result scened = measure(true);
  const double budget = 1e6 * kBlock / kSampleRate;  // one block's time

  std::printf("render: %d strips, %u frames @ %.0f Hz, %d blocks\n", kStrips, kBlock,
              kSampleRate, kBlocks);
  auto report = [&](const char* what, const Result& r) {
    std::printf("  %-28s median %.1f us   p99 %.1f us   worst %.1f us (%.1f%% of %.0f us)\n",
                what, r.median, r.p99, r.worst, 100.0 * r.worst / budget, budget);
  };
  report("no scene", plain);
  report(("a scene fading " + std::to_string(kSceneTargets) + " targets").c_str(), scened);
  std::printf("  the scene costs %.1f us a block at the median\n",
              scened.median - plain.median);
  if (scenes.current() == nirbija::SceneConductor::kNone) {
    std::fprintf(stderr, "FAIL no scene ever started: the bench measured nothing\n");
    return 1;
  }

  if (std::getenv("NIRBIJA_BENCH_STRICT") == nullptr) return 0;

  // Half the block on the worst block, in Release, on any machine that can run
  // the app: past that the same graph at 128 frames is already dropping out.
  int failures = 0;
  for (const Result& r : {plain, scened}) {
    if (r.worst > budget * 0.5) {
      std::fprintf(stderr, "FAIL worst block %.1f us is over half the budget of %.0f us\n",
                   r.worst, budget);
      ++failures;
    }
    // A worst block far above the median is the fingerprint of something
    // that is not constant-time: an allocation, a lock, a page fault.
    if (r.worst > r.median * 20.0 && r.worst > 50.0) {
      std::fprintf(stderr, "FAIL worst block %.1f us is %.0fx the median %.1f us\n",
                   r.worst, r.worst / r.median, r.median);
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
