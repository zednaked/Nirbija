// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
// A plugin read back from the scan cache is the same plugin the scan found,
// format included. The file does not store the format, and a descriptor left
// at its default says Internal: from the second launch on, every CLAP and
// VST3 plugin a session named came up "not installed here".

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hosting/common.h"

int main() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
                       ("nirbija-scan-cache-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  ::setenv("XDG_CACHE_HOME", dir.c_str(), 1);

  // Any file will do as the module: the cache only reads its mtime and size.
  const fs::path module = dir / "Synth.clap";
  std::ofstream(module) << "not really a plugin";

  nirbija::PluginDescriptor found;
  found.format = nirbija::PluginFormat::Clap;
  found.uid = "org.example.synth";
  found.name = "Synth";
  found.path = module.string();
  {
    nirbija::hosting::ScanCache cache("clap", nirbija::PluginFormat::Clap);
    cache.store(module.string(), module, {found});
    cache.save();
  }

  int failures = 0;
  std::vector<nirbija::PluginDescriptor> back;
  nirbija::hosting::ScanCache cache("clap", nirbija::PluginFormat::Clap);
  if (!cache.lookup(module.string(), module, &back) || back.size() != 1) {
    std::fprintf(stderr, "FAIL the cache did not give the plugin back\n");
    ++failures;
  } else {
    if (back[0].format != nirbija::PluginFormat::Clap) {
      std::fprintf(stderr, "FAIL a cached CLAP plugin came back as format %d\n",
                   static_cast<int>(back[0].format));
      ++failures;
    }
    if (back[0].uid != found.uid || back[0].name != found.name) {
      std::fprintf(stderr, "FAIL a cached plugin came back renamed\n");
      ++failures;
    }
  }

  fs::remove_all(dir);
  if (failures == 0) std::puts("scan_cache: ok");
  return failures == 0 ? 0 : 1;
}
