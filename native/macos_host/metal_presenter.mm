// SPDX-License-Identifier: GPL-3.0-only
#import "metal_presenter.h"
#import "metal_scene.h"
#include "display_confirmation.h"
#include "presented_trace.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace host = coastal::macos_host;

namespace {

constexpr MTLPixelFormat source_format = MTLPixelFormatRGBA8Unorm;
constexpr MTLPixelFormat drawable_format = MTLPixelFormatBGRA8Unorm;
constexpr std::size_t buffer_count = 3;
// Covers a complete five-minute 60 Hz endurance check with a fixed memory bound.
constexpr std::size_t presentation_samples = host::presentation_history_capacity;
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

bool retire_submissions(std::array<FrameBuffer, buffer_count>& buffers) {
    for (auto& buffer : buffers) {
        if (!buffer.submission) continue;
        const auto status = buffer.submission.status;
        if (status == MTLCommandBufferStatusError) return false;
        if (status == MTLCommandBufferStatusCompleted) buffer.submission = nil;
    }
    return true;
}

using host::PresentedFrame;

std::optional<host::Percentiles> summarize_intervals(std::vector<std::chrono::nanoseconds>& values) {
    if (values.empty()) return std::nullopt;
    std::sort(values.begin(), values.end());
    const auto count = values.size();
    const auto nearest_rank = [&](std::size_t percentile) {
        return values[(count * percentile + 99) / 100 - 1];
    };
    return host::Percentiles{count, nearest_rank(50), nearest_rank(95), nearest_rank(99)};
}

// Callbacks retain this history independently of the window and presenter.
class PresentationHistory {
public:
    void record_work(std::size_t kind, host::FrameClock::time_point started) {
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(host::FrameClock::now()-started);
        const std::lock_guard lock(mutex_);
        work_[kind][work_count_[kind]++ % presentation_samples]=elapsed;
    }
    void record(CFTimeInterval displayed_at, bool scene, host::FrameCommit commit) {
        const std::lock_guard lock(mutex_);
        if (!clock_.record(displayed_at)) return;
        times_[(clock_.valid_count() - 1) % times_.size()] = {displayed_at, scene};
        if (scene) ++scene_count_;
        if (commit.full_original_frame) {
            ++original_count_;
            if (displayed_at > last_original_time_) {
                last_original_time_ = displayed_at;
                last_original_objects_ = commit.active_objects;
            }
        }
    }

