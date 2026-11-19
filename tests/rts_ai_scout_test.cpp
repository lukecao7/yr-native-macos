// SPDX-License-Identifier: GPL-3.0-only
#include "rts_ai/strategy.h"

#include "rts_core/combat.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

using namespace coastal::rts;
using namespace coastal::rts::ai;

void require(bool result, const char* description) {
    if (!result) throw std::runtime_error(description);
}

World world_at(int width, int height, Cell home, Cell far) {
    World world;
    world.map.width = width;
    world.map.height = height;
    world.map.starts = {home, far};
    world.map.tiles.resize(std::size_t(width) * height);
    world.rules.spec(UnitKind::Skirmisher) = {
        .hp = 60, .move_period = 2, .vision = 5, .range = 4, .damage = 15, .shot_period = 16
    };
    world.rules.spec(UnitKind::Workshop) = {.hp = 100, .vision = 3};
    world.rules.spec(UnitKind::Artillery) = {
        .hp = 60, .move_period = 4, .vision = 8, .range = 8, .damage = 30, .shot_period = 20
    };
    world.entities = {
        Entity{.id = 1, .kind = UnitKind::Skirmisher, .side = Side::Blue, .position = home, .hp = 60,
               .destination = {}, .deposit = {}},
        Entity{.id = 2, .kind = UnitKind::Artillery, .side = Side::Copper, .position = far, .hp = 60,
               .destination = {}, .deposit = {}},
    };
    world.next_id = 3;
    world.side(Side::Blue).funds = 300;
    world.side(Side::Blue).power_percent = 72;
    world.side(Side::Blue).queue.push_back({UnitKind::Skirmisher, 10, 100});
    world.side(Side::Copper).funds = 999;
    return world;
}

bool same_tile(const SeenTile& a, const SeenTile& b) {
    return a.known == b.known && a.terrain == b.terrain && a.resource == b.resource;
}

void test_hidden_state() {
    auto world = world_at(40, 32, {4, 4}, {30, 20});
    world.map.at({3, 1}) = {.terrain = Terrain::Deposit, .resource = 28};
    world.map.at({15, 15}) = {.terrain = Terrain::Deposit, .resource = 900};
    auto changed = world;
    changed.map.starts[1] = {35, 23};
    changed.entities[1].position = {29, 24};
    changed.entities[1].kind = UnitKind::Workshop;
    changed.entities[1].hp = 12;
    changed.side(Side::Copper).funds = 130;
    changed.side(Side::Copper).power_percent = 3;
    changed.map.at({15, 15}) = {.terrain = Terrain::Rock};

    const auto first = observe(world, Side::Blue);
    const auto second = observe(changed, Side::Blue);
    require(first.home == Cell{4, 4} && second.home == first.home, "Only the friendly start is exposed");
    require(first.funds == 300 && second.funds == first.funds && first.power == 72 &&
            second.power == first.power && first.queue.size() == 1 && second.queue.size() == 1,
            "Only the observing side's economy is exposed");
    require(first.allies.size() == 1 && second.allies.size() == 1 &&
            first.enemies.empty() && second.enemies.empty(), "Hostile details outside vision are absent");
    require(std::ranges::equal(first.visible, second.visible, same_tile),
            "Changing hidden enemy information and terrain has no visible effect");
    require(same_tile(first.visible[first.index({3, 1})], {true, Terrain::Deposit, 28}) &&
            same_tile(first.visible[first.index({15, 15})], {}),
            "Observed ore is readable but hidden ore is not embedded in the observation");
    require(first.visible[first.index({9, 9})].known && !first.visible[first.index({10, 9})].known,
            "Vision includes the diagonal boundary of the existing square mask");

    AiMemory left;
    AiMemory right;
    remember(first, left);
    remember(second, right);
    const auto plan = scout(first, left, estimate_threat(first, left));
    const auto changed_plan = scout(second, right, estimate_threat(second, right));
    require(plan && changed_plan && plan->target == changed_plan->target && plan->units == changed_plan->units,
            "Hidden enemy start, unit, budget and terrain cannot change the scouting order");

    const auto copper = observe(world, Side::Copper);
    require(copper.home == Cell{30, 20} && copper.funds == 999 && copper.enemies.empty(),
            "The visibility boundary applies symmetrically to either side");
}

