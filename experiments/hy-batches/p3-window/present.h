#pragma once

#include <cstdint>

// All buffers start at the TOP-LEFT; stride is in BYTES and must be positive.
// The caller supplies at least (h - 1) * stride + w * bytesPerPixel bytes.
// Frames are copied before return, so the caller can immediately reuse them.
// Indexed palette: 256 RGB triples of 6-bit components, expanded with << 2.
// Index 0 becomes transparent black; all other indices are fully opaque.
// RGB565 is little-endian (low byte first), with fully opaque output.
// Invalid arguments, allocation/encoding failure, or no attached view => false.
bool presentIndexed(const uint8_t* indices, int w, int h, int stride,
                    const uint8_t* pal768);
bool presentRGB565(const uint8_t* pixels, int w, int h, int stride);

// No NSApplication, NSWindow, or view is created by either offscreen function.
// These can also run on worker threads, with distinct output files/buffers.
bool presentOffscreenPNG(const uint8_t* indices, int w, int h, int stride,
                         const uint8_t* pal768, const char* pngPath);
bool presentOffscreenRGB565PNG(const uint8_t* pixels, int w, int h, int stride,
                               const char* pngPath);

#ifdef __OBJC__
@class NSWindow;

// Call on the main thread AFTER the existing CCOriginalSourceWindow has its
// contentView installed. This accepts the existing NSWindow subclass directly:
//     presentAttachWindow(existingWindow);
//     presentIndexed(indices, width, height, byteStride, palette);
//     presentDetachWindow(); // before retiring/replacing that contentView
// Adds a non-interactive child view; does not replace the input contentView,
// create/show a window, activate the app, or change the first responder.
// One attached window at a time; binding/submission/detachment are main-thread
// operations. Submission queues a redraw; AppKit's normal display loop paints
// the latest frame. Images fill the client bounds without interpolation.
bool presentAttachWindow(NSWindow* window);
bool presentDetachWindow();
#endif
