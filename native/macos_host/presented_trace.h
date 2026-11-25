// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "display_clock.h"
#include <string>
#include <utility>

namespace coastal::macos_host {

inline constexpr std::size_t presentation_history_capacity = 32'769;

// Intermediate metric queries leave the request pending. A final snapshot
// consumes it once, including when exclusive file creation subsequently fails.
inline std::string take_exit_trace_path(bool finished, std::string& requested) {
    return finished ? std::exchange(requested, std::string{}) : std::string{};
}

// The ring and counters must come from one locked history snapshot. Entries
// occupy physical ring slots, with the most recent at (valid_callbacks-1)%cap.
bool write_presented_trace(const char* new_absolute_path, std::span<const PresentedFrame> ring,
                           const DisplayClockMetrics& metrics);

} // namespace coastal::macos_host
