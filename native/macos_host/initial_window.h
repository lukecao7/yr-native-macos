// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace coastal::macos_host {

enum class InitialWindowAction { Wait, Retry, Show };

constexpr InitialWindowAction initial_window_action(bool waiting, bool active, bool frontmost) noexcept {
    if (!waiting || !active) return InitialWindowAction::Wait;
    return frontmost ? InitialWindowAction::Show : InitialWindowAction::Retry;
}

}  // namespace coastal::macos_host
