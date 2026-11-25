// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace coastal::macos_host {

struct PresentedFrame {
    double time = 0;
    bool scene = false;
};

struct DisplayClockMetrics {
    std::uint64_t valid_callbacks = 0, invalid_callbacks = 0;
    std::uint64_t out_of_order = 0, adjacent_duplicates = 0;
    std::size_t retained_callbacks = 0;
    std::optional<double> first_seconds, last_seconds, elapsed_seconds, average_fps;
    std::optional<std::uint64_t> unique_times, duplicate_callbacks, unique_intervals;
};

// The presenter serializes callbacks and copies this small value with its ring.
class DisplayClock {
public:
    bool record(double seconds) {
        if (!(seconds > 0) || !std::isfinite(seconds)) {
            ++invalid_;
            return false;
        }
        if (valid_) {
            if (seconds < previous_) ++out_of_order_;
            if (seconds == previous_) ++adjacent_duplicates_;
            first_ = std::min(first_, seconds);
            last_ = std::max(last_, seconds);
        } else {
            first_ = last_ = seconds;
        }
        previous_ = seconds;
        ++valid_;
        return true;
    }

    std::uint64_t valid_count() const { return valid_; }

    // A monotonic stream has only adjacent duplicates, even after the ring
    // wraps. An unordered stream needs every timestamp to count distinct times.
    DisplayClockMetrics summarize_sorted(std::span<const PresentedFrame> retained) const {
        DisplayClockMetrics result;
        result.valid_callbacks = valid_;
        result.invalid_callbacks = invalid_;
        result.out_of_order = out_of_order_;
        result.adjacent_duplicates = adjacent_duplicates_;
        result.retained_callbacks = retained.size();
        if (valid_) {
            result.first_seconds = first_;
            result.last_seconds = last_;
            result.elapsed_seconds = last_ - first_;
        }
        if (!out_of_order_) {
            result.unique_times = valid_ - adjacent_duplicates_;
        } else if (retained.size() == valid_) {
            std::uint64_t unique = 1;
            for (std::size_t i = 1; i < retained.size(); ++i) {
                if (retained[i].time != retained[i - 1].time) ++unique;
            }
            result.unique_times = unique;
        }
        if (result.unique_times) {
            const auto unique = *result.unique_times;
            result.duplicate_callbacks = valid_ - unique;
            result.unique_intervals = unique > 0 ? unique - 1 : 0;
            if (unique > 1 && *result.elapsed_seconds > 0) {
                const double rate = static_cast<double>(unique - 1) / *result.elapsed_seconds;
                if (std::isfinite(rate)) result.average_fps = rate;
            }
        }
        return result;
    }

private:
    std::uint64_t valid_ = 0, invalid_ = 0, out_of_order_ = 0, adjacent_duplicates_ = 0;
    double first_ = 0, last_ = 0, previous_ = 0;
};

} // namespace coastal::macos_host
