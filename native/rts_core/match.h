// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "world.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace coastal::rts {

std::optional<World> open_match(const std::filesystem::path& directory, std::string_view map_name,
                                std::string& error);

}  // namespace coastal::rts