    host::MetalMetrics metrics(const std::string& trace_path) const {
        host::MetalMetrics result;
        host::DisplayClock clock;
        std::vector<PresentedFrame> times;
        std::array<std::vector<std::chrono::nanoseconds>,2> work;
        {
            const std::lock_guard lock(mutex_);
            clock = clock_;
            result.displayed_frames = clock.valid_count();
            result.displayed_original_frames = original_count_;
            result.last_original_active_objects = last_original_objects_;
            result.displayed_scene_frames = scene_count_;
            const auto size = std::min<std::uint64_t>(clock.valid_count(), times_.size());
            times.assign(times_.begin(), times_.begin() + size);
            for(std::size_t kind=0;kind<work.size();++kind){
                const auto count=std::min<std::uint64_t>(work_count_[kind],presentation_samples);
                work[kind].assign(work_[kind].begin(),work_[kind].begin()+count);
            }
        }
        result.drawable_acquire=summarize_intervals(work[0]);
        result.encode_submit=summarize_intervals(work[1]);
        // Export keeps callback order from this snapshot. Only an explicitly
        // requested exit trace needs the extra bounded copy before sorting.
        const bool export_trace = !trace_path.empty();
        const auto trace_ring = export_trace ? times : std::vector<PresentedFrame>{};
        // Callback delivery order can differ from the actual display order.
        std::sort(times.begin(), times.end(), [](const PresentedFrame& a, const PresentedFrame& b) {
            return a.time < b.time;
        });
        result.display_clock = clock.summarize_sorted(times);
        if (export_trace && !host::write_presented_trace(trace_path.c_str(), trace_ring, result.display_clock)) {
            NSLog(@"Could not create presentation trace. Choose a new absolute path in an existing directory.");
        }
        if (times.size() < 2) return result;
        std::vector<std::chrono::nanoseconds> intervals;
        std::vector<std::chrono::nanoseconds> scene_intervals;
        intervals.reserve(times.size() - 1);
        for (std::size_t i = 1; i < times.size(); ++i) {
            const auto& before = times[i - 1];
            const auto& after = times[i];
            const auto interval = std::chrono::nanoseconds(std::llround((after.time - before.time) * 1'000'000'000));
            intervals.push_back(interval);
            if (before.scene != after.scene) {
                ++result.mode_switches;
                result.max_switch_interval = std::max(result.max_switch_interval, interval);
            } else if (after.scene) {
                scene_intervals.push_back(interval);
            }
        }
        result.display_interval = summarize_intervals(intervals);
        result.scene_interval = summarize_intervals(scene_intervals);
        return result;
    }

private:
    mutable std::mutex mutex_;
    std::array<PresentedFrame, presentation_samples> times_{};
    host::DisplayClock clock_;
    std::uint64_t original_count_ = 0;
    CFTimeInterval last_original_time_ = 0;
    std::optional<std::uint64_t> last_original_objects_;
    std::uint64_t scene_count_ = 0;
    std::array<std::array<std::chrono::nanoseconds,presentation_samples>,2> work_{};
    std::array<std::uint64_t,2> work_count_{};
};

}  // namespace

@implementation CoastalMetalPresenter {
    CAMetalLayer* _layer;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    CoastalMetalScene* _scene;
    std::array<FrameBuffer, buffer_count> _buffers;
    std::shared_ptr<PresentationHistory> _history;
    std::shared_ptr<host::DisplayConfirmation> _pendingDisplay;
    std::size_t _byteCount;
    std::size_t _nextBuffer;
    std::size_t _writingBuffer;
    std::uint64_t _submittedFrames;
    std::uint64_t _submittedOriginalFrames;
    BOOL _finished;
    std::string _presentedTracePath;
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
        if (const char* path = std::getenv("COASTAL_PRESENTED_TRACE"); path && path[0] == '/') {
            _presentedTracePath = path;
        }
        return self;
    }
    return nil;
}

- (CAMetalLayer*)layer { return _layer; }

- (std::span<std::uint8_t>)writablePixels {
    if (_finished || _pendingDisplay) return {};

    if (!retire_submissions(_buffers)) throw std::runtime_error("Metal frame execution failed.");

    for (std::size_t offset = 0; offset < buffer_count; ++offset) {
        const auto index = (_nextBuffer + offset) % buffer_count;
        auto& buffer = _buffers[index];
        if (buffer.submission) continue;
        _writingBuffer = index;
        return {static_cast<std::uint8_t*>(buffer.pixels.contents), _byteCount};
    }
    return {};
}

- (BOOL)enableScene:(const std::shared_ptr<const coastal::rts::view3d::TerrainMesh>&)terrain {
    auto* renderer = [[CoastalMetalScene alloc] initWithDevice:_layer.device pixelFormat:_layer.pixelFormat];
    if (!renderer || ![renderer prepareTerrain:terrain]) return NO;
    _scene = renderer;
    return YES;
}

- (host::FramePresentation)presentFrame:(const host::Frame&)frame {
    if (_finished || !retire_submissions(_buffers)) return host::FramePresentation::Invalid;
    auto& buffer = _buffers[_writingBuffer];
    if (_pendingDisplay) {
        // An unavailable frame stays in FrameDelivery with the same RGBA slot.
        switch (_pendingDisplay->poll(frame.commit.index, buffer.submission == nil)) {
        case host::DisplayConfirmation::Poll::Waiting:
            return host::FramePresentation::Unavailable;
        case host::DisplayConfirmation::Poll::Displayed:
            _pendingDisplay.reset();
            return host::FramePresentation::Presented;
        case host::DisplayConfirmation::Poll::Retry:
            _pendingDisplay.reset();
            break;
        case host::DisplayConfirmation::Poll::WrongFrame:
            return host::FramePresentation::Invalid;
        }
    }

    const bool scene = frame.scene != nullptr;
    if (!scene && (frame.rgba.data() != buffer.pixels.contents || frame.rgba.size() != _byteCount)) return host::FramePresentation::Invalid;
    // Finish CPU simulation/raster work before reserving a scarce display
    // surface. The independent shared pixel slot needs no drawable.
    const auto acquire_started=host::FrameClock::now();
    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    _history->record_work(0,acquire_started);
    if (!drawable) return host::FramePresentation::Unavailable;
    const auto started=host::FrameClock::now();

    id<MTLCommandBuffer> command = [_queue commandBuffer];
    if (!command) return host::FramePresentation::Invalid;
    if (scene) {
        if (!_scene || ![_scene encodeScene:*frame.scene target:drawable.texture command:command slot:_writingBuffer]) {
            return host::FramePresentation::Invalid;
        }
    } else {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* color = pass.colorAttachments[0];
        color.texture = drawable.texture;
        color.loadAction = MTLLoadActionDontCare;
        color.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return host::FramePresentation::Invalid;
        [encoder setRenderPipelineState:_pipeline];
        [encoder setFragmentTexture:buffer.texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    }

    if (@available(macOS 10.15.4, *)) {
        const auto history = _history;
        const auto commit = frame.commit;
        if (!scene && commit.requires_display_confirmation)
            _pendingDisplay = std::make_shared<host::DisplayConfirmation>(commit.index);
        const auto confirmation = _pendingDisplay;
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            const auto presented_time = shown.presentedTime;
            history->record(presented_time, scene, commit);
            if (confirmation) confirmation->complete(presented_time);
        }];
    }
    [command presentDrawable:drawable];
    [command commit];
    ++_submittedFrames;
    if (frame.commit.full_original_frame) ++_submittedOriginalFrames;
    _history->record_work(1,started);
    buffer.submission = command;
    _nextBuffer = (_writingBuffer + 1) % buffer_count;
    // Poll confirmation on the next regular display pulse or fallback timer.
    return _pendingDisplay ? host::FramePresentation::Unavailable : host::FramePresentation::Presented;
}

- (host::MetalMetrics)metrics {
    auto metrics = _history->metrics(host::take_exit_trace_path(_finished, _presentedTracePath));
    metrics.submitted_frames = _submittedFrames;
    metrics.submitted_original_frames = _submittedOriginalFrames;
    return metrics;
}

- (void)finish {
    _finished = YES;
    _pendingDisplay.reset();
    for (auto& buffer : _buffers) {
        [buffer.submission waitUntilCompleted];
        buffer.submission = nil;
    }
}

@end
