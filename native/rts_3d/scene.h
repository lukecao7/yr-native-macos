// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "rts_core/iso.h"

#include <array>
#include <memory>

namespace coastal::rts::view3d {

struct Vec3 {
    double x = 0;
    double y = 0;
    double z = 0;
};

struct alignas(16) Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};

struct TerrainVertex {
    Vec4 position;
    Vec4 normal; // The fourth component marks water for the shader.
    Vec4 color;
};

struct TerrainMesh {
    std::vector<TerrainVertex> vertices;
};

// Corners are (x,z), (x+1,z), (x,z+1), (x+1,z+1).
// Terrain is emitted as triangles (0,2,1) and (1,2,3).
std::array<float, 4> tile_heights(const Map& map, Cell cell) noexcept;
Vec3 tile_anchor(const Map& map, Cell cell) noexcept;
std::shared_ptr<const TerrainMesh> make_terrain_mesh(const Map& map);

struct CameraPose {
    // Column-major matrices. Metal normalized clip z is in [0,1].
    std::array<float, 16> projection{};
    std::array<double, 16> inverse{};
    Vec4 right;
    Vec4 up;
    int width = 0;
    int height = 0;
};

struct OrbitCamera {
    Vec3 target;
    double yaw = 0.7853981634;
    double pitch = 0.75;
    double distance = 68;
    double goal_yaw = yaw;
    double goal_pitch = pitch;
    double goal_distance = distance;

    void center_on(const Map& map, Cell cell) noexcept;
    void orbit(double dx, double dy) noexcept;
    void zoom_by(double scale) noexcept;
    void advance() noexcept;
    CameraPose pose(int width, int height) const noexcept;
};

std::optional<PixelPoint> project_point(const CameraPose& camera, Vec3 point) noexcept;
std::optional<Cell> pick_terrain(const Map& map, const CameraPose& camera, PixelPoint pixel) noexcept;

struct ModelInstance {
    Vec4 anchor_yaw; // World x, y, z, and heading in radians.
    Vec4 size_health; // Width, height, depth, and health ratio.
    Vec4 paint; // RGB and a selection flag.
};

struct SceneFrame {
    std::shared_ptr<const TerrainMesh> terrain;
    CameraPose camera;
    std::vector<ModelInstance> instances;
    std::optional<SelectionBox> drag;
    std::size_t visible_units = 0;
    std::uint64_t step = 0;
};

// Reuse derived buffers without modifying world authority.
void update_scene(SceneFrame& scene, const World& world, const CameraPose& camera,
                  std::optional<SelectionBox> drag, Side viewer = Side::Blue);

}  // namespace coastal::rts::view3d
