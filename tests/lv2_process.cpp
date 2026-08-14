// Loads real LV2 plugins installed on this machine, runs audio through them and
// checks the output is sane. Mocking a plugin here would test nothing: the whole
// point of a host is surviving third-party code.

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

struct BlockStats {
  float peak = 0.0f;
  bool has_non_finite = false;
};

BlockStats stats_of(const std::vector<std::vector<float>>& buffers) {
  BlockStats stats;
  for (const auto& channel : buffers)
    for (float sample : channel) {
      if (!std::isfinite(sample)) stats.has_non_finite = true;
      stats.peak = std::max(stats.peak, std::fabs(sample));
    }
  return stats;
}

// Feeds one block of noise, then silence, and reports the peak of the final
// block. A reverb should still be ringing; a bypass would be silent.
float tail_after_impulse(nirbija::PluginInstance& plugin, int silent_blocks) {
  std::vector<std::vector<float>> in(2, std::vector<float>(kBlock, 0.0f));
  std::vector<std::vector<float>> out(2, std::vector<float>(kBlock, 0.0f));
  const float* in_ptrs[2] = {in[0].data(), in[1].data()};
  float* out_ptrs[2] = {out[0].data(), out[1].data()};

  for (uint32_t i = 0; i < kBlock; ++i) {
    // Deterministic pseudo-noise, so a failure is reproducible.
    const float value = std::sin(static_cast<float>(i) * 0.37f) * 0.5f;
    in[0][i] = in[1][i] = value;
  }
  plugin.process(in_ptrs, out_ptrs, kBlock);

  for (auto& channel : in) std::fill(channel.begin(), channel.end(), 0.0f);
  float last_peak = 0.0f;
  for (int block = 0; block < silent_blocks; ++block) {
    plugin.process(in_ptrs, out_ptrs, kBlock);
    const BlockStats stats = stats_of(out);
    if (stats.has_non_finite) {
      fail("plugin produced NaN or inf");
      return 0.0f;
    }
    last_peak = stats.peak;
  }
  return last_peak;
}

const nirbija::PluginDescriptor* find_by_name(
    const std::vector<nirbija::PluginDescriptor>& all, const std::string& name) {
  for (const auto& desc : all)
    if (desc.name == name) return &desc;
  return nullptr;
}

}  // namespace

int main() {
  std::unique_ptr<nirbija::PluginBackend> backend;
  for (auto& candidate : nirbija::make_all_backends())
    if (candidate->format() == nirbija::PluginFormat::Lv2)
      backend = std::move(candidate);

  if (backend == nullptr) {
    std::printf("LV2 backend not compiled in, skipping\n");
    return 0;
  }

  const auto all = backend->scan();
  const nirbija::PluginDescriptor* reverb =
      find_by_name(all, "Dragonfly Hall Reverb");
  if (reverb == nullptr) {
    std::printf("Dragonfly Hall Reverb not installed, skipping\n");
    return 0;
  }

  auto plugin = backend->instantiate(*reverb);
  if (plugin == nullptr) {
    fail("instantiate returned null");
    return 1;
  }

  plugin->set_channel_layout(2);
  if (!plugin->activate(kSampleRate, kBlock)) {
    fail("activate failed");
    return 1;
  }

  const auto params = plugin->parameters();
  if (params.empty()) fail("reverb reported no parameters");

  const float tail = tail_after_impulse(*plugin, 8);
  if (tail <= 1e-5f)
    fail("reverb tail is silent, so audio never reached the plugin");

  // A round-trip through save/load must leave every parameter where it was.
  if (!params.empty()) {
    const uint32_t id = params.front().id;
    const double original = plugin->parameter_value(id);
    const auto blob = plugin->save_state();
    const double moved =
        (original == params.front().max_value) ? params.front().min_value
                                               : params.front().max_value;
    plugin->set_parameter(id, moved);
    if (plugin->parameter_value(id) == original) fail("set_parameter had no effect");
    if (!plugin->load_state(blob)) fail("load_state rejected its own blob");
    if (plugin->parameter_value(id) != original) fail("state round-trip lost a value");
  }

  plugin->deactivate();

  // The same plugin as a strip insert, which is how it will actually be used.
  {
    auto insert = backend->instantiate(*reverb);
    nirbija::ChannelStrip strip("test", 2);
    strip.add_insert(std::move(insert));
    strip.prepare(kSampleRate, kBlock);
    if (strip.insert_count() != 1) fail("insert was not added to the strip");

    std::vector<std::vector<float>> buffers(2, std::vector<float>(kBlock, 0.0f));
    float* ptrs[2] = {buffers[0].data(), buffers[1].data()};
    for (uint32_t i = 0; i < kBlock; ++i)
      buffers[0][i] = buffers[1][i] = std::sin(static_cast<float>(i) * 0.37f) * 0.5f;

    strip.process(ptrs, kBlock);
    const BlockStats after = stats_of(buffers);
    if (after.has_non_finite) fail("strip insert produced NaN or inf");
    if (after.peak <= 1e-5f) fail("strip insert silenced the signal");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: %s, %zu parameters, tail %.6f\n", reverb->name.c_str(),
              params.size(), tail);
  return 0;
}
