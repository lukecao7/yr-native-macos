// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/data_directory.h"
#include "macos_host/initial_window.h"

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;
namespace host = coastal::macos_host;

namespace {

NSString* const data_directory_key = @"CoastalDataDirectory";
std::size_t check_count = 0;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto pattern = (fs::temp_directory_path() / "macos-host-data-XXXXXX").string();
        const char* directory = mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("Unable to create the directory settings fixture.");
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

struct TestPreferences {
    NSUserDefaults* const defaults = NSUserDefaults.standardUserDefaults;
    NSString* const domain;

    ~TestPreferences() {
        [defaults removePersistentDomainForName:domain];
        CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
    }
};

void expect(bool passed, std::string_view description) {
    ++check_count;
    if (!passed) throw std::runtime_error(std::string(description));
}

NSString* preference_value(const std::string& path) {
    return [[NSString alloc] initWithBytes:path.data() length:path.size() encoding:NSUTF8StringEncoding];
}

void test_saved_directory(NSUserDefaults* defaults) {
    const TemporaryDirectory temporary;
    const auto data = temporary.path / "本机资料 with spaces";
    const auto symlink = temporary.path / "Selected link";
    const auto file = temporary.path / "not a directory";
    fs::create_directory(data);
    fs::create_directory_symlink(data, symlink);
    std::ofstream(file).close();
    const auto canonical = fs::canonical(data);

    expect(!host::load_saved_data_directory(), "Absent setting is ignored");
    [defaults registerDefaults:@{data_directory_key: preference_value(data.native())}];
    expect(!host::load_saved_data_directory(), "Volatile registration is not a picker choice");

    [defaults setObject:@42 forKey:data_directory_key];
    expect(!host::load_saved_data_directory(), "A wrong preference type is ignored");
    [defaults setObject:@"" forKey:data_directory_key];
    expect(!host::load_saved_data_directory(), "An empty stored path is ignored");
    [defaults setObject:@"." forKey:data_directory_key];
    expect(!host::load_saved_data_directory(), "A relative stored path is ignored");
    [defaults setObject:preference_value((temporary.path / "moved").native()) forKey:data_directory_key];
    expect(!host::load_saved_data_directory(), "A moved directory is ignored");
    [defaults setObject:preference_value(file.native()) forKey:data_directory_key];
    expect(!host::load_saved_data_directory(), "A regular file is ignored");

    auto malformed = data.native();
    malformed.push_back('\0');
    malformed += "suffix";
    [defaults setObject:preference_value(malformed) forKey:data_directory_key];
    expect(!host::load_saved_data_directory(), "A NUL cannot truncate a stored path");
    [defaults setObject:preference_value(data.native()) forKey:data_directory_key];
    expect(host::load_saved_data_directory() == canonical, "A Unicode directory with spaces is loaded");
    [defaults setObject:preference_value(symlink.native()) forKey:data_directory_key];
    expect(host::load_saved_data_directory() == canonical, "A stored symlink resolves to its directory");

    std::string error;
    expect(!host::choose_data_directory(false, error) && !error.empty() && !NSApp &&
               host::load_saved_data_directory() == canonical,
           "An uninitialized picker leaves the saved directory untouched");
    expect(host::interactive_ui_action(true) == host::InitialWindowAction::Wait && !NSApp,
           "A background UI check does not initialize AppKit");
}

void launch_fixture(NSString* mode, const fs::path& data) {
    NSTask* task = [NSTask new];
    task.executableURL = NSBundle.mainBundle.executableURL;
    task.arguments = @[mode, preference_value(data.native())];
    NSError* error = nil;
    if (![task launchAndReturnError:&error]) {
        throw std::runtime_error(error.localizedDescription.UTF8String);
    }
    [task waitUntilExit];
    expect(task.terminationReason == NSTaskTerminationReasonExit && task.terminationStatus == 0,
           std::string("The separate directory test process failed: ") + mode.UTF8String);
}

bool open_fixture_data(const fs::path& path, std::string& error) {
    std::ifstream rules(path / "fixture.rules");
    std::string header;
    if (std::getline(rules, header) && header == "CoastalHost directory fixture") return true;
    error = "The directory fixture rules could not be opened.";
    return false;
}

void test_picker_round_trip() {
    const TemporaryDirectory temporary;
    const auto data = temporary.path / "新资料 Workspace";
    const auto invalid = temporary.path / "Empty media";
    const auto link = temporary.path / "Picked data";
    fs::create_directory(data);
    fs::create_directory(invalid);
    fs::create_directory_symlink(data, link);
    std::ofstream(data / "fixture.rules") << "CoastalHost directory fixture\n";

    std::string error = "stale error";
    bool tried_opening = false;
    const host::DirectoryValidator track_open = [&](const auto&, auto&) {
        tried_opening = true;
        return true;
    };
    expect(host::select_data_directory([](auto&) { return std::nullopt; }, track_open, error) ==
               host::DirectorySelection::Cancelled && error.empty() && !tried_opening,
           "Canceling the first selection does not open any resources");

    const auto missing = temporary.path / "missing";
    expect(host::select_data_directory([&](auto&) { return missing; }, track_open, error) ==
               host::DirectorySelection::Rejected && !error.empty() && !tried_opening &&
               !host::load_saved_data_directory(),
           "A nonexistent first selection is rejected before opening resources");
    expect(host::select_data_directory([&](auto&) { return invalid; }, open_fixture_data, error) ==
               host::DirectorySelection::Rejected && !error.empty() && !host::load_saved_data_directory(),
           "An existing data directory with missing rules is rejected without saving");
    expect(!host::resolve_data_directory({}, error) && !error.empty() && !NSApp,
           "Looking up an absent saved choice does not open a picker");

    launch_fixture(@"--select", link);
    launch_fixture(@"--relaunch", data);
    expect(host::select_data_directory([&](auto&) { return invalid; }, open_fixture_data, error) ==
               host::DirectorySelection::Rejected && host::load_saved_data_directory() == fs::canonical(data),
           "A rejected replacement preserves the last working directory");
}

void select_first_directory(const fs::path& link) {
    std::string error;
    int selections = 0;
    int validations = 0;
    expect(!host::load_saved_data_directory(), "The first launch has no saved preference");
    const auto result = host::select_data_directory([&](auto&) {
        ++selections;
        return link;
    }, [&](const auto& path, auto& detail) {
        ++validations;
        expect(!host::load_saved_data_directory(), "The directory is uncommitted while opening resources");
        return path == fs::canonical(link) && open_fixture_data(path, detail);
    }, error);
    expect(result == host::DirectorySelection::Accepted && error.empty() && selections == 1 && validations == 1,
           "The first launch opens resources from the canonical choice once");
    expect(host::load_saved_data_directory() == fs::canonical(link), "The opened directory is saved");
    // The test binary is a command-line process that exits immediately.
    expect(CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication),
           "The first process could not flush its preferences");
}

