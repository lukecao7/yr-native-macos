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

struct FlowGrid {
    const Map& map;
    const std::vector<unsigned char>& blocked;
    int width;
    int height;

    int index(Cell cell) const noexcept { return cell.y * width + cell.x; }
    Cell cell(int index) const noexcept { return {index % width, index / width}; }
};

struct RouteTargets {
    std::array<int, 8> handles{};
    std::size_t count = 0;
};

struct MoveChoices {
    std::array<Cell, 8> cells{};
    std::size_t count = 0;
    bool incomplete = false;
};

struct Intent {
    RouteTargets goals;
    MoveChoices moves;
    EntityId target = no_entity;
    Cell target_position;
    bool engaged = false;
    bool finish_move = false;
};

template<class Occupied>
RouteTargets route_targets(const FlowGrid& grid, Cell from, Cell goal, const Occupied& occupied) {
    RouteTargets targets;
    if (!grid.map.contains(from) || !grid.map.walkable(goal) || from == goal ||
        grid.blocked[grid.index(from)]) return targets;

    if (!grid.blocked[grid.index(goal)]) {
        targets.handles[targets.count++] = grid.index(goal);
        return targets;
    }
    // Structures are reached through the surrounding walkable cells.
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const Cell candidate{goal.x + dx, goal.y + dy};
            if (grid.map.contains(candidate) && !grid.blocked[grid.index(candidate)] &&
                (candidate == from || !occupied(candidate))) {
                targets.handles[targets.count++] = grid.index(candidate);
            }
        }
    }
    return targets;
}

template<class Occupied, class Sample>
MoveChoices rank_moves(const FlowGrid& grid, Cell from, const RouteTargets& targets,
                       const Occupied& occupied, const Sample& sample) {
    MoveChoices choices;
    if (!targets.count) return choices;

    std::array<int, 9> cells{};
    std::array<std::int32_t, 9> edge_costs{};
    std::array<std::int32_t, 9> costs;
    constexpr auto unreachable = std::numeric_limits<std::int32_t>::max();
    costs.fill(unreachable);
    cells[0] = grid.index(from);
    std::size_t count = 1;
    navigation::detail::for_flow_neighbour(grid.width, grid.height, grid.blocked, cells[0],
        [&](int neighbour, std::int32_t edge) {
            if (occupied(grid.cell(neighbour))) return;
            cells[count] = neighbour;
            edge_costs[count] = edge;
            ++count;
        });
    if (count == 1) return choices;

    const std::span<const int> indexes(cells.data(), count);
    std::array<std::int32_t, 9> values{};
    for (std::size_t goal = 0; goal < targets.count; ++goal) {
        if (!sample(targets.handles[goal], indexes, values)) {
            choices.incomplete = true;
            continue;
        }
        if (values[0] == 0) return {};
        if (values[0] < 0) continue;
        for (std::size_t i = 1; i < count; ++i) {
            if (values[i] >= 0 && values[i] < values[0]) {
                costs[i] = std::min(costs[i], edge_costs[i] + values[i]);
            }
        }
    }
    // Stable search order breaks ties between equivalent neighbors.
    for (;;) {
        const auto best = std::min_element(costs.begin() + 1, costs.begin() + count);
        if (*best == unreachable) break;
        choices.cells[choices.count++] = grid.cell(cells[std::distance(costs.begin(), best)]);
        *best = unreachable;
    }
    return choices;
}

bool sample_field(const navigation::FlowCache& cache, int handle, std::span<const int> cells,
                  std::array<std::int32_t, 9>& values) {
    const auto* field = cache.find(handle);
    if (!field) return false;
    for (std::size_t i = 0; i < cells.size(); ++i) values[i] = field->distance[cells[i]];
    return true;
}

bool sample_route(navigation::FlowCache& cache, int handle, std::span<const int> cells,
                  std::array<std::int32_t, 9>& values) {
    if (sample_field(cache, handle, cells, values)) return true;
    const std::vector<int> query(cells.begin(), cells.end());
    std::vector<std::int32_t> distances;
    std::string error;
    if (!cache.sample(handle, query, distances, error)) throw std::runtime_error(error);
    std::copy(distances.begin(), distances.end(), values.begin());
    return true;
}

