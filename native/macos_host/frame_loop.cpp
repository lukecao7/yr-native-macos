// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/frame_loop.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#if defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace coastal::macos_host {
namespace {

using Sample = std::chrono::nanoseconds;

void record_sample(std::vector<Sample>& samples, Sample sample, std::uint64_t index,
                   std::size_t limit) {
    if (limit == 0) {
        return;
    }
    if (samples.size() < limit) {
        samples.push_back(sample);
    } else {
        samples[static_cast<std::size_t>(index % limit)] = sample;
    }
}

std::optional<Percentiles> summarize(std::vector<Sample> samples) {
    if (samples.empty()) {
        return std::nullopt;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t count = samples.size();
    const auto nearest_rank = [&](std::size_t percent) {
        const std::size_t rank = (count / 100) * percent +
                                 ((count % 100) * percent + 99) / 100;
        return samples[rank - 1];
    };
    return Percentiles{count, nearest_rank(50), nearest_rank(95), nearest_rank(99)};
}

}  // namespace

FrameLoop::FrameLoop(FrameClock::duration interval, FrameClock::time_point launched_at,
                     Now now, std::size_t sample_limit)
    : interval_(interval),
      launched_at_(launched_at),
      sample_limit_(sample_limit),
      now_(std::move(now)),
      next_due_(launched_at) {
    if (interval_ <= FrameClock::duration::zero() || !now_) {
        throw std::invalid_argument("FrameLoop requires a positive interval and a clock");
    }
    frame_work_.reserve(sample_limit_);
    presentation_interval_.reserve(sample_limit_);
}

FrameTick FrameLoop::tick(const Step& step) {
    if (stopped_) {
        return FrameTick::Stopped;
    }
    if (in_step_) {
        return FrameTick::NotDue;
    }
    const auto started = now_();
    if (started < next_due_) {
        return FrameTick::NotDue;
    }

    in_step_ = true;
    std::optional<FrameCommit> commit;
    FrameClock::time_point finished;
    try {
        commit = step();
        finished = now_();
    } catch (...) {
        in_step_ = false;
        metrics_.full_original_frames = 0;
        stop();
        throw;
    }
    in_step_ = false;
    if (stopped_) {
        return FrameTick::Stopped;
    }

    if (finished < started || (commit && commit->index != metrics_.presented_frames)) {
        metrics_.full_original_frames = 0;
        stop();
        return FrameTick::InvalidFrame;
    }

    // Keep the cadence anchored and skip every elapsed slot.
    const auto until_next = interval_ - (finished - next_due_) % interval_;
    next_due_ = finished + until_next;
    if (!commit) {
        return FrameTick::WaitingForFrame;
    }

    const auto frame_index = metrics_.presented_frames;
    if (last_present_) {
        record_sample(presentation_interval_,
                      std::chrono::duration_cast<Sample>(finished - *last_present_),
                      frame_index - 1, sample_limit_);
    } else {
        metrics_.first_present = std::chrono::duration_cast<Sample>(finished - launched_at_);
    }
    record_sample(frame_work_, std::chrono::duration_cast<Sample>(finished - started),
                  frame_index, sample_limit_);
    last_present_ = finished;

    // The live count describes a sequence of complete frames from index zero.
    if (commit->full_original_frame && metrics_.full_original_frames == frame_index) {
        ++metrics_.full_original_frames;
    } else {
        metrics_.full_original_frames = 0;
    }
    ++metrics_.presented_frames;
    metrics_.active_objects = commit->active_objects;
    return FrameTick::Presented;
}

void FrameLoop::stop() noexcept {
    stopped_ = true;
}

FrameMetrics FrameLoop::metrics() const {
    auto result = metrics_;
    result.frame_work = summarize(frame_work_);
    result.presentation_interval = summarize(presentation_interval_);
    return result;
}

std::optional<std::uint64_t> process_peak_rss_bytes() noexcept {
#if defined(__APPLE__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0) {
        return static_cast<std::uint64_t>(usage.ru_maxrss);
    }
#endif
    return std::nullopt;
}

}  // namespace coastal::macos_host
