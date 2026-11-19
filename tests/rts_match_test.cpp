// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/match.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace {

using namespace coastal::rts;

void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

Command command(CommandKind kind, Side side, Cell target = {}, UnitKind unit = UnitKind::Pioneer) {
    Command value{};
    value.kind = kind;
    value.side = side;
    value.target = target;
    value.unit = unit;
    return value;
}

void establish(World& world, Side side) {
    const auto start = world.map.starts[side_index(side)];
    const int toward_center = side == Side::Blue ? 1 : -1;
    check(world.apply(command(CommandKind::Deploy, side)), "Deploying the Pioneer failed");
    check(world.apply(command(CommandKind::Build, side, {start.x - toward_center * 2, start.y + 2},
                              UnitKind::Generator)), "Generator placement failed");
    check(world.apply(command(CommandKind::Build, side, {start.x + toward_center * 2, start.y + 3},
                              UnitKind::Depot)), "Depot placement failed");
    check(world.apply(command(CommandKind::Build, side, {start.x + toward_center * 4, start.y + 2},
                              UnitKind::Workshop)), "Workshop placement failed");
    check(world.apply(command(CommandKind::Rally, side, {start.x + toward_center * 5, start.y},
                              UnitKind::Workshop)), "Rally point was rejected");
}

std::vector<EntityId> army(const World& world, Side side) {
    std::vector<EntityId> result;
    for (const auto& unit : world.entities) {
        if (unit.side == side && world.rules.spec(unit.kind).damage > 0 && !is_building(unit.kind)) {
            result.push_back(unit.id);
        }
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 64;
    using namespace coastal::rts;
    std::string error;
    auto match = open_match(argv[1], "Basin.map", error);
    if (!match) {
        std::cerr << error << '\n';
        return 1;
    }
    auto& world = *match;
    Navigator navigation;
    check(world.map.width == 128 && world.map.height == 128, "Expected the original 128-cell fixture");
    establish(world, Side::Blue);
    establish(world, Side::Copper);
    check(world.apply(command(CommandKind::Queue, Side::Blue, {}, UnitKind::Skirmisher)) &&
          world.apply(command(CommandKind::Queue, Side::Blue, {}, UnitKind::Skirmisher)) &&
          world.apply(command(CommandKind::Queue, Side::Blue, {}, UnitKind::Artillery)) &&
          world.apply(command(CommandKind::Queue, Side::Copper, {}, UnitKind::Skirmisher)),
          "Training commands were rejected");

    for (int elapsed = 0; elapsed < 900; ++elapsed) {
        if (world.side(Side::Blue).deliveries >= 3 && world.side(Side::Copper).deliveries >= 3 &&
            world.side(Side::Blue).queue.empty() && world.side(Side::Copper).queue.empty()) break;
        world.tick(navigation);
    }
    check(world.side(Side::Blue).deliveries >= 3 && world.side(Side::Copper).deliveries >= 3,
          "Collectors did not complete three autonomous trips");
    auto select = command(CommandKind::Select, Side::Blue);
    select.units = army(world, Side::Blue);
    check(select.units.size() == 3 && army(world, Side::Copper).size() == 1,
          "Production did not supply the expected armies");
    check(world.apply(select), "Army selection was rejected");
    const auto group = command(CommandKind::AssignGroup, Side::Blue);
    check(world.apply(group) && world.apply(command(CommandKind::Select, Side::Blue)) &&
          world.apply(command(CommandKind::RecallGroup, Side::Blue)) &&
          world.side(Side::Blue).selected == select.units, "Control-group round trip failed");
    const auto attack = command(CommandKind::AttackMove, Side::Blue, world.map.starts[1]);
    check(world.apply(attack), "Attack-move for the control group was rejected");

    const auto origin = world.find(select.units.front())->position;
    const auto start = world.step;
    for (int elapsed = 0; elapsed < 78; ++elapsed) world.tick(navigation);
    const auto* advancing = world.find(select.units.front());
    check(advancing && advancing->position != origin && world.result == MatchResult::Running,
          "The control group did not march toward the rival base");

    const auto saved = encode_snapshot(world);
    auto restored = decode_snapshot(saved, error);
    if (!restored) {
        std::cerr << "Restore: " << error << '\n';
        return 1;
    }
    check(encode_snapshot(*restored) == saved, "Canonical bytes changed on load");
    Navigator restored_navigation;
    const auto checkpoint = world.step;
    const auto end = checkpoint + 600;
    const std::array script{ReplayCommand{checkpoint + 17, attack}};
    if (!replay(world, navigation, script, end, error) ||
        !replay(*restored, restored_navigation, script, end, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    check(world.step == end && restored->step == end && encode_snapshot(world) == encode_snapshot(*restored),
          "Straight play and saved replay diverged after 600 steps");
    check(world.rng_state != 0x295F732D18C2CB5DULL, "Combat did not consume the saved RNG");
    check(world.step > start, "The simulation did not advance");

    // Sweep any surviving Copper entities with the same public attack order.
    // The comparison above includes the original 600-step battle on both paths.
    for (int sweep = 0; sweep < 5 && world.result == MatchResult::Running; ++sweep) {
        auto survivor = std::find_if(world.entities.begin(), world.entities.end(), [](const Entity& unit) {
            return unit.side == Side::Copper;
        });
        check(survivor != world.entities.end(), "No rival entity was left before match settlement");
        auto pursue = command(CommandKind::AttackMove, Side::Blue, survivor->position);
        pursue.units = army(world, Side::Blue);
        check(world.apply(pursue), "Rival clearing order was rejected");
        for (int elapsed = 0; elapsed < 200 && world.result == MatchResult::Running; ++elapsed) world.tick(navigation);
    }
    check(world.result == MatchResult::BlueWon &&
          std::none_of(world.entities.begin(), world.entities.end(), [](const Entity& unit) {
              return unit.side == Side::Copper;
          }), "Full elimination did not produce a Blue win");

    std::cout << "RTS_MATCH {\"map\":\"Basin\",\"width\":128,\"height\":128"
              << ",\"checkpoint\":" << checkpoint
              << ",\"matchedThrough\":" << end
              << ",\"snapshotBytes\":" << saved.size()
              << ",\"deliveries\":" << world.side(Side::Blue).deliveries
              << ",\"victoryStep\":" << world.step << "}\n";
}
