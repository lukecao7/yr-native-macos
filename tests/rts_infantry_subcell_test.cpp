// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace coastal::rts;

namespace {
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

World make_world(const Rules& rules) {
    World world;
    world.rules = rules;
    world.map.width = world.map.height = 12;
    world.map.starts = {Cell{1,1}, Cell{10,10}};
    world.map.tiles.resize(144);
    return world;
}

void check_capacity(const Rules& rules) {
    auto world = make_world(rules);
    constexpr Cell cell{5,5};
    for (unsigned slot : {3U, 0U, 4U}) {
        auto id = world.spawn(UnitKind::Skirmisher, Side::Blue, cell, slot);
        check(id && world.find(*id)->subcell == slot, "available authored spot was changed");
    }
    check(!world.spawn(UnitKind::Skirmisher, Side::Blue, cell, 1), "fourth infantry entered");
    check(!world.free_at(cell) && !world.free_at(cell, 0, Side::Copper), "vehicle or enemy admitted");
    world.entities[1].hp = 0;
    check(world.free_at(cell, 0, Side::Blue), "death did not release its spot");
    check(!world.free_at(cell, 0, Side::Copper), "opposing infantry admitted beside defenders");
    Navigator nav;
    const std::array<MobileOccupant,1> tails{{{cell, 90, true, 2, Side::Blue}}};
    nav.set_transient_occupancy(tails);
    nav.prepare(world);
    check(!nav.prepared_slots(cell).reserve(1, Side::Blue), "physical tail did not reserve third place");
}

std::vector<std::vector<std::uint8_t>> march(const Rules& rules, unsigned lanes) {
    auto world = make_world(rules);
    const Cell goal{8,6};
    std::vector<EntityId> ids;
    for (unsigned slot = 0; slot < 4; ++slot) {
        const Cell source{2, static_cast<int>(slot)+4};
        // All four march from the center spot, forcing a new distinct spot
        // during simultaneous arrival instead of just reusing initial IDs.
        ids.push_back(world.spawn(UnitKind::Skirmisher, Side::Blue, source, 0).value());
    }
    Command move{};
    move.kind = CommandKind::Move;
    move.side = Side::Blue;
    move.target = goal;
    move.units = ids;
    check(world.apply(move), "squad rejected");
    check(world.spawn(UnitKind::Hub, Side::Copper, {0, 0}).has_value(), "the opposing base must be present");
    Navigator nav(lanes);
    std::string error;
    std::vector<std::vector<std::uint8_t>> stream;
    for (unsigned frame=0; frame<150; ++frame) {
        world.tick(nav);
        std::map<int, unsigned> spots;
        for (const auto& actor : world.entities) {
            if (actor.side != Side::Blue) continue;
            auto& used=spots[world.map.index(actor.position)];
            check(actor.subcell < subcell_offsets.size() && !(used & (1U << actor.subcell)), "two bodies share a spot");
            used |= 1U << actor.subcell;
            check(std::popcount(used) <= 3, "more than three bodies share a tile");
        }
        stream.push_back(encode_snapshot(world));
        if (frame == 5 || frame == 50) {
            auto restored=decode_snapshot(stream.back(),error);
            check(restored && encode_snapshot(*restored) == stream.back(), "saved source/destination spot was lost");
            world=std::move(*restored);
        }
    }
    const auto on_goal=std::count_if(world.entities.begin(), world.entities.end(), [&](const Entity& e){return e.position==goal;});
    check(on_goal==3, "squad did not assemble three per tile");
    check(std::ranges::all_of(world.entities,[](const Entity& e){return e.order==OrderKind::Idle;}), "settled squad did not complete move");
    return stream;
}
}

int main(int argc, char** argv) {
    try {
        std::string error;
        auto rules=load_rules(argc>1?argv[1]:"tests/fixtures/coastal_rules.tsv",error);
        if (!rules) throw std::runtime_error(error);
        check_capacity(*rules);
        const auto serial=march(*rules,1);
        check(march(*rules,4)==serial && march(*rules,8)==serial, "1/4/8 worker snapshots differ");
        std::cout << "PASS: three distinct spots, capacity and enemy/vehicle exclusion, moving tail, 150 steps round trip and 1/4/8 byte equality\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