template<class Occupied>
Intent plan_unit(const World& world, const FlowGrid& grid, const SpatialIndex& spatial,
                 const Entity& unit, const Occupied& occupied) {
    Intent intent;
    const auto& spec = world.rules.spec(unit.kind);
    Cell goal = unit.destination;
    if (spec.damage && unit.order != OrderKind::Move && unit.order != OrderKind::Gather) {
        const bool pursues = unit.order == OrderKind::AttackMove && spec.move_period;
        const int radius = pursues ? spec.vision : std::min(spec.vision, spec.range);
        const auto* focused = unit.focus ? world.find(unit.focus) : nullptr;
        if (focused && (focused->side == unit.side || grid_distance(unit.position, focused->position) > radius)) {
            focused = nullptr;
        }
        const auto* target = focused ? focused : spatial.nearest_enemy(world, unit, radius);
        if (target) {
            if (grid_distance(unit.position, target->position) <= spec.range) {
                intent.engaged = true;
                if (!unit.shot_wait) {
                    intent.target = target->id;
                    intent.target_position = target->position;
                }
                return intent;
            }
            goal = target->position;
        }
    }
    if (!spec.move_period || unit.move_wait || unit.order == OrderKind::Hold ||
        unit.order == OrderKind::Idle) return intent;

    if (unit.order != OrderKind::Gather && goal == unit.destination &&
        (unit.position == goal ||
         (unit.order == OrderKind::Move && grid_distance(unit.position, goal) == 1 && occupied(goal)))) {
        intent.finish_move = true;
        return intent;
    }
    intent.goals = route_targets(grid, unit.position, goal, occupied);
    return intent;
}

bool launch(World& world, Entity& unit, const Intent& intent, const UnitSpec& spec) {
    if (world.projectiles.size() >= World::projectile_limit) return false;
    const int distance = grid_distance(unit.position, intent.target_position);
    const auto travel = static_cast<std::uint8_t>(distance / 2 + 1);
    const auto damage = static_cast<std::uint16_t>(spec.damage + (world.random() & 1U));
    world.projectiles.push_back({unit.id, intent.target, unit.position, intent.target_position,
                                 travel, travel, damage});
    unit.facing = direction(unit.position, intent.target_position);
    unit.shot_wait = static_cast<std::uint8_t>(std::max(0, int(spec.shot_period) - 1));
    return true;
}

void refresh_visible_rows(const World& world, std::array<std::vector<std::uint8_t>, 2>& vision,
                          int first_row, int end_row) {
    const int width = world.map.width;
    for (auto& mask : vision) {
        std::fill(mask.begin() + first_row * width, mask.begin() + end_row * width, 0);
    }
    for (const auto& unit : world.entities) {
        const int radius = world.rules.spec(unit.kind).vision;
        const int top = std::max(first_row, unit.position.y - radius);
        const int bottom = std::min(end_row, unit.position.y + radius + 1);
        if (top >= bottom) continue;
        const int left = std::max(0, unit.position.x - radius);
        const int right = std::min(width, unit.position.x + radius + 1);
        auto& mask = vision[side_index(unit.side)];
        for (int y = top; y < bottom; ++y) {
            std::fill(mask.begin() + y * width + left, mask.begin() + y * width + right, 1);
        }
    }
}

std::array<int, 2> vision_extent(const World& world) {
    int first = world.map.height;
    int end = 0;
    for (const auto& unit : world.entities) {
        const int radius = world.rules.spec(unit.kind).vision;
        first = std::min(first, unit.position.y - radius);
        end = std::max(end, unit.position.y + radius + 1);
    }
    return {std::max(0, first), std::min(world.map.height, end)};
}

