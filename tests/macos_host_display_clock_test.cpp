// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/display_clock.h"

#include <array>
#include <cassert>
#include <limits>

using coastal::macos_host::DisplayClock;
using coastal::macos_host::PresentedFrame;

int main() {
    DisplayClock clock;
    const auto empty = clock.summarize_sorted({});
    assert(empty.valid_callbacks == 0 && empty.unique_times == 0 && !empty.first_seconds && !empty.average_fps);

    assert(!clock.record(0) && !clock.record(-1));
    assert(!clock.record(std::numeric_limits<double>::quiet_NaN()));
    assert(!clock.record(std::numeric_limits<double>::infinity()));
    assert(clock.record(10));
    const std::array one{PresentedFrame{10}};
    auto sample = clock.summarize_sorted(one);
    assert(sample.invalid_callbacks == 4 && sample.valid_callbacks == 1);
    assert(sample.unique_intervals == 0 && sample.elapsed_seconds == 0 && !sample.average_fps);

    assert(clock.record(10.25) && clock.record(10.25) && clock.record(10.5));
    const std::array retained_tail{PresentedFrame{10.25}, PresentedFrame{10.5}};
    sample = clock.summarize_sorted(retained_tail);
    assert(sample.valid_callbacks == 4 && sample.retained_callbacks == 2 && sample.out_of_order == 0);
    assert(sample.first_seconds == 10 && sample.last_seconds == 10.5);
    assert(sample.unique_times == 3 && sample.unique_intervals == 2 && sample.average_fps == 4);
    assert(sample.adjacent_duplicates == 1 && sample.duplicate_callbacks == 1);

    // The same display time returns after a later callback. A complete sorted
    // history recovers the non-adjacent duplicate; a wrapped history cannot.
    assert(clock.record(10));
    const std::array all{PresentedFrame{10}, PresentedFrame{10}, PresentedFrame{10.25},
                         PresentedFrame{10.25}, PresentedFrame{10.5}};
    sample = clock.summarize_sorted(all);
    assert(sample.out_of_order == 1 && sample.adjacent_duplicates == 1 && sample.duplicate_callbacks == 2);
    assert(sample.valid_callbacks == 5 && sample.retained_callbacks == 5 && sample.average_fps == 4);
    sample = clock.summarize_sorted(retained_tail);
    assert(!sample.unique_times && !sample.unique_intervals && !sample.duplicate_callbacks && !sample.average_fps);
    assert(sample.elapsed_seconds == 0.5 && sample.first_seconds == 10 && sample.last_seconds == 10.5);
}
