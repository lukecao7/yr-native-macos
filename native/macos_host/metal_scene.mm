// SPDX-License-Identifier: GPL-3.0-only
#import "metal_scene.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace view = coastal::rts::view3d;

namespace {

constexpr MTLPixelFormat depth_format = MTLPixelFormatDepth32Float;
constexpr std::size_t slot_count = 3;
constexpr std::size_t initial_instances = 2048;

struct alignas(16) Uniforms {
    std::array<float, 16> projection;
    view::Vec4 camera_right;
    view::Vec4 camera_up;
    view::Vec4 animation;
};

static_assert(sizeof(view::Vec4) == 16);
static_assert(sizeof(view::TerrainVertex) == 48);
static_assert(sizeof(view::ModelInstance) == 48);
static_assert(sizeof(Uniforms) == 112);

constexpr char shader_source[] = R"metal(
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4x4 projection;
    float4 cameraRight;
    float4 cameraUp;
    float4 animation;
};

struct MeshVertex {
    float4 position;
    float4 normal;
    float4 color;
};

struct Model {
    float4 anchorYaw;
    float4 sizeHealth;
    float4 paint;
};

struct Surface {
    float4 position [[position]];
    float3 world;
    float4 normal;
    float3 color;
};

vertex Surface coastal_terrain(uint i [[vertex_id]],
                               device const MeshVertex* vertices [[buffer(0)]],
                               constant Uniforms& camera [[buffer(1)]]) {
    const MeshVertex v = vertices[i];
    return {camera.projection * float4(v.position.xyz, 1),
            v.position.xyz, v.normal, v.color.rgb};
}

float3 rotate_heading(float3 v, float angle) {
    const float c = cos(angle);
    const float s = sin(angle);
    return float3(c * v.x - s * v.z, v.y, s * v.x + c * v.z);
}

vertex Surface coastal_model(uint i [[vertex_id]],
                             uint instance [[instance_id]],
                             device const MeshVertex* vertices [[buffer(0)]],
                             constant Uniforms& camera [[buffer(1)]],
                             device const Model* models [[buffer(2)]]) {
    const MeshVertex v = vertices[i];
    const Model model = models[instance];
    const float3 size = max(float3(0.02f), model.sizeHealth.xyz);
    const float3 point = model.anchorYaw.xyz + rotate_heading(v.position.xyz * size, model.anchorYaw.w);
    const float3 normal = rotate_heading(v.normal.xyz / size, model.anchorYaw.w);
    const float3 paint = mix(model.paint.rgb, float3(0.89f, 0.98f, 0.55f), 0.38f * model.paint.w);
    return {camera.projection * float4(point, 1), point, float4(normal, 0), paint * v.color.x};
}

fragment half4 coastal_light(Surface input [[stage_in]], constant Uniforms& camera [[buffer(0)]]) {
    const float3 sun = float3(0.398f, 0.853f, -0.34f);
    const float light = max(0.0f, dot(normalize(input.normal.xyz), sun));
    float3 color = input.color * (0.66f + 0.43f * light);
    if (input.normal.w > 0.5f) {
        const float2 shore = input.world.xz;
        const float t = camera.animation.x;
        const float wave = sin(dot(shore, float2(1.25f, 0.92f)) + t * 17.0f) *
                           sin(dot(shore, float2(-0.29f, 1.62f)) - t * 12.0f);
        color = mix(color, float3(0.21f, 0.64f, 0.72f), 0.14f + 0.10f * wave);
        color += float3(0.10f, 0.14f, 0.15f) * pow(max(0.0f, wave), 6.0f);
    }
    return half4(half3(saturate(color)), half(1));
}

struct Health {
    float4 position [[position]];
    float2 uv;
    float health;
    float selected;
};

vertex Health coastal_health(uint i [[vertex_id]],
                             uint instance [[instance_id]],
                             constant Uniforms& camera [[buffer(1)]],
                             device const Model* models [[buffer(2)]]) {
    const float2 corners[6] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
    const float2 corner = corners[i];
    const Model model = models[instance];
    const float width = max(0.50f, max(model.sizeHealth.x, model.sizeHealth.z) * 0.86f);
    const float height = max(0.14f, min(0.24f, model.sizeHealth.y * 0.16f));
    const float3 above = model.anchorYaw.xyz + float3(0, model.sizeHealth.y + 0.18f, 0);
    const float3 point = above + camera.cameraRight.xyz * width * (corner.x - 0.5f) +
                                 camera.cameraUp.xyz * height * corner.y;
    return {camera.projection * float4(point, 1), corner,
            saturate(model.sizeHealth.w), model.paint.w};
}

