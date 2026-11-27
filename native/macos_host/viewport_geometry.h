// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "session.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace coastal::macos_host {

struct ViewportRect {
    double x, y, width, height;
};

struct WindowContentSize {
    double width, height;
};

inline WindowContentSize initial_window_content_size(WindowContentSize natural,
                                                     WindowContentSize available,
                                                     std::optional<WindowSize> requested) noexcept {
    if (requested) {
        return {std::floor(std::min(double(requested->width), available.width)),
                std::floor(std::min(double(requested->height), available.height))};
    }
    const auto fit = std::min({1.0, available.width / natural.width, available.height / natural.height});
    return {std::floor(natural.width * fit), std::floor(natural.height * fit)};
}

// Work in backing pixels so the image starts and ends on pixel boundaries.
// Reduce the aspect ratio to keep both dimensions integral at any display scale.
inline ViewportRect fit_viewport(ViewportRect bounds, Viewport frame) noexcept {
    if (!frame.width || !frame.height || bounds.width <= 0 || bounds.height <= 0) return {};
    const auto divisor = std::gcd(frame.width, frame.height);
    const auto width = frame.width / divisor;
    const auto height = frame.height / divisor;
    const auto scale = std::min(bounds.width / width, bounds.height / height);
    if (scale < 1) return {bounds.x + (bounds.width - width * scale) / 2,
                           bounds.y + (bounds.height - height * scale) / 2,
                           width * scale, height * scale};
    const auto fitted_width = std::floor(scale) * width;
    const auto fitted_height = std::floor(scale) * height;
    return {std::floor(bounds.x + (bounds.width - fitted_width) / 2),
            std::floor(bounds.y + (bounds.height - fitted_height) / 2),
            fitted_width, fitted_height};
}

} // namespace coastal::macos_host
