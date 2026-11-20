// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "session.h"

#include <functional>

namespace coastal::macos_host {

// Presented means a real backend submission; Metal scan-out remains measured
// by the drawable's presented handler.
enum class FramePresentation { Presented, Unavailable, Invalid };

// Main-thread delivery of one ready frame. A temporarily unavailable surface
// must not advance the session or acquire another writable pixel slot.
class FrameDelivery {
public:
    using Advance = std::function<std::optional<Frame>()>;
    using Present = std::function<FramePresentation(const Frame&)>;

    std::optional<FrameCommit> step(const FrameLoop&, const Advance&, const Present&);
    void clear() noexcept;

private:
    std::optional<Frame> pending_;
    std::shared_ptr<const rts::view3d::SceneFrame> scene_snapshot_;
};

}  // namespace coastal::macos_host
