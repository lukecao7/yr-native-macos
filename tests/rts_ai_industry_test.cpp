// SPDX-License-Identifier: GPL-3.0-only
#include "rts_ai/strategy.h"
#include "rts_core/economy.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace coastal::rts;
using namespace coastal::rts::ai;

void expect(bool passed, std::string_view message) {
    if (passed) return;
    std::cerr << message << '\n';
    std::exit(1);
}

const Command* first_order(const IndustryPlan& plan, CommandKind kind, UnitKind unit) {
    const auto order = std::find_if(plan.commands.begin(), plan.commands.end(), [&](const Command& order) {
        return order.kind == kind && order.unit == unit;
    });
    return order == plan.commands.end() ? nullptr : &*order;
}

bool same_orders(const IndustryPlan& first, const IndustryPlan& second) {
    return first.scores == second.scores && first.preference == second.preference &&
           first.expansion == second.expansion &&
           std::ranges::equal(first.commands, second.commands, [](const Command& a, const Command& b) {
               return a.kind == b.kind && a.side == b.side && a.target == b.target &&
                      a.unit == b.unit && a.index == b.index && a.units == b.units &&
                      a.add == b.add && a.focus == b.focus;
           });
}

Observation terrain(const Rules& rules) {
    Observation observation;
    observation.rules = rules;
    observation.width = 24;
    observation.height = 14;
    observation.home = {7, 6};
    observation.funds = rules.initial_funds;
    observation.visible.assign(std::size_t(observation.width) * observation.height, {true, Terrain::Ground, 0});
    return observation;
}

void mine(Observation& observation, Cell cell, std::uint16_t resource) {
    observation.visible[observation.index(cell)] = {true, Terrain::Deposit, resource};
}

void spawn(Observation& observation, UnitKind kind, Cell cell) {
    observation.allies.push_back({.id = static_cast<EntityId>(observation.allies.size() + 1),
                                  .kind = kind, .side = observation.side, .position = cell,
                                  .hp = observation.rules.spec(kind).hp, .destination = {}, .deposit = {}});
}

Observation established(const Rules& rules) {
    auto observation = terrain(rules);
    spawn(observation, UnitKind::Hub, observation.home);
    spawn(observation, UnitKind::Generator, {7, 4});
    spawn(observation, UnitKind::Depot, {4, 6});
    spawn(observation, UnitKind::Workshop, {7, 8});
    spawn(observation, UnitKind::Collector, {5, 6});
    spawn(observation, UnitKind::Skirmisher, {8, 7});
    spawn(observation, UnitKind::Sentinel, {8, 8});
    mine(observation, {5, 6}, 120);
    return observation;
}

void test_opening(const Rules& rules) {
    auto observation = terrain(rules);
    spawn(observation, UnitKind::Pioneer, observation.home);
    spawn(observation, UnitKind::Collector, {8, 6});
    mine(observation, {10, 6}, 240);
    const auto plan = plan_industry(observation, {}, {});
    expect(!plan.commands.empty() && plan.commands.front().kind == CommandKind::Deploy &&
           plan.commands.front().units == std::vector<EntityId>{1}, "Deploy an owned Pioneer first");
    expect(first_order(plan, CommandKind::Build, UnitKind::Generator) &&
           first_order(plan, CommandKind::Build, UnitKind::Depot) &&
           first_order(plan, CommandKind::Build, UnitKind::Workshop) &&
           first_order(plan, CommandKind::Queue, UnitKind::Skirmisher) &&
           first_order(plan, CommandKind::Queue, UnitKind::Sentinel),
           "Open a powered economy and train a scout with a Sentinel screen");

    World world;
    world.rules = rules;
    world.map.width = observation.width;
    world.map.height = observation.height;
    world.map.starts = {observation.home, Cell{20, 8}};
    for (const SeenTile tile : observation.visible) world.map.tiles.push_back({tile.terrain, 0, tile.resource});
    world.side(Side::Blue).funds = rules.initial_funds;
    for (const auto& ally : observation.allies) {
        expect(world.spawn(ally.kind, ally.side, ally.position) == ally.id, "Reproduce the observed starting pieces");
    }
    for (const auto& command : plan.commands) expect(world.apply(command), "Opening must use legal real commands");
    expect(world.side(Side::Blue).funds == 195 && world.side(Side::Blue).power_percent == 100 &&
           world.side(Side::Blue).queue.size() == 2, "Opening preserves exact cost, power and queue");

    observation.funds = 465;
    const auto constrained = plan_industry(observation, {}, {});
    unsigned cost = 0;
    for (const auto& command : constrained.commands) {
        if (command.kind == CommandKind::Build || command.kind == CommandKind::Queue) cost += rules.spec(command.unit).cost;
    }
    expect(cost <= observation.funds && !first_order(constrained, CommandKind::Queue, UnitKind::Sentinel),
           "A poor opening cannot buy past its actual funds");
}

