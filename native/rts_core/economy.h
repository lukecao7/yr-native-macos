// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "world.h"

#include <filesystem>
#include <string>

namespace coastal::rts {

std::optional<Rules> load_rules(const std::filesystem::path& path, std::string& error);
bool valid_rules(const Rules& rules) noexcept;
bool can_build(const World& world, Side side, UnitKind kind, Cell position) noexcept;
bool apply_economy(World& world, const Command& command);
void tick_economy(World& world);

}  // namespace coastal::rts
