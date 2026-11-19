// SPDX-License-Identifier: GPL-3.0-only
#import "metal_presenter.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <stdexcept>

namespace host = coastal::macos_host;

namespace {

constexpr MTLPixelFormat source_format = MTLPixelFormatRGBA8Unorm;
constexpr MTLPixelFormat drawable_format = MTLPixelFormatBGRA8Unorm;
constexpr std::size_t buffer_count = 2;
constexpr std::size_t presentation_samples = 4097;
constexpr std::uint32_t max_texture_dimension = 16384;

constexpr char shader_source[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Vertex {
    float4 position [[position]];
    float2 uv;
};

vertex Vertex coastal_frame_vertex(uint index [[vertex_id]]) {
    const float2 corners[3] = {{0, 0}, {2, 0}, {0, 2}};
    float2 uv = corners[index];
    return {float4(uv * float2(2, -2) + float2(-1, 1), 0, 1), uv};
}

fragment half4 coastal_frame_fragment(Vertex input [[stage_in]],
                                      texture2d<half> frame [[texture(0)]]) {
    constexpr sampler lookup(coord::normalized, address::clamp_to_edge, filter::nearest);
    return half4(frame.sample(lookup, input.uv).rgb, half(1));
}
)metal";

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device) {
    NSString* source = [NSString stringWithUTF8String:shader_source];
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        NSLog(@"Coastal Mac Host: Metal frame shader failed: %@", error.localizedDescription);
        return nil;
    }

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [library newFunctionWithName:@"coastal_frame_vertex"];
    desc.fragmentFunction = [library newFunctionWithName:@"coastal_frame_fragment"];
    desc.colorAttachments[0].pixelFormat = drawable_format;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline) NSLog(@"Coastal Mac Host: Metal pipeline creation failed: %@", error.localizedDescription);
    return pipeline;
}

struct FrameBuffer {
    id<MTLBuffer> pixels = nil;
    id<MTLTexture> texture = nil;
    id<MTLCommandBuffer> submission = nil;
};

// Callbacks retain this history independently of the window and presenter.
class PresentationHistory {
public:
    void record(CFTimeInterval displayed_at) {
        if (displayed_at <= 0 || !std::isfinite(displayed_at)) return;
        const std::lock_guard lock(mutex_);
        times_[count_ % times_.size()] = displayed_at;
        ++count_;
    }

    host::MetalMetrics metrics() const {
        host::MetalMetrics result;
        std::array<CFTimeInterval, presentation_samples> times;
        std::size_t size;
        {
            const std::lock_guard lock(mutex_);
            result.displayed_frames = count_;
            size = std::min<std::uint64_t>(count_, times.size());
            std::copy_n(times_.begin(), size, times.begin());
        }
        if (size < 2) return result;

        // Callback delivery order can differ from the actual display order.
        std::sort(times.begin(), times.begin() + size);
        std::array<std::chrono::nanoseconds, presentation_samples - 1> intervals;
        for (std::size_t i = 1; i < size; ++i) {
            const double seconds = times[i] - times[i - 1];
            intervals[i - 1] = std::chrono::nanoseconds(std::llround(seconds * 1'000'000'000));
        }
        const std::size_t sample_count = size - 1;
        std::sort(intervals.begin(), intervals.begin() + sample_count);
        const auto nearest_rank = [&](std::size_t percentile) {
            return intervals[(sample_count * percentile + 99) / 100 - 1];
        };
        result.display_interval = host::Percentiles{sample_count, nearest_rank(50),
                                                    nearest_rank(95), nearest_rank(99)};
        return result;
    }

private:
    mutable std::mutex mutex_;
    std::array<CFTimeInterval, presentation_samples> times_{};
    std::uint64_t count_ = 0;
};

}  // namespace

@implementation CoastalMetalPresenter {
    CAMetalLayer* _layer;
    id<CAMetalDrawable> _drawable;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    std::array<FrameBuffer, buffer_count> _buffers;
    std::shared_ptr<PresentationHistory> _history;
    std::size_t _byteCount;
    std::size_t _nextBuffer;
    std::size_t _writingBuffer;
    BOOL _finished;
}

- (instancetype)initWithViewport:(host::Viewport)viewport {
    if (!(self = [super init])) return nil;
    if (@available(macOS 10.15.4, *)) {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device || !device.hasUnifiedMemory || viewport.width == 0 || viewport.height == 0 ||
            viewport.width > max_texture_dimension || viewport.height > max_texture_dimension) {
            return nil;
        }

        const NSUInteger row_bytes = NSUInteger(viewport.width) * 4;
        const NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:source_format];
        if (alignment == 0 || row_bytes % alignment != 0 ||
            viewport.height > device.maxBufferLength / row_bytes) {
            return nil;
        }
        _byteCount = row_bytes * viewport.height;
        _queue = [device newCommandQueueWithMaxCommandBufferCount:buffer_count];
        _pipeline = make_pipeline(device);
        if (!_queue || !_pipeline) return nil;

