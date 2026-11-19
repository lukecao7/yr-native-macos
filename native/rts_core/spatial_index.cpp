// SPDX-License-Identifier: GPL-3.0-only
#include "spatial_index.h"

#include <algorithm>
#include <cstdlib>

namespace coastal::rts {

void SpatialIndex::rebuild(const World& world) {
    const bool valid = world.map.width > 0 && world.map.width <= Map::max_dimension &&
                       world.map.height > 0 && world.map.height <= Map::max_dimension;
    const int width = valid ? world.map.width : 0;
    const int height = valid ? world.map.height : 0;
    if (width != width_ || height != height_) {
        occupants_.assign(std::size_t(width) * height, -1);
    } else {
        for (const int cell : occupied_cells_) occupants_[cell] = -1;
    }
    occupied_cells_.clear();
    occupied_cells_.reserve(2 * world.entities.size());
    width_ = width;
    height_ = height;
    bucket_width_ = (width_ + bucket_extent - 1) / bucket_extent;
    const int bucket_height = (height_ + bucket_extent - 1) / bucket_extent;
    heads_.assign(std::size_t(bucket_width_) * bucket_height, -1);
    next_.resize(world.entities.size());

    // A cell normally contains at most one live unit. Reverse insertion keeps
    // the lowest ID authoritative if a caller supplies overlapping entities.
    for (std::size_t i = world.entities.size(); i > 0; --i) {
        const auto entity_index = static_cast<int>(i - 1);
        const auto& unit = world.entities[entity_index];
        if (!unit.hp || unit.position.x < 0 || unit.position.x >= width_ ||
            unit.position.y < 0 || unit.position.y >= height_) continue;
        const auto [x, y] = unit.position;
        const int cell = y * width_ + x;
        if (occupants_[cell] == -1) occupied_cells_.push_back(cell);
        occupants_[cell] = entity_index;
        const int bucket = (y / bucket_extent) * bucket_width_ + x / bucket_extent;
        next_[entity_index] = heads_[bucket];
        heads_[bucket] = entity_index;
    }
}

int SpatialIndex::at(Cell cell) const noexcept {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) return -1;
    return occupants_[cell.y * width_ + cell.x];
}

bool SpatialIndex::claim(Cell cell, int entity_index) {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_) return false;
    const int index = cell.y * width_ + cell.x;
    if (occupants_[index] != -1) return false;
    occupied_cells_.push_back(index);
    occupants_[index] = entity_index;
    return true;
}

const Entity* SpatialIndex::nearest_enemy(const World& world, const Entity& from,
                                          int radius) const noexcept {
    if (radius < 0 || width_ != world.map.width || height_ != world.map.height ||
        !world.map.contains(from.position)) return nullptr;
    radius = std::min(radius, std::max(width_, height_));
    const auto [x, y] = from.position;
    const int left = std::max(0, x - radius) / bucket_extent;
    const int right = std::min(width_ - 1, x + radius) / bucket_extent;
    const int top = std::max(0, y - radius) / bucket_extent;
    const int bottom = std::min(height_ - 1, y + radius) / bucket_extent;
    const Entity* best = nullptr;
    int best_distance = radius;

    for (int by = top; by <= bottom; ++by) {
        for (int bx = left; bx <= right; ++bx) {
            for (int i = heads_[by * bucket_width_ + bx]; i >= 0; i = next_[i]) {
                const auto& unit = world.entities[i];
                if (unit.side == from.side) continue;
                const int distance = std::max(std::abs(unit.position.x - x),
                                              std::abs(unit.position.y - y));
                if (distance < best_distance ||
                    (distance == best_distance && (!best || unit.id < best->id))) {
                    best = &unit;
                    best_distance = distance;
                }
            }
        }
    }
    return best;
}

}  // namespace coastal::rts
