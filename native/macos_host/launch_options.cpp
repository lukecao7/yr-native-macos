// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/launch_options.h"

#include <algorithm>
#include <charconv>
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
    std::span<const std::string_view> arguments, std::string& error, FixtureReplay replay) {
    error.clear();
    LaunchOptions options;
    bool threads_set = false;

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (argument == "--fullscreen" || argument == "--windowed") {
            if (options.fullscreen.has_value()) {
                error = "Choose one of --fullscreen or --windowed.";
                return std::nullopt;
            }
            options.fullscreen = argument == "--fullscreen";
            continue;
        }
        if (argument.starts_with("--threads=")) {
            const auto value = argument.substr(sizeof("--threads=") - 1);
            if (threads_set || (value != "1" && value != "4" && value != "8")) {
                error = "Specify --threads=1, --threads=4, or --threads=8 once.";
                return std::nullopt;
            }
            options.threads = static_cast<unsigned>(value.front() - '0');
            threads_set = true;
            continue;
        }
        if (argument == "--strict-audit" || argument == "--help" || argument == "--stress" ||
            argument == "--adaptive-ai" || argument == "--cycle-views") {
            bool& enabled = argument == "--strict-audit" ? options.strict_audit :
                            argument == "--help" ? options.help :
                            argument == "--stress" ? options.stress :
                            argument == "--adaptive-ai" ? options.adaptive_ai : options.cycle_views;
            if (enabled) {
                error = "Specify " + std::string(argument) + " once.";
                return std::nullopt;
            }
            enabled = true;
            continue;
        }

        const auto separator = argument.find('=');
        const auto name = argument.substr(0, separator);
        std::filesystem::path* path = nullptr;
        if (name == "--data-dir") path = &options.data_directory;
        if (replay == FixtureReplay::Enabled && name == "--script-file") path = &options.script_file;
        if (replay == FixtureReplay::Enabled && name == "--script-output") path = &options.script_output;
        const bool window_size = name == "--window-size";
        if (!path && !window_size && name != "--skirmish-map") {
            error = "Unrecognized launch argument " + std::string(argument) + ".";
            return std::nullopt;
        }
        const bool repeated = window_size ? options.window_size.has_value() :
            path ? !path->empty() : !options.skirmish_map.empty();
        if (repeated) {
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

        if (window_size) {
            const auto split = value.find('x');
            WindowSize size{};
            const auto dimension = [](std::string_view field, unsigned minimum, unsigned& number) {
                const auto result = std::from_chars(field.data(), field.data() + field.size(), number);
                return result.ec == std::errc{} && result.ptr == field.data() + field.size()
                    && number >= minimum && number <= 16384;
            };
            if (split == value.npos || !dimension(value.substr(0, split), 400, size.width) ||
                !dimension(value.substr(split + 1), 300, size.height)) {
                error = "Use --window-size WIDTHxHEIGHT in points (at least 400x300).";
                return std::nullopt;
            }
            options.window_size = size;
        } else if (path) {
            *path = std::filesystem::path(value);
        } else {
            if (!valid_map_filename(value)) {
                error = "--skirmish-map requires a single .map or .yrm filename.";
                return std::nullopt;
            }
            options.skirmish_map = value;
        }
    }
    if (options.script_file.empty() != options.script_output.empty()) {
        error = "Specify --script-file and --script-output together.";
        return std::nullopt;
    }
    if (!options.script_file.empty() && (options.data_directory.empty() || options.stress)) {
        error = "Fixture replay requires --data-dir and is unavailable in stress mode.";
        return std::nullopt;
    }
    if (options.stress && !options.skirmish_map.empty()) {
        error = "--stress uses its own map.";
        return std::nullopt;
    }
    if (options.stress && options.adaptive_ai) {
        error = "--adaptive-ai is for skirmishes.";
        return std::nullopt;
    }
    if (options.cycle_views && !options.stress) {
        error = "--cycle-views requires --stress.";
        return std::nullopt;
    }
    if ((options.fullscreen.has_value() || options.window_size) &&
        (options.stress || !options.script_file.empty())) {
        error = "Window options require an interactive game window.";
        return std::nullopt;
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
