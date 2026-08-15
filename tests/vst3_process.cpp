// Runs audio through a real VST3 plugin installed on this machine: the same
// bar the LV2 and CLAP backends had to clear.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/channel_strip.h"
#include "core/plugin.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;

int failures = 0;

void fail(const std::string& what) {
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
  ++failures;
}

struct Buffers {
  Buffers() : data(2, std::vector<float>(kBlock, 0.0f)) {
    ptrs[0] = data[0].data();
    ptrs[1] = data[1].data();
  }
  void tone() {
    for (uint32_t i = 0; i < kBlock; ++i)
      data[0][i] = data[1][i] = std::sin(static_cast<float>(i) * 0.37f) * 0.5f;
  }
  float peak() const {
    float peak = 0.0f;
    for (const auto& channel : data)
      for (float sample : channel) peak = std::max(peak, std::fabs(sample));
    return peak;
  }
  bool bad() const {
    for (const auto& channel : data)
      for (float sample : channel)
        if (!std::isfinite(sample)) return true;
    return false;
  }
  std::vector<std::vector<float>> data;
  float* ptrs[2];
};

}  // namespace

int main(int argc, char* argv[]) {
  const std::string wanted = argc > 1 ? argv[1] : "Dragonfly Hall Reverb";

  std::unique_ptr<nirbija::PluginBackend> backend;
  for (auto& candidate : nirbija::make_all_backends())
    if (candidate->format() == nirbija::PluginFormat::Vst3)
      backend = std::move(candidate);
  if (backend == nullptr) {
    std::printf("VST3 backend not compiled in, skipping\n");
    return 0;
  }

  const auto all = backend->scan();
  const nirbija::PluginDescriptor* chosen = nullptr;
  for (const auto& desc : all)
    if (desc.name == wanted) chosen = &desc;
  if (chosen == nullptr) {
    std::printf("%s not installed as VST3, skipping\n", wanted.c_str());
    return 0;
  }

  auto plugin = backend->instantiate(*chosen);
  if (plugin == nullptr) {
    fail("instantiate returned null");
    return 1;
  }
  plugin->set_channel_layout(2);
  if (!plugin->activate(kSampleRate, kBlock)) {
    fail("activate failed");
    return 1;
  }
  if (plugin->descriptor().audio_inputs < 2 ||
      plugin->descriptor().audio_outputs < 2)
    fail("bus layout did not come back stereo");

  Buffers in, out;
  in.tone();
  const float* in_ptrs[2] = {in.ptrs[0], in.ptrs[1]};
  plugin->process(in_ptrs, out.ptrs, kBlock);
  if (out.bad()) fail("output has NaN or inf");

  // A reverb keeps ringing after the input stops; a broken host goes silent.
  for (auto& channel : in.data) std::fill(channel.begin(), channel.end(), 0.0f);
  float tail = 0.0f;
  for (int block = 0; block < 8; ++block) {
    plugin->process(in_ptrs, out.ptrs, kBlock);
    tail = std::max(tail, out.peak());
  }
  if (tail <= 1e-5f) fail("no reverb tail, audio never reached the plugin");

  const auto params = plugin->parameters();
  if (params.empty()) {
    fail("no parameters reported");
  } else {
    const auto& first = params.front();
    const double original = plugin->parameter_value(first.id);
    const double moved = original < 0.5 ? 1.0 : 0.0;
    plugin->set_parameter(first.id, moved);
    plugin->process(in_ptrs, out.ptrs, kBlock);  // edits land on process
    if (plugin->parameter_value(first.id) != moved)
      fail("parameter change did not stick");

    const auto blob = plugin->save_state();
    if (blob.empty()) fail("state blob is empty");
    plugin->set_parameter(first.id, original);
    plugin->process(in_ptrs, out.ptrs, kBlock);
    if (!plugin->load_state(blob)) fail("load_state rejected its own blob");
  }

  // As a strip insert, which is how it is actually used.
  {
    auto insert = backend->instantiate(*chosen);
    nirbija::ChannelStrip strip("test", 2);
    strip.add_insert(std::move(insert));
    strip.prepare(kSampleRate, kBlock);

    Buffers buffers;
    buffers.tone();
    strip.process(buffers.ptrs, kBlock);
    if (buffers.bad()) fail("strip insert produced NaN or inf");
    if (buffers.peak() <= 1e-5f) fail("strip insert silenced the signal");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: %s, %zu parameters, tail %.6f\n", wanted.c_str(),
              params.size(), tail);
  return 0;
}
