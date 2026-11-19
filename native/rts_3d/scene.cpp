// SPDX-License-Identifier: GPL-3.0-only
#include "scene.h"

#include <algorithm>
#include <cmath>

namespace coastal::rts::view3d {
namespace {

// Full model dimensions in tile units. Compact footprints leave space between
// adjacent units, with taller profiles for structures and heavier vehicles.
constexpr std::array<Vec4, static_cast<std::size_t>(UnitKind::Count)> model_sizes{{
    {0.77F, 0.37F, 0.55F, 0}, // Pioneer
    {0.90F, 0.64F, 0.84F, 0}, // Hub
    {0.51F, 0.74F, 0.48F, 0}, // Generator
    {0.83F, 0.38F, 0.79F, 0}, // Depot
    {0.88F, 0.50F, 0.67F, 0}, // Workshop
    {0.75F, 0.40F, 0.53F, 0}, // Collector
    {0.58F, 0.30F, 0.38F, 0}, // Skirmisher
    {0.72F, 0.46F, 0.57F, 0}, // Sentinel
    {0.84F, 0.42F, 0.44F, 0}, // Artillery
}};

constexpr Vec4 blue{0.28F, 0.66F, 0.93F, 0};
constexpr Vec4 copper{0.90F, 0.53F, 0.26F, 0};

}  // namespace

void update_scene(SceneFrame& scene, const World& world, const CameraPose& camera,
                  std::optional<SelectionBox> drag, Side viewer) {
    scene.camera = camera;
    scene.drag = drag;
    scene.visible_units = 0;
    scene.step = world.step;
    scene.instances.clear();
    scene.instances.reserve(world.entities.size());

    const auto& selected = world.side(viewer).selected;
    for (const auto& entity : world.entities) {
        if (!entity.hp || entity.kind >= UnitKind::Count || !world.map.contains(entity.position)) continue;
        const auto anchor = tile_anchor(world.map, entity.position);
        const auto yaw = std::atan2(float(entity.facing.y), float(entity.facing.x));

        auto size = model_sizes[static_cast<std::size_t>(entity.kind)];
        const auto full_hp = world.rules.spec(entity.kind).hp;
        size.w = full_hp ? float(std::min(entity.hp, full_hp)) / full_hp : 0.0F;

        auto paint = entity.side == Side::Blue ? blue : copper;
        paint.w = entity.side == viewer && std::binary_search(selected.begin(), selected.end(), entity.id)
                      ? 1.0F : 0.0F;
        scene.instances.push_back({{float(anchor.x), float(anchor.y), float(anchor.z), yaw}, size, paint});
        if (project_point(camera, {anchor.x, anchor.y + size.y * 0.5, anchor.z})) {
            ++scene.visible_units;
        }
    }
}

}  // namespace coastal::rts::view3d
