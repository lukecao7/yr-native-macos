// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace coastal::rts;

void require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

World open_world(int width = 12, int height = 10) {
    World world;
    world.map.width = width;
    world.map.height = height;
    world.map.tiles.resize(std::size_t(width) * height);
    world.rng_state = 41;
    world.rules.spec(UnitKind::Pioneer) = {
        .hp = 50, .move_period = 1, .vision = 8,
    };
    world.rules.spec(UnitKind::Collector) = {
        .hp = 50, .move_period = 1, .vision = 8,
    };
    world.rules.spec(UnitKind::Skirmisher) = {
        .hp = 50, .move_period = 1, .vision = 8, .range = 4, .damage = 10, .armor = 1, .shot_period = 18,
    };
    world.rules.spec(UnitKind::Generator) = {.hp = 70, .armor = 4};
    return world;
}

EntityId spawn(World& world, UnitKind kind, Side side, Cell cell) {
    const auto result = world.spawn(kind, side, cell);
    require(result.has_value(), "Failed to create self-authored entity");
    return *result;
}

Command make_command(CommandKind kind, Side side = Side::Blue) {
    return {.kind = kind, .side = side, .target = {}, .units = {}};
}

void selection_groups_and_commands() {
    auto world = open_world();
    const auto lead = spawn(world, UnitKind::Skirmisher, Side::Blue, {2, 3});
    const auto second = spawn(world, UnitKind::Skirmisher, Side::Blue, {1, 3});
    const auto enemy = spawn(world, UnitKind::Pioneer, Side::Copper, {9, 3});
    auto& blue = world.side(Side::Blue);

    auto command = make_command(CommandKind::Select);
    command.units = {second, enemy, 76, lead, second};
    require(apply_combat(world, command) && blue.selected == std::vector{lead, second},
            "Select keeps only living friendly IDs in stable order");

    command = make_command(CommandKind::AssignGroup);
    command.index = 8;
    require(apply_combat(world, command), "The ninth group should be assignable");
    command.index = 9;
    require(!apply_combat(world, command), "Group indices above nine should be rejected");
    require(apply_combat(world, make_command(CommandKind::Select)) && blue.selected.empty(),
            "A blank selection should clear the active group");

    command = make_command(CommandKind::RecallGroup);
    command.index = 8;
    require(apply_combat(world, command) && blue.selected == std::vector{lead, second},
            "Recall restores the selected group");
    command = make_command(CommandKind::Select);
    command.units = {lead};
    command.add = true;
    require(apply_combat(world, command) && blue.selected == std::vector{lead, second},
            "An additive selection retains current IDs without duplicates");

    world.map.at({8, 4}).terrain = Terrain::Water;
    command = make_command(CommandKind::Move);
    command.target = {8, 4};
    require(!apply_combat(world, command) && world.find(second)->order == OrderKind::Idle,
            "A blocked ground command should not change the previous order");
    command.target = {7, 4};
    require(apply_combat(world, command) && world.find(second)->destination == command.target &&
            world.find(enemy)->order == OrderKind::Idle,
            "Move applies to the current friendly group");
    command = make_command(CommandKind::Hold);
    command.units = {lead, enemy};
    require(apply_combat(world, command) && world.find(lead)->order == OrderKind::Hold &&
            world.find(second)->order == OrderKind::Move && world.find(enemy)->order == OrderKind::Idle,
            "Explicit Hold applies to its owned movable units only");
}

void movement_detour_and_gather() {
    auto world = open_world();
    auto navigator = Navigator{};
    for (int y = 0; y < 8; ++y) world.map.at({5, y}).terrain = Terrain::Rock;
    const auto traveler = spawn(world, UnitKind::Pioneer, Side::Blue, {2, 3});
    auto command = make_command(CommandKind::Move);
    command.target = {9, 3};
    command.units = {traveler};
    require(apply_combat(world, command), "Open detour command should be admitted");
    bool crossed_gap = false;
    for (int i = 0; i < 40; ++i) {
        tick_combat(world, navigator);
        const Cell cell = world.find(traveler)->position;
        require(world.map.walkable(cell), "Movement must not enter a rock");
        if (cell.x == 5 && cell.y >= 8) crossed_gap = true;
    }
    require(crossed_gap && world.find(traveler)->position == command.target &&
            world.find(traveler)->order == OrderKind::Idle,
            "Distance-field movement should route around the wall and finish");

    for (int y = 8; y < world.map.height; ++y) world.map.at({5, y}).terrain = Terrain::Water;
    navigator.prepare(world);
    require(!navigator.next(world, command.target, {2, 3}), "Closing the gap must invalidate the route");

    world = open_world(6, 5);
    const auto collector = spawn(world, UnitKind::Collector, Side::Blue, {1, 2});
    world.map.at({4, 2}) = {Terrain::Deposit, 0, 15};
    world.find(collector)->order = OrderKind::Gather;
    world.find(collector)->destination = {4, 2};
    for (int i = 0; i < 10; ++i) tick_combat(world, navigator);
    require(world.find(collector)->position == Cell{4, 2} &&
            world.find(collector)->order == OrderKind::Gather,
            "Gather reaches an open deposit and remains owned by the economy state machine");
}

