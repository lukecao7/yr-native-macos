#include "macos_host/launch_options.h"

#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

using coastal::macos_host::FixtureReplay;
using coastal::macos_host::LaunchOptions;
using coastal::macos_host::parse_launch_options;
using coastal::macos_host::validate_data_directory;
namespace fs = std::filesystem;

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = (fs::temp_directory_path() / "macos-host-launch-XXXXXX").string();
        const char* directory = mkdtemp(pattern.data());
        if (!directory) {
            throw std::runtime_error("Unable to create the launch options test directory.");
        }
        path = directory;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    fs::path path;
};

std::size_t check_count = 0;

void expect(bool passed, std::string_view description) {
    ++check_count;
    if (!passed) {
        throw std::runtime_error(std::string(description));
    }
}

std::optional<LaunchOptions> parse(std::initializer_list<std::string_view> args,
                                   std::string& error, FixtureReplay replay = FixtureReplay::Disabled) {
    return parse_launch_options({args.begin(), args.size()}, error, replay);
}

void expect_invalid(std::initializer_list<std::string_view> args, std::string_view description) {
    std::string error;
    expect(!parse(args, error) && !error.empty(), description);
}

void test_launch_options() {
    std::string error = "Previous error.";
    const auto defaults = parse({}, error);
    expect(defaults && defaults->data_directory.empty() && defaults->skirmish_map.empty() &&
           !defaults->strict_audit && !defaults->help && error.empty(), "Defaults and error reset");

    const auto split = parse({"--strict-audit", "--data-dir", "Local files/資料",
                              "--skirmish-map", "測試 Grid.MAP"}, error);
    expect(split && split->data_directory == "Local files/資料" &&
           split->skirmish_map == "測試 Grid.MAP" && split->strict_audit && !split->help &&
           error.empty(), "Separated flags and Unicode filenames with spaces");

    const auto attached = parse({"--data-dir=./Local=files", "--skirmish-map=Board 2.YrM"}, error);
    expect(attached && attached->data_directory == "./Local=files" &&
           attached->skirmish_map == "Board 2.YrM" && !attached->strict_audit &&
           error.empty(), "Inline values and case-insensitive map suffix");

    const auto fullscreen = parse({"--fullscreen", "--window-size=960x600"}, error);
    expect(fullscreen && fullscreen->fullscreen == true && fullscreen->window_size &&
           fullscreen->window_size->width == 960 && fullscreen->window_size->height == 600,
           "Full screen can specify its later restored window geometry");
    const auto windowed = parse({"--windowed", "--window-size", "640x480"}, error);
    expect(windowed && windowed->fullscreen == false && windowed->window_size &&
           windowed->window_size->width == 640 && windowed->window_size->height == 480,
           "An explicit window mode and geometry are independent of application defaults");
    expect(!defaults->fullscreen && !defaults->window_size, "Normal launch lets AppKit apply the display preference");
    expect_invalid({"--fullscreen", "--windowed"}, "Conflicting desktop modes fail");
    expect_invalid({"--fullscreen", "--fullscreen"}, "Repeated desktop modes fail");
    expect_invalid({"--window-size", "399x300"}, "Window dimensions support a visible minimum");
    expect_invalid({"--window-size", "800x600x1"}, "Reject malformed window geometry");
    expect_invalid({"--window-size", "18446744073709551616x600"}, "Reject overflowing window geometry");
    expect_invalid({"--stress", "--fullscreen"}, "Stress retains its benchmark window behavior");
    expect_invalid({"--window-size=640x480", "--window-size=800x600"}, "Repeated window dimensions fail");
    const std::string_view replay_display[] = {"--data-dir=fixture", "--script-file=-",
                                               "--script-output=fresh", "--windowed"};
    expect(!parse_launch_options(replay_display, error, coastal::macos_host::FixtureReplay::Enabled) &&
           error == "Window options require an interactive game window.",
           "The windowless fixture replay keeps its own input options");

    const auto help = parse({"--help"}, error);
    expect(help && help->help && error.empty(), "Help flag");
    const auto map_only = parse({"--skirmish-map", "Demo.map"}, error);
    expect(map_only && map_only->data_directory.empty() && map_only->skirmish_map == "Demo.map",
           "Map argument allows the directory picker");

    auto stress = parse({"--stress", "--threads=8", "--strict-audit"}, error);
    expect(stress && stress->stress && stress->threads == 8 && stress->strict_audit, "Stress setup");
    expect(defaults && defaults->threads == 1 && !defaults->stress, "Single-thread default");
    expect_invalid({"--threads=0"}, "Zero workers is invalid");
    expect_invalid({"--threads=16"}, "Unsupported worker count");
    expect_invalid({"--threads=1", "--threads=4"}, "Duplicate worker count");
    expect_invalid({"--stress", "--skirmish-map=Basin.map"}, "Stress fixture has a fixed map");
    expect_invalid({"--stress", "--stress"}, "Duplicate stress flag");
    auto adaptive = parse({"--adaptive-ai", "--threads=4"}, error);
    expect(adaptive && adaptive->adaptive_ai && adaptive->threads == 4, "Adaptive rival");
    expect_invalid({"--adaptive-ai", "--stress"}, "Stress retains its fixed script");
    expect_invalid({"--adaptive-ai", "--adaptive-ai"}, "Duplicate AI flag");
    auto cycling = parse({"--stress", "--cycle-views"}, error);
    expect(cycling && cycling->cycle_views, "Repeatable stress view switching");
    expect_invalid({"--cycle-views"}, "The timed view cycle belongs to the stress scenario");

    expect_invalid({"--data-dir"}, "Missing data directory argument");
    expect_invalid({"--data-dir", "--strict-audit"}, "A flag cannot be taken as the directory");
    expect_invalid({"--data-dir="}, "Empty inline data directory");
    expect_invalid({"--data-dir", ""}, "Empty separated data directory");
    expect_invalid({"--data-dir", std::string_view("data\0other", 10)}, "NUL in data directory");
    expect_invalid({"--data-dir=first", "--data-dir", "second"}, "Duplicate data directory");
    expect_invalid({"--skirmish-map"}, "Missing map filename");
    expect_invalid({"--skirmish-map="}, "Empty inline map filename");
    expect_invalid({"--skirmish-map=Demo.map", "--skirmish-map=Second.yrm"}, "Duplicate map filename");
    expect_invalid({"--skirmish-map=subdir/Demo.map"}, "Subdirectory in map filename");
    expect_invalid({"--skirmish-map=../Demo.map"}, "Parent directory traversal");
    expect_invalid({"--skirmish-map=/Demo.map"}, "Absolute map path");
    expect_invalid({"--skirmish-map=..\\Demo.map"}, "Windows path components");
    expect_invalid({"--skirmish-map=C:Demo.map"}, "Drive-relative map path");
    expect_invalid({"--skirmish-map=demo.txt"}, "Incorrect map suffix");
    expect_invalid({"--skirmish-map=.map"}, "Missing map basename");
    expect_invalid({"--skirmish-map", std::string_view("Demo.map\0other", 14)}, "NUL in map filename");
    expect_invalid({"--strict-audit", "--strict-audit"}, "Duplicate audit flag");
    expect_invalid({"--strict-audit=true"}, "Audit flag has no value");
    expect_invalid({"--help", "--help"}, "Duplicate help flag");
    expect_invalid({"--unknown"}, "Unknown flag");
    expect_invalid({"Demo.map"}, "Unknown positional value");

    expect_invalid({"--data-dir=fixture", "--script-file=-", "--script-output=fresh"},
                   "The normal executable rejects the fixture script flags");
    const auto fixture_defaults = parse({}, error, FixtureReplay::Enabled);
    expect(fixture_defaults && fixture_defaults->script_file.empty() &&
           fixture_defaults->script_output.empty() && error.empty(), "Fixture default startup stays interactive");
    const auto replay = parse({"--data-dir=fixture", "--script-file=-", "--script-output", "fresh"},
                              error, FixtureReplay::Enabled);
    expect(replay && replay->script_file == "-" && replay->script_output == "fresh" && error.empty(),
           "Explicit fixture replay from stdin");
    expect(!parse({"--script-file=commands.jsonl", "--script-output=fresh"}, error, FixtureReplay::Enabled),
           "Missing data directory is rejected before a picker is possible");
    expect(!parse({"--data-dir=fixture", "--script-file=commands.jsonl"}, error, FixtureReplay::Enabled),
           "Replay needs a new output directory");
    expect(!parse({"--data-dir=fixture", "--script-output=fresh"}, error, FixtureReplay::Enabled),
           "Replay needs a script source");
    expect(!parse({"--data-dir=fixture", "--script-file=-", "--script-output=fresh", "--stress"},
                  error, FixtureReplay::Enabled), "Replay rejects stress mode");
}

