// SPDX-License-Identifier: GPL-3.0-only
#include "rts_3d/scene.h"

#include "rts_core/snapshot.h"
#include "rts_core/stress.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>

namespace {

using namespace coastal::rts;
using namespace coastal::rts::view3d;

constexpr int viewport_width = 1024;
constexpr int viewport_height = 768;

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

bool close(float actual, double expected) {
    return std::abs(actual - expected) < 1e-5;
}

void model_attributes() {
    World world;
    world.map.width = world.map.height = 16;
    world.map.tiles.resize(16 * 16);
    world.map.at({7, 7}) = {Terrain::Ramp, 1, 0};
    world.map.at({8, 7}) = {Terrain::Ground, 2, 0};
    world.rules.spec(UnitKind::Skirmisher).hp = 72;
    world.rules.spec(UnitKind::Generator).hp = 115;

    const auto fighter = world.spawn(UnitKind::Skirmisher, Side::Blue, {7, 7}).value();
    const auto generator = world.spawn(UnitKind::Generator, Side::Copper, {9, 6}).value();
    const auto destroyed = world.spawn(UnitKind::Skirmisher, Side::Blue, {7, 8}).value();
    world.find(fighter)->hp = 18;
    world.find(fighter)->facing = {0, 1};
    world.find(generator)->facing = {-1, 0};
    world.find(destroyed)->hp = 0;
    world.side(Side::Blue).selected = {fighter};
    world.side(Side::Copper).selected = {generator};
    world.step = 90;

    OrbitCamera camera;
    camera.center_on(world.map, {7, 7});
    const auto pose = camera.pose(viewport_width, viewport_height);
    SceneFrame scene;
    scene.terrain = make_terrain_mesh(world.map);
    const auto mesh = scene.terrain;
    const SelectionBox drag{{20, 10}, {50, 60}};
    update_scene(scene, world, pose, drag);

    require(scene.instances.size() == 2 && scene.visible_units == 2,
            "Living mobile and stationary entities both enter the frame");
    require(scene.step == 90 && scene.terrain == mesh && scene.camera.width == viewport_width &&
            scene.drag && scene.drag->start.x == 20 && scene.drag->end.y == 60,
            "Frame extraction preserves the terrain and copies the camera and drag state");

    const auto& blue = scene.instances[0];
    const auto& copper = scene.instances[1];
    const auto expected = tile_anchor(world.map, {7, 7});
    require(close(blue.anchor_yaw.x, expected.x) && close(blue.anchor_yaw.y, expected.y) &&
            close(blue.anchor_yaw.z, expected.z), "Body origin matches the terrain surface");
    require(close(blue.anchor_yaw.w, std::numbers::pi / 2) &&
            close(copper.anchor_yaw.w, std::numbers::pi), "Heading matches the unit's grid direction");
    require(close(blue.size_health.w, 0.25) && close(copper.size_health.w, 1.0) &&
            copper.size_health.y > blue.size_health.y, "Health fraction and geometry reflect the entity");
    require(blue.paint.z > blue.paint.x && copper.paint.x > copper.paint.z &&
            blue.paint.w == 1 && copper.paint.w == 0, "Faction tint and viewer selection are independent");

    update_scene(scene, world, pose, std::nullopt, Side::Copper);
    require(!scene.drag && scene.instances[0].paint.w == 0 && scene.instances[1].paint.w == 1,
            "Changing viewer updates selection and clears a stale drag overlay");
}

void stress_views_do_not_modify_world() {
    const auto world = make_stress_match();
    const auto saved = encode_snapshot(world);
    const Cell center{world.map.width / 2, world.map.height / 2};

    OrbitCamera orbit;
    orbit.center_on(world.map, center);
    orbit.distance = orbit.goal_distance = 116;

    SceneFrame scene;
    scene.terrain = make_terrain_mesh(world.map);
    const auto mesh = scene.terrain;
    update_scene(scene, world, orbit.pose(viewport_width, viewport_height), std::nullopt);
    require(scene.instances.size() == 1200 && scene.visible_units >= 1000,
            "At least a thousand of the authored stress units are on screen");
    const auto* storage = scene.instances.data();

    Camera isometric;
    isometric.zoom = isometric.target_zoom = 0.55;
    isometric.center_on(center, world.map);
    std::vector<std::uint8_t> pixels(viewport_width * viewport_height * 4);
    for (int view = 0; view < 12; ++view) {
        if (view % 2 == 0) {
            require(render_isometric(world, isometric, {}, pixels, viewport_width, viewport_height),
                    "The same world also draws in the isometric view");
        } else {
            orbit.orbit(14, view % 4 == 1 ? 3 : -3);
            orbit.zoom_by(view % 4 == 1 ? 0.96 : 1.045);
            orbit.advance();
            update_scene(scene, world, orbit.pose(viewport_width, viewport_height), std::nullopt);
            require(scene.instances.size() == 1200 && scene.instances.data() == storage &&
                    scene.terrain == mesh, "View changes reuse the scene instance buffer and terrain");
        }
        require(encode_snapshot(world) == saved, "Switching views keeps the full world byte-identical");
    }

    orbit.center_on(world.map, {24, 24});
    update_scene(scene, world, orbit.pose(viewport_width, viewport_height), std::nullopt);
    require(scene.instances.size() == 1200 && scene.visible_units == 0,
            "Entities outside the camera are retained but not counted as visible");
}

}  // namespace

int main() {
    model_attributes();
    stress_views_do_not_modify_world();
    std::cout << "3D scene tests passed (terrain anchors, instance data, visibility and view parity)\n";
}
