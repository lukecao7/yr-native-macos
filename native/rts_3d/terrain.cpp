// SPDX-License-Identifier: GPL-3.0-only
#include "scene.h"

#include <algorithm>
#include <cmath>

namespace coastal::rts::view3d {
namespace {

bool valid_map(const Map& map) noexcept {
    return map.width > 0 && map.width <= Map::max_dimension &&
           map.height > 0 && map.height <= Map::max_dimension &&
           map.tiles.size() == std::size_t(map.width) * map.height;
}

// Every cell using this lattice corner gets the same height. Around a ramp
// the adjacent low and high cells define its direction without a separate
// orientation field in the map.
float corner_height(const Map& map, int x, int z) noexcept {
    unsigned sum = 0;
    unsigned count = 0;
    for (int row = std::max(0, z - 1); row < std::min(map.height, z + 1); ++row) {
        for (int column = std::max(0, x - 1); column < std::min(map.width, x + 1); ++column) {
            sum += map.at({column, row}).height;
            ++count;
        }
    }
    return static_cast<float>(sum) / count;
}

Vec4 paint(Terrain terrain) noexcept {
    switch (terrain) {
    case Terrain::Ramp: return {.52f, .49f, .33f, 1};
    case Terrain::Water: return {.11f, .32f, .43f, 1};
    case Terrain::Rock: return {.36f, .38f, .37f, 1};
    case Terrain::Deposit: return {.70f, .53f, .24f, 1};
    case Terrain::Ground: return {.37f, .49f, .32f, 1};
    }
    return {.37f, .49f, .32f, 1};
}

void add_triangle(TerrainMesh& mesh, Vec4 a, Vec4 b, Vec4 c, Terrain terrain) {
    const float bx = b.x - a.x;
    const float by = b.y - a.y;
    const float bz = b.z - a.z;
    const float cx = c.x - a.x;
    const float cy = c.y - a.y;
    const float cz = c.z - a.z;
    const float nx = by * cz - bz * cy;
    const float ny = bz * cx - bx * cz;
    const float nz = bx * cy - by * cx;
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    const Vec4 normal{nx / length, ny / length, nz / length,
                      terrain == Terrain::Water ? 1.0f : 0.0f};
    const auto color = paint(terrain);
    for (const auto position : {a, b, c}) mesh.vertices.push_back({position, normal, color});
}

void add_quad(TerrainMesh& mesh, int x, int z, int end_x, int end_z,
              const std::array<float, 4>& heights, Terrain terrain) {
    const Vec4 a{float(x), heights[0], float(z), 1};
    const Vec4 b{float(end_x), heights[1], float(z), 1};
    const Vec4 c{float(x), heights[2], float(end_z), 1};
    const Vec4 d{float(end_x), heights[3], float(end_z), 1};
    add_triangle(mesh, a, c, b, terrain);
    add_triangle(mesh, b, c, d, terrain);
}

}  // namespace

std::array<float, 4> tile_heights(const Map& map, Cell cell) noexcept {
    if (!valid_map(map) || !map.contains(cell)) return {};
    return {corner_height(map, cell.x, cell.y),
            corner_height(map, cell.x + 1, cell.y),
            corner_height(map, cell.x, cell.y + 1),
            corner_height(map, cell.x + 1, cell.y + 1)};
}

Vec3 tile_anchor(const Map& map, Cell cell) noexcept {
    if (!valid_map(map) || !map.contains(cell)) return {};
    // The center belongs to the shared edge between corners 1 and 2.
    const float east = corner_height(map, cell.x + 1, cell.y);
    const float south = corner_height(map, cell.x, cell.y + 1);
    return {cell.x + 0.5, (east + south) * 0.5, cell.y + 0.5};
}

std::shared_ptr<const TerrainMesh> make_terrain_mesh(const Map& map) {
    if (!valid_map(map)) return {};
    auto mesh = std::make_shared<TerrainMesh>();
    const int stride = map.width + 1;
    std::vector<float> corners(std::size_t(stride) * (map.height + 1));
    for (int z = 0; z <= map.height; ++z) {
        for (int x = 0; x <= map.width; ++x) corners[z * stride + x] = corner_height(map, x, z);
    }
    const auto heights_at = [&](Cell cell) {
        const int index = cell.y * stride + cell.x;
        return std::array{corners[index], corners[index + 1],
                          corners[index + stride], corners[index + stride + 1]};
    };

    // A flat patch of one material has identical normals and color throughout,
    // so contiguous equal patches can be drawn as large quads. Slopes always
    // keep the exact per-cell triangles that picking tests.
    std::vector<std::uint8_t> covered(map.tiles.size(), 0);
    for (int z = 0; z < map.height; ++z) {
        for (int x = 0; x < map.width; ++x) {
            const Cell cell{x, z};
            if (covered[map.index(cell)]) continue;

            const auto terrain = map.at(cell).terrain;
            const auto heights = heights_at(cell);
            const std::array<float, 4> levelled{heights[0], heights[0], heights[0], heights[0]};
            if (heights != levelled) {
                add_quad(*mesh, x, z, x + 1, z + 1, heights, terrain);
                continue;
            }

            const auto mergeable = [&](Cell candidate) {
                const int index = map.index(candidate);
                return !covered[index] && map.tiles[index].terrain == terrain &&
                       heights_at(candidate) == levelled;
            };
            int end_x = x + 1;
            while (end_x < map.width && mergeable({end_x, z})) ++end_x;
            int end_z = z + 1;
            for (; end_z < map.height; ++end_z) {
                int column = x;
                while (column < end_x && mergeable({column, end_z})) ++column;
                if (column < end_x) break;
            }
            for (int row = z; row < end_z; ++row) {
                std::fill_n(covered.begin() + map.index({x, row}), end_x - x, 1);
            }
            add_quad(*mesh, x, z, end_x, end_z, heights, terrain);
        }
    }
    return mesh;
}

}  // namespace coastal::rts::view3d
