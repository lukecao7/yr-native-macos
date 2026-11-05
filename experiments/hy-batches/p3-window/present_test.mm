#include "present.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
constexpr int width = 320;
constexpr int height = 200;
constexpr int indexedStride = width + 17;
constexpr int rgb565Stride = width * 2 + 19; // Deliberately odd and unaligned.
using Pixel = std::array<uint8_t, 4>;

// Fixed oracles, including mixed channel values; no production conversion
// function/formula is reused to compute expected pixels.
constexpr std::array<std::array<uint8_t, 3>, 10> palette6 = {{
    {63, 31, 17}, {63, 0, 0}, {0, 63, 0}, {0, 0, 63}, {63, 63, 63},
    {63, 63, 0}, {0, 63, 63}, {63, 0, 63}, {1, 17, 32}, {31, 47, 63}
}};
constexpr std::array<Pixel, 10> indexedExpected = {{
    {0, 0, 0, 0}, {252, 0, 0, 255}, {0, 252, 0, 255}, {0, 0, 252, 255},
    {252, 252, 252, 255}, {252, 252, 0, 255}, {0, 252, 252, 255},
    {252, 0, 252, 255}, {4, 68, 128, 255}, {124, 188, 252, 255}
}};
constexpr std::array<uint16_t, 10> rgb565Values = {
    0xf800, 0x07e0, 0x001f, 0xffff, 0x0000,
    0xffe0, 0x07ff, 0xf81f, 0x8410, 0x1234
};
constexpr std::array<Pixel, 10> rgb565Expected = {{
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
    {255, 255, 255, 255}, {0, 0, 0, 255}, {255, 255, 0, 255},
    {0, 255, 255, 255}, {255, 0, 255, 255},
    {132, 130, 132, 255}, {16, 69, 165, 255}
}};

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int tile(int x, int y) { return (x / 32 + y / 20) % 10; }

struct DecodedPNG {
    std::size_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    Pixel at(int x, int y) const {
        const auto offset = (static_cast<std::size_t>(y) * w + x) * 4;
        return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
    }
};

// Read the ACTUAL encoded file back using ImageIO, then normalize its channel
// layout through an explicit RGBA bitmap. Its storage is already top-row first;
// unlike a flipped NSView drawing context, this bitmap needs no extra transform.
DecodedPNG decode(const char* path) {
    NSString* name = [NSString stringWithUTF8String:path];
    NSURL* url = [NSURL fileURLWithPath:name];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    require(source != nullptr, "cannot reopen PNG");
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    require(image != nullptr, "cannot decode PNG");
    DecodedPNG out;
    out.w = CGImageGetWidth(image);
    out.h = CGImageGetHeight(image);
    out.rgba.resize(out.w * out.h * 4);
    CGColorSpaceRef colors = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        out.rgba.data(), out.w, out.h, 8, out.w * 4, colors,
        CGBitmapInfo(kCGBitmapByteOrder32Big) |
            CGBitmapInfo(kCGImageAlphaPremultipliedLast));
    CGColorSpaceRelease(colors);
    if (!context) {
        CGImageRelease(image);
        throw std::runtime_error("cannot create PNG decoding bitmap");
    }
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextDrawImage(context, CGRectMake(0, 0, out.w, out.h), image);
    CGContextRelease(context);
    CGImageRelease(image);
    return out;
}

void verifyAll(const DecodedPNG& image, const std::array<Pixel, 10>& expected) {
    require(image.w == width && image.h == height, "incorrect PNG dimensions");
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (image.at(x, y) != expected[tile(x, y)]) {
                const auto actual = image.at(x, y);
                const auto wanted = expected[tile(x, y)];
                std::fprintf(stderr,
                    "pixel (%d,%d): got (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n",
                    x, y, actual[0], actual[1], actual[2], actual[3],
                    wanted[0], wanted[1], wanted[2], wanted[3]);
                throw std::runtime_error("PNG pixel mismatch / stride / orientation");
            }
        }
    }
}