        MTLTextureDescriptor* texture_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source_format
                                                               width:viewport.width
                                                              height:viewport.height
                                                           mipmapped:NO];
        texture_desc.storageMode = MTLStorageModeShared;
        texture_desc.usage = MTLTextureUsageShaderRead;
        for (auto& buffer : _buffers) {
            buffer.pixels = [device newBufferWithLength:_byteCount options:MTLResourceStorageModeShared];
            buffer.texture = [buffer.pixels newTextureWithDescriptor:texture_desc
                                                              offset:0
                                                         bytesPerRow:row_bytes];
            if (!buffer.texture) {
                NSLog(@"Coastal Mac Host: Metal shared frame allocation failed.");
                return nil;
            }
        }

        _layer = [CAMetalLayer layer];
        _layer.device = device;
        _layer.pixelFormat = drawable_format;
        _layer.framebufferOnly = YES;
        _layer.maximumDrawableCount = buffer_count;
        _layer.drawableSize = CGSizeMake(viewport.width, viewport.height);
        // Frame bytes are already sRGB encoded. Both texture formats are plain
        // unorm so Metal preserves the values and Core Animation matches colors.
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if (!space) return nil;
        _layer.colorspace = space;
        CGColorSpaceRelease(space);
        _history = std::make_shared<PresentationHistory>();
        return self;
    }
    return nil;
}

- (CAMetalLayer*)layer { return _layer; }

- (std::span<std::uint8_t>)writablePixels {
    _drawable = nil;
    if (_finished) return {};

    for (auto& buffer : _buffers) {
        if (!buffer.submission) continue;
        const auto status = buffer.submission.status;
        if (status == MTLCommandBufferStatusError) {
            throw std::runtime_error("Metal frame execution failed.");
        }
        if (status == MTLCommandBufferStatusCompleted) buffer.submission = nil;
    }

    for (std::size_t offset = 0; offset < buffer_count; ++offset) {
        const auto index = (_nextBuffer + offset) % buffer_count;
        auto& buffer = _buffers[index];
        if (buffer.submission) continue;
        _drawable = [_layer nextDrawable];
        if (!_drawable) return {};
        _writingBuffer = index;
        return {static_cast<std::uint8_t*>(buffer.pixels.contents), _byteCount};
    }
    return {};
}

- (BOOL)presentFrame:(const host::Frame&)frame {
    id<CAMetalDrawable> drawable = _drawable;
    _drawable = nil;
    if (!drawable) return NO;
    auto& buffer = _buffers[_writingBuffer];
    if (frame.rgba.data() != buffer.pixels.contents || frame.rgba.size() != _byteCount) return NO;

    id<MTLCommandBuffer> command = [_queue commandBuffer];
    if (!command) return NO;
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    auto* color = pass.colorAttachments[0];
    color.texture = drawable.texture;
    color.loadAction = MTLLoadActionDontCare;
    color.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return NO;
    [encoder setRenderPipelineState:_pipeline];
    [encoder setFragmentTexture:buffer.texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];

    if (@available(macOS 10.15.4, *)) {
        const auto history = _history;
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            history->record(shown.presentedTime);
        }];
    }
    [command presentDrawable:drawable];
    [command commit];
    buffer.submission = command;
    _nextBuffer = (_writingBuffer + 1) % buffer_count;
    return YES;
}

- (host::MetalMetrics)metrics {
    return _history->metrics();
}

- (void)finish {
    _finished = YES;
    _drawable = nil;
    for (auto& buffer : _buffers) {
        [buffer.submission waitUntilCompleted];
        buffer.submission = nil;
    }
}

@end
