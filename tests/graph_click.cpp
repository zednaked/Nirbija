// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// What the graph does around the master: the metronome click is rendered
// inside it, so it is parked with the music and goes through the limiter; a
// destination the strip cannot index is the master, never a read past the
// bus array; and a scratch buffer written one block is empty the next, so a
// reroute does not leave a copy of the signal behind.

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/audio_graph.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;
int failures = 0;

void expect(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
    ++failures;
  }
}

class DcSource : public nirbija::AudioSource {
 public:
  explicit DcSource(float level) : level_(level) {}
  void read(float* const* dest, int channels, uint32_t frames) override {
    for (int ch = 0; ch < channels; ++ch) std::fill_n(dest[ch], frames, level_);
  }

 private:
  float level_;
};

struct Block {
  float peak = 0.0f;
  float last = 0.0f;
  float max_jump = 0.0f;
};

Block render(nirbija::AudioGraph& graph, int blocks = 1) {
  std::vector<float> left(kBlock), right(kBlock);
  float* master[2] = {left.data(), right.data()};
  Block b;
  bool have = false;
  float previous = 0.0f;
  for (int i = 0; i < blocks; ++i) {
    graph.render(master, kBlock);
    for (uint32_t f = 0; f < kBlock; ++f) {
      b.peak = std::max(b.peak, std::fabs(left[f]));
      if (have) b.max_jump = std::max(b.max_jump, std::fabs(left[f] - previous));
      previous = left[f];
      have = true;
    }
  }
  b.last = previous;
  return b;
}

}  // namespace

int main() {
  nirbija::AudioGraph graph;
  graph.prepare(kSampleRate, kBlock);

  // --- the click ----------------------------------------------------------------
  {
    expect(render(graph).peak == 0.0f, "an empty graph is not silent");
    graph.schedule_click(0, true);
    Block b = render(graph);
    expect(b.peak > 0.3f, "a scheduled click made no sound");
    // A 1.5 kHz sine at half scale moves at most ~0.1 per sample; the click
    // is a slope, not a burst of steps.
    expect(b.max_jump < 0.12f, "the click stepped: " + std::to_string(b.max_jump));
    // 30 ms long: still sounding two blocks later, gone after eight.
    expect(render(graph).peak > 0.0f, "the click did not carry into the next block");
    expect(render(graph, 8).peak > 0.0f && render(graph).peak == 0.0f,
           "the click did not end");

    // Mid-block: silent before the frame, sound from it.
    graph.schedule_click(100, false);
    std::vector<float> left(kBlock), right(kBlock);
    float* master[2] = {left.data(), right.data()};
    graph.render(master, kBlock);
    bool quiet_before = true;
    for (uint32_t f = 0; f < 100; ++f) quiet_before = quiet_before && left[f] == 0.0f;
    bool sound_after = false;
    for (uint32_t f = 100; f < kBlock; ++f) sound_after = sound_after || left[f] != 0.0f;
    expect(quiet_before && sound_after, "a mid-block click did not start at its frame");
    render(graph, 8);

    // A click into a parked, quiet graph is not heard, and not owed later.
    graph.park();
    render(graph, 4);
    expect(graph.parked(), "park did not take");
    graph.schedule_click(0, true);
    expect(render(graph).peak == 0.0f, "a click sounded through a park");
    graph.unpark();
    Block back = render(graph, 4);
    expect(back.peak == 0.0f, "the parked click was played back after unpark");
    graph.schedule_click(0, true);
    expect(render(graph).peak > 0.3f, "no click after unpark");
    render(graph, 8);

    // Through the limiter: a click on top of a hot master is held under the
    // ceiling, which it would not be if it were added after the limiter.
    const size_t hot = graph.add_channel("hot", 2, std::make_unique<DcSource>(0.9f));
    expect(hot < nirbija::kMaxChannels, "could not add the hot channel");
    graph.set_master_limiter(true);
    render(graph, 64);
    graph.schedule_click(0, true);
    Block limited = render(graph);
    expect(limited.peak <= 0.9661f,
           "the click passed the limiter's ceiling: " + std::to_string(limited.peak));
    graph.set_master_limiter(false);
    render(graph, 64);
    graph.schedule_click(0, true);
    Block open = render(graph);
    expect(open.peak > 1.0f, "with the limiter off the click plus the hot master stayed under 1");
    render(graph, 8);
    graph.remove_channel(hot);
    graph.reclaim(false);
    render(graph, 64);
  }

  // --- destinations the graph can index -----------------------------------------
  {
    nirbija::ChannelStrip strip("s", 2);
    strip.set_destination(3);
    expect(strip.destination() == 3, "a real bus index was refused");
    strip.set_destination(static_cast<int>(nirbija::kMaxBuses));
    expect(strip.destination() == nirbija::kMasterDestination,
           "a bus index past kMaxBuses was accepted");
    strip.set_destination(40);
    expect(strip.destination() == nirbija::kMasterDestination, "bus 40 was accepted");
    strip.set_destination(-7);
    expect(strip.destination() == nirbija::kMasterDestination, "-7 was accepted");
    strip.set_destination(nirbija::channel_destination(5));
    expect(strip.destination() == nirbija::channel_destination(5),
           "a channel destination was refused");
    strip.set_destination(nirbija::channel_destination(nirbija::kMaxChannels));
    expect(strip.destination() == nirbija::kMasterDestination,
           "a channel slot past kMaxChannels was accepted");

    // And a strip pointed at a bus that does not exist, with another bus
    // soloed, renders without touching the bus array past its end.
    const size_t a = graph.add_channel("a", 2, std::make_unique<DcSource>(0.25f));
    const size_t bus = graph.add_bus("fx");
    graph.channel(a).set_destination(static_cast<int>(nirbija::kMaxBuses) - 1);
    graph.bus(bus).set_soloed(true);
    render(graph, 8);
    graph.bus(bus).set_soloed(false);
    graph.channel(a).set_destination(nirbija::kMasterDestination);
    graph.remove_bus(bus);
    graph.remove_channel(a);
    graph.reclaim(false);
    render(graph, 64);
  }

  // --- scratch cleared between blocks -------------------------------------------
  {
    const size_t a = graph.add_channel("a", 2, std::make_unique<DcSource>(0.5f));
    const size_t b = graph.add_channel("b", 2, std::make_unique<DcSource>(0.0f));
    const size_t bus = graph.add_bus("fx");
    render(graph, 64);
    expect(std::fabs(render(graph).last - 0.5f) < 1e-4f, "a alone is not 0.5");

    graph.channel(a).set_destination(static_cast<int>(bus));
    render(graph, 64);
    expect(std::fabs(render(graph).last - 0.5f) < 1e-4f, "through the bus is not 0.5");
    graph.channel(a).set_destination(nirbija::channel_destination(b));
    render(graph, 64);
    expect(std::fabs(render(graph).last - 0.5f) < 1e-4f, "through channel b is not 0.5");
    graph.channel(a).set_destination(nirbija::kMasterDestination);
    render(graph, 64);
    Block after = render(graph);
    // Were the bus or channel b's input scratch not cleared once nothing
    // fed them, their last block would keep being mixed in: 1.0 or more.
    expect(std::fabs(after.last - 0.5f) < 1e-4f,
           "a reroute left a copy behind: " + std::to_string(after.last));
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
