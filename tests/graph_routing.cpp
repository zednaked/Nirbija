// Drives the graph with synthetic sources, so routing, mute, solo, fader and
// meters are covered without a JACK server.

#include <cmath>
#include <cstdio>
#include <vector>

#include "core/audio_graph.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;

// Emits a constant level on every channel.
class DcSource : public nirbija::AudioSource {
 public:
  explicit DcSource(float level) : level_(level) {}
  void read(float* const* dest, int channels, uint32_t frames) override {
    for (int ch = 0; ch < channels; ++ch)
      std::fill_n(dest[ch], frames, level_);
  }

 private:
  float level_;
};

int failures = 0;

void expect_near(const char* what, float actual, float expected, float tolerance) {
  if (std::fabs(actual - expected) > tolerance) {
    std::fprintf(stderr, "FAIL %s: got %.6f, want %.6f (+-%.6f)\n", what, actual,
                 expected, tolerance);
    ++failures;
  }
}

// Runs enough blocks for the strip's parameter smoothing to settle, then
// returns the peak of the last block's master output.
float settle(nirbija::AudioGraph& graph, int blocks = 64) {
  std::vector<float> left(kBlock), right(kBlock);
  float* master[2] = {left.data(), right.data()};
  for (int i = 0; i < blocks; ++i) graph.render(master, kBlock);
  return left[kBlock - 1];
}

}  // namespace

int main() {
  nirbija::AudioGraph graph;
  graph.prepare(kSampleRate, kBlock);

  const size_t a = graph.add_channel("a", 2, std::make_unique<DcSource>(0.5f));
  const size_t b = graph.add_channel("b", 2, std::make_unique<DcSource>(0.25f));
  if (a != 0 || b != 1) {
    std::fprintf(stderr, "FAIL: unexpected channel indices %zu %zu\n", a, b);
    return 1;
  }

  expect_near("two strips sum at unity", settle(graph), 0.75f, 1e-4f);

  graph.channel(a).set_gain(0.5f);
  expect_near("fader on strip a", settle(graph), 0.5f, 1e-4f);

  graph.channel(a).set_muted(true);
  expect_near("muting a leaves only b", settle(graph), 0.25f, 1e-4f);
  graph.channel(a).set_muted(false);

  graph.channel(b).set_soloed(true);
  expect_near("solo on b silences a", settle(graph), 0.25f, 1e-4f);
  graph.channel(b).set_soloed(false);

  graph.channel(a).set_gain(1.0f);
  graph.channel(a).set_pan(-1.0f);
  settle(graph);
  std::vector<float> left(kBlock), right(kBlock);
  float* master[2] = {left.data(), right.data()};
  graph.render(master, kBlock);
  expect_near("hard-left keeps a on the left", left[kBlock - 1], 0.75f, 1e-4f);
  expect_near("hard-left drops a from the right", right[kBlock - 1], 0.25f, 1e-4f);
  graph.channel(a).set_pan(0.0f);

  graph.set_master_gain(0.5f);
  expect_near("master fader", settle(graph), 0.375f, 1e-4f);
  graph.set_master_gain(1.0f);

  settle(graph);
  expect_near("master meter follows output", graph.read_master_peak(0), 0.75f, 1e-3f);
  expect_near("reading the meter resets it", graph.read_master_peak(0), 0.0f, 1e-6f);

  settle(graph);
  expect_near("strip meter is post-fader", graph.channel(b).read_peak(0), 0.25f, 1e-3f);

  // --- one channel feeding another -------------------------------------------
  // Channel a points at channel b instead of the master; b carries both its own
  // source and a's output.
  {
    graph.channel(a).set_destination(nirbija::channel_destination(b));
    const float combined = settle(graph);
    // a (0.75 through its fader at 1.0... reset state first)
    graph.channel(a).set_destination(nirbija::kMasterDestination);
    if (combined <= 0.0f) {
      std::fprintf(stderr, "FAIL channel-to-channel produced nothing\n");
      ++failures;
    }
  }

  // A cleaner check with fresh numbers: a=0.5 into b=0.25 gives the master
  // 0.75 through one path only.
  {
    graph.channel(a).set_gain(1.0f);
    graph.channel(b).set_gain(1.0f);
    graph.set_master_gain(1.0f);
    graph.channel(a).set_destination(nirbija::channel_destination(b));
    expect_near("a routed through b still sums to the same total",
                settle(graph), 0.75f, 1e-3f);

    // Muting b silences a too now, which is the whole point of the routing.
    graph.channel(b).set_muted(true);
    expect_near("muting b silences a as well", settle(graph), 0.0f, 1e-4f);
    graph.channel(b).set_muted(false);
    graph.channel(a).set_destination(nirbija::kMasterDestination);
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