void group_approaches_shared_goal() {
    auto world = open_world();
    auto navigator = Navigator{};
    auto command = make_command(CommandKind::Move);
    command.target = {8, 4};
    for (int y = 3; y <= 5; ++y) command.units.push_back(spawn(world, UnitKind::Pioneer, Side::Blue, {1, y}));
    require(apply_combat(world, command), "Shared destination should be accepted");
    for (int i = 0; i < 26; ++i) tick_combat(world, navigator);
    for (const auto& unit : world.entities) {
        require(std::abs(unit.position.x - command.target.x) <= 1 &&
                std::abs(unit.position.y - command.target.y) <= 1,
                "Every group member should approach the shared destination");
        require(std::count_if(world.entities.begin(), world.entities.end(), [&](const Entity& other) {
            return other.position == unit.position;
        }) == 1, "Stable movement must not put two entities on one cell");
    }
}

void shot_flight_armor_and_retirement() {
    auto world = open_world();
    auto navigator = Navigator{};
    const auto lead = spawn(world, UnitKind::Skirmisher, Side::Blue, {1, 4});
    const auto target = spawn(world, UnitKind::Generator, Side::Copper, {6, 4});
    auto command = make_command(CommandKind::AttackMove);
    command.units = {lead};
    command.target = {8, 4};
    require(apply_combat(world, command), "Attack move should be accepted");
    tick_combat(world, navigator);
    require(world.find(lead)->position == Cell{2, 4} && world.projectiles.empty(),
            "Attack move must approach enemies acquired outside weapon range");
    tick_combat(world, navigator);
    require(world.projectiles.size() == 1 && world.find(target)->hp == 70 && world.rng_state != 41,
            "Firing uses the seeded roll and delays damage until impact");
    const auto flight = world.projectiles.front();
    require(flight.shooter == lead && flight.target == target && flight.remaining == 3,
            "Projectile should carry stable endpoint IDs and flight time");
    tick_combat(world, navigator);
    tick_combat(world, navigator);
    require(world.find(target)->hp == 70 && world.projectiles.size() == 1 &&
            world.projectiles.front().remaining == 1,
            "An in-flight projectile must not change health");
    tick_combat(world, navigator);
    require(world.projectiles.empty() && world.find(target)->hp == 70 - (flight.damage - 4),
            "Impact applies flat armor reduction once");

    command = make_command(CommandKind::Select, Side::Copper);
    command.units = {target};
    apply_combat(world, command);
    apply_combat(world, make_command(CommandKind::AssignGroup, Side::Copper));
    world.find(lead)->shot_wait = 0;
    world.find(target)->hp = 1;
    command = make_command(CommandKind::Hold);
    command.units = {lead};
    apply_combat(world, command);
    const Cell firing_cell = world.find(lead)->position;
    for (int i = 0; i < 5; ++i) tick_combat(world, navigator);
    require(world.find(lead)->position == firing_cell && !world.find(target) && world.projectiles.empty() &&
            world.side(Side::Copper).selected.empty() && world.side(Side::Copper).groups.front().empty(),
            "Hold fires from its own cell and destroyed units retire from groups and projectiles");
}

void explicit_focus_uses_hostile_target() {
    auto world = open_world();
    Navigator navigator;
    const auto attacker = spawn(world, UnitKind::Skirmisher, Side::Blue, {2, 3});
    const auto friendly = spawn(world, UnitKind::Pioneer, Side::Blue, {2, 4});
    const auto nearest = spawn(world, UnitKind::Pioneer, Side::Copper, {3, 3});
    const auto focus = spawn(world, UnitKind::Pioneer, Side::Copper, {6, 3});
    auto attack = make_command(CommandKind::AttackMove);
    attack.units = {attacker};
    attack.target = {6, 3};
    attack.focus = friendly;
    require(!world.apply(attack) && world.find(attacker)->order == OrderKind::Idle,
            "An invalid friendly focus leaves the command unapplied");
    attack.focus = focus;
    require(world.apply(attack), "The attack order can focus a known opposing unit");
    world.tick(navigator);
    require(world.projectiles.size() == 1 && world.projectiles.front().target == focus &&
            world.projectiles.front().target != nearest, "The engine fires at the specified target");
    auto move = make_command(CommandKind::Move);
    move.units = {attacker};
    move.target = {2, 2};
    require(world.apply(move) && world.find(attacker)->focus == no_entity,
            "An ordinary movement replaces the focus order");
}

}  // namespace

int main() {
    selection_groups_and_commands();
    movement_detour_and_gather();
    group_approaches_shared_goal();
    shot_flight_armor_and_retirement();
    explicit_focus_uses_hostile_target();
    std::cout << "RTS combat tests passed (orders, paths, grouping, flight, armor, retirement)\n";
}
