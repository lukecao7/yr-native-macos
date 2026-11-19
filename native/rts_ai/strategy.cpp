// SPDX-License-Identifier: GPL-3.0-only
#include "strategy.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ostream>

namespace coastal::rts::ai {
namespace {

Command command(CommandKind kind, Side side) {
    Command value{};
    value.kind = kind;
    value.side = side;
    return value;
}

void establish_scripted_base(World& world, Side side) {
    world.apply(command(CommandKind::Deploy, side));
    const auto start = world.map.starts[side_index(side)];
    for (const auto kind : {UnitKind::Generator, UnitKind::Depot, UnitKind::Workshop}) {
        auto build = command(CommandKind::Build, side);
        build.unit = kind;
        bool placed = false;
        for (int radius = 2; radius <= 8 && !placed; ++radius) {
            for (int y = -radius; y <= radius && !placed; ++y) {
                for (int x = -radius; x <= radius; ++x) {
                    if (std::abs(x) + std::abs(y) != radius) continue;
                    build.target = {start.x + x, start.y + y};
                    if (world.apply(build)) {
                        placed = true;
                        break;
                    }
                }
            }
        }
    }
}

std::array<unsigned, static_cast<std::size_t>(UnitKind::Count)> types(const AiMemory& memory) {
    std::array<unsigned, static_cast<std::size_t>(UnitKind::Count)> counts{};
    for (const auto& enemy : memory.enemies) ++counts[static_cast<std::size_t>(enemy.kind)];
    return counts;
}

}  // namespace

void drive_scripted(World& world, Side side, UnitKind doctrine) {
    if (world.result != MatchResult::Running) return;
    if (world.step == 0) establish_scripted_base(world, side);
    if (world.step % 150 == 0 && world.side(side).queue.size() < 2) {
        auto train = command(CommandKind::Queue, side);
        train.unit = doctrine;
        world.apply(train);
    }
    if (world.step >= 600 && world.step % 180 == 0) {
        auto attack = command(CommandKind::AttackMove, side);
        attack.target = world.map.starts[1 - side_index(side)];
        for (const auto& unit : world.entities) {
            if (unit.side == side && unit.kind == doctrine) attack.units.push_back(unit.id);
        }
        if (!attack.units.empty()) world.apply(attack);
    }
}

void drive_adaptive(World& world, Side side, std::ostream* audit) {
    if (world.result != MatchResult::Running || world.step % 12 != 0) return;
    auto& current = world.side(side).ai;
    if (!current) current.emplace();
    auto& memory = *current;

    const auto observation = observe(world, side);
    const auto previous_contacts = types(memory);
    remember(observation, memory);
    const auto contacts = types(memory);
    const auto threat = estimate_threat(observation, memory);
    const auto plan = plan_industry(observation, memory, threat);
    const auto scouting = scout(observation, memory, threat);
    const auto maneuvers = plan_tactics(observation, memory, threat,
                                      scouting && !scouting->units.empty() ? scouting->units.front() : no_entity);

    unsigned issued = 0;
    bool expansion_placed = false;
    for (const auto& order : plan.commands) {
        if (!world.apply(order)) continue;
        ++issued;
        if (plan.expansion && order.kind == CommandKind::Build && order.unit == UnitKind::Depot &&
            order.target == *plan.expansion) {
            memory.expansion = *plan.expansion;
            expansion_placed = true;
        }
    }
    for (const auto& order : maneuvers) issued += world.apply(order);
    if (scouting) issued += world.apply(*scouting);

    const auto previous = memory.preference;
    memory.preference = plan.preference;
    if (!audit || (previous == plan.preference && previous_contacts == contacts && !expansion_placed)) return;
    *audit << "COASTAL_AI_DECISION {\"step\":" << observation.step
           << ",\"side\":" << side_index(side)
           << ",\"visibleEnemies\":" << observation.enemies.size()
           << ",\"knownCombatTypes\":[" << contacts[static_cast<unsigned>(UnitKind::Skirmisher)] << ','
           << contacts[static_cast<unsigned>(UnitKind::Sentinel)] << ','
           << contacts[static_cast<unsigned>(UnitKind::Artillery)] << ']'
           << ",\"utility\":[" << plan.scores[0] << ',' << plan.scores[1] << ',' << plan.scores[2] << ']'
           << ",\"from\":" << static_cast<unsigned>(previous)
           << ",\"to\":" << static_cast<unsigned>(plan.preference)
           << ",\"ordersApplied\":" << issued << ",\"expansion\":";
    if (expansion_placed) *audit << '[' << memory.expansion.x << ',' << memory.expansion.y << ']';
    else *audit << "null";
    *audit << "}\n";
}

}  // namespace coastal::rts::ai
