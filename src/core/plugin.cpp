#include "core/plugin.h"

#if NIRBIJA_HAVE_LV2
#include "hosting/lv2_backend.h"
#endif
#if NIRBIJA_HAVE_CLAP
#include "hosting/clap_backend.h"
#endif

namespace nirbija {

std::vector<std::unique_ptr<PluginBackend>> make_all_backends() {
  std::vector<std::unique_ptr<PluginBackend>> backends;
#if NIRBIJA_HAVE_LV2
  backends.push_back(make_lv2_backend());
#endif
#if NIRBIJA_HAVE_CLAP
  backends.push_back(make_clap_backend());
#endif
  return backends;
}

}  // namespace nirbija
