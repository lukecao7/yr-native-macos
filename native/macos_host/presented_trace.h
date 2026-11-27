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

struct DrawableTraceRow {
    std::uint64_t commit = 0, drawable_id = 0;
    double request_begin = 0, request_return = 0;
    double commit_begin = 0, commit_return = 0;
    double scheduled_callback = 0, gpu_callback = 0;
    double gpu_start = 0, gpu_end = 0;
    double presented_at = 0, presented_callback = 0;
    bool gpu_completed = false;
};

// The rings and counters must come from one locked history snapshot. Entries
// occupy physical ring slots, with the most recent at (valid_callbacks-1)%cap.
bool write_presented_trace(const char* new_absolute_path, std::span<const PresentedFrame> ring,
                           const DisplayClockMetrics& metrics,
                           std::span<const DrawableTraceRow> drawables = {},
                           std::uint64_t total_drawables = 0);

} // namespace coastal::macos_host
