// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/frame_image.h"

#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace coastal::macos_host;

namespace {

constexpr Viewport viewport{3, 2};
using Pixels = std::array<std::uint8_t, viewport.width * viewport.height * 4>;
constexpr Pixels expected{
    255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
    18, 52, 86, 255, 180, 21, 94, 255, 234, 187, 55, 255,
};
using Image = std::unique_ptr<CGImage, decltype(&CGImageRelease)>;
using Context = std::unique_ptr<CGContext, decltype(&CGContextRelease)>;
using ColorSpace = std::unique_ptr<CGColorSpace, decltype(&CGColorSpaceRelease)>;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void check_image(CGImageRef image, CGColorSpaceRef color_space) {
    expect(image != nullptr, "Image creation succeeded");
    expect(CGImageGetWidth(image) == viewport.width &&
               CGImageGetHeight(image) == viewport.height &&
               CGImageGetBitsPerComponent(image) == 8 &&
               CGImageGetBitsPerPixel(image) == 32 &&
               CGImageGetBytesPerRow(image) == viewport.width * 4 &&
               CGImageGetAlphaInfo(image) == kCGImageAlphaNoneSkipLast &&
               (CGImageGetBitmapInfo(image) & kCGBitmapByteOrderMask) == kCGBitmapByteOrder32Big &&
               !CGImageGetShouldInterpolate(image) &&
               CGImageGetRenderingIntent(image) == kCGRenderingIntentDefault &&
               CFEqual(CGImageGetColorSpace(image), color_space),
           "Both paths preserve the presentation format");

    Pixels drawn{};
    Context context(CGBitmapContextCreate(
                        drawn.data(), viewport.width, viewport.height, 8, viewport.width * 4,
                        color_space,
                        CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big),
                    CGContextRelease);
    expect(context != nullptr, "Offscreen image context created");
    CGContextSetBlendMode(context.get(), kCGBlendModeCopy);
    CGContextDrawImage(context.get(), CGRectMake(0, 0, viewport.width, viewport.height), image);
    expect(drawn == expected, "Every offscreen pixel matches its expected row and channel");
}

void copy_transient_pixels(CGColorSpaceRef color_space) {
    auto transient = expected;
    const Image image(create_rgba_frame_image(viewport, Frame{{0, 0}, transient}, color_space),
                      CGImageRelease);
    transient.fill(0);
    check_image(image.get(), color_space);
}

void retain_owned_pixels(CGColorSpaceRef color_space) {
    std::weak_ptr<const Pixels> pixels_lifetime;
    Image image(nullptr, CGImageRelease);
    {
        auto pixels = std::make_shared<const Pixels>(expected);
        pixels_lifetime = pixels;
        const Frame frame{{0, 0}, *pixels, pixels};
        image.reset(create_rgba_frame_image(viewport, frame, color_space));
    }
    expect(!pixels_lifetime.expired(), "Image owns the pixel storage after the producer releases it");
    check_image(image.get(), color_space);
    image.reset();
    expect(pixels_lifetime.expired(), "Releasing the image returns pixel ownership");
}

void reject_invalid_input(CGColorSpaceRef color_space) {
    const auto max_extent = std::numeric_limits<std::uint32_t>::max();
    auto pixels = std::make_shared<const Pixels>(expected);
    const Frame frame{{0, 0}, *pixels, pixels};
    for (const auto invalid : {Viewport{0, 1}, Viewport{1, 0}, Viewport{max_extent, max_extent},
                               Viewport{2, 2}, Viewport{4, 2}}) {
        const Image image(create_rgba_frame_image(invalid, frame, color_space), CGImageRelease);
        expect(!image, "Empty, oversized, and mismatched geometry is rejected");
    }
    const Image image(create_rgba_frame_image(viewport, frame, nullptr), CGImageRelease);
    expect(!image, "Missing color space is rejected");
    expect(pixels.use_count() == 2, "Invalid images do not retain the pixel storage");
}

}  // namespace

int main() {
    try {
        const ColorSpace color_space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB), CGColorSpaceRelease);
        expect(color_space != nullptr, "sRGB color space created");
        copy_transient_pixels(color_space.get());
        retain_owned_pixels(color_space.get());
        reject_invalid_input(color_space.get());
        std::cout << "macos_host_frame_image_test passed (offscreen RGBA, ownership, invalid inputs)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "macos_host_frame_image_test: " << error.what() << '\n';
        return 1;
    }
}
