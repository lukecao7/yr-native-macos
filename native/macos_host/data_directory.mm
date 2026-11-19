// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/launch_options.h"

#import <AppKit/AppKit.h>

namespace coastal::macos_host {

std::optional<std::filesystem::path> choose_data_directory(std::string& error) {
    error.clear();
    @autoreleasepool {
        if (![NSThread isMainThread] || !NSApp) {
            error = "The data directory picker requires an AppKit application on its main thread.";
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

            const NSModalResponse response = [panel runModal];
            if (response == NSModalResponseCancel) {
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
            return validate_data_directory(std::filesystem::path(path), error);
        } @catch (NSException* exception) {
            (void)exception;
            error = "The data directory picker failed.";
            return std::nullopt;
        }
    }
}

}  // namespace coastal::macos_host