void sample(const char* label, const DecodedPNG& image, int x, int y) {
    const auto p = image.at(x, y);
    std::printf("%s at (%d,%d): RGBA=(%u,%u,%u,%u)\n",
                label, x, y, p[0], p[1], p[2], p[3]);
}
} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            require(NSApp == nil, "test must start without NSApplication");
            std::array<uint8_t, 768> palette{};
            for (std::size_t i = 0; i < palette6.size(); ++i) {
                for (int c = 0; c < 3; ++c) palette[i * 3 + c] = palette6[i][c];
            }
            // An unrendered poison index also makes accidental padding reads
            // obvious; RGB565 padding bytes form distinct non-primary colours.
            palette[255 * 3] = 63;
            palette[255 * 3 + 1] = 17;
            palette[255 * 3 + 2] = 39;
            std::vector<uint8_t> indices(indexedStride * height, 255);
            std::vector<uint8_t> rgb565(rgb565Stride * height, 0xa5);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto i = tile(x, y);
                    indices[y * indexedStride + x] = static_cast<uint8_t>(i);
                    const auto word = rgb565Values[i];
                    const auto offset = y * rgb565Stride + x * 2;
                    rgb565[offset] = static_cast<uint8_t>(word & 255);
                    rgb565[offset + 1] = static_cast<uint8_t>(word >> 8);
                }
            }
            const auto originalIndices = indices;
            const auto originalRGB565 = rgb565;
            const auto originalPalette = palette;
            // Export one self-authored fixture in a resource-limited child.
            if (argc != 1) {
                if (argc != 3) return 2;
                bool ok;
                if (std::strcmp(argv[1], "--export-indexed") == 0)
                    ok = presentOffscreenPNG(indices.data(), width, height,
                                             indexedStride, palette.data(), argv[2]);
                else if (std::strcmp(argv[1], "--export-rgb565") == 0)
                    ok = presentOffscreenRGB565PNG(rgb565.data(), width, height,
                                                   rgb565Stride, argv[2]);
                else return 2;
                std::printf("OFFSCREEN_WRITE_OK=%d\n", ok);
                return ok ? 0 : 1;
            }
            require(presentOffscreenPNG(indices.data(), width, height, indexedStride,
                                        palette.data(), "test_indexed.png"),
                    "indexed PNG encoding failed");
            require(presentOffscreenRGB565PNG(rgb565.data(), width, height,
                                              rgb565Stride, "test_rgb565.png"),
                    "RGB565 PNG encoding failed");
            const auto indexed = decode("test_indexed.png");
            const auto rgb = decode("test_rgb565.png");
            verifyAll(indexed, indexedExpected);
            verifyAll(rgb, rgb565Expected);
            require(indices == originalIndices && rgb565 == originalRGB565 &&
                        palette == originalPalette, "caller input was modified");

            // Invalid calls must reject their input before encoding. Recheck
            // both output files afterwards to catch any accidental overwrite.
            const auto invalidIndexed = [&](const uint8_t* p, int w, int h, int s,
                                             const uint8_t* pal) {
                return !presentOffscreenPNG(p, w, h, s, pal, "test_indexed.png");
            };
            require(invalidIndexed(nullptr, width, height, indexedStride, palette.data()),
                    "null indexed input accepted");
            require(invalidIndexed(indices.data(), width, height, indexedStride, nullptr),
                    "null palette accepted");
            require(invalidIndexed(indices.data(), 0, height, indexedStride, palette.data()),
                    "zero width accepted");
            require(invalidIndexed(indices.data(), width, -1, indexedStride, palette.data()),
                    "negative height accepted");
            require(invalidIndexed(indices.data(), width, height, width - 1, palette.data()),
                    "short indexed stride accepted");
            require(invalidIndexed(indices.data(), width, height, -indexedStride, palette.data()),
                    "negative indexed stride accepted");
            require(invalidIndexed(indices.data(), INT_MAX, INT_MAX, INT_MAX, palette.data()),
                    "overflowing image accepted");
            require(!presentOffscreenRGB565PNG(nullptr, width, height, rgb565Stride,
                                                "test_rgb565.png"), "null RGB565 accepted");
            require(!presentOffscreenRGB565PNG(rgb565.data(), width, height,
                                                width * 2 - 1, "test_rgb565.png"),
                    "short RGB565 stride accepted");
            require(!presentOffscreenRGB565PNG(rgb565.data(), INT_MAX, 1, INT_MAX,
                                                "test_rgb565.png"),
                    "overflowing RGB565 row accepted");
            require(!presentOffscreenPNG(indices.data(), width, height, indexedStride,
                                         palette.data(), nullptr), "null path accepted");
            require(!presentOffscreenRGB565PNG(rgb565.data(), width, height,
                                                rgb565Stride, ""), "empty path accepted");
            require(!presentAttachWindow(nil), "nil window accepted");
            require(!presentIndexed(indices.data(), width, height, indexedStride,
                                     palette.data()), "unbound indexed present accepted");
            require(!presentRGB565(rgb565.data(), width, height, rgb565Stride),
                    "unbound RGB565 present accepted");
            require(presentDetachWindow(), "empty detach failed");
            // Re-read after negative tests to guard against output corruption.
            verifyAll(decode("test_indexed.png"), indexedExpected);
            verifyAll(decode("test_rgb565.png"), rgb565Expected);
            require(NSApp == nil, "offscreen test unexpectedly created NSApplication");

            std::printf("PASS: both PNGs decoded and all 128000 pixels match fixed RGBA oracles.\n");
            std::printf("test_indexed.png: %d x %d; input stride=%d bytes (width=%d).\n",
                        width, height, indexedStride, width);
            std::printf("test_rgb565.png: %d x %d; input stride=%d bytes (packed row=%d).\n",
                        width, height, rgb565Stride, width * 2);
            sample("Indexed index 0 (nonblack palette entry, alpha must be 0)", indexed, 0, 0);
            sample("Indexed 6-bit red 63 << 2", indexed, 32, 0);
            sample("Indexed mixed (1,17,32) << 2", indexed, 256, 0);
            sample("RGB565 0xF800", rgb, 0, 0);
            sample("RGB565 0x07E0", rgb, 32, 0);
            sample("RGB565 0x001F", rgb, 64, 0);
            sample("RGB565 mixed 0x8410", rgb, 256, 0);
            sample("RGB565 mixed 0x1234", rgb, 288, 0);
            sample("Indexed padded next row", indexed, 0, 20);
            sample("RGB565 padded next row", rgb, 0, 20);
            sample("Indexed bottom-right orientation", indexed, width - 1, height - 1);
            sample("RGB565 bottom-right orientation", rgb, width - 1, height - 1);
            std::puts("PASS: invalid inputs rejected; input buffers unchanged; NSApp remains nil.");
            std::puts("Window attachment/draw path: compiled; real window not opened or displayed.");
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            return 1;
        }
    }
}