void test_counters(const Rules& rules) {
    auto observation = established(rules);
    observation.step = 1000;
    const auto blind = plan_industry(observation, {}, {});
    expect(blind.preference == UnitKind::Skirmisher, "The unseen default favors a mobile scout");

    constexpr std::array preferred{UnitKind::Sentinel, UnitKind::Artillery, UnitKind::Skirmisher};
    for (std::size_t i = 0; i < combat_kinds.size(); ++i) {
        const EnemyContact contact{400, combat_kinds[i], {19, 8}, rules.spec(combat_kinds[i]).hp, 990};
        observation.enemies = {contact};
        const auto live = plan_industry(observation, {}, {});
        expect(live.preference == preferred[i] && first_order(live, CommandKind::Queue, preferred[i]),
               "Seen light, armored and long-range formations choose distinct counter units");
        AiMemory memory;
        memory.enemies = {contact};
        expect(same_orders(live, plan_industry(observation, memory, {})),
               "A current sighting must not count again as a remembered enemy");
        observation.enemies.clear();
        expect(plan_industry(observation, memory, {}).preference == preferred[i],
               "A recent contact informs off-screen recruitment");
        memory.enemies.front().seen_at = observation.step - AiMemory::contact_lifetime;
        expect(same_orders(blind, plan_industry(observation, memory, {})),
               "An expired contact no longer influences recruitment");
    }
}

void test_siege_support(const Rules& rules) {
    auto observation = established(rules);
    spawn(observation, UnitKind::Sentinel, {9, 8});
    observation.enemies.push_back({400, UnitKind::Skirmisher, {19, 8}, 60, 0});
    observation.funds = rules.spec(UnitKind::Artillery).cost - 1;
    const auto saving = plan_industry(observation, {}, {});
    expect(saving.preference == UnitKind::Sentinel && saving.commands.empty(),
           "Save for missing siege support when a frontline already exists");
    ++observation.funds;
    const auto funded = plan_industry(observation, {}, {});
    expect(funded.preference == UnitKind::Sentinel && first_order(funded, CommandKind::Queue, UnitKind::Artillery),
           "Train siege support without losing the observed counter preference");
}

void test_expansion(const Rules& rules) {
    auto observation = established(rules);
    constexpr Cell far_mine{15, 6};
    mine(observation, far_mine, 240);
    expect(!plan_industry(observation, {}, {}).expansion, "No expansion before the existing depot delivers");
    observation.deliveries = 4;
    const auto expanded = plan_industry(observation, {}, {});
    const auto* depot = first_order(expanded, CommandKind::Build, UnitKind::Depot);
    expect(expanded.expansion && depot && depot->target == expanded.expansion &&
               std::abs(depot->target.x - far_mine.x) + std::abs(depot->target.y - far_mine.y) <= 3,
           "A productive economy chooses a second depot next to a distant deposit");
    expect(first_order(expanded, CommandKind::Queue, UnitKind::Collector),
           "A funded expansion recruits an extra collector");

    auto saving = observation;
    spawn(saving, UnitKind::Artillery, {8, 9});
    saving.funds = 150;
    expect(plan_industry(saving, {}, {}).commands.empty(),
           "A safe and productive route can justify saving for a second depot");

    ThreatGrid threat{observation.width, observation.height,
                      std::vector<std::uint16_t>(observation.visible.size())};
    threat.cells[observation.index(far_mine)] = 100;
    expect(!plan_industry(observation, {}, threat).expansion, "An unsafe deposit does not justify expansion");

    for (int y = 0; y < observation.height; ++y) {
        observation.visible[observation.index({12, y})].terrain = Terrain::Water;
    }
    expect(!plan_industry(observation, {}, {}).expansion, "An isolated island does not justify a depot");

    auto unknown = established(rules);
    unknown.deliveries = 4;
    auto hidden = unknown;
    unknown.visible[unknown.index(far_mine)] = {false, Terrain::Ground, 0};
    hidden.visible[hidden.index(far_mine)] = {false, Terrain::Deposit, 900};
    expect(same_orders(plan_industry(unknown, {}, {}), plan_industry(hidden, {}, {})),
           "Unscouted terrain payload cannot affect production or expansion");
}

}  // namespace

int main() {
    std::string error;
    const auto rules = load_rules("tests/fixtures/coastal_rules.tsv", error);
    expect(rules.has_value(), "Load the original open rules");
    test_opening(*rules);
    test_counters(*rules);
    test_siege_support(*rules);
    test_expansion(*rules);
    std::cout << "RTS AI industry tests passed (real costs, counters, siege support and safe expansion)\n";
}
