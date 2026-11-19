// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/snapshot.h"

#include "rts_core/combat.h"
#include "rts_core/economy.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace coastal::rts;

namespace {

void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto name = (std::filesystem::temp_directory_path() / "coastal-snapshot-test-XXXXXX").string();
        if (!::mkdtemp(name.data())) throw std::runtime_error("Could not create a test directory.");
        path = name;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

World small_world(const Rules& rules) {
    World world;
    world.rules = rules;
    world.map.width = 6;
    world.map.height = 5;
    world.map.starts = {Cell{1, 1}, Cell{4, 3}};
    world.map.tiles.resize(30);
    world.map.at({0, 2}) = {Terrain::Water, 0, 0};
    world.map.at({1, 2}) = {Terrain::Rock, 1, 0};
    world.map.at({3, 1}) = {Terrain::Deposit, 2, 123};
    world.map.at({3, 2}) = {Terrain::Ramp, 1, 0};
    for (const auto side : {Side::Blue, Side::Copper}) {
        world.side(side).funds = rules.initial_funds;
        require(world.spawn(UnitKind::Pioneer, side, world.map.starts[side_index(side)]).has_value(),
                "Each side can start.");
    }
    return world;
}

World rich_world(const Rules& rules) {
    World world = small_world(rules);
    const auto collector = world.spawn(UnitKind::Collector, Side::Blue, {2, 1});
    const auto depot = world.spawn(UnitKind::Depot, Side::Blue, {2, 2});
    require(collector.has_value() && depot.has_value(), "The save has dynamic entities.");
    auto& hauler = *world.find(*collector);
    hauler.order = OrderKind::Gather;
    hauler.destination = {3, 1};
    hauler.facing = {-1, 1};
    hauler.move_wait = 1;
    hauler.gather = GatherPhase::Harvest;
    hauler.deposit = {3, 1};
    hauler.depot = *depot;
    hauler.cargo = 1;

    auto& blue = world.side(Side::Blue);
    blue.funds = 0x0102030405060708ULL;
    blue.income = 48;
    blue.deliveries = 3;
    blue.power_percent = 65;
    blue.production_paused = true;
    blue.rally = {5, 2};
    const auto& skirmisher = rules.spec(UnitKind::Skirmisher);
    blue.queue.push_back({UnitKind::Skirmisher, skirmisher.cost, 30});
    blue.selected = {*collector};
    blue.groups[2] = {1, *collector};

    // Entity IDs remain monotonic after a shooter or depot has been retired.
    world.next_id = *depot + 2;
    world.projectiles.push_back({world.next_id - 1, 2, {3, 2}, {4, 3}, 3, 6, 22});
    world.step = 0x0102030405060708ULL;
    world.rng_state = 0x1827364554637281ULL;
    return world;
}

// Preserve checksum validity when exercising structural decoding independently.
void recompute_checksum(std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : std::span(bytes).first(bytes.size() - sizeof(hash))) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    for (auto& value : std::span(bytes).last(sizeof(hash))) {
        value = static_cast<std::uint8_t>(hash);
        hash >>= 8;
    }
}

void check_codec(const Rules& rules) {
    const auto world = rich_world(rules);
    const auto bytes = encode_snapshot(world);
    constexpr std::array<std::uint8_t, 14> prefix{
        'C', 'R', 'T', 'S', 1, 0, 8, 7, 6, 5, 4, 3, 2, 1,
    };
    require(std::ranges::equal(std::span(bytes).first(prefix.size()), prefix),
            "Header, schema version, and tick counter have fixed little-endian bytes.");

    std::string error;
    const auto restored = decode_snapshot(bytes, error);
    require(restored.has_value() && error.empty(), "Decode a valid save.");
    const auto& restored_blue = restored->side(Side::Blue);
    const auto& restored_hauler = *restored->find(3);
    require(restored->map.starts == world.map.starts &&
                restored->map.at({3, 1}).height == 2 &&
                restored->map.at({3, 1}).resource == 123 &&
                restored->map.at({0, 2}).terrain == Terrain::Water,
            "Terrain, height, reserves, and start cells survive the round trip.");
    require(restored->rules.spec(UnitKind::Depot).power == rules.spec(UnitKind::Depot).power &&
                restored_blue.power_percent == 65 && restored_blue.income == 48 &&
                restored_blue.deliveries == 3 && restored_blue.production_paused &&
                restored_blue.funds == world.side(Side::Blue).funds &&
                restored_blue.rally == Cell{5, 2} &&
                restored_blue.queue.size() == 1 && restored_blue.queue.front().remaining_work == 30 &&
                restored_blue.groups[2] == std::vector<EntityId>{1, 3},
            "Power, economy, production, rally point, and control groups survive the round trip.");
    require(restored_hauler.gather == GatherPhase::Harvest && restored_hauler.cargo == 1 &&
                restored_hauler.order == OrderKind::Gather && restored_hauler.depot == 4 &&
                restored_hauler.facing == Cell{-1, 1} && restored_hauler.destination == Cell{3, 1} &&
                restored->projectiles.size() == 1 && restored->projectiles.front().remaining == 3 &&
                restored->projectiles.front().damage == 22 && restored->rng_state == world.rng_state &&
                restored->next_id == world.next_id,
            "Orders, collection cargo, in-flight projectiles, and RNG state survive the round trip.");
    require(encode_snapshot(*restored) == bytes, "Equivalent authority produces identical save bytes.");

    const auto reject = [&](std::span<const std::uint8_t> malformed, const char* description) {
        require(!decode_snapshot(malformed, error) && !error.empty(), description);
    };
    reject(std::span(bytes).first(bytes.size() - 1), "The decoder rejects the truncated save.");
    auto corrupt = bytes;
    corrupt[15] ^= 0x80;
    reject(corrupt, "The decoder rejects a damaged checksum.");
    corrupt = bytes;
    corrupt[4] = 99;
    recompute_checksum(corrupt);
    reject(corrupt, "The decoder rejects unknown format versions.");
    corrupt = bytes;
    corrupt[27] = 0;
    corrupt[28] = 3;
    recompute_checksum(corrupt);
    reject(corrupt, "The decoder bounds map allocation even if the checksum is valid.");
    corrupt = bytes;
    corrupt.insert(corrupt.end() - 8, 0);
    recompute_checksum(corrupt);
    reject(corrupt, "The decoder rejects extra records before a valid checksum.");
}

