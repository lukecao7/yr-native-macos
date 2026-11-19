// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#import <Metal/Metal.h>

#include "rts_3d/scene.h"

@interface CoastalMetalScene : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device pixelFormat:(MTLPixelFormat)format;
- (BOOL)prepareTerrain:(const std::shared_ptr<const coastal::rts::view3d::TerrainMesh>&)terrain;
- (BOOL)encodeScene:(const coastal::rts::view3d::SceneFrame&)scene
            target:(id<MTLTexture>)color
           command:(id<MTLCommandBuffer>)command
              slot:(NSUInteger)slot;
@end
