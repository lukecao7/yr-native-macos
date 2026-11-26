// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coastal::macos_host {

// One presentation attempt. A retried raster has a new confirmation object
// so a callback retained by an old drawable cannot affect the new submission.
class DisplayConfirmation {
public:
    enum class Poll { Waiting, Displayed, Retry, WrongFrame };

    explicit DisplayConfirmation(std::uint64_t frame_index) noexcept : frame_index_(frame_index) {}

    void complete(double presented_time) noexcept {
        auto expected = State::Pending;
        const auto next = std::isfinite(presented_time) && presented_time > 0 ? State::Displayed : State::Skipped;
        state_.compare_exchange_strong(expected, next, std::memory_order_release, std::memory_order_relaxed);
    }

    Poll poll(std::uint64_t frame_index, bool gpu_retired) const noexcept {
        if (frame_index != frame_index_) return Poll::WrongFrame;
        const auto state = state_.load(std::memory_order_acquire);
        if (state == State::Displayed) return Poll::Displayed;
        if (state == State::Skipped && gpu_retired) return Poll::Retry;
        return Poll::Waiting;
    }

private:
    enum class State { Pending, Displayed, Skipped };
    const std::uint64_t frame_index_;
    std::atomic<State> state_{State::Pending};
};

}  // namespace coastal::macos_host
