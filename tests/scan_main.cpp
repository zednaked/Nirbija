// Lists every plugin the compiled-in backends can see. Fails if no backend
// found anything at all, since this machine is expected to have LV2 plugins.
//
// Also the eye on classification: each format states its kind in its own
// vocabulary, and the tally at the end is how you see whether a backend has
// stopped reading it. A pile of Unknown means something regressed.

#include <cstdio>
#include <map>

#include "core/plugin.h"

namespace {

const char* kind_name(nirbija::PluginKind kind) {
  switch (kind) {
    case nirbija::PluginKind::Instrument: return "instrument";
    case nirbija::PluginKind::Effect: return "effect";
    case nirbija::PluginKind::MidiEffect: return "midi";
    case nirbija::PluginKind::Analyzer: return "analyzer";
    case nirbija::PluginKind::Utility: return "utility";
    case nirbija::PluginKind::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace

int main() {
  size_t total = 0;
  std::map<std::string, size_t> by_kind;

  for (const auto& backend : nirbija::make_all_backends()) {
    const char* label = "";
    switch (backend->format()) {
      case nirbija::PluginFormat::Lv2: label = "LV2"; break;
      case nirbija::PluginFormat::Clap: label = "CLAP"; break;
      case nirbija::PluginFormat::Vst3: label = "VST3"; break;
      case nirbija::PluginFormat::Internal: label = "internal"; break;
    }
    const auto found = backend->scan();
    std::printf("%s: %zu plugin(s)\n", label, found.size());
    for (const auto& desc : found) {
      std::printf("  %-38s %-11s %2d in %2d out  %s\n", desc.name.c_str(),
                  kind_name(desc.kind), desc.audio_inputs, desc.audio_outputs,
                  desc.category.empty() ? "-" : desc.category.c_str());
      ++by_kind[kind_name(desc.kind)];
    }
    total += found.size();
  }

  std::printf("\nby kind:\n");
  for (const auto& [kind, count] : by_kind)
    std::printf("  %-11s %zu\n", kind.c_str(), count);

  if (total == 0) {
    std::fprintf(stderr, "no plugins found by any backend\n");
    return 1;
  }
  return 0;
}