void restore_on_relaunch(const fs::path& expected) {
    std::string error;
    const auto options = host::parse_launch_options({}, error);
    expect(options.has_value() && options->data_directory.empty(), "No data argument on relaunch");
    expect(host::resolve_data_directory(options->data_directory, error) ==
               fs::canonical(expected) && error.empty() && !NSApp,
           "An argument-free relaunch reads the preference and skips the picker");

    const auto override_directory = fs::canonical(expected.parent_path());
    expect(host::resolve_data_directory(override_directory, error) ==
               override_directory && error.empty() && !NSApp,
           "A one-run override takes priority over the preference");
    expect(!host::resolve_data_directory(expected / "missing", error) &&
               !error.empty() && !NSApp,
           "An invalid explicit directory is rejected without a picker fallback");
    expect(host::load_saved_data_directory() == fs::canonical(expected),
           "An override leaves the picker preference intact");
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            NSString* domain = NSBundle.mainBundle.bundleIdentifier;
            if (![domain hasPrefix:@"org.coastalcommand.host-data-directory-test."]) {
                throw std::runtime_error("Run this test with an isolated test bundle identifier.");
            }
            if (argc == 3) {
                const std::string_view mode = argv[1];
                if (mode == "--select") select_first_directory(argv[2]);
                else if (mode == "--relaunch") restore_on_relaunch(argv[2]);
                else throw std::runtime_error("Unknown directory fixture mode.");
                return 0;
            }
            if (argc != 1) throw std::runtime_error("Unexpected directory fixture arguments.");
            if ([NSUserDefaults.standardUserDefaults persistentDomainForName:domain].count) {
                throw std::runtime_error("The test preferences domain is already in use.");
            }
            const TestPreferences preferences{.domain = domain};
            test_saved_directory(preferences.defaults);
            [preferences.defaults removePersistentDomainForName:domain];
            expect(CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication),
                   "The existing settings test could not clear preferences");
            test_picker_round_trip();
            std::cout << "{\"all_passed\":true,\"check_count\":" << check_count << "}\n";
            return 0;
        } catch (const std::exception& failure) {
            std::cerr << failure.what() << '\n';
            return 1;
        }
    }
}
