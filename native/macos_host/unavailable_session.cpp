// SPDX-License-Identifier: GPL-3.0-only
#include "session.h"

namespace coastal::macos_host {

std::unique_ptr<Session> create_session(const LaunchOptions&, std::string& error) {
    error = "Attach a compatible session runtime to open a map.";
    return nullptr;
}

}  // namespace coastal::macos_host
