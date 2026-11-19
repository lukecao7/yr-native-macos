// SPDX-License-Identifier: GPL-3.0-only
#include "stress.h"

#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace coastal::rts {
namespace {

constexpr int extent = 512;
constexpr int center = extent / 2;
constexpr int firing_rows = 25;
constexpr int firing_pairs = 12;
constexpr int movement_rows = 20;
constexpr int movers_per_row = 15;
constexpr int west_goal = center - 22;
constexpr int east_goal = center + 22;
constexpr std::uint64_t move_interval = 6;
constexpr std::uint8_t fire_interval = 1;
constexpr std::uint16_t endurance = std::numeric_limits<std::uint16_t>::max();

constexpr std::array sides{Side::Blue, Side::Copper};

constexpr int movement_row_start(Side side) {
    return side == Side::Blue ? center - 32 : center + 13;
}

Rules stress_rules() {
    Rules rules;
    rules.cargo_capacity = 24;
    rules.harvest_ticks = 8;
    rules.spec(UnitKind::Pioneer) = {.hp = 120, .move_period = 3, .vision = 8};
    rules.spec(UnitKind::Hub) = {.hp = 240, .vision = 9, .power = 12};
    rules.spec(UnitKind::Generator) = {.hp = 110, .cost = 100, .vision = 5, .power = 30};
    rules.spec(UnitKind::Depot) = {.hp = 150, .cost = 140, .vision = 5, .power = -8};
    rules.spec(UnitKind::Workshop) = {.hp = 170, .cost = 200, .vision = 6, .power = -18};
    rules.spec(UnitKind::Collector) = {.hp = 90, .cost = 70, .train_ticks = 40,
                                     .move_period = 3, .vision = 5};
    // One shot per firing unit per step. The adjacent opposing pairs and
    // one damage per impact keep the line alive through the endurance run.
    rules.spec(UnitKind::Skirmisher) = {.hp = endurance, .cost = 80, .train_ticks = 30,
                                      .move_period = 1, .vision = 22, .range = 9,
                                      .damage = 1, .armor = 2, .shot_period = fire_interval};
    rules.spec(UnitKind::Sentinel) = {.hp = endurance, .cost = 140, .train_ticks = 42,
                                    .move_period = 1, .vision = 18, .range = 4,
                                    .damage = 1, .armor = 2, .shot_period = fire_interval};
    rules.spec(UnitKind::Artillery) = {.hp = 90, .cost = 170, .train_ticks = 54,
                                     .move_period = 4, .vision = 12, .range = 9,
                                     .damage = 35, .armor = 2, .shot_period = 27};
    return rules;
}

Map island() {
    Map map;
    map.width = extent;
    map.height = extent;
    map.tiles.resize(std::size_t(extent) * extent, Tile{Terrain::Water, 0, 0});
    map.starts = {Cell{center - 22, center - 15}, Cell{center + 24, center + 16}};

    for (int y = center - 62; y <= center + 62; ++y) {
        for (int x = center - 62; x <= center + 62; ++x) {
            const int dx = std::abs(x - center);
            const int dy = std::abs(y - center);
            // Beveled coast and low terraces outside the battle lines.
            if (dx + dy > 102) continue;
            auto& tile = map.at({x, y});
            tile.terrain = Terrain::Ground;
            if (dx > 40 && dy < 28) {
                tile.height = dx == 41 || dy == 27 ? 1 : 2;
                if (tile.height == 1) tile.terrain = Terrain::Ramp;
            } else if (dy == 48 && dx < 22 && x % 3 == 0) {
                tile.terrain = Terrain::Rock;
            }
        }
    }
    return map;
}

Entity& place_unit(World& world, UnitKind kind, Side side, Cell position) {
    if (!world.spawn(kind, side, position)) {
        throw std::logic_error("The stress layout overlaps or blocks a unit.");
    }
    return world.entities.back();
}

}  // namespace

World make_stress_match() {
    World world;
    world.map = island();
    world.rules = stress_rules();
    world.rng_state = 0x8E4261B9D8A573CFULL;
    world.entities.reserve(1'200);

    for (int row = 0; row < firing_rows; ++row) {
        for (int pair = 0; pair < firing_pairs; ++pair) {
            const int x = center - 22 + 4 * pair;
            const int y = center - 12 + row;
            for (const auto side : sides) {
                const int team = static_cast<int>(side_index(side));
                auto& unit = place_unit(world, UnitKind::Skirmisher, side, {x + team, y});
                unit.order = OrderKind::Hold;
            }
        }
    }

    // Each flank has 300 movers. Turning the whole row around before the
    // leading unit reaches the goal keeps one free tile between actors.
    for (const auto side : sides) {
        for (int row = 0; row < movement_rows; ++row) {
            const int y = movement_row_start(side) + row;
            for (int column = 0; column < movers_per_row; ++column) {
                const int x = center - (movers_per_row - 1) + 2 * column;
                place_unit(world, UnitKind::Sentinel, side, {x, y});
            }
        }
    }
    return world;
}

void drive_stress(World& world) {
    if (world.result != MatchResult::Running || world.step % move_interval != 0) return;

    const bool blue_east = (world.step / move_interval) % 2 == 0;
    std::array<std::array<Command, movement_rows>, 2> moves{};
    for (const auto side : sides) {
        const bool east = side == Side::Blue ? blue_east : !blue_east;
        const int start = movement_row_start(side);
        for (int row = 0; row < movement_rows; ++row) {
            auto& move = moves[side_index(side)][row];
            move.kind = CommandKind::Move;
            move.side = side;
            move.target = {east ? east_goal : west_goal, start + row};
        }
    }
    for (const auto& unit : world.entities) {
        if (unit.kind != UnitKind::Sentinel) continue;
        const int row = unit.position.y - movement_row_start(unit.side);
        if (row >= 0 && row < movement_rows) moves[side_index(unit.side)][row].units.push_back(unit.id);
    }
    for (const auto& team : moves) {
        for (const auto& move : team) {
            if (!move.units.empty()) world.apply(move);
        }
    }
}

}  // namespace coastal::rts
