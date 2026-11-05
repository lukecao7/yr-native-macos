#include "present.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

namespace {
struct ReleaseImage {
    void operator()(CGImageRef image) const noexcept { CGImageRelease(image); }
};
using Image = std::unique_ptr<std::remove_pointer_t<CGImageRef>, ReleaseImage>;

struct Layout {
    std::size_t rgbaStride = 0;
    std::size_t rgbaBytes = 0;
};

bool checkLayout(int w, int h, int stride, std::size_t bytesPerPixel,
                 Layout& out) noexcept {
    if (w <= 0 || h <= 0 || stride <= 0) return false;
    const auto width = static_cast<std::size_t>(w);
    const auto height = static_cast<std::size_t>(h);
    const auto sourceStride = static_cast<std::size_t>(stride);
    const auto limit = static_cast<std::size_t>(
        std::numeric_limits<std::ptrdiff_t>::max());
    if (width > limit / bytesPerPixel) return false;
    const auto sourceRowBytes = width * bytesPerPixel;
    if (sourceStride < sourceRowBytes ||
        height - 1 > (limit - sourceRowBytes) / sourceStride ||
        width > limit / 4) return false;
    out.rgbaStride = width * 4;
    if (height > limit / out.rgbaStride) return false;
    out.rgbaBytes = out.rgbaStride * height;
    return true;
}

void freePixels(void*, const void* bytes, std::size_t) {
    std::free(const_cast<void*>(bytes));
}

// The provider owns this private RGBA snapshot, not the caller's buffer.
// Every entry has alpha 0 or 255, so its RGBA bytes are also premultiplied RGBA.
Image makeImage(const uint8_t* pixels, int w, int h, int stride,
                const uint8_t* palette, bool indexed) noexcept {
    Layout layout;
    if (!pixels || (indexed && !palette) ||
        !checkLayout(w, h, stride, indexed ? 1 : 2, layout)) return {};
    auto* rgba = static_cast<uint8_t*>(std::malloc(layout.rgbaBytes));
    if (!rgba) return {};
    for (int y = 0; y < h; ++y) {
        const auto* src = pixels + static_cast<std::size_t>(y) * stride;
        auto* dst = rgba + static_cast<std::size_t>(y) * layout.rgbaStride;
        for (int x = 0; x < w; ++x, dst += 4) {
            if (indexed) {
                const auto index = src[x];
                if (index == 0) {
                    std::memset(dst, 0, 4);
                } else {
                    const auto* rgb = palette + static_cast<unsigned>(index) * 3;
                    dst[0] = static_cast<uint8_t>(unsigned(rgb[0]) << 2);
                    dst[1] = static_cast<uint8_t>(unsigned(rgb[1]) << 2);
                    dst[2] = static_cast<uint8_t>(unsigned(rgb[2]) << 2);
                    dst[3] = 255;
                }
            } else {
                // Byte loads also support odd row strides and unaligned input.
                const auto offset = static_cast<std::size_t>(x) * 2;
                const unsigned value = unsigned(src[offset]) |
                                       (unsigned(src[offset + 1]) << 8);
                const unsigned r = (value >> 11) & 31;
                const unsigned g = (value >> 5) & 63;
                const unsigned b = value & 31;
                dst[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
                dst[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
                dst[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
                dst[3] = 255;
            }
        }
    }
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, rgba, layout.rgbaBytes, freePixels);
    if (!provider) {
        std::free(rgba);
        return {};
    }
    CGColorSpaceRef colors = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = nullptr;
    if (colors) {
        image = CGImageCreate(static_cast<std::size_t>(w),
                              static_cast<std::size_t>(h), 8, 32,
                              layout.rgbaStride, colors,
                              CGBitmapInfo(kCGBitmapByteOrder32Big) |
                                  CGBitmapInfo(kCGImageAlphaPremultipliedLast),
                              provider, nullptr, false,
                              kCGRenderingIntentDefault);
        CGColorSpaceRelease(colors);
    }
    CGDataProviderRelease(provider);
    return Image(image);
}

bool writePNG(CGImageRef image, const char* path) noexcept {
    if (!image || !path || !*path) return false;
    const auto length = std::strlen(path);
    if (length > static_cast<std::size_t>(
                     std::numeric_limits<CFIndex>::max())) return false;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path),
        static_cast<CFIndex>(length), false);
    if (!url) return false;
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
        url, CFSTR("public.png"), 1, nullptr);
    CFRelease(url);
    if (!destination) return false;
    CGImageDestinationAddImage(destination, image, nullptr);
    const bool ok = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    return ok;
}
} // namespace

