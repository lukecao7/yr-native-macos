// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#import <QuartzCore/CAMetalLayer.h>

#include "frame_delivery.h"

namespace coastal::macos_host {

struct MetalMetrics {
    std::uint64_t displayed_frames = 0;
    std::uint64_t displayed_original_frames = 0;
    std::optional<std::uint64_t> last_original_active_objects;
    std::uint64_t displayed_scene_frames = 0;
    std::size_t mode_switches = 0;
    std::optional<Percentiles> display_interval;
    std::optional<Percentiles> scene_interval;
    std::optional<Percentiles> drawable_acquire;
    std::optional<Percentiles> encode_submit;
    std::chrono::nanoseconds max_switch_interval{};
};

}

// AppKit main thread. A returned span is valid until presentFrame: submits it
// or writablePixels is called again. Unavailable keeps the slot writable only
// by its existing frame; retry presentFrame: without calling writablePixels.
// GPU-owned slots are never writable.
@interface CoastalMetalPresenter : NSObject
- (instancetype)initWithViewport:(coastal::macos_host::Viewport)viewport;
@property(nonatomic, readonly) CAMetalLayer* layer;
- (std::span<std::uint8_t>)writablePixels;
- (BOOL)enableScene:(const std::shared_ptr<const coastal::rts::view3d::TerrainMesh>&)terrain;
- (coastal::macos_host::FramePresentation)presentFrame:(const coastal::macos_host::Frame&)frame;
- (coastal::macos_host::MetalMetrics)metrics;
- (void)finish;
@end