void prepare_routes(navigation::FlowCache& cache, const std::vector<Intent>& intents,
                    unsigned threads) {
    std::vector<int> handles;
    for (const auto& intent : intents) {
        const auto& goals = intent.goals;
        handles.insert(handles.end(), goals.handles.begin(), goals.handles.begin() + goals.count);
    }
    std::sort(handles.begin(), handles.end());
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
    constexpr std::size_t reference_limit = 2200;
    if (handles.size() > reference_limit) handles.resize(reference_limit);
    std::string error;
    if (cache.export_active().active_goals != handles && !cache.set_active(handles, error)) {
        throw std::runtime_error(error);
    }
    const bool needs_build = std::any_of(handles.begin(), handles.end(),
                                        [&](int handle) { return !cache.find(handle); });
    if (needs_build && !cache.prepare_active(error, threads)) throw std::runtime_error(error);
}

void commit_intents(World& world, const std::vector<Intent>& intents,
                    SpatialIndex& spatial, StepWork& work) {
    for (std::size_t i = 0; i < intents.size(); ++i) {
        auto& unit = world.entities[i];
        const auto& intent = intents[i];
        const auto& spec = world.rules.spec(unit.kind);
        if (unit.move_wait) --unit.move_wait;
        if (unit.shot_wait) --unit.shot_wait;
        if (intent.engaged) {
            ++work.engaged;
            ++work.active;
            if (intent.target && launch(world, unit, intent, spec)) ++work.fired;
            continue;
        }
        if (intent.finish_move) {
            unit.order = OrderKind::Idle;
            unit.destination = {};
            unit.focus = no_entity;
            continue;
        }
        for (std::size_t candidate = 0; candidate < intent.moves.count; ++candidate) {
            const auto step = intent.moves.cells[candidate];
            if (!spatial.claim(step, static_cast<int>(i))) continue;
            unit.facing = direction(unit.position, step);
            unit.position = step;
            unit.move_wait = static_cast<std::uint8_t>(spec.move_period - 1);
            ++work.moved;
            ++work.active;
            break;
        }
    }
}

}  // namespace

