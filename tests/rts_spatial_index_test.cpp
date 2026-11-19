// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/spatial_index.h"

#include <iostream>
#include <stdexcept>

namespace {

using namespace coastal::rts;

void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

Entity unit(EntityId id, Side side, Cell cell, std::uint16_t hp = 1) {
    Entity value{};
    value.id = id;
    value.kind = UnitKind::Skirmisher;
    value.side = side;
    value.position = cell;
    value.hp = hp;
    return value;
}

void test_index() {
    World world;
    world.map.width = 17;
    world.map.height = 19;
    world.entities = {
        unit(1, Side::Blue, {8, 8}),
        unit(2, Side::Copper, {9, 8}),
        unit(3, Side::Copper, {7, 7}),
        unit(4, Side::Copper, {8, 9}, 0),
        unit(5, Side::Blue, {8, 7}),
        unit(6, Side::Copper, {16, 18}),
    };
    SpatialIndex index;
    const auto& from = world.entities.front();
    check(!index.nearest_enemy(world, from, 1), "Index requires its first rebuild");
    index.rebuild(world);
    check(index.at({9, 8}) == 1 && index.at({16, 18}) == 5, "Cells map to their entity slots");
    check(index.at({8, 9}) == -1 && index.at({17, 11}) == -1 && index.at({0, -1}) == -1,
          "Dead units and out-of-bounds points are absent");
    check(index.nearest_enemy(world, from, 1) == &world.entities[1],
          "Equal Chebyshev distances prefer the lower ID across bucket boundaries");
    check(!index.nearest_enemy(world, from, 0) && !index.nearest_enemy(world, from, -1),
          "The query obeys inclusive range and handles invalid range");
    world.entities[1].position = {12, 8};
    index.rebuild(world);
    check(index.at({9, 8}) == -1 && index.at({12, 8}) == 1, "Rebuild clears a vacated cell");
    check(index.nearest_enemy(world, from, 1) == &world.entities[2],
          "A diagonal adjacent enemy is visible at radius one");
    check(index.nearest_enemy(world, world.entities.back(), 64) == &world.entities[0],
          "Ranges larger than the board cover the entire valid map");
}

}  // namespace

int main() {
    try {
        test_index();
        std::cout << "Spatial index checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
