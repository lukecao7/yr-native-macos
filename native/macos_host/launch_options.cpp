// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/launch_options.h"

#include <algorithm>
#include <system_error>

namespace coastal::macos_host {
namespace {

char ascii_lower(char letter) noexcept {
    if (letter >= 'A' && letter <= 'Z') {
        return static_cast<char>(letter + ('a' - 'A'));
    }
    return letter;
}

bool valid_map_filename(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 ||
        name.find_first_of("/\\:") != std::string_view::npos) {
        return false;
    }

    const auto extension = name.substr(dot);
    const auto matches = [extension](std::string_view expected) {
        return std::equal(extension.begin(), extension.end(), expected.begin(), expected.end(),
                          [](char actual, char target) { return ascii_lower(actual) == target; });
    };
    return matches(".map") || matches(".yrm");
}

}  // namespace

std::optional<LaunchOptions> parse_launch_options(
    std::span<const std::string_view> arguments, std::string& error) {
    error.clear();
    LaunchOptions options;

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (argument == "--strict-audit" || argument == "--help") {
            bool& enabled = argument == "--strict-audit" ? options.strict_audit : options.help;
            if (enabled) {
                error = "Specify " + std::string(argument) + " once.";
                return std::nullopt;
            }
            enabled = true;
            continue;
        }

        const auto separator = argument.find('=');
        const auto name = argument.substr(0, separator);
        const bool is_data_directory = name == "--data-dir";
        if (!is_data_directory && name != "--skirmish-map") {
            error = "Unrecognized launch argument " + std::string(argument) + ".";
            return std::nullopt;
        }
        if (is_data_directory ? !options.data_directory.empty() : !options.skirmish_map.empty()) {
            error = "Specify " + std::string(name) + " once.";
            return std::nullopt;
        }

        std::string_view value;
        if (separator != std::string_view::npos) {
            value = argument.substr(separator + 1);
        } else if (i + 1 < arguments.size() && !arguments[i + 1].starts_with("--")) {
            value = arguments[++i];
        }
        if (value.empty() || value.find('\0') != std::string_view::npos) {
            error = "Provide a nonempty value for " + std::string(name) + ".";
            return std::nullopt;
        }

        if (is_data_directory) {
            options.data_directory = std::filesystem::path(value);
        } else {
            if (!valid_map_filename(value)) {
                error = "--skirmish-map requires a single .map or .yrm filename.";
                return std::nullopt;
            }
            options.skirmish_map = value;
        }
    }
    return options;
}

std::optional<std::filesystem::path> validate_data_directory(
    const std::filesystem::path& path, std::string& error) {
    error.clear();
    if (path.empty() ||
        path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) {
        error = "Provide a valid data directory path.";
        return std::nullopt;
    }

    std::error_code status;
    auto resolved = std::filesystem::canonical(path, status);
    if (status) {
        error = "Unable to resolve the data directory (" + status.message() + ").";
        return std::nullopt;
    }
    const bool directory = std::filesystem::is_directory(resolved, status);
    if (status) {
        error = "Unable to inspect the data directory (" + status.message() + ").";
        return std::nullopt;
    }
    if (!directory) {
        error = "Choose a data directory.";
        return std::nullopt;
    }
    return resolved;
}

}  // namespace coastal::macos_host
