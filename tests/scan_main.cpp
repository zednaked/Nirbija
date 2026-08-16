// Lists every plugin the compiled-in backends can see. Fails if no backend
// found anything at all, since this machine is expected to have LV2 plugins.

#include <cstdio>

#include "core/plugin.h"

int main() {
  size_t total = 0;
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
    for (const auto& desc : found)
      std::printf("  %-40s %2d in %2d out  %s\n", desc.name.c_str(),
                  desc.audio_inputs, desc.audio_outputs, desc.uid.c_str());
    total += found.size();
  }

  if (total == 0) {
    std::fprintf(stderr, "no plugins found by any backend\n");
    return 1;
  }
  return 0;
}