@interface CCPresentFramebufferView : NSView
@property(nonatomic, strong) NSImage* frameImage;
@end

@implementation CCPresentFramebufferView
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstResponder { return NO; }
- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil; // Let CCOriginalSourceInputView receive its original events.
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext currentContext].imageInterpolation = NSImageInterpolationNone;
    [self.frameImage drawInRect:self.bounds fromRect:NSZeroRect
                     operation:NSCompositingOperationSourceOver fraction:1.0
                respectFlipped:YES hints:nil];
    [NSGraphicsContext restoreGraphicsState];
}
@end

namespace {
// Only touched from the main thread. Weak references do not own the window or
// its original input view; the private drawing view retains just its snapshot.
__weak NSWindow* attachedWindow = nil;
__weak NSView* attachedContent = nil;
CCPresentFramebufferView* attachedView = nil;

bool hasTarget() {
    return [NSThread isMainThread] && attachedWindow && attachedContent &&
           attachedView && attachedWindow.contentView == attachedContent &&
           attachedView.superview == attachedContent &&
           attachedView.window == attachedWindow;
}

bool submit(CGImageRef image) {
    if (!image || !hasTarget()) return false;
    NSImage* snapshot = [[NSImage alloc]
        initWithCGImage:image
                   size:NSMakeSize(CGImageGetWidth(image), CGImageGetHeight(image))];
    if (!snapshot) return false;
    attachedView.frameImage = snapshot;
    attachedView.needsDisplay = YES;
    return true;
}
} // namespace

bool presentAttachWindow(NSWindow* window) {
    if (![NSThread isMainThread] || !window) return false;
    @autoreleasepool {
        @try {
            NSView* content = window.contentView;
            if (!content) return false;
            if (hasTarget() && attachedWindow == window) return true;
            CCPresentFramebufferView* view = [[CCPresentFramebufferView alloc]
                initWithFrame:content.bounds];
            if (!view) return false;
            view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            view.wantsLayer = YES;
            [content addSubview:view positioned:NSWindowAbove relativeTo:nil];
            [attachedView removeFromSuperview];
            attachedWindow = window;
            attachedContent = content;
            attachedView = view;
            return true;
        } @catch (NSException* error) {
            (void)error;
            return false;
        }
    }
}

bool presentDetachWindow() {
    if (![NSThread isMainThread]) return false;
    @try {
        [attachedView removeFromSuperview];
        attachedView = nil;
        attachedContent = nil;
        attachedWindow = nil;
        return true;
    } @catch (NSException* error) {
        (void)error;
        return false;
    }
}

bool presentIndexed(const uint8_t* indices, int w, int h, int stride,
                    const uint8_t* pal768) {
    @autoreleasepool {
        @try {
            if (!hasTarget()) return false;
            const auto image = makeImage(indices, w, h, stride, pal768, true);
            return submit(image.get());
        } @catch (NSException* error) {
            (void)error;
            return false;
        }
    }
}

bool presentRGB565(const uint8_t* pixels, int w, int h, int stride) {
    @autoreleasepool {
        @try {
            if (!hasTarget()) return false;
            const auto image = makeImage(pixels, w, h, stride, nullptr, false);
            return submit(image.get());
        } @catch (NSException* error) {
            (void)error;
            return false;
        }
    }
}

bool presentOffscreenPNG(const uint8_t* indices, int w, int h, int stride,
                         const uint8_t* pal768, const char* pngPath) {
    const auto image = makeImage(indices, w, h, stride, pal768, true);
    return writePNG(image.get(), pngPath);
}

bool presentOffscreenRGB565PNG(const uint8_t* pixels, int w, int h, int stride,
                               const char* pngPath) {
    const auto image = makeImage(pixels, w, h, stride, nullptr, false);
    return writePNG(image.get(), pngPath);
}
