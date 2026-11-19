// SPDX-License-Identifier: GPL-3.0-only
#include "rts_ai/strategy.h"
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace coastal::rts;

constexpr int extent = 128;
constexpr std::uint64_t step_limit = 4'800;

struct Scenario {
    const char* name;
    UnitKind opponent;
    int terrain;
};

constexpr std::array scenarios{
    Scenario{"Saltgrass", UnitKind::Skirmisher, 0},
    Scenario{"Cleft", UnitKind::Sentinel, 1},
    Scenario{"Tidelands", UnitKind::Artillery, 2},
};

Map make_map(int terrain) {
    Map map;
    map.width = map.height = extent;
    map.starts = {Cell{39, 64}, Cell{88, 64}};
    map.tiles.resize(extent * extent);
    const auto mirrored = [&](Cell at, Tile tile) {
        map.at(at) = tile;
        map.at({extent - 1 - at.x, at.y}) = tile;
    };
    for (int y = 0; y < extent; ++y) {
        for (int x = 0; x < extent / 2; ++x) {
            if (x < 4 || y < 4 || y >= extent - 4) {
                mirrored({x, y}, {Terrain::Water, 0, 0});
            } else if (terrain == 0 && x >= 56 && x < 63 && (y == 48 || y == 81)) {
                mirrored({x, y}, {Terrain::Ramp, 1, 0});
            } else if (terrain == 1 && x >= 60 && (y < 55 || y > 74) && y >= 27 && y <= 101) {
                mirrored({x, y}, {Terrain::Rock, 0, 0});
            } else if (terrain == 2 && x >= 53 && x < 61 && y >= 51 && y <= 76 &&
                       (y <= 58 || y >= 69)) {
                mirrored({x, y}, {Terrain::Water, 0, 0});
            }
        }
    }
    for (const auto cell : {Cell{43, 64}, Cell{44, 65}, Cell{44, 63}}) {
        mirrored(cell, {Terrain::Deposit, 0, 300});
    }
    for (const auto cell : {Cell{45, 68}, Cell{46, 68}, Cell{45, 69}}) {
        mirrored(cell, {Terrain::Deposit, 0, 460});
    }
    return map;
}

World start_match(const Scenario& scenario, const Rules& rules, unsigned trial) {
    World world;
    world.rules = rules;
    world.map = make_map(scenario.terrain);
    world.rng_state = 0x09F487C2C7EAE6BDULL ^ (0x9E3779B97F4A7C15ULL * (trial + 1));
    for (Side side : {Side::Blue, Side::Copper}) {
        world.side(side).funds = rules.initial_funds;
        const Cell start = world.map.starts[side_index(side)];
        const int direction = side == Side::Blue ? 1 : -1;
        if (!world.spawn(UnitKind::Pioneer, side, start) ||
            !world.spawn(UnitKind::Collector, side, {start.x + direction, start.y})) {
            throw std::runtime_error("Could not create the symmetric starting force.");
        }
    }
    return world;
}

bool fighting_contact(const AiMemory& memory) {
    return std::any_of(memory.enemies.begin(), memory.enemies.end(), [](const EnemyContact& enemy) {
        return enemy.kind >= UnitKind::Skirmisher && enemy.kind <= UnitKind::Artillery;
    });
}

void run_step(World& world, Navigator& navigator, Side adaptive, const Scenario& scenario,
              std::ostream* audit) {
    ai::drive_adaptive(world, adaptive, audit);
    ai::drive_scripted(world, adaptive == Side::Blue ? Side::Copper : Side::Blue, scenario.opponent);
    world.tick(navigator);
}

struct Result {
    bool won = false;
    bool completed = false;
    unsigned contact_switches = 0;
    bool expanded = false;
};

