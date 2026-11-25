// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "macos_host/frame_loop.h"
#include <algorithm>
#include <optional>

namespace coastal::macos_host {

// The ready FIFO already carries future pictures. Assign each picture an
// ordered scroll sample from the display cadence, independently of how long
// its preceding render or queue wait took. Input timestamps use FrameClock.
class DisplayCameraClock {
public:
    void observe(FrameClock::time_point pulse) noexcept {
        if(!latest_||pulse>*latest_)latest_=pulse;
    }
    void reset() noexcept { origin_.reset(); planned_.reset(); }

    std::optional<FrameClock::time_point> sample(FrameClock::time_point input_cutoff,
            FrameClock::duration interval,std::size_t ready_limit) noexcept {
        if(!latest_)return std::nullopt;
        if(!origin_) {
            // Spare one interval for each packet already allowed to queue
            // ahead of this sample. Never read a time beyond accepted input.
            const auto queued_ahead=ready_limit ? ready_limit-1 : 0;
            const auto lead=interval*static_cast<FrameClock::duration::rep>(queued_ahead);
            origin_=input_cutoff-lead;
            planned_=anchor_=*latest_;
        }else{
            planned_=std::max(*latest_,*planned_+interval);
        }
        return std::min(input_cutoff,*origin_+(*planned_-anchor_));
    }
private:
    std::optional<FrameClock::time_point> latest_,origin_,planned_;
    FrameClock::time_point anchor_{};
};

}  // namespace coastal::macos_host
