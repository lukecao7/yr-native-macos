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

// Sample at the point of frame production so unavailable draws and skipped
// display pulses are included in the next interval.
class FrameElapsedTime {
public:
    std::chrono::nanoseconds sample(FrameClock::time_point now = FrameClock::now()) noexcept {
        const auto elapsed = now - previous_.value_or(now);
        previous_ = now;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    }
    void reset() noexcept { previous_.reset(); }
private:
    std::optional<FrameClock::time_point> previous_;
};

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
    // Complete original commits across menu/match transitions. Invalid input
    // still invalidates this count; diagnostic frames never contribute to it.
    std::uint64_t full_original_frames = 0;
    std::optional<std::uint64_t> active_objects;
    std::optional<std::uint64_t> last_original_active_objects;
    std::optional<std::chrono::nanoseconds> first_present;
    std::optional<Percentiles> frame_work;
    std::optional<Percentiles> presentation_interval;
};

enum class FrameTick { NotDue, WaitingForFrame, Presented, Stopped, InvalidFrame };

// Called once per display refresh by its single callback owner. Fractional
// refresh ratios preserve the session rate on, for example, a 144 Hz display.
class DisplayCadence {
public:
    explicit DisplayCadence(FrameClock::duration interval);
    bool accepts_refresh(FrameClock::duration refresh_period) noexcept;

private:
    const FrameClock::duration interval_;
    double phase_ = 0;
    bool started_ = false;
};

// Single main-thread owner. A step reports a commit after presentation has
// completed at the host boundary. A pending step reports no commit.
class FrameLoop {
public:
    using Now = std::function<FrameClock::time_point()>;
    using Step = std::function<std::optional<FrameCommit>()>;

    FrameLoop(FrameClock::duration interval, FrameClock::time_point launched_at,
              Now now = FrameClock::now, std::size_t sample_limit = 4096);
    FrameTick tick(const Step& step);
    // A display-driven host already has a cadence. Do not gate its pulses on a
    // second launch-relative timer. An empty attempt may retry the same pulse
    // once; each increasing pulse permits at most one real, sequential commit.
    FrameTick display_tick(std::uint64_t pulse, const Step& step);
    bool can_retry_display(std::uint64_t pulse) const noexcept {
        return !stopped_ && !in_step_ && display_pulse_ && *display_pulse_ == pulse &&
               !display_pulse_committed_ && !display_pulse_retried_;
    }
    // Check the next index before showing a frame. Return a rejected commit
    // from Step so tick reports InvalidFrame without counting a presentation.
    bool accepts_frame(const FrameCommit& commit) const noexcept {
        return commit.index == metrics_.presented_frames;
    }
    FrameClock::time_point next_due() const noexcept { return next_due_; }
    void stop() noexcept;
    FrameMetrics metrics() const;

private:
    FrameTick run_step(const Step& step, bool clock_gated);

    const FrameClock::duration interval_;
    const FrameClock::time_point launched_at_;
    const std::size_t sample_limit_;
    Now now_;
    FrameClock::time_point next_due_;
    std::optional<FrameClock::time_point> last_present_;
    std::vector<std::chrono::nanoseconds> frame_work_;
    std::vector<std::chrono::nanoseconds> presentation_interval_;
    FrameMetrics metrics_;
    std::optional<std::uint64_t> display_pulse_;
    bool display_pulse_committed_ = false;
    bool display_pulse_retried_ = false;
    bool stopped_ = false;
    bool in_step_ = false;
};

std::optional<std::uint64_t> process_peak_rss_bytes() noexcept;

}  // namespace coastal::macos_host
