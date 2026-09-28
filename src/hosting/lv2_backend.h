// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Nirbija contributors
#pragma once

#include "core/plugin.h"

namespace nirbija {

std::unique_ptr<PluginBackend> make_lv2_backend();

}  // namespace nirbija
