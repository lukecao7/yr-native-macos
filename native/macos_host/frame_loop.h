// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace coastal::macos_host {

using FrameClock = std::chrono::steady_clock;

struct FrameCommit {
    std::uint64_t index;
    std::uint64_t active_objects;
    bool full_original_frame = false;
};

struct Percentiles {
    std::size_t sample_count;
    std::chrono::nanoseconds p50;
    std::chrono::nanoseconds p95;
    std::chrono::nanoseconds p99;
};

struct FrameMetrics {
    std::uint64_t presented_frames = 0;
    std::uint64_t full_original_frames = 0;
    std::optional<std::uint64_t> active_objects;
    std::optional<std::chrono::nanoseconds> first_present;
    std::optional<Percentiles> frame_work;
    std::optional<Percentiles> presentation_interval;
};

enum class FrameTick { NotDue, WaitingForFrame, Presented, Stopped, InvalidFrame };

// Single main-thread owner. A step reports a commit after presentation has
// completed at the host boundary. A pending step reports no commit.
class FrameLoop {
public:
    using Now = std::function<FrameClock::time_point()>;
    using Step = std::function<std::optional<FrameCommit>()>;

    FrameLoop(FrameClock::duration interval, FrameClock::time_point launched_at,
              Now now = FrameClock::now, std::size_t sample_limit = 4096);
    FrameTick tick(const Step& step);
    // Check the next index before showing a frame. Return a rejected commit
    // from Step so tick reports InvalidFrame without counting a presentation.
    bool accepts_frame(const FrameCommit& commit) const noexcept {
        return commit.index == metrics_.presented_frames;
    }
    FrameClock::time_point next_due() const noexcept { return next_due_; }
    void stop() noexcept;
    FrameMetrics metrics() const;

private:
    const FrameClock::duration interval_;
    const FrameClock::time_point launched_at_;
    const std::size_t sample_limit_;
    Now now_;
    FrameClock::time_point next_due_;
    std::optional<FrameClock::time_point> last_present_;
    std::vector<std::chrono::nanoseconds> frame_work_;
    std::vector<std::chrono::nanoseconds> presentation_interval_;
    FrameMetrics metrics_;
    bool stopped_ = false;
    bool in_step_ = false;
};

std::optional<std::uint64_t> process_peak_rss_bytes() noexcept;

}  // namespace coastal::macos_host
