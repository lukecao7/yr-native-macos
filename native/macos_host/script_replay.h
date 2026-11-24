// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "session.h"

namespace coastal::macos_host {

// Runs JSON Lines against a synchronous fixture Session. It uses no AppKit
// application or window and advances frames only when the script requests it.
int replay_script(const LaunchOptions& options, Session& session);

}  // namespace coastal::macos_host
