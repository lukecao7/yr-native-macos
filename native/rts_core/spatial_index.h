// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "world.h"

namespace coastal::rts {

// Rebuilt on the owner thread. Its read-only views are shared by worker lanes.
class SpatialIndex {
public:
    void rebuild(const World& world);
    int at(Cell cell) const noexcept; // Entity vector index, or -1.
    bool claim(Cell cell, int entity_index); // Owner-only, after shared readers join.
    const Entity* nearest_enemy(const World& world, const Entity& from, int radius) const noexcept;

private:
    static constexpr int bucket_extent = 8;
    int width_ = 0;
    int height_ = 0;
    int bucket_width_ = 0;
    std::vector<int> occupants_;
    std::vector<int> occupied_cells_;
    std::vector<int> heads_;
    std::vector<int> next_;
};

}  // namespace coastal::rts
