// Nothing in the graph may step. Every switch a person can throw while the
// music plays - mute, solo, bypass, a send, the master fader, dim, mono, the
// park around a state load - has to reach the speaker as a slope, and the
// limiter has to hold the ceiling without touching what is under it. Each
// check here feeds a strip a constant and watches the master sample by
// sample for a jump.

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

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

void expect(bool ok, const std::string& what) {
  if (!ok) fail(what);
}

void expect_near(const char* what, float actual, float expected,
                 float tolerance) {
  if (std::fabs(actual - expected) > tolerance) {
    std::fprintf(stderr, "FAIL %s: got %.6f, want %.6f (+-%.6f)\n", what,
                 actual, expected, tolerance);
    ++failures;
  }
}

class DcSource : public nirbija::AudioSource {
 public:
  explicit DcSource(float level) : level_(level) {}
  void read(float* const* dest, int channels, uint32_t frames) override {
    ++reads;
    for (int ch = 0; ch < channels; ++ch)
      std::fill_n(dest[ch], frames, level_);
  }
  int reads = 0;

 private:
  float level_;
};

// An insert that replaces whatever it is given with a constant, so a bypass
// is a visible change of level.
class ConstantInsert : public nirbija::PluginInstance {
 public:
  explicit ConstantInsert(float level) : level_(level) {}
  void set_channel_layout(int) override {}
  bool activate(double, uint32_t) override { return true; }
  void deactivate() override {}
  void process(const float* const*, float* const* outputs,
               uint32_t frames) override {
    for (int ch = 0; ch < 2; ++ch) std::fill_n(outputs[ch], frames, level_);
  }
  std::vector<nirbija::ParameterInfo> parameters() const override { return {}; }
  double parameter_value(uint32_t) const override { return 0.0; }
  void set_parameter(uint32_t, double) override {}
  std::vector<uint8_t> save_state() const override { return {}; }
  bool load_state(const std::vector<uint8_t>&) override { return true; }
  const nirbija::PluginDescriptor& descriptor() const override { return desc_; }

 private:
  float level_;
  nirbija::PluginDescriptor desc_{.format = nirbija::PluginFormat::Internal,
                                  .uid = "test.constant",
                                  .name = "Constant",
                                  .kind = nirbija::PluginKind::Effect,
                                  .audio_inputs = 2,
                                  .audio_outputs = 2};
};

// Renders `blocks` blocks and reports the largest sample-to-sample jump seen
// on the left master output, across block edges included, plus the level it
// ended on. A step is a jump of the whole difference in one sample; a slope
// spreads it over hundreds.
struct Watch {
  float max_jump = 0.0f;
  float last = 0.0f;
  float peak = 0.0f;
  int quiet_blocks = 0;
};

Watch watch(nirbija::AudioGraph& graph, int blocks, float* carry = nullptr) {
  std::vector<float> left(kBlock), right(kBlock);
  float* master[2] = {left.data(), right.data()};
  Watch w;
  float previous = carry != nullptr ? *carry : 0.0f;
  bool have_previous = carry != nullptr;
  for (int i = 0; i < blocks; ++i) {
    const uint64_t quiet = graph.quiet_generation();
    graph.render(master, kBlock);
    if (graph.quiet_generation() != quiet) ++w.quiet_blocks;
    for (uint32_t f = 0; f < kBlock; ++f) {
      if (have_previous)
        w.max_jump = std::max(w.max_jump, std::fabs(left[f] - previous));
      previous = left[f];
      have_previous = true;
      w.peak = std::max(w.peak, std::fabs(left[f]));
    }
  }
  w.last = previous;
  if (carry != nullptr) *carry = previous;
  return w;
}

// The steepest slope any of the ramps here is allowed: a full swing of 1.0
// over 5 ms (the park fade, the fastest of them) is 1/240 per sample at
// 48 kHz. Everything else is slower. A step would be the whole swing at once.
constexpr float kSteepest = 1.0f / (0.005f * 48000.0f) * 1.05f;

}  // namespace

