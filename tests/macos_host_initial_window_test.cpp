// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/initial_window.h"

#include <cstdio>

using coastal::macos_host::InitialWindowAction;
using coastal::macos_host::initial_window_action;

static_assert(initial_window_action(true, false, false) == InitialWindowAction::Wait,
              "An inactive launch waits for application activation.");
static_assert(initial_window_action(true, false, true) == InitialWindowAction::Wait,
              "Workspace identity cannot replace AppKit activation.");
static_assert(initial_window_action(true, true, false) == InitialWindowAction::Retry,
              "Activation can arrive before the workspace identifies the host as frontmost.");
static_assert(initial_window_action(true, true, true) == InitialWindowAction::Show,
              "A user-activated frontmost host can show its first window.");
static_assert(initial_window_action(false, true, true) == InitialWindowAction::Wait,
              "Activation cannot repeat a completed or stopped initial display.");

int main() {
    std::puts("MAC_HOST_INITIAL_WINDOW_PASS inactiveWait=true workspaceRetry=true frontmostShow=true noRepeat=true");
}
