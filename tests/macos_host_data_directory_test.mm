// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/launch_options.h"

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

    ~TestPreferences() { [defaults removePersistentDomainForName:domain]; }
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
    expect(!host::choose_data_directory(error) && !error.empty() &&
               host::load_saved_data_directory() == canonical,
           "A picker failure leaves the saved directory available");
}

}  // namespace

int main() {
    @autoreleasepool {
        try {
            NSString* domain = NSBundle.mainBundle.bundleIdentifier;
            if (![domain hasPrefix:@"org.coastalcommand.host-data-directory-test."]) {
                throw std::runtime_error("Run this test with an isolated test bundle identifier.");
            }
            if ([NSUserDefaults.standardUserDefaults persistentDomainForName:domain].count) {
                throw std::runtime_error("The test preferences domain is already in use.");
            }
            const TestPreferences preferences{.domain = domain};
            test_saved_directory(preferences.defaults);
            std::cout << "{\"all_passed\":true,\"check_count\":" << check_count << "}\n";
            return 0;
        } catch (const std::exception& failure) {
            std::cerr << failure.what() << '\n';
            return 1;
        }
    }
}