fragment half4 coastal_health_paint(Health bar [[stage_in]]) {
    if (bar.uv.x < 0.035f || bar.uv.x >= 0.965f || bar.uv.y < 0.18f || bar.uv.y > 0.82f) {
        return bar.selected > 0.5f ? half4(0.74f, 0.89f, 0.62f, 1) : half4(0.04f, 0.07f, 0.10f, 1);
    }
    const float amount = (bar.uv.x - 0.035f) / 0.93f;
    if (amount > bar.health) return half4(0.15f, 0.19f, 0.22f, 1);
    const half3 tint = mix(half3(0.96f, 0.37f, 0.27f), half3(0.47f, 0.91f, 0.48f), half(bar.health));
    return half4(tint, half(1));
}

struct Outline {
    float4 position [[position]];
};

vertex Outline coastal_outline(uint i [[vertex_id]],
                               constant float4* points [[buffer(0)]]) {
    return {points[i]};
}

fragment half4 coastal_outline_paint() {
    return half4(0.54f, 0.83f, 0.97f, 1);
}
)metal";

BOOL fail(NSString* reason) {
    NSLog(@"Coastal Mac Host 3D: %@", reason);
    return NO;
}

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                         MTLPixelFormat color_format, NSString* vertex,
                                         NSString* fragment) {
    auto* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = vertex;
    descriptor.vertexFunction = [library newFunctionWithName:vertex];
    descriptor.fragmentFunction = [library newFunctionWithName:fragment];
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction) {
        fail([NSString stringWithFormat:@"Could not find shaders for %@.", vertex]);
        return nil;
    }
    descriptor.colorAttachments[0].pixelFormat = color_format;
    descriptor.depthAttachmentPixelFormat = depth_format;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipeline) {
        fail([NSString stringWithFormat:@"Could not build %@: %@", vertex, error.localizedDescription]);
    }
    return pipeline;
}

id<MTLDepthStencilState> make_depth_state(id<MTLDevice> device, MTLCompareFunction comparison, BOOL write) {
    auto* descriptor = [[MTLDepthStencilDescriptor alloc] init];
    descriptor.depthCompareFunction = comparison;
    descriptor.depthWriteEnabled = write;
    return [device newDepthStencilStateWithDescriptor:descriptor];
}

std::vector<view::TerrainVertex> unit_mesh() {
    std::vector<view::TerrainVertex> vertices;
    vertices.reserve(150);
    // Each top ring is slightly inset so the hull and turret catch light.
    const auto hull = [&](float x0, float x1, float y0, float y1, float z0, float z1,
                          float inset, float paint) {
        const std::array<view::Vec4, 8> corners{{
            {x0, y0, z0, 1},
            {x1, y0, z0, 1},
            {x0, y0, z1, 1},
            {x1, y0, z1, 1},
            {x0 + inset, y1, z0 + inset, 1},
            {x1 - inset, y1, z0 + inset, 1},
            {x0 + inset, y1, z1 - inset, 1},
            {x1 - inset, y1, z1 - inset, 1},
        }};
        constexpr std::array<std::array<int, 4>, 5> faces{{
            {4, 6, 7, 5}, {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6},
        }};
        for (const auto& face : faces) {
            const auto& a = corners[face[0]];
            const auto& b = corners[face[1]];
            const auto& c = corners[face[2]];
            const view::Vec4 u{b.x - a.x, b.y - a.y, b.z - a.z, 0};
            const view::Vec4 v{c.x - a.x, c.y - a.y, c.z - a.z, 0};
            view::Vec4 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                         u.x * v.y - u.y * v.x, 0};
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            n = {n.x / len, n.y / len, n.z / len, 0};
            for (const int i : {0, 1, 2, 0, 2, 3}) {
                vertices.push_back({corners[face[i]], n, {paint, 0, 0, 1}});
            }
        }
    };
    hull(-0.48F, 0.40F, 0.04F, 0.30F, -0.50F, -0.25F, 0.035F, 0.65F);
    hull(-0.48F, 0.40F, 0.04F, 0.30F,  0.25F,  0.50F, 0.035F, 0.65F);
    hull(-0.50F, 0.46F, 0.20F, 0.54F, -0.40F,  0.40F, 0.07F, 0.92F);
    hull(-0.33F, 0.23F, 0.50F, 1.00F, -0.27F,  0.27F, 0.09F, 1.09F);
    hull( 0.03F, 0.50F, 0.66F, 0.83F, -0.06F,  0.06F, 0.01F, 1.14F);
    return vertices;
}

