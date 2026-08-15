#pragma once

#include "core/plugin.h"

namespace nirbija {

std::unique_ptr<PluginBackend> make_vst3_backend();

}  // namespace nirbija