void check_atomic_save(const Rules& rules) {
    const TemporaryDirectory temp;
    const auto save = temp.path / "test.save";
    std::string error;
    auto world = rich_world(rules);
    const auto expected = encode_snapshot(world);
    require(save_world(save, world, error) && error.empty(), "Write a valid save.");
    auto restored = load_world(save, error);
    require(restored.has_value() && error.empty() && encode_snapshot(*restored) == expected,
            "The atomic write persists the exact binary world.");

    world.rng_state = 0;
    require(!save_world(save, world, error) && !error.empty(), "Reject an invalid replacement.");
    restored = load_world(save, error);
    require(restored.has_value() && encode_snapshot(*restored) == expected,
            "A failed replacement preserves the prior save.");

    const auto directory_target = temp.path / "existing-directory";
    std::filesystem::create_directory(directory_target);
    require(!save_world(directory_target, *restored, error) && !error.empty(),
            "Rename failure leaves an existing destination intact.");
    require(std::filesystem::is_directory(directory_target) &&
                std::distance(std::filesystem::directory_iterator(temp.path),
                              std::filesystem::directory_iterator{}) == 2,
            "The failed rename removes its own temporary file.");
}

Command blue_command(CommandKind kind, Cell target = {}) {
    return {kind, Side::Blue, target, UnitKind::Pioneer, 0, {}, false};
}

void check_replay(const Rules& rules) {
    auto start = small_world(rules);
    auto select = blue_command(CommandKind::Select);
    select.units = {1};
    const std::array commands{
        ReplayCommand{0, select},
        ReplayCommand{0, blue_command(CommandKind::AssignGroup)},
        ReplayCommand{0, blue_command(CommandKind::Move, {3, 1})},
        ReplayCommand{4, blue_command(CommandKind::RecallGroup)},
        ReplayCommand{4, blue_command(CommandKind::Move, {5, 1})},
        ReplayCommand{14, blue_command(CommandKind::Move, {-2, -2})},
    };

    auto continuous = start;
    Navigator continuous_routes;
    std::string error;
    require(replay(continuous, continuous_routes, commands, 14, error),
            "Apply ordered same-step events without executing the end boundary.");
    auto saved = start;
    Navigator initial_routes;
    require(replay(saved, initial_routes, commands, 4, error), "Play the first part of the replay.");
    const auto middle_bytes = encode_snapshot(saved);
    auto resumed = decode_snapshot(middle_bytes, error);
    require(resumed.has_value(), "Resume the replay from a saved step.");
    Navigator resumed_routes;
    require(replay(*resumed, resumed_routes, commands, 14, error) &&
                encode_snapshot(*resumed) == encode_snapshot(continuous),
            "Split and continuous command streams end with identical authority.");
    require(!replay(*resumed, resumed_routes, commands, 15, error) &&
                resumed->step == 14 && error.find("14") != std::string::npos,
            "Report the exact rejected command step without advancing it.");

    const std::array reversed{commands[3], commands[0]};
    Navigator unused_routes;
    const auto before = encode_snapshot(start);
    require(!replay(start, unused_routes, reversed, 9, error) && encode_snapshot(start) == before,
            "Reject unordered scripts before mutating the world.");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto path = argc > 1 ? argv[1] : "tests/fixtures/coastal_rules.tsv";
        std::string error;
        const auto rules = load_rules(path, error);
        if (!rules) throw std::runtime_error(error);
        check_codec(*rules);
        check_atomic_save(*rules);
        check_replay(*rules);
        std::cout << "RTS snapshot tests passed (canonical bytes, malformed saves, atomic replace, replay)\n";
    } catch (const std::exception& error) {
        std::cerr << "rts_snapshot_test: " << error.what() << '\n';
        return 1;
    }
}
