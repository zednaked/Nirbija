// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// Runs audio through real CLAP plugins installed on this machine: an effect for
// the insert path, and an instrument for the no-audio-input path. The Drone
// built next to the host is always there, so the backend itself - scan cache,
// re-activation, parameters while inactive - is exercised on every machine.

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/channel_strip.h"
#include "core/plugin.h"
#include "hosting/common.h"

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

// The backend must survive deactivate() + activate(): a JACK period change
// re-activates every insert with a bigger block, and the plugin must come back
// as it was, not at its defaults. Also the paths only the Drone can show on
// every machine: a parameter set while inactive lands through params.flush.
void check_lifecycle(nirbija::PluginBackend& backend,
                     const nirbija::PluginDescriptor& desc) {
  auto plugin = backend.instantiate(desc);
  if (plugin == nullptr) {
    fail("instantiate returned null for " + desc.name);
    return;
  }
  plugin->set_channel_layout(2);
  const auto params = plugin->parameters();
  if (params.empty()) {
    fail(desc.name + " reported no parameters");
    return;
  }
  const auto& first = params.front();

  // Inactive: no process() will ever carry this, so it must go straight in.
  const double original = plugin->parameter_value(first.id);
  const double moved =
      (original == first.max_value) ? first.min_value : first.max_value;
  plugin->set_parameter(first.id, moved);
  if (plugin->parameter_value(first.id) != moved)
    fail(desc.name + ": a parameter set before activate was lost");

  if (!plugin->activate(kSampleRate, kBlock)) {
    fail("activate failed for " + desc.name);
    return;
  }
  Buffers in, out;
  const float* in_ptrs[2] = {in.ptrs[0], in.ptrs[1]};
  plugin->process(in_ptrs, out.ptrs, kBlock);
  if (plugin->parameter_value(first.id) != moved)
    fail(desc.name + ": the pre-activate value did not survive activation");

  // Period change: down and up again with a bigger block. Same instance,
  // same state.
  plugin->deactivate();
  const uint32_t bigger = kBlock * 4;
  if (!plugin->activate(kSampleRate, bigger)) {
    fail(desc.name + ": re-activate with a bigger block failed");
    return;
  }
  if (plugin->parameter_value(first.id) != moved)
    fail(desc.name + ": re-activation lost a parameter");

  std::vector<std::vector<float>> big_in(2, std::vector<float>(bigger, 0.0f));
  std::vector<std::vector<float>> big_out(2, std::vector<float>(bigger, 0.0f));
  const float* big_in_ptrs[2] = {big_in[0].data(), big_in[1].data()};
  float* big_out_ptrs[2] = {big_out[0].data(), big_out[1].data()};
  for (int block = 0; block < 4; ++block)
    plugin->process(big_in_ptrs, big_out_ptrs, bigger);
  for (const auto& channel : big_out)
    for (float sample : channel)
      if (!std::isfinite(sample)) {
        fail(desc.name + " produced NaN or inf after re-activation");
        break;
      }

  // And a value set while deactivated, picked up by the next activation.
  plugin->deactivate();
  plugin->set_parameter(first.id, original);
  if (!plugin->activate(kSampleRate, kBlock)) {
    fail(desc.name + ": third activate failed");
    return;
  }
  plugin->process(in_ptrs, out.ptrs, kBlock);
  if (plugin->parameter_value(first.id) != original)
    fail(desc.name + ": a parameter set while deactivated was lost");
  plugin->deactivate();
  std::printf("  %s: survived deactivate/activate with state intact\n",
              desc.name.c_str());
}

// Where the Drone .clap was built: next to this binary's tree, unless told.
std::string drone_dir() {
  if (const char* env = std::getenv("NIRBIJA_DRONE_CLAP_DIR")) return env;
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return {};
  // <build>/tests/nirbija_clap_process -> <build>/clap
  return (exe.parent_path().parent_path() / "clap").string();
}

}  // namespace

int main() {
  std::unique_ptr<nirbija::PluginBackend> backend;
  for (auto& candidate : nirbija::make_all_backends())
    if (candidate->format() == nirbija::PluginFormat::Clap)
      backend = std::move(candidate);

  if (backend == nullptr) {
    std::printf("CLAP backend not compiled in, skipping\n");
    return 77;
  }

  // The Drone built with the host joins the search path, so there is always
  // one CLAP to run. The scan cache goes to a private directory: this test
  // must neither read the user's cache nor leave anything in it.
  const std::string drone = drone_dir();
  if (!drone.empty()) setenv("CLAP_PATH", drone.c_str(), 1);
  const std::filesystem::path cache_dir =
      std::filesystem::temp_directory_path() /
      ("nirbija-clap-process-" + std::to_string(getpid()));
  std::filesystem::create_directories(cache_dir);
  setenv("XDG_CACHE_HOME", cache_dir.c_str(), 1);

  auto& opens = nirbija::hosting::module_open_count();
  const uint64_t before = opens.load();
  const auto all = backend->scan();
  const uint64_t after_first = opens.load();
  if (after_first == before) fail("the first scan opened no module at all");

  // Second scan: every module is unchanged, so none may be opened again.
  const auto again = backend->scan();
  if (opens.load() != after_first)
    fail("the second scan re-opened " + std::to_string(opens.load() - after_first) +
         " module(s) the cache should have answered for");
  if (again.size() != all.size())
    fail("the cached scan listed " + std::to_string(again.size()) +
         " plugins, the real one " + std::to_string(all.size()));
  for (size_t i = 0; i < all.size() && i < again.size(); ++i) {
    if (all[i].uid != again[i].uid || all[i].name != again[i].name ||
        all[i].kind != again[i].kind || all[i].path != again[i].path ||
        all[i].category != again[i].category) {
      fail("cached descriptor differs from the scanned one for " + all[i].name);
      break;
    }
  }

  // A corrupt cache is ignored and rebuilt, not trusted and not fatal.
  {
    std::ofstream garbage(cache_dir / "nirbija" / "plugins-clap.json",
                          std::ios::trunc);
    garbage << "{\"version\":1,\"modules\":[{\"path\":\"x\",\"plugins\":[{\"kind\":99}]},";
  }
  const auto rebuilt = backend->scan();
  if (rebuilt.size() != all.size())
    fail("scanning over a corrupt cache changed the result");
  if (opens.load() == after_first)
    fail("scanning over a corrupt cache opened nothing: it trusted garbage");

  std::error_code ec;
  std::filesystem::remove_all(cache_dir, ec);

  if (all.empty()) {
    std::printf("no CLAP plugins found, not even the Drone; skipping\n");
    return failures > 0 ? 1 : 77;
  }

  bool found_drone = false;
  for (const auto& desc : all) {
    if (desc.uid != "com.nirbija.drone") continue;
    found_drone = true;
    check_lifecycle(*backend, desc);
    check_instrument(*backend, desc);
  }
  if (!found_drone) fail("the built Drone .clap was not found under " + drone);

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