void test_contacts() {
    auto world = world_at(40, 32, {9, 14}, {14, 19});
    world.step = 12;
    const Cell old_cell = world.entities[1].position;
    auto first = observe(world, Side::Blue);
    require(first.enemies.size() == 1 && first.enemies[0].id == 2 && first.enemies[0].seen_at == 12,
            "An enemy at the inclusive vision boundary is observed");

    AiMemory memory;
    remember(first, memory);
    const auto hot = estimate_threat(first, memory);
    require(hot.at(old_cell) > hot.at({14, 23}) && hot.at({32, 30}) == 0,
            "The observed weapon creates a localized threat gradient");
    require(!scout(first, memory, hot), "A lone defender is kept for an observed engagement");

    world.entities[0].position = {2, 2};
    world.entities[1].position = {31, 28};
    world.entities[1].hp = 10;
    world.step += AiMemory::contact_lifetime / 2;
    auto hidden = observe(world, Side::Blue);
    remember(hidden, memory);
    const auto cool = estimate_threat(hidden, memory);
    require(memory.enemies.size() == 1 && memory.enemies[0].position == old_cell &&
            memory.enemies[0].hp == 60 && memory.enemies[0].seen_at == 12,
            "A contact retains its last known data after it leaves view");
    require(cool.at(old_cell) > 0 && cool.at(old_cell) < hot.at(old_cell) &&
            cool.at({31, 28}) == 0, "Threat fades without learning the hidden current location");

    AiMemory expire = memory;
    world.entities[0].position = {9, 14};
    remember(observe(world, Side::Blue), memory);
    require(memory.enemies.empty(), "Revisiting an empty last-known cell disproves the stale contact");

    world.entities[0].position = {2, 2};
    world.step = 12 + AiMemory::contact_lifetime;
    const auto aged = observe(world, Side::Blue);
    remember(aged, expire);
    require(expire.enemies.empty() && estimate_threat(aged, expire).at(old_cell) == 0,
            "Old contacts expire even without revisiting their cells");
}

void test_survey() {
    auto world = world_at(81, 35, {6, 16}, {71, 18});
    // Use a stationary contact to isolate the scout's movement.
    world.rules.spec(UnitKind::Artillery).damage = 0;
    // The known route must go around a water barrier. Its southern end has
    // a passage learned through ordinary observation.
    for (int y = 0; y < 27; ++y) world.map.at({35, y}).terrain = Terrain::Water;

    AiMemory memory;
    Navigator navigation;
    bool contacted = false;
    bool detoured = false;
    while (world.step < 500) {
        const auto sight = observe(world, Side::Blue);
        remember(sight, memory);
        const auto heat = estimate_threat(sight, memory);
        if (!sight.enemies.empty()) {
            contacted = true;
            require(!scout(sight, memory, heat), "Combat takes over once a scout finds opposition");
            break;
        }
        const auto move = scout(sight, memory, heat);
        require(move && move->kind == CommandKind::Move && move->units == std::vector<EntityId>{1},
                "The scout keeps a single explicit survey assignment");
        const auto& destination = memory.chart[sight.index(move->target)];
        require(destination.known && destination.terrain != Terrain::Water,
                "Survey moves only target reachable charted ground");
        require(world.apply(*move), "The public movement engine accepts the scouting order");
        while (world.entities.front().position != move->target && world.step < 500) {
            world.tick(navigation);
            const auto position = world.entities.front().position;
            detoured |= position.x == 35 && position.y >= 27;
        }
    }
    require(detoured && contacted && world.step < 500,
            "Frontier exploration finds a distant enemy after routing around the water");
}

}  // namespace

int main() {
    try {
        test_hidden_state();
        test_contacts();
        test_survey();
        std::cout << "AI scout tests passed (restricted sight, contact memory, threat and frontier routes)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
