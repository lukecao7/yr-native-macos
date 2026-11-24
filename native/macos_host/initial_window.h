// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#endif

namespace coastal::macos_host {

enum class InitialWindowAction { Wait, Retry, Show };

constexpr InitialWindowAction initial_window_action(bool waiting, bool active, bool frontmost) noexcept {
    if (!waiting || !active) return InitialWindowAction::Wait;
    return frontmost ? InitialWindowAction::Show : InitialWindowAction::Retry;
}

#ifdef __OBJC__
inline InitialWindowAction interactive_ui_action(bool waiting) {
    const bool active = waiting && NSApp.isActive;
    const bool frontmost = active &&
        NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier ==
            NSProcessInfo.processInfo.processIdentifier;
    return initial_window_action(waiting, active, frontmost);
}
#endif

}  // namespace coastal::macos_host