Navigator::Navigator(unsigned threads, bool measure)
    : cache_(128U * 1024U * 1024U), workers_(threads, measure) {
    profile_.set_enabled(measure);
}

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
    const auto impassable = [](const Tile& tile) {
        return tile.terrain == Terrain::Water || tile.terrain == Terrain::Rock;
    };
    if (map.width == flow_width) {
        std::transform(map.tiles.begin(), map.tiles.end(), blocked.begin(), impassable);
    } else {
        for (int y = 0; y < map.height; ++y) {
            const auto row = map.tiles.begin() + y * map.width;
            std::transform(row, row + map.width, blocked.begin() + y * flow_width, impassable);
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
    if (blocked_.empty() || map.width != width_ || map.height != height_) return std::nullopt;

    const FlowGrid grid{map, blocked_, std::max(minimum_flow_extent, width_),
                        std::max(minimum_flow_extent, height_)};
    const auto occupied = [&](Cell cell) { return !world.free_at(cell); };
    const auto routes = route_targets(grid, from, goal, occupied);
    const auto choices = rank_moves(grid, from, routes, occupied,
        [&](int handle, std::span<const int> cells, auto& values) {
            return sample_route(cache_, handle, cells, values);
        });
    if (!choices.count) return std::nullopt;
    return choices.cells[0];
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
    if (command.focus != no_entity) {
        const auto* target = world.find(command.focus);
        if (order != OrderKind::AttackMove || !target || target->side == command.side) return false;
    }
    const auto ids = owned_ids(world, command.side, command.units.empty() ? side.selected : command.units);
    bool changed = false;
    for (const EntityId id : ids) {
        Entity& unit = *world.find(id);
        if (!world.rules.spec(unit.kind).move_period) continue;
        unit.order = order;
        unit.destination = hold ? Cell{} : command.target;
        unit.focus = world.rules.spec(unit.kind).damage ? command.focus : no_entity;
        changed = true;
    }
    return changed;
}

void tick_combat(World& world, Navigator& navigator) {
    using profiling::Phase;
    profiling::Scope step(navigator.profile_, Phase::Step);
    {
        profiling::Scope impacts(navigator.profile_, Phase::ProjectileAdvance);
        advance_projectiles(world);
    }
    {
        profiling::Scope topology(navigator.profile_, Phase::ObstacleUpdate);
        navigator.prepare(world);
        navigator.spatial_.rebuild(world);
    }
    navigator.work_ = {};
    const World& snapshot = world;
    const FlowGrid grid{snapshot.map, navigator.blocked_, std::max(minimum_flow_extent, navigator.width_),
                        std::max(minimum_flow_extent, navigator.height_)};
    const auto occupied = [&](Cell cell) { return navigator.spatial_.at(cell) != -1; };
    for (auto& mask : navigator.visible_) mask.resize(snapshot.map.tiles.size());
    std::vector<Intent> intents(snapshot.entities.size());
    if (intents.empty()) {
        refresh_visible_rows(snapshot, navigator.visible_, 0, snapshot.map.height);
        prepare_routes(navigator.cache_, intents, navigator.workers_.count());
        return;
    }

    const auto vision_rows = vision_extent(snapshot);
    const auto lanes = static_cast<unsigned>(std::min<std::size_t>(navigator.workers_.count(), intents.size()));
    const auto& prepared_cache = std::as_const(navigator.cache_);
    const auto evaluate_move = [&](Intent& intent, const Entity& unit) {
        intent.moves = rank_moves(grid, unit.position, intent.goals, occupied,
            [&](int handle, std::span<const int> cells, auto& values) {
                return sample_field(prepared_cache, handle, cells, values);
            });
    };
    {
        profiling::Scope workers(navigator.profile_, Phase::WorkersWall);
        navigator.workers_.run(intents.size(), [&](unsigned lane, std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const auto& unit = snapshot.entities[i];
                auto& intent = intents[i];
                intent = plan_unit(snapshot, grid, navigator.spatial_, unit, occupied);
                if (intent.goals.count) evaluate_move(intent, unit);
            }
            // Split the region that contains visibility, then clear and fill
            // disjoint row ranges. Outside rows belong to the end lanes.
            const auto split = [&](unsigned index) {
                return vision_rows[0] + (vision_rows[1] - vision_rows[0]) *
                           static_cast<int>(index) / static_cast<int>(lanes);
            };
            refresh_visible_rows(snapshot, navigator.visible_, lane ? split(lane) : 0,
                                 lane + 1 == lanes ? snapshot.map.height : split(lane + 1));
        });
    }
    {
        profiling::Scope fields(navigator.profile_, Phase::FlowPrepare);
        prepare_routes(navigator.cache_, intents, navigator.workers_.count());
    }
    // Cache hits are resolved in the same dispatch as targeting. Only cold
    // routes need a second dispatch after the owner has prepared the fields.
    if (std::any_of(intents.begin(), intents.end(), [](const Intent& intent) { return intent.moves.incomplete; })) {
        profiling::Scope workers(navigator.profile_, Phase::WorkersWall);
        navigator.workers_.run(intents.size(), [&](unsigned, std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                if (intents[i].moves.incomplete) evaluate_move(intents[i], snapshot.entities[i]);
            }
        });
    }

    // Capped fields can be sampled exactly by the owner once all shared
    // readers have joined, even if that cold query evicts another field.
    for (std::size_t i = 0; i < intents.size(); ++i) {
        auto& intent = intents[i];
        if (!intent.moves.incomplete) continue;
        intent.moves = rank_moves(grid, snapshot.entities[i].position, intent.goals, occupied,
            [&](int handle, std::span<const int> cells, auto& values) {
                return sample_route(navigator.cache_, handle, cells, values);
            });
    }
    profiling::Scope commit(navigator.profile_, Phase::Commit);
    commit_intents(world, intents, navigator.spatial_, navigator.work_);
    navigator.profile_.add(profiling::Counter::Ticks);
    navigator.profile_.add(profiling::Counter::LivingCalculated, intents.size());
    navigator.profile_.add(profiling::Counter::NewLaunches, navigator.work_.fired);
}

}  // namespace coastal::rts
