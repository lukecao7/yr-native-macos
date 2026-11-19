// SPDX-License-Identifier: GPL-3.0-only
#include "rts_ai/strategy.h"

#include <iostream>
#include <stdexcept>

namespace {

using namespace coastal::rts;
using namespace coastal::rts::ai;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

Observation field(bool known = true) {
    Observation observation;
    observation.step = 100;
    observation.width = 36;
    observation.height = 26;
    observation.home = {4, 10};
    observation.visible.assign(std::size_t(observation.width) * observation.height,
                               SeenTile{.known = known});
    observation.rules.spec(UnitKind::Skirmisher) = {
        .hp = 75, .cost = 85, .move_period = 2, .vision = 8, .range = 4, .damage = 18,
    };
    observation.rules.spec(UnitKind::Sentinel) = {
        .hp = 100, .cost = 140, .move_period = 3, .vision = 7, .range = 3, .damage = 22,
    };
    observation.rules.spec(UnitKind::Artillery) = {
        .hp = 80, .cost = 180, .move_period = 4, .vision = 11, .range = 9, .damage = 36,
    };
    observation.rules.spec(UnitKind::Collector) = {.hp = 100, .cost = 80, .move_period = 3};
    return observation;
}

Entity ally(EntityId id, UnitKind kind, Cell position, std::uint16_t hp) {
    Entity unit{};
    unit.id = id;
    unit.kind = kind;
    unit.position = position;
    unit.hp = hp;
    return unit;
}

void groups_and_focus() {
    auto observation = field();
    observation.allies = {
        ally(6, UnitKind::Artillery, {16, 10}, 80),
        ally(2, UnitKind::Skirmisher, {9, 10}, 75),
        ally(7, UnitKind::Skirmisher, {11, 11}, 75), // Dedicated scout.
        ally(3, UnitKind::Sentinel, {10, 10}, 100),
        ally(5, UnitKind::Collector, {5, 10}, 100),
        ally(1, UnitKind::Skirmisher, {8, 10}, 75),
    };
    observation.enemies = {
        {30, UnitKind::Skirmisher, {18, 10}, 70, 100},
        {31, UnitKind::Artillery, {22, 10}, 6, 100},
        {32, UnitKind::Collector, {8, 17}, 1, 100},
    };
    const auto orders = plan_tactics(observation, {}, {}, 7);
    check(orders.size() == 4, "Only combat units other than the dedicated scout receive orders");
    check(orders[0].units == std::vector<EntityId>{1} &&
          orders[1].units == std::vector<EntityId>{2} &&
          orders[2].units == std::vector<EntityId>{3} &&
          orders[3].units == std::vector<EntityId>{6}, "Orders are issued in stable unit-ID order");
    check(orders[0].kind == CommandKind::Move && orders[1].kind == CommandKind::Move &&
          orders[0].target.y > 10 && orders[1].target.y < 10 &&
          orders[0].focus == no_entity && orders[1].focus == no_entity,
          "Fast units approach the longer-ranged gun from opposite sides");
    check(orders[2].kind == CommandKind::AttackMove && orders[2].target == Cell{22, 10} &&
          orders[2].focus == 31, "The screen closes on the weakened high-value gun");
    check(orders[3].kind == CommandKind::AttackMove && orders[3].target == Cell{16, 10} &&
          orders[3].focus == 31, "The supporting gun concentrates fire at its current range");
}

void connected_retreat() {
    auto observation = field(false);
    observation.allies = {ally(2, UnitKind::Sentinel, {9, 9}, 20)};
    observation.enemies = {{30, UnitKind::Skirmisher, {13, 9}, 70, 100}};
    for (int x = 6; x <= 9; ++x) {
        observation.visible[observation.index({x, 9})] = {.known = true};
    }
    ThreatGrid threat{observation.width, observation.height,
                      std::vector<std::uint16_t>(observation.visible.size())};
    threat.cells[observation.index({9, 9})] = 8;
    threat.cells[observation.index({8, 9})] = 4;
    const auto withdrawal = plan_tactics(observation, {}, threat);
    check(withdrawal.size() == 1 && withdrawal[0].kind == CommandKind::Move &&
          withdrawal[0].target == Cell{6, 9} && withdrawal[0].focus == no_entity,
          "A wounded unit withdraws along the known walkable corridor");
    observation.visible[observation.index({8, 9})].terrain = Terrain::Rock;
    const auto blocked = plan_tactics(observation, {}, threat);
    check(blocked.size() == 1 && blocked[0].kind == CommandKind::AttackMove &&
          blocked[0].focus == 30, "A disconnected or unknown cell cannot be a retreat destination");

    auto gun_bare = field();
    auto gun = ally(3, UnitKind::Artillery, {11, 9}, 80);
    gun.shot_wait = 12;
    gun_bare.allies = {gun};
    gun_bare.enemies = observation.enemies;
    const auto spacing = plan_tactics(gun_bare, {}, {});
    check(spacing.size() == 1 && spacing[0].kind == CommandKind::Move &&
          spacing[0].target.x <= 8, "An exposed gun takes distance while reloading");
}

void firing_around_an_enclosure() {
    auto observation = field();
    observation.width = 64;
    observation.height = 80;
    observation.home = {53, 63};
    observation.visible.assign(std::size_t(observation.width) * observation.height,
                               SeenTile{.known = true});
    observation.allies = {ally(1, UnitKind::Artillery, {49, 62}, 80)};
    observation.enemies = {
        {20, UnitKind::Sentinel, {39, 63}, 68, 100},
        {21, UnitKind::Hub, {39, 64}, 100, 100},
        {22, UnitKind::Generator, {39, 62}, 100, 100},
        {23, UnitKind::Depot, {38, 63}, 100, 100},
        {24, UnitKind::Workshop, {40, 63}, 100, 100},
    };
    // The Workshop is already in range and offers a shot in this decision.
    auto orders = plan_tactics(observation, {}, {});
    check(orders.size() == 1 && orders[0].kind == CommandKind::AttackMove &&
          orders[0].target == Cell{49, 62} && orders[0].focus == 24,
          "A distant squad target does not make a gun forgo a visible shot");
    // With no live target in range, use a firing cell outside the far pocket.
    observation.allies[0].position = {50, 62};
    orders = plan_tactics(observation, {}, {});
    check(orders.size() == 1 && orders[0].kind == CommandKind::Move &&
          orders[0].target.x == 48 && orders[0].focus == no_entity,
          "Artillery approaches a reachable firing cell instead of entering the pocket");
    observation.allies[0].position = orders[0].target;
    orders = plan_tactics(observation, {}, {});
    check(orders.size() == 1 && orders[0].kind == CommandKind::AttackMove &&
          orders[0].target == observation.allies[0].position && orders[0].focus == 20,
          "The artillery aims at the foremost observed threat once in range");
}

void remembered_contact_and_frontier() {
    auto observation = field();
    observation.allies = {ally(1, UnitKind::Sentinel, {9, 9}, 100)};
    AiMemory memory;
    memory.enemies = {{99, UnitKind::Skirmisher, {14, 8}, 10, 80}};
    memory.scout_target = {21, 12};
    observation.visible[observation.index({14, 8})].terrain = Terrain::Water;
    const auto search = plan_tactics(observation, memory, {});
    check(search.size() == 1 && search[0].kind == CommandKind::AttackMove &&
          search[0].focus == no_entity && search[0].target != Cell{14, 8},
          "Remembered enemies give a search area without becoming focused targets");
    observation.step = 80 + AiMemory::contact_lifetime + 1;
    const auto frontier = plan_tactics(observation, memory, {});
    check(frontier.size() == 1 && frontier[0].target == memory.scout_target &&
          frontier[0].focus == no_entity, "Expired contact information falls back to the scout frontier");
    observation.allies[0].order = OrderKind::AttackMove;
    observation.allies[0].destination = frontier[0].target;
    check(plan_tactics(observation, memory, {}).empty(), "Current orders are retained without reissue");
}

}  // namespace

int main() {
    try {
        groups_and_focus();
        connected_retreat();
        firing_around_an_enclosure();
        remembered_contact_and_frontier();
        std::cout << "RTS tactical AI checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
