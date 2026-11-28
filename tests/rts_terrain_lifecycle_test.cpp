// SPDX-License-Identifier: GPL-3.0-only
#include "rts_ai/strategy.h"
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/iso.h"
#include "rts_core/snapshot.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace coastal::rts;

constexpr Cell plain{3, 1};
constexpr Cell slope{4, 1};
constexpr Cell water{2, 3};
constexpr Cell rock{3, 3};

void require(bool condition, const std::string& description) {
    if (!condition) throw std::runtime_error(description);
}

World load_fixture(const std::filesystem::path& fixtures) {
    World world;
    std::string error;
    const auto rules = load_rules(fixtures / "coastal_rules.tsv", error);
    require(rules.has_value(), error);
    const auto map = load_map(fixtures / "terrain-lifecycle.map", error);
    require(map.has_value(), error);
    world.rules = *rules;
    world.map = *map;
    world.side(Side::Blue).funds = rules->initial_funds;
    require(world.spawn(UnitKind::Hub, Side::Blue, map->starts[0]).has_value(), "spawn a hub");
    require(world.spawn(UnitKind::Pioneer, Side::Copper, map->starts[1]).has_value(), "spawn the other house");
    return world;
}

SeenTile observe_tile(const World& world, Cell cell) {
    const auto observation = coastal::rts::ai::observe(world, Side::Blue);
    const auto tile = observation.visible.at(observation.index(cell));
    require(tile.known, "the fixture cell is visible");
    return tile;
}

World roundtrip(const World& world) {
    std::string error;
    const auto first = encode_snapshot(world);
    auto restored = decode_snapshot(first, error);
    require(restored.has_value(), error);
    const auto second = encode_snapshot(*restored);
    require(first == second, "restoring and saving again preserves exact state");
    return std::move(*restored);
}

std::vector<std::uint8_t> render(const World& world) {
    constexpr int width = 384, height = 256;
    std::vector<std::uint8_t> pixels(width * height * 4);
    Camera camera;
    camera.center_on(slope, world.map);
    require(render_isometric(world, camera, {}, pixels, width, height), "render the fixture offscreen");
    return pixels;
}

void check_empty(const World& world) {
    require(world.map.at(plain).resource == 0, "map input contains an empty deposit marker");
    require(can_build(world, Side::Blue, UnitKind::Generator, plain), "empty marker exposes buildable ground");
    const auto seen = observe_tile(world, plain);
    require(seen.terrain == Terrain::Ground && seen.resource == 0, "AI sees the ground below the empty marker");
    require(!can_build(world, Side::Blue, UnitKind::Generator, slope), "an empty slope remains a slope");
}

void check_lifecycle(World world) {
    const auto empty_pixels = render(world);
    world.map.at(plain).resource = 5;
    world.map.at(slope).resource = 7;
    require(!can_build(world, Side::Blue, UnitKind::Generator, plain), "live deposit prevents construction");
    const auto seen = observe_tile(world, slope);
    require(seen.terrain == Terrain::Deposit && seen.resource == 7, "AI sees regrowth on the underlying slope");
    require(render(world) != empty_pixels, "live deposit changes the 2D view");
    world = roundtrip(world);
    require(world.map.at(slope).terrain == Terrain::Ramp && world.map.at(slope).height == 1 &&
                observe_tile(world, slope).terrain == Terrain::Deposit,
            "save preserves the underlying slope, height and the active deposit");

    const auto collector = world.spawn(UnitKind::Collector, Side::Blue, slope);
    require(collector.has_value(), "spawn a collector at the deposit");
    tick_economy(world);
    require(world.find(*collector)->gather == GatherPhase::Harvest, "economy discovers the new deposit");
    for (unsigned step = 0; step < world.rules.harvest_ticks; ++step) tick_economy(world);
    require(world.find(*collector)->cargo == 7, "the collector removes the remaining deposit");
    require(observe_tile(world, slope).terrain == Terrain::Ramp && world.map.at(slope).resource == 0,
            "depletion exposes the original slope");
    world.map.at(plain).resource = 0;
    check_empty(roundtrip(world));

    world.map.at(slope).resource = 7;
    require(observe_tile(world, slope).terrain == Terrain::Deposit, "resource replenishment exposes the deposit again");
    world.map.at(slope).resource = 0;
    require(observe_tile(roundtrip(world), slope).terrain == Terrain::Ramp,
            "repeat depletion and restore expose the same slope");
}

void check_routes_and_invalid_overlay(World world) {
    Navigator navigator;
    for (const auto amount : {0, 7, 0}) {
        world.map.at(plain).resource = amount;
        world.map.at(slope).resource = amount;
        world.map.at(water).resource = amount;
        world.map.at(rock).resource = amount;
        navigator.prepare(world);
        require(navigator.next(world, plain, slope) == slope, "the route crosses the same walkable slope in each state");
        require(!navigator.next(world, plain, water) && !navigator.next(world, plain, rock),
                "bad resource quantities do not open water or rock in the routing cache");
        if (amount == 0) continue;
        bool rejected = false;
        try { (void)encode_snapshot(world); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "the writer rejects resource quantities on blocked terrain");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path fixtures = argc >= 2 ? argv[1] : "tests/fixtures";
        auto world = load_fixture(fixtures);
        if (argc == 4 && std::string_view(argv[2]) == "--write-legacy") {
            std::string error;
            require(save_world(argv[3], world, error), error);
            return 0;
        }
        bool passed = true;
        const auto run = [&](std::string_view name, auto&& test) {
            try {
                test();
                std::cout << "PASS " << name << '\n';
            } catch (const std::exception& error) {
                std::cerr << "FAIL " << name << ": " << error.what() << '\n';
                passed = false;
            }
        };
        run("empty map marker", [&] { check_empty(world); });
        run("replenishment, harvest and save/load", [&] { check_lifecycle(world); });
        run("navigation and blocked overlay", [&] { check_routes_and_invalid_overlay(world); });
        if (argc == 4 && std::string_view(argv[2]) == "--read-legacy") {
            run("save from the baseline executable", [&] {
                std::string error;
                const auto legacy = load_world(argv[3], error);
                require(legacy.has_value(), error);
                check_empty(roundtrip(*legacy));
            });
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