struct SceneSlot {
    id<MTLTexture> depth = nil;
    id<MTLBuffer> instances = nil;
};

std::array<view::Vec4, 24> outline_vertices(const coastal::rts::SelectionBox& box, int width, int height) {
    const auto [min_x, max_x] = std::minmax(box.start.x, box.end.x);
    const auto [min_y, max_y] = std::minmax(box.start.y, box.end.y);
    const int left = std::clamp(min_x, 0, width);
    const int top = std::clamp(min_y, 0, height);
    const int right = std::clamp(max_x, 0, width);
    const int bottom = std::clamp(max_y, 0, height);
    constexpr int stroke = 2;
    std::array<view::Vec4, 24> vertices;
    std::size_t index = 0;
    const auto point = [&](int x, int y) {
        return view::Vec4{2.0F * x / width - 1, 1 - 2.0F * y / height, 0, 1};
    };
    const auto strip = [&](int l, int t, int r, int b) {
        for (const auto& position : {point(l, t), point(r, t), point(l, b),
                                     point(l, b), point(r, t), point(r, b)}) {
            vertices[index++] = position;
        }
    };
    strip(left, top, right, std::min(top + stroke, bottom));
    strip(left, std::max(bottom - stroke, top), right, bottom);
    strip(left, top, std::min(left + stroke, right), bottom);
    strip(std::max(right - stroke, left), top, right, bottom);
    return vertices;
}

}  // namespace

@implementation CoastalMetalScene {
    id<MTLDevice> _device;
    MTLPixelFormat _colorFormat;
    id<MTLRenderPipelineState> _terrainPipeline;
    id<MTLRenderPipelineState> _modelPipeline;
    id<MTLRenderPipelineState> _healthPipeline;
    id<MTLRenderPipelineState> _outlinePipeline;
    id<MTLDepthStencilState> _solidDepth;
    id<MTLDepthStencilState> _healthDepth;
    id<MTLDepthStencilState> _outlineDepth;
    std::shared_ptr<const view::TerrainMesh> _terrainSource;
    id<MTLBuffer> _terrainVertices;
    id<MTLBuffer> _modelVertices;
    std::size_t _modelCount;
    std::array<SceneSlot, slot_count> _slots;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device pixelFormat:(MTLPixelFormat)format {
    if (!(self = [super init]) || !device) return nil;
    _device = device;
    _colorFormat = format;
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:shader_source];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        fail([NSString stringWithFormat:@"Could not compile the scene shaders: %@", error.localizedDescription]);
        return nil;
    }
    _terrainPipeline = make_pipeline(device, library, format, @"coastal_terrain", @"coastal_light");
    _modelPipeline = make_pipeline(device, library, format, @"coastal_model", @"coastal_light");
    _healthPipeline = make_pipeline(device, library, format, @"coastal_health", @"coastal_health_paint");
    _outlinePipeline = make_pipeline(device, library, format, @"coastal_outline", @"coastal_outline_paint");
    if (!_terrainPipeline || !_modelPipeline || !_healthPipeline || !_outlinePipeline) return nil;

    _solidDepth = make_depth_state(device, MTLCompareFunctionLessEqual, YES);
    _healthDepth = make_depth_state(device, MTLCompareFunctionLessEqual, NO);
    _outlineDepth = make_depth_state(device, MTLCompareFunctionAlways, NO);
    const auto model = unit_mesh();
    _modelCount = model.size();
    _modelVertices = [device newBufferWithBytes:model.data()
                                          length:_modelCount * sizeof(model[0])
                                         options:MTLResourceStorageModeShared];
    for (auto& slot : _slots) {
        slot.instances = [device newBufferWithLength:initial_instances * sizeof(view::ModelInstance)
                                            options:MTLResourceStorageModeShared];
        if (!slot.instances) {
            fail(@"Could not allocate model staging buffers.");
            return nil;
        }
    }
    if (!_solidDepth || !_healthDepth || !_outlineDepth || !_modelVertices) {
        fail(@"Could not allocate the model mesh or depth state.");
        return nil;
    }
    return self;
}

