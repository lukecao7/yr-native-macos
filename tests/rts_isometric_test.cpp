// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/iso.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>

using namespace coastal::rts;

namespace {

class FixtureFiles {
public:
    FixtureFiles() {
        std::random_device suffix;
        directory_ = std::filesystem::temp_directory_path() /
                     ("coastal-isometric-" + std::to_string(suffix()) + "-" + std::to_string(suffix()));
        if (!std::filesystem::create_directory(directory_)) throw std::runtime_error("Fixture directory exists");
    }

    ~FixtureFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path write_map(std::string_view contents) const {
        const auto path = directory_ / "fixture.map";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << contents;
        file.close();
        if (!file) throw std::runtime_error("Unable to write the test map");
        return path;
    }

private:
    std::filesystem::path directory_;
};

constexpr std::string_view landscape =
    "COASTAL_MAP 1\r\n"
    "8 6 365\r\n"
    "........\r\n"
    "..../=..\r\n"
    "..a.#...\r\n"
    "....~o..\r\n"
    "......b.\r\n"
    "........\r\n";

constexpr int width = 768;
constexpr int height = 512;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Map map_fixture(const FixtureFiles& files) {
    std::string error;
    const auto map = load_map(files.write_map(landscape), error);
    expect(map && error.empty(), "The authored map loads using CRLF line endings");
    expect(map->width == 8 && map->height == 6 && map->tiles.size() == 48,
           "Map dimensions determine exactly how many tiles are loaded");
    expect(map->starts[0] == Cell{2, 2} && map->starts[1] == Cell{6, 4},
           "Each side retains its distinct map start");
    expect(map->at({4, 1}).terrain == Terrain::Ramp && map->at({4, 1}).height == 1 &&
           map->at({5, 1}).height == 2 && map->walkable({4, 1}),
           "A traversable ramp and raised tile retain their heights");
    expect(map->at({4, 2}).terrain == Terrain::Rock && !map->walkable({4, 2}) &&
           map->at({4, 3}).terrain == Terrain::Water && !map->walkable({4, 3}),
           "Rock and water block movement");
    expect(map->at({5, 3}).terrain == Terrain::Deposit && map->at({5, 3}).resource == 365,
           "The rules for the resource amount come from the authored map");
    return *map;
}

void reject_invalid_maps(const FixtureFiles& files) {
    constexpr std::array invalid{
        "OTHER_MAP 1\n2 1 0\nab",
        "COASTAL_MAP 2\n2 1 0\nab",
        "COASTAL_MAP 1\n513 1 0\nab",
        "COASTAL_MAP 1\n2 1 65536\nab",
        "COASTAL_MAP 1\n2 2 0\nab\n",
        "COASTAL_MAP 1\n2 1 0\na",
        "COASTAL_MAP 1\n2 1 0\naba",
        "COASTAL_MAP 1\n3 1 0\naba",
        "COASTAL_MAP 1\n3 1 0\nab%",
        "COASTAL_MAP 1\n2 1 0\nab\n..",
        "COASTAL_MAP 1\n2 1 0 extra\nab\n",
    };
    for (const auto* contents : invalid) {
        std::string error;
        expect(!load_map(files.write_map(contents), error) && !error.empty(),
               "Invalid map input is rejected with an explanation");
    }
    std::string error;
    const auto tiny = load_map(files.write_map("COASTAL_MAP 1\n2 1 0\nab"), error);
    expect(tiny && tiny->width == 2, "A correct last row does not require a final newline");
}

void check_projection(const Map& map) {
    Camera camera;
    camera.center_on(map.starts[0], map);
    const auto start = project_cell(map, camera, map.starts[0], width, height);
    expect(start.x == width / 2 && start.y == height / 2, "Centering uses the tile foot anchor");

    auto round_trip = [&] {
        for (int y = 0; y < map.height; ++y) {
            for (int x = 0; x < map.width; ++x) {
                const Cell cell{x, y};
                const auto pixel = project_cell(map, camera, cell, width, height);
                expect(pick_cell(map, camera, pixel, width, height) == cell,
                       "Every projected tile center picks that tile, including raised terrain");
            }
        }
    };
    round_trip();
    expect(!pick_cell(map, camera, {-1, 20}, width, height) &&
           !pick_cell(map, camera, {width, 20}, width, height) &&
           !pick_cell(map, camera, {0, 0}, width, height), "Out-of-map or out-of-window clicks miss");

    camera.zoom_by(1.7);
    camera.advance();
    expect(camera.zoom > 1 && camera.zoom < 1.7, "The camera gradually changes scale");
    const auto anchor = project_cell(map, camera, map.starts[0], width, height);
    expect(anchor.x == start.x && anchor.y == start.y, "Zoom is anchored to the middle of the view");
    round_trip();

    const double old_x = camera.x;
    const double old_y = camera.y;
    camera.pan(38, -22);
    camera.advance();
    expect(camera.x > old_x && camera.y < old_y && camera.x < camera.target_x &&
           camera.y > camera.target_y, "Pan moves incrementally toward its destination");
    round_trip();

    camera.zoom_by(0.0001);
    for (int step = 0; step < 90; ++step) camera.advance();
    expect(camera.zoom == 0.55, "Extreme zoom requests stop at the supported minimum");
    round_trip();

    camera.zoom_by(10);
    for (int step = 0; step < 90; ++step) camera.advance();
    expect(camera.zoom == 2, "Extreme zoom requests stop at the supported maximum");
    round_trip();
}

using Color = std::array<std::uint8_t, 4>;

Color sample(std::span<const std::uint8_t> rgba, int x, int y) {
    Color rgba_color{};
    std::copy_n(rgba.begin() + ((std::size_t(y) * width + x) * rgba_color.size()),
                rgba_color.size(), rgba_color.begin());
    return rgba_color;
}

void check_layers(Map map) {
    World world;
    world.map = std::move(map);
    world.rules.spec(UnitKind::Sentinel).hp = 100;
    world.rules.spec(UnitKind::Workshop).hp = 120;
    Entity unit{};
    unit.id = 1;
    unit.kind = UnitKind::Sentinel;
    unit.side = Side::Blue;
    unit.position = world.map.starts[0];
    unit.hp = 25;
    world.entities.push_back(unit);
    world.sides[0].selected.push_back(unit.id);
    Entity building{};
    building.id = 2;
    building.kind = UnitKind::Workshop;
    building.side = Side::Copper;
    building.position = world.map.starts[1];
    building.hp = 120;
    world.entities.push_back(building);
    world.projectiles.push_back({1, 2, {3, 0}, {5, 0}, 6, 12, 12});

    Camera camera;
    camera.center_on(unit.position, world.map);
    const auto project = [&](Cell cell) { return project_cell(world.map, camera, cell, width, height); };
    ViewOverlay overlay;
    overlay.build_cell = Cell{4, 4};
    overlay.can_build = true;
    overlay.drag = SelectionBox{{172, 111}, {49, 65}};
    const std::size_t frame_size = std::size_t(width) * height * 4;
    std::vector<std::uint8_t> guarded(frame_size + 2, 0x7b);
    auto rgba = std::span(guarded).subspan(1, frame_size);
    expect(render_isometric(world, camera, overlay, rgba, width, height), "The scene is rendered");
    expect(guarded.front() == 0x7b && guarded.back() == 0x7b, "Rendering does not write outside the frame");
    for (std::size_t offset = 3; offset < rgba.size(); offset += 4) {
        if (rgba[offset] != 255) throw std::runtime_error("Every rendered pixel is opaque");
    }
    expect(sample(rgba, 2, 2) == Color{20, 33, 43, 255}, "Outside terrain uses the scene background");
    expect(sample(rgba, 70, 65) == Color{224, 244, 237, 255}, "Reversed drag corners retain the selection border");
    const auto preview = project(*overlay.build_cell);
    expect(sample(rgba, preview.x, preview.y - 8) == Color{129, 232, 162, 255},
           "A valid placement preview is visible in green");
    const auto anchor = project(unit.position);
    expect(sample(rgba, anchor.x - 6, anchor.y - 12) == Color{241, 126, 99, 255},
           "The damaged unit paints its health bar");
    expect(sample(rgba, anchor.x - 14, anchor.y) == Color{224, 244, 237, 255},
           "A selected unit is outlined on the ground");
    const auto from = project({3, 0});
    const auto to = project({5, 0});
    expect(sample(rgba, (from.x + to.x) / 2, (from.y + to.y) / 2 - 7) == Color{235, 180, 89, 255},
           "A projectile is drawn partway between source and target");
    constexpr int minimap_extent = 164;
    constexpr int minimap_left = width - 12 - minimap_extent;
    constexpr int minimap_top = height - 12 - (minimap_extent * 6 / 8);
    expect(sample(rgba, minimap_left + building.position.x * minimap_extent / 8,
                  minimap_top + building.position.y * (minimap_extent * 6 / 8) / 6) ==
               Color{236, 146, 103, 255}, "The opposing side is visible on the minimap");

    std::vector<std::uint8_t> repeated(frame_size);
    expect(render_isometric(world, camera, overlay, repeated, width, height) &&
           std::ranges::equal(rgba, repeated), "Identical scene state produces byte-identical pixels");

    overlay.can_build = false;
    expect(render_isometric(world, camera, overlay, rgba, width, height) &&
           sample(rgba, preview.x, preview.y - 8) == Color{247, 121, 106, 255},
           "An invalid placement preview is visible in red");

    Camera invalid_camera = camera;
    invalid_camera.zoom = std::numeric_limits<double>::quiet_NaN();
    expect(!render_isometric(world, invalid_camera, overlay, rgba, width, height) &&
           !render_isometric(world, camera, overlay, rgba.first(rgba.size() - 1), width, height),
           "Invalid cameras and mismatched frame sizes are rejected");
}

}  // namespace

int main() {
    try {
        const FixtureFiles files;
        const Map map = map_fixture(files);
        reject_invalid_maps(files);
        check_projection(map);
        check_layers(map);
        std::cout << "RTS isometric tests passed (map, camera, picking and rendered layers)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rts_isometric_test: " << error.what() << '\n';
        return 1;
    }
}
