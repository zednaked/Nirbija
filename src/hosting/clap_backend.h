#pragma once

#include "core/plugin.h"

namespace nirbija {

std::unique_ptr<PluginBackend> make_clap_backend();

}  // namespace nirbija
