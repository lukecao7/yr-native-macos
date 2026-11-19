// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "world.h"

#include <filesystem>
#include <span>
#include <string>

namespace coastal::rts {

std::vector<std::uint8_t> encode_snapshot(const World& world);
std::optional<World> decode_snapshot(std::span<const std::uint8_t> bytes, std::string& error);
bool save_world(const std::filesystem::path& path, const World& world, std::string& error);
std::optional<World> load_world(const std::filesystem::path& path, std::string& error);

struct ReplayCommand {
    std::uint64_t step;
    Command command;
};

// Applies commands before the matching step, preserving file order for ties.
// Includes commands in [world.step, end_step), fails on rejection or bad order.
bool replay(World& world, Navigator& navigator, std::span<const ReplayCommand> commands,
            std::uint64_t end_step, std::string& error);

}  // namespace coastal::rts
