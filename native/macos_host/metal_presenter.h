// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#import <QuartzCore/CAMetalLayer.h>

#include "session.h"

namespace coastal::macos_host {

struct MetalMetrics {
    std::uint64_t displayed_frames = 0;
    std::optional<Percentiles> display_interval;
};

}

// AppKit main thread. A returned span is valid until presentFrame: submits it
// or writablePixels is called again. GPU-owned slots are never writable.
@interface CoastalMetalPresenter : NSObject
- (instancetype)initWithViewport:(coastal::macos_host::Viewport)viewport;
@property(nonatomic, readonly) CAMetalLayer* layer;
- (std::span<std::uint8_t>)writablePixels;
- (BOOL)presentFrame:(const coastal::macos_host::Frame&)frame;
- (coastal::macos_host::MetalMetrics)metrics;
- (void)finish;
@end
