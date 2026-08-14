// Runs audio through real CLAP plugins installed on this machine: an effect for
// the insert path, and an instrument for the no-audio-input path.

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

  void fill_with_tone() {
    for (uint32_t i = 0; i < kBlock; ++i)
      data[0][i] = data[1][i] = std::sin(static_cast<float>(i) * 0.37f) * 0.5f;
  }

  float peak() const {
    float peak = 0.0f;
    for (const auto& channel : data)
      for (float sample : channel) peak = std::max(peak, std::fabs(sample));
    return peak;
  }

  bool has_non_finite() const {
    for (const auto& channel : data)
      for (float sample : channel)
        if (!std::isfinite(sample)) return true;
    return false;
  }

  std::vector<std::vector<float>> data;
  float* ptrs[2];
};

const nirbija::PluginDescriptor* find_by_name(
    const std::vector<nirbija::PluginDescriptor>& all, const std::string& name) {
  for (const auto& desc : all)
    if (desc.name == name) return &desc;
  return nullptr;
}

void check_effect(nirbija::PluginBackend& backend,
                  const nirbija::PluginDescriptor& desc) {
  auto plugin = backend.instantiate(desc);
  if (plugin == nullptr) {
    fail("instantiate returned null for " + desc.name);
    return;
  }

  plugin->set_channel_layout(2);
  if (!plugin->activate(kSampleRate, kBlock)) {
    fail("activate failed for " + desc.name);
    return;
  }

  // Port counts are unknown until the plugin is live, so this is the first
  // point they can be checked.
  if (plugin->descriptor().audio_inputs != 2 ||
      plugin->descriptor().audio_outputs != 2)
    fail(desc.name + " did not report a stereo in/out layout");

  Buffers in, out;
  in.fill_with_tone();
  const float* in_ptrs[2] = {in.ptrs[0], in.ptrs[1]};
  plugin->process(in_ptrs, out.ptrs, kBlock);

  if (out.has_non_finite()) fail(desc.name + " produced NaN or inf");
  if (out.peak() <= 1e-5f) fail(desc.name + " output is silent");

  const auto params = plugin->parameters();
  if (params.empty()) {
    fail(desc.name + " reported no parameters");
    return;
  }

  // A CLAP parameter change only lands when the plugin next processes, so the
  // value is read after a block, not straight after the call.
  const auto& first = params.front();
  const double original = plugin->parameter_value(first.id);
  const double moved =
      (original == first.max_value) ? first.min_value : first.max_value;
  plugin->set_parameter(first.id, moved);
  plugin->process(in_ptrs, out.ptrs, kBlock);
  if (plugin->parameter_value(first.id) != moved)
    fail(desc.name + ": parameter event did not reach the plugin");

  const auto blob = plugin->save_state();
  if (blob.empty()) {
    fail(desc.name + " saved an empty state blob");
    return;
  }
  plugin->set_parameter(first.id, original);
  plugin->process(in_ptrs, out.ptrs, kBlock);
  if (!plugin->load_state(blob)) fail(desc.name + ": load_state rejected its own blob");
  if (plugin->parameter_value(first.id) != moved)
    fail(desc.name + ": state round-trip lost a parameter");

  plugin->deactivate();
  std::printf("  %s: %zu parameters, %zu-byte state\n", desc.name.c_str(),
              params.size(), blob.size());
}

// An instrument has no audio inputs, so the host must hand the plugin a null
// input bus rather than an empty one.
void check_instrument(nirbija::PluginBackend& backend,
                      const nirbija::PluginDescriptor& desc) {
  auto plugin = backend.instantiate(desc);
  if (plugin == nullptr) {
    fail("instantiate returned null for " + desc.name);
    return;
  }

  plugin->set_channel_layout(2);
  if (!plugin->activate(kSampleRate, kBlock)) {
    fail("activate failed for " + desc.name);
    return;
  }
  if (plugin->descriptor().audio_inputs != 0)
    std::printf("  note: %s reports %d audio input(s)\n", desc.name.c_str(),
                plugin->descriptor().audio_inputs);

  Buffers in, out;
  const float* in_ptrs[2] = {in.ptrs[0], in.ptrs[1]};
  for (int block = 0; block < 4; ++block)
    plugin->process(in_ptrs, out.ptrs, kBlock);

  // Silence is the correct output with no notes playing; only NaN is a failure.
  if (out.has_non_finite()) fail(desc.name + " produced NaN or inf");

  plugin->deactivate();
  std::printf("  %s: ran idle without producing garbage\n", desc.name.c_str());
}

}  // namespace

int main() {
  std::unique_ptr<nirbija::PluginBackend> backend;
  for (auto& candidate : nirbija::make_all_backends())
    if (candidate->format() == nirbija::PluginFormat::Clap)
      backend = std::move(candidate);

  if (backend == nullptr) {
    std::printf("CLAP backend not compiled in, skipping\n");
    return 0;
  }

  const auto all = backend->scan();
  if (all.empty()) {
    std::printf("no CLAP plugins installed, skipping\n");
    return 0;
  }

  if (const auto* effect = find_by_name(all, "Dragonfly Room Reverb")) {
    check_effect(*backend, *effect);

    // The same plugin through a channel strip, which is how it gets used.
    auto insert = backend->instantiate(*effect);
    nirbija::ChannelStrip strip("test", 2);
    strip.add_insert(std::move(insert));
    strip.prepare(kSampleRate, kBlock);

    Buffers buffers;
    buffers.fill_with_tone();
    strip.process(buffers.ptrs, kBlock);
    if (buffers.has_non_finite()) fail("strip insert produced NaN or inf");
    if (buffers.peak() <= 1e-5f) fail("strip insert silenced the signal");
  } else {
    std::printf("Dragonfly Room Reverb not installed, skipping the effect check\n");
  }

  if (const auto* instrument = find_by_name(all, "Surge XT")) {
    check_instrument(*backend, *instrument);
  } else {
    std::printf("Surge XT not installed, skipping the instrument check\n");
  }

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("ok: %zu CLAP plugins visible\n", all.size());
  return 0;
}
