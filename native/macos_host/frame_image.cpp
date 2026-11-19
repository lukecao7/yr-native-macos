// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/frame_image.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <new>

namespace coastal::macos_host {
namespace {

CGDataProviderRef create_rgba_provider(const Frame& frame) {
    if (frame.rgba_owner.use_count() != 0) {
        using Owner = std::shared_ptr<const void>;
        std::unique_ptr<Owner> owner(new (std::nothrow) Owner(frame.rgba_owner));
        if (!owner) return nullptr;
        auto* provider = CGDataProviderCreateWithData(
            owner.get(), frame.rgba.data(), frame.rgba.size(),
            [](void* info, const void*, std::size_t) { delete static_cast<Owner*>(info); });
        if (provider) owner.release();
        return provider;
    }

    auto* data = CFDataCreate(kCFAllocatorDefault, frame.rgba.data(),
                              static_cast<CFIndex>(frame.rgba.size()));
    if (!data) return nullptr;
    auto* provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    return provider;
}

}  // namespace

CGImageRef create_rgba_frame_image(Viewport viewport, const Frame& frame,
                                   CGColorSpaceRef color_space) {
    constexpr std::size_t bytes_per_pixel = 4;
    constexpr auto max_bytes = static_cast<std::size_t>(std::numeric_limits<CFIndex>::max());
    const auto width = static_cast<std::size_t>(viewport.width);
    const auto height = static_cast<std::size_t>(viewport.height);
    if (!color_space || width == 0 || height == 0 || width > max_bytes / bytes_per_pixel) {
        return nullptr;
    }
    const auto row_bytes = width * bytes_per_pixel;
    if (height > max_bytes / row_bytes || frame.rgba.size() != row_bytes * height) {
        return nullptr;
    }
    auto* provider = create_rgba_provider(frame);
    if (!provider) return nullptr;
    auto* image = CGImageCreate(
        width, height, 8, 32, row_bytes, color_space,
        CGBitmapInfo(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrder32Big,
        provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    return image;
}

}  // namespace coastal::macos_host
