// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/snapshot.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace coastal::rts;

constexpr std::array<EntityId, 5> ids{1, 0x10001, 0x10103, 0x20006, 0xfff00008};
static_assert(std::is_aggregate_v<World>);

void require(bool result, const char* why) {
    if (!result) throw std::runtime_error(why);
}

Rules fixture_rules() {
    Rules rules;
    rules.cargo_capacity = 8;
    rules.harvest_ticks = 4;
    for (std::size_t i = 0; i < rules.units.size(); ++i) {
        const auto kind = static_cast<UnitKind>(i);
        auto& unit = rules.spec(kind);
        unit.hp = 100;
        unit.vision = 5;
        unit.move_period = is_building(kind) ? 0 : 1;
        unit.train_ticks = is_producible(kind) ? 2 : 0;
        unit.cost = kind == UnitKind::Pioneer || kind == UnitKind::Hub ? 0 : 20;
    }
    rules.spec(UnitKind::Generator).power = 10;
    rules.spec(UnitKind::Depot).power = -1;
    rules.spec(UnitKind::Workshop).power = -1;
    return rules;
}

World sparse_world() {
    World world{.rules = fixture_rules()};
    world.map.width = world.map.height = 16;
    world.map.tiles.resize(256);
    world.map.starts = {Cell{1, 1}, Cell{14, 14}};
    // The first two IDs share a slot in the small fixture table.
    // The last one exercises a span close to the limits of an EntityId.
    for (std::size_t i = 0; i < ids.size(); ++i) {
        world.next_id = ids[i];
        const auto side = i % 2 ? Side::Copper : Side::Blue;
        require(world.spawn(UnitKind::Pioneer, side, {2 + int(i), 5}) == ids[i],
                "Could not spawn a sparse fixture actor");
    }
    return world;
}

World without_hints(const World& source) {
    return World{
        .map = source.map,
        .rules = source.rules,
        .sides = source.sides,
        .entities = source.entities,
        .projectiles = source.projectiles,
        .step = source.step,
        .rng_state = source.rng_state,
        .next_id = source.next_id,
        .result = source.result,
    };
}

void verify_slots(const World& world) {
    for (const auto& entity : world.entities)
        require(world.find(entity.id) == &entity, "Lookup did not return the live entity slot");
    for (const auto missing : {no_entity, EntityId{2}, EntityId{0x20001}, std::numeric_limits<EntityId>::max()})
        require(!world.find(missing), "A colliding candidate was returned as another entity");
}

void test_sparse_lookup() {
    auto world = sparse_world();
    const auto archive = encode_snapshot(without_hints(world));
    verify_slots(world);
    world.refresh_entity_index();
    require(encode_snapshot(world) == archive, "Refreshing hints changed version-one archive bytes");

    std::string error;
    const auto reloaded = decode_snapshot(archive, error);
    require(reloaded && encode_snapshot(*reloaded) == archive, "Sparse version-one archive changed after a round trip");
    verify_slots(*reloaded);

    // A caller can edit the public sorted vector without publishing a new
    // hint. Shift slots, replace an ID, and move vector storage at the same size.
    world.entities.erase(world.entities.begin() + 1);
    world.entities[1].id = 0x10105;
    auto returning = reloaded->entities[1];
    returning.id = 0x20005;
    world.entities.insert(world.entities.begin() + 2, returning);
    std::vector<Entity>(world.entities).swap(world.entities);
    require(!world.find(ids[1]) && !world.find(ids[2]), "A stale hint resurrected a removed ID");
    verify_slots(world);
    const auto expected = encode_snapshot(without_hints(world));
    for (const unsigned lanes : {1u, 4u, 8u}) {
        const auto& read_only = std::as_const(world);
        WorkerPool workers(lanes);
        workers.run(64, [&](unsigned, std::size_t begin, std::size_t end) {
            for (auto i = begin; i < end; ++i) verify_slots(read_only);
        });
        require(encode_snapshot(world) == expected, "Joined read-only lookup changed saved state");
    }
}

void test_combat_compaction() {
    auto world = sparse_world();
    const auto shooter = ids[0];
    const auto victim = ids[1];
    auto& copper = world.side(Side::Copper);
    copper.selected = {victim, ids[3]};
    copper.groups[2] = copper.selected;
    world.projectiles.push_back({shooter, victim, world.find(shooter)->position,
                                 world.find(victim)->position, 1, 1, 100});
    const Command move{.kind = CommandKind::Move, .side = Side::Blue, .target = {9, 7}, .units = {ids[2]}};
    require(world.apply(move), "Fixture actor could not receive its Move order");

    auto serial = without_hints(world);
    Navigator serial_routes(1);
    Navigator parallel_routes(4);
    for (int i = 0; i < 16; ++i) {
        serial.tick(serial_routes);
        world.tick(parallel_routes);
        require(encode_snapshot(serial) == encode_snapshot(world), "A tick changed canonical state across lanes");
        verify_slots(world);
        if (i != 0) continue;

        require(!world.find(victim) && copper.selected == std::vector{ids[3]} && copper.groups[2] == copper.selected,
                "Projectile retirement did not clear the destroyed entity and its group entries");
        const auto replacement = world.spawn(UnitKind::Pioneer, Side::Copper, {9, 11});
        require(replacement && serial.spawn(UnitKind::Pioneer, Side::Copper, {9, 11}) == replacement,
                "Replenishment after compaction failed");
        require(world.find(*replacement) == &world.entities.back() &&
                    encode_snapshot(serial) == encode_snapshot(world),
                "Replenishment changed slot identity or canonical state");
    }
    const auto* moved = world.find(ids[2]);
    require(moved && moved->position == Cell{9, 7}, "Fixture movement did not complete");
}
}  // namespace

int main() {
    try {
        test_sparse_lookup();
        test_combat_compaction();
        std::cout << "World entity index passed sparse archives, stale hints, joined reads and combat retirement\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
