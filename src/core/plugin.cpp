#include "core/plugin.h"

#if NIRBIJA_HAVE_LV2
#include "hosting/lv2_backend.h"
#endif
#if NIRBIJA_HAVE_CLAP
#include "hosting/clap_backend.h"
#endif

#include "core/file_player.h"

namespace nirbija {
namespace {

// The plugins built into the host itself. First in scan order so they head the
// picker rather than drowning among two hundred effects.
class InternalBackend : public PluginBackend {
 public:
  PluginFormat format() const override { return PluginFormat::Internal; }

  std::vector<PluginDescriptor> scan() override {
    return {FilePlayerInstance::make_descriptor()};
  }

  std::unique_ptr<PluginInstance> instantiate(
      const PluginDescriptor& desc) override {
    if (desc.uid == "nirbija.fileplayer")
      return std::make_unique<FilePlayerInstance>();
    return nullptr;
  }
};

}  // namespace

std::vector<std::unique_ptr<PluginBackend>> make_all_backends() {
  std::vector<std::unique_ptr<PluginBackend>> backends;
  backends.push_back(std::make_unique<InternalBackend>());
#if NIRBIJA_HAVE_LV2
  backends.push_back(make_lv2_backend());
#endif
#if NIRBIJA_HAVE_CLAP
  backends.push_back(make_clap_backend());
#endif
  return backends;
}

}  // namespace nirbija
