#include "hosting/clap_backend.h"

#include <clap/clap.h>
#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>

namespace nirbija {
namespace {

namespace fs = std::filesystem;

std::vector<fs::path> clap_search_paths() {
  std::vector<fs::path> paths{"/usr/lib/clap", "/usr/local/lib/clap"};
  if (const char* home = std::getenv("HOME")) paths.emplace_back(fs::path(home) / ".clap");
  if (const char* extra = std::getenv("CLAP_PATH")) paths.emplace_back(extra);
  return paths;
}

class ClapBackend : public PluginBackend {
 public:
  PluginFormat format() const override { return PluginFormat::Clap; }

  std::vector<PluginDescriptor> scan() override {
    std::vector<PluginDescriptor> found;
    for (const fs::path& dir : clap_search_paths()) {
      std::error_code ec;
      if (!fs::is_directory(dir, ec)) continue;
      for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".clap") continue;
        scan_module(entry.path(), found);
      }
    }
    return found;
  }

  std::unique_ptr<PluginInstance> instantiate(const PluginDescriptor&) override {
    // TODO(phase-3): create via the factory, supply a clap_host with the log,
    // thread-check, params and state extensions, then activate.
    return nullptr;
  }

 private:
  // A .clap module is a shared object exporting clap_entry. Scanning opens it,
  // reads the descriptors, and closes it again so a scan leaves nothing loaded.
  static void scan_module(const fs::path& path, std::vector<PluginDescriptor>& out) {
    void* handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr) return;

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (entry == nullptr || !entry->init(path.c_str())) {
      dlclose(handle);
      return;
    }

    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory != nullptr) {
      const uint32_t count = factory->get_plugin_count(factory);
      for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory, i);
        if (d == nullptr) continue;
        PluginDescriptor desc;
        desc.format = PluginFormat::Clap;
        desc.uid = d->id != nullptr ? d->id : "";
        desc.name = d->name != nullptr ? d->name : "";
        desc.vendor = d->vendor != nullptr ? d->vendor : "";
        desc.path = path.string();
        // Port counts need an instantiated plugin, so they stay zero until the
        // plugin is actually loaded.
        out.push_back(std::move(desc));
      }
    }

    entry->deinit();
    dlclose(handle);
  }
};

}  // namespace

std::unique_ptr<PluginBackend> make_clap_backend() {
  return std::make_unique<ClapBackend>();
}

}  // namespace nirbija
