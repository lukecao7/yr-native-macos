// SPDX-License-Identifier: GPL-3.0-only
#include "combat.h"

#include "navigation/flow_repair.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace coastal::rts {
namespace {

constexpr int minimum_flow_extent = 8;

int grid_distance(Cell a, Cell b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

Cell direction(Cell from, Cell to) {
    return {(to.x > from.x) - (to.x < from.x), (to.y > from.y) - (to.y < from.y)};
}

std::vector<EntityId> owned_ids(const World& world, Side side, std::vector<EntityId> ids) {
    std::erase_if(ids, [&](EntityId id) {
        const auto* entity = world.find(id);
        return !entity || entity->side != side || entity->hp == 0;
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void select(SideState& state, std::vector<EntityId> ids, bool add) {
    if (!add) {
        state.selected = std::move(ids);
        return;
    }
    std::vector<EntityId> combined;
    combined.reserve(state.selected.size() + ids.size());
    std::set_union(state.selected.begin(), state.selected.end(), ids.begin(), ids.end(),
                   std::back_inserter(combined));
    state.selected = std::move(combined);
}

void advance_projectiles(World& world) {
    for (Projectile& projectile : world.projectiles) {
        auto* target = world.find(projectile.target);
        if (!target || target->hp == 0 || projectile.remaining == 0) continue;
        projectile.to = target->position;
        if (--projectile.remaining == 0) {
            const int damage = std::max(1, int(projectile.damage) - world.rules.spec(target->kind).armor);
            target->hp = static_cast<std::uint16_t>(std::max(0, int(target->hp) - damage));
        }
    }
    std::erase_if(world.projectiles, [&](const Projectile& shot) {
        const auto* target = world.find(shot.target);
        return shot.remaining == 0 || !target || target->hp == 0;
    });

    if (!std::erase_if(world.entities, [](const Entity& unit) { return unit.hp == 0; })) return;
    for (SideState& side : world.sides) {
        const auto retire = [&](std::vector<EntityId>& group) {
            std::erase_if(group, [&](EntityId id) { return !world.find(id); });
        };
        retire(side.selected);
        for (auto& group : side.groups) retire(group);
    }
}

const Entity* nearest_enemy(const World& world, const Entity& unit, int radius) {
    const Entity* found = nullptr;
    int best = radius + 1;
    for (const auto& candidate : world.entities) {
        if (candidate.side == unit.side || candidate.hp == 0) continue;
        const int distance = grid_distance(unit.position, candidate.position);
        if (distance >= best) continue;
        found = &candidate;
        best = distance;
    }
    // Stable entity iteration breaks distance ties by increasing entity ID.
    return found;
}

void launch(World& world, Entity& unit, const Entity& target, const UnitSpec& spec, int distance) {
    if (world.projectiles.size() >= World::projectile_limit) return;
    const auto travel = static_cast<std::uint8_t>(distance / 2 + 1);
    const auto damage = static_cast<std::uint16_t>(spec.damage + (world.random() & 1U));
    world.projectiles.push_back({unit.id, target.id, unit.position, target.position, travel, travel, damage});
    unit.facing = direction(unit.position, target.position);
    unit.shot_wait = static_cast<std::uint8_t>(std::max(0, int(spec.shot_period) - 1));
}

void tick_unit(World& world, Navigator& navigator, Entity& unit) {
    const UnitSpec& spec = world.rules.spec(unit.kind);
    const bool can_move = spec.move_period && unit.move_wait == 0;
    const bool can_shoot = unit.shot_wait == 0;
    if (unit.move_wait) --unit.move_wait;
    if (unit.shot_wait) --unit.shot_wait;

    Cell goal = unit.destination;
    if (spec.damage && unit.order != OrderKind::Move && unit.order != OrderKind::Gather) {
        const bool pursues = unit.order == OrderKind::AttackMove && spec.move_period;
        const int radius = pursues ? spec.vision : std::min(spec.vision, spec.range);
        if (const auto* enemy = nearest_enemy(world, unit, radius)) {
            const int distance = grid_distance(unit.position, enemy->position);
            if (distance <= spec.range) {
                if (can_shoot) launch(world, unit, *enemy, spec, distance);
                return;
            }
            goal = enemy->position;
        }
    }
    if (!can_move || unit.order == OrderKind::Hold || unit.order == OrderKind::Idle) return;

    if (unit.order != OrderKind::Gather && goal == unit.destination &&
        (unit.position == goal ||
         (unit.order == OrderKind::Move && grid_distance(unit.position, goal) == 1 && !world.free_at(goal)))) {
        unit.order = OrderKind::Idle;
        unit.destination = {};
        return;
    }

    if (const auto next = navigator.next(world, unit.position, goal)) {
        unit.facing = direction(unit.position, *next);
        unit.position = *next;
        unit.move_wait = static_cast<std::uint8_t>(spec.move_period - 1);
    }
}

}  // namespace

void Navigator::prepare(const World& world) {
    const Map& map = world.map;
    if (map.width <= 0 || map.height <= 0 ||
        map.width > Map::max_dimension || map.height > Map::max_dimension ||
        map.tiles.size() != std::size_t(map.width) * map.height) {
        throw std::invalid_argument("The navigation map does not match its dimensions.");
    }

    // Smaller game maps use a blocked rim so the shared FlowCache can keep its
    // existing supported dimensions. World coordinates still use the real map.
    const int flow_width = std::max(minimum_flow_extent, map.width);
    const int flow_height = std::max(minimum_flow_extent, map.height);
    std::vector<unsigned char> blocked(std::size_t(flow_width) * flow_height, 1);
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            blocked[y * flow_width + x] = !map.walkable({x, y});
        }
    }
    for (const Entity& unit : world.entities) {
        if (unit.hp && is_building(unit.kind)) {
            blocked[unit.position.y * flow_width + unit.position.x] = 1;
        }
    }

    const bool size_changed = width_ != map.width || height_ != map.height;
    if (!size_changed && blocked == blocked_) return;
    std::string error;
    const auto revision = revision_ + 1;
    const bool updated = size_changed ? cache_.configure(flow_width, flow_height, blocked, revision, error)
                                      : cache_.set_grid(blocked, revision, error);
    if (!updated) throw std::runtime_error(error);
    width_ = map.width;
    height_ = map.height;
    revision_ = revision;
    blocked_ = std::move(blocked);
}

std::optional<Cell> Navigator::next(const World& world, Cell from, Cell goal) {
    const Map& map = world.map;
    if (blocked_.empty() || map.width != width_ || map.height != height_ ||
        !map.contains(from) || !map.contains(goal) || from == goal) return std::nullopt;

    const int flow_width = std::max(minimum_flow_extent, width_);
    const int flow_height = std::max(minimum_flow_extent, height_);
    const auto flow_index = [=](Cell cell) { return cell.y * flow_width + cell.x; };
    const auto flow_cell = [=](int index) { return Cell{index % flow_width, index / flow_width}; };
    const int start = flow_index(from);
    if (blocked_[start]) return std::nullopt;

    // A blocked command target, such as a structure under attack, is approached
    // through its open adjacent cells. Ordinary move and gather share one goal.
    std::array<Cell, 8> destinations{};
    std::size_t count = 0;
    if (!blocked_[flow_index(goal)]) {
        destinations[count++] = goal;
    } else if (map.walkable(goal)) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const Cell candidate{goal.x + dx, goal.y + dy};
                if ((dx || dy) && map.contains(candidate) && !blocked_[flow_index(candidate)] &&
                    (candidate == from || world.free_at(candidate))) {
                    destinations[count++] = candidate;
                }
            }
        }
    }

    std::vector<int> cells{start};
    std::array<std::int32_t, 8> edge_costs{};
    navigation::detail::for_flow_neighbour(flow_width, flow_height, blocked_, start,
        [&](int neighbour, std::int32_t edge) {
            if (world.free_at(flow_cell(neighbour))) {
                edge_costs[cells.size() - 1] = edge;
                cells.push_back(neighbour);
            }
        });
    if (cells.size() == 1) return std::nullopt;

    std::optional<Cell> next;
    std::int32_t best = std::numeric_limits<std::int32_t>::max();
    std::vector<std::int32_t> distances;
    std::string error;
    for (std::size_t goal_index = 0; goal_index < count; ++goal_index) {
        const auto handle = cache_.handle_for_goal(flow_index(destinations[goal_index]));
        if (!cache_.sample(handle, cells, distances, error)) throw std::runtime_error(error);
        if (distances[0] == 0) return std::nullopt;
        if (distances[0] < 0) continue;
        for (std::size_t i = 1; i < cells.size(); ++i) {
            if (distances[i] < 0 || distances[i] >= distances[0]) continue;
            const std::int32_t cost = edge_costs[i - 1] + distances[i];
            if (cost < best) {
                best = cost;
                next = flow_cell(cells[i]);
            }
        }
    }
    return next;
}

