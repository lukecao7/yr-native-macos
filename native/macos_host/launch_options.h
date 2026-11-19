// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <functional>
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
};

// Arguments exclude argv[0]. An omitted directory or map is represented by an empty value.
std::optional<LaunchOptions> parse_launch_options(
    std::span<const std::string_view> arguments, std::string& error);
std::optional<std::filesystem::path> validate_data_directory(
    const std::filesystem::path& path, std::string& error);

// Reads the containing macOS app's most recent valid picker selection.
std::optional<std::filesystem::path> load_saved_data_directory();

// AppKit main thread only. An empty result with an empty error denotes cancellation.
std::optional<std::filesystem::path> choose_data_directory(std::string& error);

using DirectorySelector = std::function<std::optional<std::filesystem::path>(std::string&)>;

// Use a one-run override, then the saved directory, then a picker. Only a
// valid picker result is saved. The default picker requires the AppKit main
// thread. A selector may return no path and leave error empty to cancel.
std::optional<std::filesystem::path> resolve_data_directory(
    const std::filesystem::path& requested, std::string& error,
    const DirectorySelector& selector = {});

}  // namespace coastal::macos_host
