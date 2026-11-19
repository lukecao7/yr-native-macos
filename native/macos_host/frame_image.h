// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "session.h"

#include <CoreGraphics/CoreGraphics.h>

namespace coastal::macos_host {

// Returns an owned image or nullptr. Release the image with CGImageRelease.
CGImageRef create_rgba_frame_image(Viewport viewport, const Frame& frame,
                                   CGColorSpaceRef color_space);

}  // namespace coastal::macos_host