bool apply_combat(World& world, const Command& command) {
    if (side_index(command.side) >= world.sides.size()) return false;
    SideState& side = world.side(command.side);
    switch (command.kind) {
    case CommandKind::Select:
        select(side, owned_ids(world, command.side, command.units), command.add);
        return true;
    case CommandKind::AssignGroup:
        if (command.index >= side.groups.size()) return false;
        side.groups[command.index] = side.selected;
        return true;
    case CommandKind::RecallGroup:
        if (command.index >= side.groups.size()) return false;
        select(side, side.groups[command.index], command.add);
        return true;
    case CommandKind::Move:
    case CommandKind::AttackMove:
    case CommandKind::Hold:
        break;
    default:
        return false;
    }

    const bool hold = command.kind == CommandKind::Hold;
    if (!hold && !world.map.walkable(command.target)) return false;
    const OrderKind order = hold ? OrderKind::Hold :
        (command.kind == CommandKind::Move ? OrderKind::Move : OrderKind::AttackMove);
    const auto ids = owned_ids(world, command.side, command.units.empty() ? side.selected : command.units);
    bool changed = false;
    for (const EntityId id : ids) {
        Entity& unit = *world.find(id);
        if (!world.rules.spec(unit.kind).move_period) continue;
        unit.order = order;
        unit.destination = hold ? Cell{} : command.target;
        changed = true;
    }
    return changed;
}

void tick_combat(World& world, Navigator& navigator) {
    advance_projectiles(world);
    navigator.prepare(world);
    for (Entity& unit : world.entities) tick_unit(world, navigator, unit);
}

}  // namespace coastal::rts