int main() {
  nirbija::AudioGraph graph;
  graph.prepare(kSampleRate, kBlock);

  auto source_a = std::make_unique<DcSource>(0.5f);
  DcSource* dc_a = source_a.get();
  const size_t a = graph.add_channel("a", 2, std::move(source_a));
  const size_t b = graph.add_channel("b", 2, std::make_unique<DcSource>(0.25f));
  expect(a == 0 && b == 1, "unexpected channel indices");

  float carry = 0.0f;
  watch(graph, 64, &carry);  // settle
  expect_near("starts at the sum", carry, 0.75f, 1e-4f);

  // --- strip mute ---------------------------------------------------------
  {
    graph.channel(a).set_muted(true);
    const Watch w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "mute stepped: jump " + std::to_string(w.max_jump));
    expect_near("mute lands at b alone", w.last, 0.25f, 1e-4f);
    graph.channel(a).set_muted(false);
    const Watch back = watch(graph, 16, &carry);
    expect(back.max_jump <= kSteepest,
           "unmute stepped: jump " + std::to_string(back.max_jump));
    expect_near("unmute lands back at the sum", back.last, 0.75f, 1e-4f);
  }

  // --- solo -----------------------------------------------------------------
  {
    graph.channel(b).set_soloed(true);
    const Watch w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "solo stepped: jump " + std::to_string(w.max_jump));
    expect_near("solo lands at b alone", w.last, 0.25f, 1e-4f);
    graph.channel(b).set_soloed(false);
    const Watch back = watch(graph, 16, &carry);
    expect(back.max_jump <= kSteepest,
           "unsolo stepped: jump " + std::to_string(back.max_jump));
    expect_near("unsolo lands back at the sum", back.last, 0.75f, 1e-4f);
  }

  // --- master fader, dim, mute, mono ----------------------------------------
  {
    graph.set_master_gain(0.25f);
    Watch w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "master fader stepped");
    expect_near("master fader lands", w.last, 0.1875f, 1e-4f);
    graph.set_master_gain(1.0f);
    watch(graph, 16, &carry);

    graph.set_master_dim(true);
    w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "dim stepped");
    expect_near("dim is 12 dB down", w.last, 0.1875f, 1e-4f);
    graph.set_master_dim(false);
    watch(graph, 16, &carry);

    graph.set_master_mute(true);
    w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "master mute stepped");
    expect_near("master mute lands at silence", w.last, 0.0f, 1e-6f);
    graph.set_master_mute(false);
    w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "master unmute stepped");

    // Mono on a hard-panned mono strip moves the left side; it must slide.
    graph.channel(a).set_pan(1.0f);
    watch(graph, 64, &carry);
    graph.set_master_mono(true);
    w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "mono stepped");
    graph.set_master_mono(false);
    graph.channel(a).set_pan(0.0f);
    watch(graph, 64, &carry);
  }

  // --- a send ---------------------------------------------------------------
  {
    const size_t bus = graph.add_bus("fx");
    expect(bus < nirbija::kMaxBuses, "could not add a bus");
    graph.channel(a).set_send(0, static_cast<int>(bus), 1.0f);
    Watch w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "send level stepped up");
    expect_near("send adds a on top", w.last, 1.25f, 1e-3f);
    graph.channel(a).set_send(0, static_cast<int>(bus), 0.0f);
    w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "send level stepped down");
    graph.channel(a).set_send(0, -1, 0.0f);
    graph.remove_bus(bus);
    watch(graph, 16, &carry);
  }

  // --- insert bypass --------------------------------------------------------
  {
    nirbija::ChannelStrip& strip = graph.channel(b);
    size_t slot = 0;
    expect(strip.add_insert(std::make_unique<ConstantInsert>(0.1f), &slot),
           "could not add the constant insert");
    Watch w = watch(graph, 16, &carry);
    expect_near("the insert replaces b", w.last, 0.6f, 1e-4f);
    strip.set_insert_bypassed(slot, true);
    w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "bypass stepped: jump " + std::to_string(w.max_jump));
    expect_near("bypass lands on the dry", w.last, 0.75f, 1e-4f);
    strip.set_insert_bypassed(slot, false);
    w = watch(graph, 16, &carry);
    expect(w.max_jump <= kSteepest, "un-bypass stepped");
    expect_near("un-bypass lands on the wet", w.last, 0.6f, 1e-4f);
    strip.remove_insert(slot);
    strip.reclaim(false);
    watch(graph, 16, &carry);
  }

  // --- park -----------------------------------------------------------------
  {
    const int reads_before = dc_a->reads;
    graph.park();
    Watch w = watch(graph, 8, &carry);
    expect(w.max_jump <= kSteepest, "park stepped: jump " + std::to_string(w.max_jump));
    expect_near("parked master is silent", w.last, 0.0f, 1e-6f);
    expect(w.quiet_blocks > 0, "no block was ever rendered quiet");
    // The fade takes 240 samples, one block; every block after that runs
    // no strip at all.
    expect(dc_a->reads - reads_before < 8, "a parked graph kept running strips");
    const int reads_parked = dc_a->reads;
    watch(graph, 4, &carry);
    expect(dc_a->reads == reads_parked, "strips ran during a quiet block");

    graph.unpark();
    w = watch(graph, 8, &carry);
    expect(w.max_jump <= kSteepest, "unpark stepped: jump " + std::to_string(w.max_jump));
    expect_near("unpark comes back to the sum", w.last, 0.75f, 1e-4f);
    expect(w.quiet_blocks == 0, "a block went quiet after unpark");
  }

  // --- limiter --------------------------------------------------------------
  {
    expect(graph.master_latency_samples() == 0, "limiter off reports latency");
    (void)graph.read_master_clip();  // the send check above ran hot on purpose
    graph.set_master_limiter(true);
    expect(graph.master_latency_samples() == 72, "limiter latency is not 1.5 ms");
    Watch w = watch(graph, 32, &carry);
    expect(w.max_jump <= kSteepest, "switching the limiter on stepped");
    expect_near("under the ceiling the limiter is transparent", w.last, 0.75f,
                1e-4f);
    expect_near("nothing was held back", graph.read_limiter_floor(), 1.0f, 1e-3f);

    graph.channel(a).set_gain(3.0f);  // 1.5 + 0.25 = 1.75 into the ceiling
    watch(graph, 64, &carry);
    w = watch(graph, 32, &carry);
    expect(w.peak <= 0.9661f, "limiter let " + std::to_string(w.peak) + " out");
    expect(w.peak > 0.96f, "limiter squashed well under the ceiling");
    expect(graph.read_limiter_floor() < 0.6f, "reduction went unreported");
    expect(graph.read_master_clip() == 0.0f, "the clip light lit under the limiter");

    graph.set_master_limiter(false);
    watch(graph, 64, &carry);
    w = watch(graph, 8, &carry);
    expect(w.peak > 1.7f, "limiter off still limits");
    expect(graph.read_master_clip() > 0.0f, "the clip light stayed dark over full scale");
    graph.channel(a).set_gain(1.0f);
    watch(graph, 64, &carry);
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