- (BOOL)prepareTerrain:(const std::shared_ptr<const view::TerrainMesh>&)terrain {
    if (terrain && terrain == _terrainSource) return YES;
    if (!terrain || terrain->vertices.empty() || terrain->vertices.size() % 3 != 0 ||
        terrain->vertices.size() > _device.maxBufferLength / sizeof(view::TerrainVertex)) {
        return fail(@"Invalid terrain geometry.");
    }
    id<MTLBuffer> uploaded = [_device newBufferWithBytes:terrain->vertices.data()
                                                  length:terrain->vertices.size() * sizeof(view::TerrainVertex)
                                                 options:MTLResourceStorageModeShared];
    if (!uploaded) return fail(@"Could not upload the terrain mesh.");
    _terrainVertices = uploaded;
    _terrainSource = terrain;
    return YES;
}

- (BOOL)encodeScene:(const view::SceneFrame&)scene
            target:(id<MTLTexture>)color
           command:(id<MTLCommandBuffer>)command
              slot:(NSUInteger)slotIndex {
    if (!color || !command || color.pixelFormat != _colorFormat || slotIndex >= _slots.size() ||
        scene.camera.width <= 0 || scene.camera.height <= 0 ||
        NSUInteger(scene.camera.width) != color.width || NSUInteger(scene.camera.height) != color.height) {
        return fail(@"Invalid scene render target.");
    }
    if (![self prepareTerrain:scene.terrain]) return NO;

    // The presenter acquires this slot only after its earlier GPU job finished.
    auto& slot = _slots[slotIndex];
    if (!slot.depth || slot.depth.width != color.width || slot.depth.height != color.height) {
        auto* depth = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth_format
                                                                      width:color.width height:color.height
                                                                  mipmapped:NO];
        depth.storageMode = MTLStorageModePrivate;
        depth.usage = MTLTextureUsageRenderTarget;
        slot.depth = [_device newTextureWithDescriptor:depth];
        if (!slot.depth) return fail(@"Could not allocate the scene depth texture.");
    }
    if (scene.instances.size() > _device.maxBufferLength / sizeof(view::ModelInstance)) {
        return fail(@"Too many instances for one Metal buffer.");
    }
    const auto instanceBytes = scene.instances.size() * sizeof(view::ModelInstance);
    if (instanceBytes > slot.instances.length) {
        const auto grown = std::min(_device.maxBufferLength, slot.instances.length * 2);
        slot.instances = [_device newBufferWithLength:std::max(instanceBytes, grown)
                                             options:MTLResourceStorageModeShared];
        if (!slot.instances) return fail(@"Could not grow the model staging buffer.");
    }
    if (instanceBytes) std::memcpy(slot.instances.contents, scene.instances.data(), instanceBytes);

    // A wrapped phase with integer harmonics is continuous across long runs.
    constexpr float radians_per_step = 6.283185307F / 3600.0F;
    const Uniforms camera{scene.camera.projection, scene.camera.right, scene.camera.up,
                          {float(scene.step % 3600) * radians_per_step, 0, 0, 0}};
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    auto* output = pass.colorAttachments[0];
    output.texture = color;
    output.clearColor = MTLClearColorMake(0.064, 0.098, 0.13, 1.0);
    output.loadAction = MTLLoadActionClear;
    output.storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = slot.depth;
    pass.depthAttachment.clearDepth = 1;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return fail(@"Could not begin the 3D render pass.");
    [encoder setVertexBytes:&camera length:sizeof(camera) atIndex:1];
    [encoder setFragmentBytes:&camera length:sizeof(camera) atIndex:0];
    [encoder setDepthStencilState:_solidDepth];
    [encoder setRenderPipelineState:_terrainPipeline];
    [encoder setVertexBuffer:_terrainVertices offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:scene.terrain->vertices.size()];

    if (!scene.instances.empty()) {
        [encoder setRenderPipelineState:_modelPipeline];
        [encoder setVertexBuffer:_modelVertices offset:0 atIndex:0];
        [encoder setVertexBuffer:slot.instances offset:0 atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:_modelCount
                  instanceCount:scene.instances.size()];
        [encoder setRenderPipelineState:_healthPipeline];
        [encoder setDepthStencilState:_healthDepth];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
                  instanceCount:scene.instances.size()];
    }
    if (scene.drag) {
        const auto outline = outline_vertices(*scene.drag, scene.camera.width, scene.camera.height);
        [encoder setRenderPipelineState:_outlinePipeline];
        [encoder setDepthStencilState:_outlineDepth];
        [encoder setVertexBytes:outline.data() length:sizeof(outline) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:outline.size()];
    }
    [encoder endEncoding];
    return YES;
}

@end