void test_directories() {
    const TemporaryDirectory temporary;
    const auto data = temporary.path / "資料 with spaces";
    const auto alias = temporary.path / "linked data";
    const auto loose_file = temporary.path / "one file";
    fs::create_directory(data);
    fs::create_directory_symlink(data, alias);
    std::ofstream stream(loose_file);
    stream.close();
    const auto expected = fs::canonical(data);

    std::string error = "Previous error.";
    expect(validate_data_directory(data, error) == expected && error.empty(), "Existing directory");
    expect(validate_data_directory(fs::relative(data), error) == expected && error.empty(),
           "Relative directory is resolved against the working directory");
    expect(validate_data_directory(alias, error) == expected && error.empty(), "Symlinked directory");
    expect(!validate_data_directory(loose_file, error) && !error.empty(), "File is rejected");
    expect(!validate_data_directory(temporary.path / "missing", error) && !error.empty(),
           "Missing directory is rejected");
    expect(!validate_data_directory({}, error) && !error.empty(), "Empty directory path is rejected");

    auto invalid = data.native();
    invalid.push_back(fs::path::value_type{});
    invalid.append(fs::path("ignored").native());
    expect(!validate_data_directory(invalid, error) && !error.empty(),
           "NUL cannot truncate a directory path");
}

}  // namespace

int main() {
    try {
        test_launch_options();
        test_directories();
        std::cout << "{\"all_passed\":true,\"check_count\":" << check_count << "}\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
