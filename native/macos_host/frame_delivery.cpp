// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/frame_delivery.h"
#include "rts_3d/scene.h"

#include <stdexcept>

namespace coastal::macos_host {

std::optional<FrameCommit> FrameDelivery::step(const FrameLoop& loop,
                                             const Advance& advance, const Present& present) {
    if (!pending_) pending_ = advance();
    if (!pending_) return std::nullopt;
    if (!loop.accepts_frame(pending_->commit)) return pending_->commit;

    const auto result = present(*pending_);
    if (result == FramePresentation::Unavailable) {
        // Input or loading may update a session's borrowed scene between timer
        // callbacks. Snapshot it only on a retry; the usual path has no copy.
        if (pending_->scene && !scene_snapshot_) {
            scene_snapshot_ = std::make_shared<rts::view3d::SceneFrame>(*pending_->scene);
            pending_->scene = scene_snapshot_.get();
        }
        return std::nullopt;
    }
    if (result != FramePresentation::Presented) {
        throw std::runtime_error("Could not present a session frame.");
    }
    const auto commit = pending_->commit;
    clear();
    return commit;
}

void FrameDelivery::clear() noexcept {
    pending_.reset();
    scene_snapshot_.reset();
}

}  // namespace coastal::macos_host
