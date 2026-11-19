// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/economy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>

using namespace coastal::rts;

namespace {

void expect(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto time = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto base = std::filesystem::temp_directory_path();
        for (int i = 0; i < 64; ++i) {
            path = base / ("coastal-economy-test-" + std::to_string(time) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(path)) return;
        }
        expect(false, "reserve test directory");
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

Rules parse_rules() {
    const std::filesystem::path fixture = "tests/fixtures/coastal_rules.tsv";
    std::string error = "stale";
    const auto rules = load_rules(fixture, error);
    expect(rules.has_value() && error.empty() && valid_rules(*rules), "read the original numeric fixture");
    expect(rules->initial_funds == 900 && rules->cargo_capacity == 24 && rules->harvest_ticks == 8 &&
           rules->spec(UnitKind::Workshop).power == -18, "preserve numeric fields");

    std::ifstream table(fixture);
    const std::string bytes{std::istreambuf_iterator<char>{table}, std::istreambuf_iterator<char>{}};
    TempDir temp;
    const auto path = temp.path / "rules.tsv";
    const auto check_bad = [&](std::string input, std::string_view label) {
        { std::ofstream file(path); file << input; }
        expect(!load_rules(path, error) && !error.empty(), label);
    };
    check_bad(bytes + "unknown\t1\n", "reject trailing table rows");
    auto changed = bytes;
    changed.replace(changed.find("900"), 3, "4294967296");
    check_bad(changed, "reject numeric overflow before narrowing");
    changed = bytes;
    changed.replace(changed.find("-18"), 3, "18");
    check_bad(changed, "reject an invalid production power sign");
    check_bad(bytes.substr(0, bytes.rfind("Artillery")), "reject missing types");
    expect(!load_rules(temp.path / "absent.tsv", error), "reject a missing table");
    return *rules;
}

World base(const Rules& rules) {
    World world;
    world.rules = rules;
    world.map.width = 16;
    world.map.height = 12;
    world.map.tiles.resize(std::size_t(world.map.width) * world.map.height);
    world.map.starts = {{{2, 2}, {14, 10}}};
    for (const auto side : {Side::Blue, Side::Copper}) {
        world.side(side).funds = rules.initial_funds;
        expect(world.spawn(UnitKind::Pioneer, side, world.map.starts[side_index(side)]).has_value(),
               "spawn both independent players");
    }
    return world;
}

Command order(CommandKind kind, Cell target = {}, UnitKind unit = UnitKind::Pioneer,
              std::uint32_t index = 0) {
    return {.kind = kind, .side = Side::Blue, .target = target, .unit = unit, .index = index, .units = {}};
}

void test_production(const Rules& rules) {
    auto world = base(rules);
    auto& blue = world.side(Side::Blue);
    const auto workshop = order(CommandKind::Build, {3, 2}, UnitKind::Workshop);
    expect(!world.apply(workshop) && blue.funds == rules.initial_funds, "require a deployed hub");
    auto deploy = order(CommandKind::Deploy);
    deploy.units = {2};
    expect(!world.apply(deploy), "cannot deploy the other player's pioneer");
    deploy.units = {1};
    world.find(1)->hp = 62;
    expect(world.apply(deploy) && world.find(1)->kind == UnitKind::Hub && world.find(1)->hp == 115,
           "deployed hub retains entity ID and scaled health");
    expect(!world.apply(deploy), "cannot redeploy an existing hub");
    world.map.at({4, 3}).terrain = Terrain::Water;
    expect(!can_build(world, Side::Blue, UnitKind::Depot, {4, 3}) &&
           !can_build(world, Side::Blue, UnitKind::Depot, {15, 1}) &&
           !can_build(world, Side::Blue, UnitKind::Depot, {2, 2}),
           "reject water, distant and occupied build sites");
    expect(world.apply(order(CommandKind::Build, {4, 2}, UnitKind::Depot)) && world.apply(workshop),
           "place both consumers");
    expect(blue.power_percent == 38 && blue.funds == 540, "fractional power and building charges");

    const auto rover = order(CommandKind::Queue, {}, UnitKind::Skirmisher);
    expect(world.apply(rover) && blue.queue.size() == 1 && blue.funds == 455, "reserve unit cost");
    expect(world.apply(order(CommandKind::Queue, {}, UnitKind::Sentinel)) &&
           world.apply(order(CommandKind::Cancel, {}, UnitKind::Pioneer, 1)) &&
           blue.queue.size() == 1 && blue.funds == 455, "cancel and return the exact paid cost");
    expect(!world.apply(order(CommandKind::Cancel, {}, UnitKind::Pioneer, 1)) && blue.funds == 455,
           "duplicate cancellation never credits funds");
    expect(world.apply(order(CommandKind::TogglePause)), "pause production");
    Navigator navigator;
    world.tick(navigator);
    expect(blue.queue.front().remaining_work == 3200, "paused work stays frozen");
    expect(world.apply(order(CommandKind::TogglePause)), "resume production");
    world.tick(navigator);
    expect(blue.queue.front().remaining_work == 3162, "shortage advances the precise fraction");
    expect(world.apply(order(CommandKind::Build, {1, 2}, UnitKind::Generator)) &&
           blue.power_percent == 100 && blue.funds == 335, "generation restores full production");
    expect(world.apply(order(CommandKind::Rally, {8, 2})), "change the rally tile");
    for (int i = 0; i < 31; ++i) world.tick(navigator);
    expect(blue.queue.size() == 1 && blue.queue.front().remaining_work == 62,
           "unit waits until all paid training work completes");
    world.tick(navigator);
    const auto trained = std::find_if(world.entities.begin(), world.entities.end(), [](const auto& entity) {
        return entity.kind == UnitKind::Skirmisher && entity.side == Side::Blue;
    });
    expect(blue.queue.empty() && trained != world.entities.end() && trained->order == OrderKind::Move &&
           trained->destination == Cell{8, 2} && blue.funds == 335, "spawn unit and apply the rally route");
}

void test_collection(const Rules& rules) {
    auto world = base(rules);
    auto& blue = world.side(Side::Blue);
    expect(world.apply(order(CommandKind::Deploy)), "deploy with initial owned pioneer");
    expect(world.apply(order(CommandKind::Build, {3, 2}, UnitKind::Depot)), "place the first depot");
    const auto id = world.spawn(UnitKind::Collector, Side::Blue, {2, 4});
    expect(id.has_value(), "spawn the starting collector");
    for (Cell rock : {Cell{2, 5}, Cell{1, 6}, Cell{3, 6}, Cell{2, 7}}) world.map.at(rock).terrain = Terrain::Rock;
    world.map.at({2, 6}) = {Terrain::Deposit, 0, 240};
    world.map.at({7, 4}) = {Terrain::Deposit, 0, 79};

    Navigator navigator;
    for (int i = 0; i < 240 && world.find(*id)->cargo == 0; ++i) world.tick(navigator);
    const auto* collector = world.find(*id);
    expect(collector->cargo == 24 && collector->gather == GatherPhase::Return && blue.income == 0,
           "skip a closer isolated deposit and fill cargo from a reachable deposit");

    world.find(3)->hp = 0;
    for (int i = 0; i < 30; ++i) world.tick(navigator);
    collector = world.find(*id);
    expect(collector->cargo == 24 && collector->depot == no_entity && blue.income == 0,
           "retain cargo if the only depot is destroyed");
    expect(world.apply(order(CommandKind::Build, {3, 1}, UnitKind::Depot)), "replace the depot");
    const auto start_funds = blue.funds;
    for (int i = 0; i < 800 && blue.deliveries < 4; ++i) world.tick(navigator);
    expect(blue.deliveries == 4 && blue.income == 79 && blue.funds == start_funds + 79 &&
           world.map.at({7, 4}).resource == 0 && world.map.at({2, 6}).resource == 240,
           "three full autonomous trips and one partial trip conserve resources");
}

}  // namespace

int main() {
    const auto rules = parse_rules();
    test_production(rules);
    test_collection(rules);
    std::cout << "Coastal economy: original rules, power, production and four resource trips passed.\n";
}
