// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/data_directory.h"
#include "macos_host/initial_window.h"

#import <AppKit/AppKit.h>

namespace coastal::macos_host {
namespace {

NSString* const data_directory_key = @"CoastalDataDirectory";

void remember_data_directory(const std::filesystem::path& directory) {
    if (!NSBundle.mainBundle.bundleIdentifier.length) return;
    const auto& path = directory.native();
    NSString* value = [[NSString alloc] initWithBytes:path.data()
                                               length:path.size()
                                             encoding:NSUTF8StringEncoding];
    if (value) [NSUserDefaults.standardUserDefaults setObject:value forKey:data_directory_key];
}

}  // namespace

std::optional<std::filesystem::path> choose_data_directory(bool retry, std::string& error) {
    @autoreleasepool {
        error.clear();
        if (![NSThread isMainThread] || interactive_ui_action(true) != InitialWindowAction::Show) {
            error = "The data directory picker requires the active app on its main thread.";
            return std::nullopt;
        }

        @try {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            if (!panel) {
                error = "Unable to open the data directory picker.";
                return std::nullopt;
            }

            panel.canChooseDirectories = YES;
            panel.canChooseFiles = NO;
            panel.allowsMultipleSelection = NO;
            panel.canCreateDirectories = NO;
            panel.title = @"Choose Data Directory";
            panel.prompt = @"Choose";
            panel.message = retry ? @"CoastalHost could not open that data. Choose another directory."
                                  : @"Choose the directory that contains the game data.";

            const NSModalResponse response = [panel runModal];
            if (response == NSModalResponseCancel || response == NSModalResponseAbort) {
                return std::nullopt;
            }
            if (response != NSModalResponseOK) {
                error = "Unable to complete data directory selection.";
                return std::nullopt;
            }

            NSURL* url = panel.URL;
            const char* path = url.isFileURL ? url.fileSystemRepresentation : nullptr;
            if (!path) {
                error = "The selected directory does not have a local filesystem path.";
                return std::nullopt;
            }
            return std::filesystem::path(path);
        } @catch (NSException* exception) {
            (void)exception;
            error = "The data directory picker failed.";
            return std::nullopt;
        }
    }
}

std::optional<std::filesystem::path> load_saved_data_directory() {
    @autoreleasepool {
        NSString* bundle_id = NSBundle.mainBundle.bundleIdentifier;
        if (!bundle_id.length) return std::nullopt;

        id value = [NSUserDefaults.standardUserDefaults persistentDomainForName:bundle_id][data_directory_key];
        if (![value isKindOfClass:NSString.class]) return std::nullopt;
        NSData* bytes = [value dataUsingEncoding:NSUTF8StringEncoding];
        if (!bytes.length) return std::nullopt;

        const std::string_view path_bytes(static_cast<const char*>(bytes.bytes), bytes.length);
        const std::filesystem::path path(path_bytes);
        if (!path.is_absolute()) return std::nullopt;

        std::string error;
        return validate_data_directory(path, error);
    }
}

std::optional<std::filesystem::path> resolve_data_directory(
    const std::filesystem::path& requested, std::string& error) {
    error.clear();
    if (!requested.empty()) return validate_data_directory(requested, error);
    if (auto saved = load_saved_data_directory()) return saved;
    error = "No usable saved game data directory. Choose it in an interactive launch or pass --data-dir.";
    return std::nullopt;
}

DirectorySelection select_data_directory(
    const DirectorySelector& select, const DirectoryValidator& open, std::string& error) {
    error.clear();
    const auto selected = select(error);
    if (!selected) return error.empty() ? DirectorySelection::Cancelled : DirectorySelection::Unavailable;
    const auto directory = validate_data_directory(*selected, error);
    if (!directory || !open(*directory, error)) return DirectorySelection::Rejected;
    remember_data_directory(*directory);
    return DirectorySelection::Accepted;
}

}  // namespace coastal::macos_host
