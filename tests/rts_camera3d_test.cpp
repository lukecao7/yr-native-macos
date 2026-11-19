// SPDX-License-Identifier: GPL-3.0-only
#include "rts_3d/scene.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>

namespace {

using namespace coastal::rts;
namespace view = coastal::rts::view3d;

void require(bool condition, const char* detail) {
    if (condition) return;
    std::cerr << detail << '\n';
    std::exit(EXIT_FAILURE);
}

Map make_board(int width, int height) {
    Map map;
    map.width = width;
    map.height = height;
    map.tiles.resize(std::size_t(width) * height);
    return map;
}

Map raised_board() {
    auto map = make_board(54, 42);
    for (int z = 0; z < map.height; ++z) {
        for (int x = 0; x < map.width; ++x) {
            auto& tile = map.at({x, z});
            tile.height = static_cast<std::uint8_t>((x >= 27) + (x >= 31));
            if (z < 5 && x < 15) tile.terrain = Terrain::Water;
            else if (x >= 26 && x <= 31) tile.terrain = Terrain::Ramp;
            else if (z == 23 && x >= 15 && x <= 18) tile = {Terrain::Deposit, 0, 80};
            else if (z == 25 && x >= 20 && x <= 23) tile.terrain = Terrain::Rock;
        }
    }
    return map;
}

std::size_t compare_projection_and_picking() {
    constexpr Cell center{26, 21};
    const auto map = raised_board();
    struct Sample {
        double yaw;
        double pitch;
        double distance;
        int width;
        int height;
    };
    const std::array samples{
        Sample{0.785, 0.87, 48, 1024, 768},
        Sample{-0.90, 1.18, 71, 853, 641},
        Sample{2.20, 0.78, 93, 1024, 768},
        Sample{-2.90, 1.12, 59, 853, 641},
    };
    std::size_t compared = 0;
    std::size_t raised = 0;
    std::size_t ramps = 0;
    std::size_t water = 0;
    for (const auto& sample : samples) {
        view::OrbitCamera camera;
        camera.yaw = sample.yaw;
        camera.pitch = sample.pitch;
        camera.distance = sample.distance;
        camera.center_on(map, center);
        const auto pose = camera.pose(sample.width, sample.height);

        Camera iso;
        iso.zoom = 0.75;
        iso.center_on(center, map);

        for (int index = 0; index < static_cast<int>(map.tiles.size()); ++index) {
            const auto cell = map.cell(index);
            const auto projected = view::project_point(pose, view::tile_anchor(map, cell));
            if (!projected) continue;
            const auto picked = view::pick_terrain(map, pose, *projected);
            if (picked != cell) {
                std::cerr << "3D pick differed at yaw=" << sample.yaw
                          << " pitch=" << sample.pitch << " cell=(" << cell.x << ',' << cell.y
                          << ") pixel=(" << projected->x << ',' << projected->y << ')';
                if (picked) std::cerr << " picked=(" << picked->x << ',' << picked->y << ')';
                std::cerr << '\n';
                std::exit(EXIT_FAILURE);
            }
            const auto point = project_cell(map, iso, cell, sample.width, sample.height);
            if (point.x < 0 || point.x >= sample.width || point.y < 0 || point.y >= sample.height) continue;
            require(pick_cell(map, iso, point, sample.width, sample.height) == picked,
                    "Both viewport modes select the same original cell");

            ++compared;
            raised += map.at(cell).height > 0;
            ramps += map.at(cell).terrain == Terrain::Ramp;
            water += map.at(cell).terrain == Terrain::Water;
        }
    }
    require(compared >= 4800 && raised > 0 && ramps > 0 && water > 0,
            "Round trips cover thousands of cells, including ramps, raised ground and water");
    return compared;
}

void clip_and_orient() {
    constexpr int width = 1001;
    constexpr int height = 751;
    const auto map = make_board(5, 5);
    view::OrbitCamera camera;
    camera.distance = 12;
    camera.center_on(map, {2, 2});
    const auto pose = camera.pose(width, height);
    const auto center = view::project_point(pose, camera.target);
    require(center && std::abs(center->x - width / 2) <= 1 && std::abs(center->y - height / 2) <= 1,
            "The orbit target is the projected screen center");

    const auto right = view::project_point(pose, view::tile_anchor(map, {3, 2}));
    const auto left = view::project_point(pose, view::tile_anchor(map, {2, 3}));
    require(right && left && right->x > center->x && left->x < center->x &&
            right->y > center->y && left->y > center->y,
            "The default orientation uses screen-right X and screen-down depth");
    const auto outside = view::project_point(pose, {5.5, 0, 2.5});
    require(outside && !view::pick_terrain(map, pose, *outside),
            "A visible ray beyond the terrain edge has no hit");
    require(!view::pick_terrain(map, pose, {-1, 0}) &&
            !view::pick_terrain(map, pose, {width, 0}) &&
            !view::pick_terrain(map, pose, {0, height}) &&
            !view::pick_terrain(Map{}, pose, *center) &&
            !view::pick_terrain(map, view::CameraPose{}, *center),
            "Invalid maps, viewports and outside pixels have no hit");

    const double back = 2 * camera.distance * std::cos(camera.pitch);
    const view::Vec3 behind{camera.target.x + back * std::sin(camera.yaw),
                            camera.target.y + 2 * camera.distance * std::sin(camera.pitch),
                            camera.target.z + back * std::cos(camera.yaw)};
    require(!view::project_point(pose, behind) && !view::project_point(pose, {1e6, 0, 0}),
            "Behind-camera and off-screen points are clipped");
}

void camera_controls() {
    view::OrbitCamera camera;
    camera.yaw = camera.goal_yaw = std::numbers::pi - 0.02;
    const double old_yaw = camera.yaw;
    const double old_pitch = camera.pitch;
    const double old_distance = camera.distance;
    camera.orbit(-28, 22);
    camera.zoom_by(1.5);
    require(camera.yaw == old_yaw && camera.pitch == old_pitch && camera.distance == old_distance,
            "Input sets target values before interpolation");
    camera.advance();
    const double yaw_delta = std::remainder(camera.yaw - old_yaw, 2 * std::numbers::pi);
    require(yaw_delta > 0 && yaw_delta < 0.224 &&
            camera.pitch > old_pitch && camera.pitch < camera.goal_pitch &&
            camera.distance < old_distance && camera.distance > camera.goal_distance,
            "Orbit crosses the angle seam smoothly and larger scale zooms in");

    camera.orbit(0, 1e8);
    camera.zoom_by(1e8);
    const double goal_yaw = camera.goal_yaw;
    const double goal_pitch = camera.goal_pitch;
    const double goal_distance = camera.goal_distance;
    camera.orbit(std::numeric_limits<double>::infinity(), 1);
    camera.zoom_by(-1);
    camera.zoom_by(std::numeric_limits<double>::quiet_NaN());
    require(camera.goal_yaw == goal_yaw && camera.goal_pitch == goal_pitch &&
            camera.goal_distance == goal_distance && camera.goal_pitch < std::numbers::pi / 2 &&
            camera.goal_distance > 0,
            "Invalid input is ignored and extreme input is bounded");
    for (int step = 0; step < 120; ++step) camera.advance();
    require(camera.yaw == camera.goal_yaw && camera.pitch == camera.goal_pitch &&
            camera.distance == camera.goal_distance,
            "Interpolation settles at its target");
}

void large_board() {
    const auto map = make_board(512, 512);
    view::OrbitCamera camera;
    camera.distance = 116;
    camera.center_on(map, {256, 256});
    const auto pose = camera.pose(1024, 768);
    for (int y = 224; y <= 290; y += 3) {
        for (int x = 226; x <= 285; x += 3) {
            const Cell cell{x, y};
            const auto pixel = view::project_point(pose, view::tile_anchor(map, cell));
            require(pixel && view::pick_terrain(map, pose, *pixel) == cell,
                    "The default thousand-unit framing keeps the entire formation pickable");
        }
    }
}

}  // namespace

int main() {
    const auto compared = compare_projection_and_picking();
    clip_and_orient();
    camera_controls();
    large_board();
    std::cout << "RTS 3D camera tests passed (" << compared
              << " matching 2D/3D cell picks, zero mismatches)\n";
}