Result run_match(const Scenario& scenario, const Rules& rules, unsigned trial) {
    auto world = start_match(scenario, rules, trial);
    const auto adaptive = trial % 2 == 0 ? Side::Blue : Side::Copper;
    const auto fixed = adaptive == Side::Blue ? Side::Copper : Side::Blue;
    const auto victory = adaptive == Side::Blue ? MatchResult::BlueWon : MatchResult::CopperWon;
    const auto seed = world.rng_state;
    Navigator navigator;
    std::ostringstream audit;
    Result result;
    std::optional<World> resumed;
    Navigator resumed_navigator;
    auto previous = UnitKind::Count;
    while (world.result == MatchResult::Running && world.step < step_limit) {
        if (trial == 0 && world.step == 168) {
            std::string error;
            resumed = decode_snapshot(encode_snapshot(world), error);
            if (!resumed) throw std::runtime_error(error);
        }
        run_step(world, navigator, adaptive, scenario, &audit);
        if (resumed) {
            run_step(*resumed, resumed_navigator, adaptive, scenario, nullptr);
            if (world.step == 768 || world.result != MatchResult::Running) {
                if (encode_snapshot(world) != encode_snapshot(*resumed)) {
                    throw std::runtime_error("Adaptive skirmish diverged when resumed from a save.");
                }
                resumed.reset();
            }
        }
        if (const auto& memory = world.side(adaptive).ai) {
            if (memory->preference != previous && fighting_contact(*memory)) ++result.contact_switches;
            previous = memory->preference;
            result.expanded |= world.map.contains(memory->expansion);
        }
    }
    result.won = world.result == victory;
    result.completed = world.result != MatchResult::Running;
    const auto count = [&](Side side) {
        return std::count_if(world.entities.begin(), world.entities.end(), [side](const Entity& unit) {
            return unit.side == side;
        });
    };
    std::cout << "RTS_AI_AUDIT {\"map\":\"" << scenario.name << "\",\"trial\":" << trial
              << ",\"seed\":" << seed << "}\n" << audit.str();
    std::cout << "RTS_AI_MATCH {\"map\":\"" << scenario.name
              << "\",\"trial\":" << trial << ",\"adaptiveSide\":" << side_index(adaptive)
              << ",\"fixedDoctrine\":" << static_cast<unsigned>(scenario.opponent)
              << ",\"sameStartingFunds\":" << rules.initial_funds
              << ",\"step\":" << world.step << ",\"result\":" << static_cast<unsigned>(world.result)
              << ",\"won\":" << std::boolalpha << result.won
              << ",\"contactSwitches\":" << result.contact_switches
              << ",\"expanded\":" << result.expanded
              << ",\"adaptiveUnits\":" << count(adaptive) << ",\"fixedUnits\":" << count(fixed)
              << ",\"adaptiveFunds\":" << world.side(adaptive).funds
              << ",\"fixedFunds\":" << world.side(fixed).funds << "}\n" << std::flush;
    if (!result.completed) {
        for (const auto& unit : world.entities) {
            std::cerr << scenario.name << " trial " << trial << " unresolved: unit " << unit.id
                      << " side " << side_index(unit.side) << " kind " << static_cast<unsigned>(unit.kind)
                      << " HP " << unit.hp << " cell " << unit.position.x << ',' << unit.position.y
                      << " order " << static_cast<unsigned>(unit.order)
                      << " destination " << unit.destination.x << ',' << unit.destination.y
                      << " focus " << unit.focus << '\n';
        }
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3) return 64;
        unsigned trials = 10;
        if (argc == 3) {
            const std::string_view value(argv[2]);
            const auto [end, failure] = std::from_chars(value.data(), value.data() + value.size(), trials);
            if (failure != std::errc{} || end != value.data() + value.size() || trials < 1 || trials > 10) return 64;
        }
        std::string error;
        const auto rules = load_rules(argc >= 2 ? argv[1] : "tests/fixtures/coastal_rules.tsv", error);
        if (!rules) throw std::runtime_error(error);
        unsigned wins = 0;
        unsigned matches = 0;
        unsigned completed = 0;
        unsigned switched = 0;
        unsigned expanded = 0;
        for (const auto& scenario : scenarios) {
            for (unsigned trial = 0; trial < trials; ++trial) {
                const auto result = run_match(scenario, *rules, trial);
                wins += result.won;
                completed += result.completed;
                switched += result.contact_switches > 0;
                expanded += result.expanded;
                ++matches;
            }
        }
        std::cout << "RTS_AI_SUMMARY {\"wins\":" << wins << ",\"matches\":" << matches
                  << ",\"completed\":" << completed << ",\"withContactSwitch\":" << switched
                  << ",\"withExpansion\":" << expanded
                  << ",\"winRate\":" << double(wins) / matches << "}\n";
        return wins * 5 >= matches * 4 && completed == matches && switched > 0 ? 0 : 1;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
