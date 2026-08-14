#pragma once

#include "core/plugin.h"

namespace nirbija {

std::unique_ptr<PluginBackend> make_lv2_backend();

}  // namespace nirbija
