// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace coastal::macos_host {

struct LaunchOptions {
    std::filesystem::path data_directory;
    std::string skirmish_map;
    bool strict_audit = false;
    bool help = false;
    bool stress = false;
    unsigned threads = 1;
    bool adaptive_ai = false;
    bool cycle_views = false;
    std::filesystem::path script_file;
    std::filesystem::path script_output;
};

enum class FixtureReplay { Disabled, Enabled };

// Arguments exclude argv[0]. An omitted directory or map is represented by an empty value.
std::optional<LaunchOptions> parse_launch_options(
    std::span<const std::string_view> arguments, std::string& error,
    FixtureReplay replay = FixtureReplay::Disabled);
std::optional<std::filesystem::path> validate_data_directory(
    const std::filesystem::path& path, std::string& error);

}  // namespace coastal::macos_host
