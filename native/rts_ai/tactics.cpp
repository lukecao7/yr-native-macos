// SPDX-License-Identifier: GPL-3.0-only
#include "strategy.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>

namespace coastal::rts::ai {
namespace {

constexpr Cell steps[] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1},
                         {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

enum class Ground { Unknown, Open, Blocked };

int distance(Cell from, Cell to) noexcept {
    return std::max(std::abs(from.x - to.x), std::abs(from.y - to.y));
}

int sign(int number) noexcept {
    return (number > 0) - (number < 0);
}

bool combatant(const Observation& observation, const Entity& unit) noexcept {
    const auto& spec = observation.rules.spec(unit.kind);
    return unit.side == observation.side && unit.hp && spec.move_period && spec.damage;
}

class GroundView {
public:
    GroundView(const Observation& observation, const AiMemory& memory)
        : observation_(observation), memory_(memory),
          occupied_(std::size_t(observation.width) * observation.height, no_entity) {
        for (const auto& unit : observation.allies) put(unit.position, unit.id);
        for (const auto& enemy : observation.enemies) put(enemy.position, enemy.id);
    }

    bool occupied(Cell cell, EntityId except = no_entity) const noexcept {
        if (!observation_.contains(cell)) return true;
        const auto id = occupied_[observation_.index(cell)];
        return id != no_entity && id != except;
    }

    Ground at(Cell cell) const noexcept {
        if (!observation_.contains(cell)) return Ground::Blocked;
        const auto index = static_cast<std::size_t>(observation_.index(cell));
        const SeenTile* tile = nullptr;
        if (index < observation_.visible.size() && observation_.visible[index].known) {
            tile = &observation_.visible[index];
        } else if (index < memory_.chart.size() && memory_.chart[index].known) {
            tile = &memory_.chart[index];
        }
        if (!tile) return Ground::Unknown;
        return tile->terrain == Terrain::Water || tile->terrain == Terrain::Rock
                   ? Ground::Blocked : Ground::Open;
    }

    void reserve(Cell position, EntityId unit) { put(position, unit); }

    std::optional<Cell> near(Cell position, bool known_only, EntityId unit = no_entity) const noexcept {
        // Keep search order fixed when two approach cells are equivalent.
        for (int radius = 0; radius <= 3; ++radius) {
            for (int y = -radius; y <= radius; ++y) {
                for (int x = -radius; x <= radius; ++x) {
                    if (std::max(std::abs(x), std::abs(y)) != radius) continue;
                    const Cell cell{position.x + x, position.y + y};
                    const auto ground = at(cell);
                    if (ground != Ground::Blocked && (!known_only || ground == Ground::Open) &&
                        !occupied(cell, unit)) return cell;
                }
            }
        }
        return std::nullopt;
    }

private:
    void put(Cell cell, EntityId id) {
        if (observation_.contains(cell)) occupied_[observation_.index(cell)] = id;
    }

    const Observation& observation_;
    const AiMemory& memory_;
    std::vector<EntityId> occupied_;
};

int nearest_weapon(const Observation& observation, Cell position) noexcept {
    // Negative clearance means the point is inside an observed weapon's range.
    int clearance = Map::max_dimension;
    for (const auto& enemy : observation.enemies) {
        const auto& spec = observation.rules.spec(enemy.kind);
        if (spec.damage) clearance = std::min(clearance, distance(position, enemy.position) - spec.range);
    }
    return clearance;
}

int risk(const ThreatGrid& threat, Cell cell) noexcept {
    if (cell.x < 0 || cell.y < 0 || cell.x >= threat.width || cell.y >= threat.height) return 0;
    const auto index = std::size_t(cell.y) * threat.width + cell.x;
    return index < threat.cells.size() ? threat.cells[index] : 0;
}

template <typename Visitor>
void visit_reachable(const GroundView& ground, const Entity& unit, Visitor&& visit) noexcept {
    constexpr int radius = 5;
    constexpr int diameter = radius * 2 + 1;
    struct Reachable {
        Cell position;
        int hops;
    };
    std::array<Reachable, diameter * diameter> queue{};
    std::array<bool, diameter * diameter> visited{};
    const auto index = [&](Cell cell) {
        return (cell.y - unit.position.y + radius) * diameter + cell.x - unit.position.x + radius;
    };
    const auto open = [&](Cell cell) {
        return ground.at(cell) == Ground::Open && !ground.occupied(cell, unit.id);
    };
    std::size_t count = 1;
    queue[0] = {unit.position, 0};
    visited[index(unit.position)] = true;
    for (std::size_t head = 0; head < count; ++head) {
        const auto [position, hops] = queue[head];
        if (hops) visit(position, hops);
        if (hops == radius) continue;
        for (const auto step : steps) {
            const Cell cell{position.x + step.x, position.y + step.y};
            const auto cell_index = index(cell);
            if (visited[cell_index] || !open(cell)) continue;
            if (step.x && step.y &&
                (!open({position.x + step.x, position.y}) || !open({position.x, position.y + step.y}))) {
                continue;
            }
            visited[cell_index] = true;
            queue[count++] = {cell, hops + 1};
        }
    }
}

std::optional<Cell> retreat(const Observation& observation, const ThreatGrid& threat,
                            const GroundView& ground, const Entity& unit, int clearance) noexcept {
    std::optional<Cell> best;
    const int initial_risk = risk(threat, unit.position);
    auto best_rank = std::tuple{initial_risk, -clearance, distance(unit.position, observation.home),
                                0, unit.position.y, unit.position.x};
    visit_reachable(ground, unit, [&](Cell position, int hops) {
        const int candidate_risk = risk(threat, position);
        const int safe_distance = nearest_weapon(observation, position);
        const auto rank = std::tuple{candidate_risk, -safe_distance, distance(position, observation.home),
                                     hops, position.y, position.x};
        if ((candidate_risk < initial_risk || safe_distance > clearance) && rank < best_rank) {
            best = position;
            best_rank = rank;
        }
    });
    return best;
}

std::optional<Cell> firing_cell(const Observation& observation, const ThreatGrid& threat,
                                const GroundView& ground, const Entity& unit,
                                const EnemyContact& focus) noexcept {
    const auto& spec = observation.rules.spec(unit.kind);
    if (distance(unit.position, focus.position) > int(spec.range) + 5) return std::nullopt;

    std::optional<Cell> best;
    const bool artillery = unit.kind == UnitKind::Artillery;
    std::tuple<bool, int, int, int, int, int> best_rank;
    visit_reachable(ground, unit, [&](Cell position, int hops) {
        const int range = distance(position, focus.position);
        if (range > spec.range) return;
        const auto rank = std::tuple{artillery && nearest_weapon(observation, position) <= 0,
                                     hops, risk(threat, position), -range, position.y, position.x};
        if (!best || rank < best_rank) {
            best = position;
            best_rank = rank;
        }
    });
    return best;
}

const EnemyContact* focus_contact(const Observation& observation, Cell center,
                                  int max_range = Map::max_dimension) noexcept {
    const EnemyContact* chosen = nullptr;
    int best = std::numeric_limits<int>::min();
    for (const auto& enemy : observation.enemies) {
        if (!enemy.hp || !observation.contains(enemy.position)) continue;
        const auto range = distance(center, enemy.position);
        if (range > max_range) continue;
        const auto& spec = observation.rules.spec(enemy.kind);
        const bool armed = spec.damage != 0;
        const int loss = spec.hp ? (100 * (int(spec.hp) - int(enemy.hp))) / spec.hp : 0;
        const int score = (armed ? 280 + 2 * spec.damage + 6 * spec.range : 0) +
                          spec.cost / 3 + loss * 3 - 10 * range;
        if (!chosen || score > best || (score == best && enemy.id < chosen->id)) {
            chosen = &enemy;
            best = score;
        }
    }
    return chosen;
}

std::optional<Cell> remembered_position(const Observation& observation, const AiMemory& memory,
                                        Cell center) noexcept {
    const EnemyContact* chosen = nullptr;
    for (const auto& enemy : memory.enemies) {
        if (enemy.seen_at > observation.step ||
            observation.step - enemy.seen_at > AiMemory::contact_lifetime) continue;
        if (!chosen ||
            std::tuple{distance(center, enemy.position), enemy.id} <
            std::tuple{distance(center, chosen->position), chosen->id}) chosen = &enemy;
    }
    if (chosen) return chosen->position;
    if (observation.contains(memory.scout_target)) return memory.scout_target;
    return std::nullopt;
}

Cell front_center(const Observation& observation, const std::vector<const Entity*>& squad) noexcept {
    int x = 0, y = 0, count = 0;
    for (const auto* unit : squad) {
        if (unit->kind == UnitKind::Artillery) continue;
        x += unit->position.x;
        y += unit->position.y;
        ++count;
    }
    if (count) return {x / count, y / count};
    if (!squad.empty()) return squad.front()->position;
    return observation.home;
}

std::optional<Cell> flank(const Observation& observation, const GroundView& ground,
                         const Entity& unit, const EnemyContact& target) noexcept {
    const auto& here = unit.position;
    const int separation = distance(here, target.position);
    const auto& spec = observation.rules.spec(unit.kind);
    const auto& enemy_spec = observation.rules.spec(target.kind);
    // A fast unit can make a lateral approach when a longer-ranged weapon
    // covers the direct route. Stay engaged after reaching either open side.
    if (enemy_spec.range <= spec.range || separation <= spec.range || separation > 22) return std::nullopt;
    const int fx = sign(target.position.x - observation.home.x);
    const int fy = sign(target.position.y - observation.home.y);
    if (!fx && !fy) return std::nullopt;
    const int lateral = -fy * (here.x - target.position.x) + fx * (here.y - target.position.y);
    const int wing = lateral == 0 ? (unit.id % 2 ? 1 : -1) : sign(lateral);
    const int offset = std::max(3, int(spec.range));
    if (std::abs(lateral) >= offset - 1) return std::nullopt;
    const Cell staging{target.position.x - fx * (offset + 1) - wing * fy * offset,
                       target.position.y - fy * (offset + 1) + wing * fx * offset};
    return ground.near(staging, true, unit.id);
}

void issue(std::vector<Command>& orders, Side side, const Entity& unit, CommandKind kind,
           Cell goal, EntityId focus = no_entity) {
    const bool move = kind == CommandKind::Move;
    if (unit.order == (move ? OrderKind::Move : OrderKind::AttackMove) &&
        unit.destination == goal && unit.focus == focus) return;
    Command order{};
    order.kind = kind;
    order.side = side;
    order.target = goal;
    order.units = {unit.id};
    order.focus = focus;
    orders.push_back(std::move(order));
}

}  // namespace

std::vector<Command> plan_tactics(const Observation& observation, const AiMemory& memory,
                                  const ThreatGrid& threat, EntityId scout_id) {
    if (observation.width <= 0 || observation.height <= 0 ||
        observation.width > Map::max_dimension || observation.height > Map::max_dimension) return {};
    std::vector<const Entity*> squad;
    for (const auto& unit : observation.allies) {
        if (unit.id != scout_id && combatant(observation, unit)) squad.push_back(&unit);
    }
    if (squad.empty()) return {};
    std::sort(squad.begin(), squad.end(), [](const Entity* lhs, const Entity* rhs) {
        return lhs->id < rhs->id;
    });

    GroundView ground(observation, memory);
    const auto center = front_center(observation, squad);
    const auto* focus = focus_contact(observation, center);
    auto destination = focus ? std::optional{focus->position} : remembered_position(observation, memory, center);
    if (!destination) return {};
    if (ground.at(*destination) == Ground::Blocked) destination = ground.near(*destination, false);
    if (!destination) return {};

    std::vector<Command> orders;
    for (const auto* unit : squad) {
        const auto& spec = observation.rules.spec(unit->kind);
        const bool artillery = unit->kind == UnitKind::Artillery;
        const int clearance = nearest_weapon(observation, unit->position);
        const bool low_health = int(unit->hp) * 4 <= spec.hp && clearance <= 2;
        const bool gun_cooling = artillery && unit->shot_wait && clearance <= 1;
        if (low_health || gun_cooling) {
            if (const auto safe = retreat(observation, threat, ground, *unit, clearance)) {
                issue(orders, observation.side, *unit, CommandKind::Move, *safe);
                ground.reserve(*safe, unit->id);
                continue;
            }
        }
        const auto* target = focus;
        if (focus && distance(unit->position, focus->position) > spec.range) {
            if (const auto* local = focus_contact(observation, unit->position, spec.range)) target = local;
        }
        const bool in_range = target && distance(unit->position, target->position) <= spec.range;
        if (unit->kind == UnitKind::Skirmisher && focus && !in_range && squad.size() >= 2) {
            if (const auto side = flank(observation, ground, *unit, *focus)) {
                issue(orders, observation.side, *unit, CommandKind::Move, *side);
                ground.reserve(*side, unit->id);
                continue;
            }
        }
        if (target && !in_range) {
            // The enemy can be inside a blocked pocket. Approach a reachable
            // firing position, then aim on the next observed turn.
            if (const auto cell = firing_cell(observation, threat, ground, *unit, *target)) {
                issue(orders, observation.side, *unit, CommandKind::Move, *cell);
                ground.reserve(*cell, unit->id);
                continue;
            }
        }

        Cell goal = *destination;
        if (target && ground.at(target->position) != Ground::Blocked) goal = target->position;
        if (artillery && in_range) goal = unit->position;
        issue(orders, observation.side, *unit, CommandKind::AttackMove, goal,
              target ? target->id : no_entity);
    }
    return orders;
}

}  // namespace coastal::rts::ai
