// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "launch_options.h"

#include <functional>

namespace coastal::macos_host {

std::optional<std::filesystem::path> load_saved_data_directory();

// Look up an explicit or saved directory without involving AppKit.
std::optional<std::filesystem::path> resolve_data_directory(
    const std::filesystem::path& requested, std::string& error);

using DirectorySelector = std::function<std::optional<std::filesystem::path>(std::string&)>;
using DirectoryValidator = std::function<bool(const std::filesystem::path&, std::string&)>;

enum class DirectorySelection { Accepted, Rejected, Cancelled, Unavailable };

// Save the selected directory once its resources open successfully.
DirectorySelection select_data_directory(
    const DirectorySelector& select, const DirectoryValidator& open, std::string& error);

// AppKit main thread only. An empty result with an empty error denotes cancellation.
std::optional<std::filesystem::path> choose_data_directory(bool retry, std::string& error);

}  // namespace coastal::macos_host
