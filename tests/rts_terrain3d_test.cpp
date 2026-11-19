// SPDX-License-Identifier: GPL-3.0-only
#include "rts_3d/scene.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

using namespace coastal::rts;
using namespace coastal::rts::view3d;

void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

Map board(int width, int height) {
    Map map;
    map.width = width;
    map.height = height;
    map.tiles.resize(std::size_t(width) * height);
    return map;
}

bool same_position(Vec4 a, Vec4 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

void test_shared_ramp_edges_and_normals() {
    auto map = board(3, 2);
    for (int z = 0; z < map.height; ++z) {
        map.at({1, z}) = {Terrain::Ramp, 1, 0};
        map.at({2, z}).height = 2;
    }
    const auto left = tile_heights(map, {0, 0});
    const auto ramp = tile_heights(map, {1, 0});
    const auto right = tile_heights(map, {2, 0});
    const auto below = tile_heights(map, {1, 1});
    require(ramp == std::array{0.5f, 1.5f, 0.5f, 1.5f}, "The ramp ascends between authored terraces");
    require(left[1] == ramp[0] && left[3] == ramp[2] && ramp[1] == right[0] &&
                ramp[3] == right[2] && ramp[2] == below[0] && ramp[3] == below[1],
            "Neighboring top surfaces meet at identical corner heights");

    const auto mesh = make_terrain_mesh(map);
    require(mesh && mesh->vertices.size() == 36, "Sloping cells retain their separate triangles");
    const auto& corner = mesh->vertices[6];
    const auto& adjacent = mesh->vertices[7];
    require(same_position(corner.position, {1, 0.5f, 0, 1}) &&
                same_position(adjacent.position, {1, 0.5f, 1, 1}) &&
                same_position(mesh->vertices[8].position, {2, 1.5f, 0, 1}),
            "Vertex geometry uses the same corner order as the picker");
    const auto n = corner.normal;
    require(n.x < 0 && n.y > 0 && n.z == 0 &&
                std::abs(n.x * n.x + n.y * n.y - 1) < 0.00001f,
            "Normals are normalized and face up on a sloping tile");
}

void test_anchor_on_nonplanar_tile() {
    auto map = board(2, 2);
    map.at({1, 1}).height = 4;
    require(tile_heights(map, {0, 0}) == std::array{0.0f, 0.0f, 0.0f, 1.0f},
            "The raised corner makes the sample tile nonplanar");
    const auto anchor = tile_anchor(map, {0, 0});
    require(anchor.x == 0.5 && anchor.y == 0 && anchor.z == 0.5,
            "The center lies on the shared diagonal, on the drawn surface");
}

void test_materials_and_large_flat_runs() {
    auto map = board(Map::max_dimension, Map::max_dimension);
    for (int z = 0; z < map.height; ++z) {
        for (int x = 0; x < map.width / 2; ++x) map.at({x, z}).terrain = Terrain::Water;
    }
    const auto mesh = make_terrain_mesh(map);
    require(mesh && mesh->vertices.size() == 12,
            "A 512-square plane of two materials coalesces into two quads");
    const auto& water = mesh->vertices[0];
    const auto& ground = mesh->vertices[6];
    require(water.normal.w == 1 && ground.normal.w == 0 && water.color.z > ground.color.z,
            "Water carries its own shading marker and palette");

    auto samples = board(2, 1);
    samples.at({0, 0}).terrain = Terrain::Rock;
    samples.at({1, 0}).terrain = Terrain::Deposit;
    const auto details = make_terrain_mesh(samples);
    require(details && details->vertices.size() == 12 &&
                details->vertices[0].color.x < details->vertices[6].color.x &&
                details->vertices[0].normal.w == 0 && details->vertices[6].normal.w == 0,
            "Rock and deposit tiles retain distinct land materials");
}

void test_incomplete_maps() {
    auto map = board(2, 2);
    const auto anchor = tile_anchor(map, {-1, 0});
    require(anchor.x == 0 && anchor.y == 0 && anchor.z == 0,
            "Outside cells have no terrain anchor");
    map.tiles.pop_back();
    require(!make_terrain_mesh(map) && tile_heights(map, {1, 1}) == std::array<float, 4>{},
            "Incomplete map storage is rejected");
}

}  // namespace

int main() {
    try {
        test_shared_ramp_edges_and_normals();
        test_anchor_on_nonplanar_tile();
        test_materials_and_large_flat_runs();
        test_incomplete_maps();
        std::cout << "RTS terrain 3D tests passed (shared heights, normals, materials and mesh coalescing)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
