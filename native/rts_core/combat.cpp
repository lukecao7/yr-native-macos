// SPDX-License-Identifier: GPL-3.0-only
#include "combat.h"

#include "navigation/flow_repair.h"

#include <algorithm>
#include <array>
#include <atomic>
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
    world.refresh_entity_index();
    for (SideState& side : world.active_sides()) {
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

// An actual closed dependency cycle is distinct from congestion at a shared
// future goal. Every node must have a live ordinary Move to the adjacent real
// body of the next node; a settled/terminal actor, remote goal, source-only
// tail, static blocker, or path entering another cycle terminates this query.
// Only the lowest stable ID in that same cycle may take an equal-field step.
// The canonical destination remains unchanged, so once its old source clears
// the other nodes can follow their original commands. No retry/history state
// or generic off-gradient wandering is introduced.
bool leads_closed_goal_cycle(const World& world, const FlowGrid& grid, const Entity& requester,
                             const Navigator& navigator) {
    const auto advance = [&](const Entity* node) -> const Entity* {
        if (!node || !node->hp || node->side != requester.side || node->order != OrderKind::Move ||
            !world.rules.spec(node->kind).move_period || node->move_wait ||
            navigator.transient_owner_moving(node->id) ||
            node->position == node->destination || grid_distance(node->position, node->destination) != 1 ||
            !grid.map.contains(node->position) || !grid.map.contains(node->destination) ||
            grid.blocked[grid.index(node->position)] || grid.blocked[grid.index(node->destination)] ||
            !grid.map.can_travel(node->position, node->destination)) return nullptr;
        const auto* next = world.find(navigator.prepared_body_owner(node->destination));
        return next && next->hp && next->position == node->destination ? next : nullptr;
    };
    const Entity* node = &requester;
    const Entity* slow = &requester;
    const Entity* fast = &requester;
    // Floyd detects entry into someone else's cycle without an allocated or
    // quadratically searched visited list. Prepared body lookup is O(1), and
    // the existing stable-ID World::find is used to validate each live owner.
    for (std::size_t count = 0; count < world.entities.size(); ++count) {
        node = advance(node);
        if (!node || node->id < requester.id) return false;
        if (node->id == requester.id) return count > 0;
        slow = advance(slow);
        fast = advance(advance(fast));
        if (!slow || !fast || slow->id == fast->id) return false;
    }
    return false;
}

struct RouteTargets {
    std::array<int, 8> handles{};
    std::size_t count = 0;
};

// On a one-cell square grid only opposite diagonals have a strict interior
// intersection. Cardinal crossings or shared endpoints are already excluded
// by complete body/source/next cell admission. This keeps mobile side cells
// traversable and requires neither a guessed vehicle radius nor time history.
struct DiagonalKey {
    int cell = -1;
    unsigned direction = 0;
};

DiagonalKey diagonal_key(int width, Cell from, Cell to) noexcept {
    const int dx = to.x - from.x, dy = to.y - from.y;
    if (std::abs(dx) != 1 || std::abs(dy) != 1) return {};
    return {std::min(from.y, to.y) * width + std::min(from.x, to.x),
            dx == dy ? 0U : 1U};
}

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

template<class Occupied, class Sample, class Crossed>
MoveChoices rank_moves(const FlowGrid& grid, Cell from, const RouteTargets& targets,
                       const Occupied& occupied, const Sample& sample, const Crossed& crossed,
                       bool allow_equal = false) {
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
            if (!grid.map.travel_edges.empty() && !grid.map.can_travel(from,grid.cell(neighbour))) return;
            if (occupied(grid.cell(neighbour)) || crossed(from, grid.cell(neighbour))) return;
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
            if (values[i] >= 0 && (values[i] < values[0] || (allow_equal && values[i] == values[0]))) {
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
                 const Entity& unit, const Occupied& occupied, const Navigator& navigator) {
    Intent intent;
    if (world.defeated_houses && world.house_defeated(unit.side)) return intent;
    const auto& spec = world.rules.spec(unit.kind);
    Cell goal = unit.destination;
    if (spec.damage && unit.order != OrderKind::Move && unit.order != OrderKind::Gather) {
        const bool pursues = unit.order == OrderKind::AttackMove && spec.move_period;
        const int radius = pursues ? spec.vision : std::min(spec.vision, spec.range);
        const auto* focused = unit.focus ? world.find(unit.focus) : nullptr;
        if (focused && (focused->side == unit.side || grid_distance(unit.position, focused->position) > radius ||
                        (world.defeated_houses && world.house_defeated(focused->side)))) {
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
        (unit.position == goal || unit.order == OrderKind::Move) &&
        navigator.can_finish_move(unit.position, goal, occupied(goal), &unit)) {
        intent.finish_move = true;
        return intent;
    }
    const auto destination_occupied = [&](Cell cell) {
        if (unit.subcell == whole_cell) return occupied(cell) || navigator.transient_occupied(cell, unit.id);
        return !navigator.prepared_slots(cell).reserve(unit.subcell, unit.side);
    };
    intent.goals = route_targets(grid, unit.position, goal, destination_occupied);
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

void refresh_visible_rows(const World& world, std::span<std::vector<std::uint8_t>> vision,
                          int first_row, int end_row) {
    const int width = world.map.width;
    for (auto& mask : vision) {
        std::fill(mask.begin() + first_row * width, mask.begin() + end_row * width, 0);
    }
    for (const auto& unit : world.entities) {
        if (world.defeated_houses && world.house_defeated(unit.side)) continue;
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
                    SpatialIndex& spatial, StepWork& work, const Navigator& navigator) {
    // Owner-only scratch for this stable-ID commit. Earlier accepted moves
    // claim their diagonal, so later intents choose the next noncrossing
    // ranked alternative or wait. No persistent arbitration state is added.
    std::vector<std::uint8_t> committed_diagonals(world.map.tiles.size(), 0);
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
            if ((unit.subcell == whole_cell && navigator.transient_occupied(step, unit.id)) ||
                navigator.transient_crossed(unit.position, step, unit.id) ||
                !navigator.prepared_step_allowed(unit.id, step)) continue;
            const auto diagonal = diagonal_key(world.map.width, unit.position, step);
            if (diagonal.cell >= 0 &&
                (committed_diagonals[diagonal.cell] & (1U << (diagonal.direction ^ 1U)))) continue;
            if (unit.subcell == whole_cell) {
                if (!spatial.claim(step, static_cast<int>(i))) continue;
            } else {
                auto slot = spatial.claim_subcell(step, static_cast<int>(i), unit, navigator.prepared_slots(step));
                if (!slot) continue;
                unit.previous_subcell = unit.subcell;
                unit.subcell = *slot;
            }
            if (diagonal.cell >= 0) committed_diagonals[diagonal.cell] |= 1U << diagonal.direction;
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

void Navigator::set_obstacle_mask(std::span<const std::uint8_t> mask) {
    if(mask.size()==obstacle_mask_.size()&&std::equal(mask.begin(),mask.end(),obstacle_mask_.begin()))return;
    obstacle_mask_.assign(mask.begin(),mask.end());
}

void Navigator::set_transient_occupancy(std::span<const MobileOccupant> occupants) {
    transient_occupants_.assign(occupants.begin(), occupants.end());
}

void Navigator::set_transient_segments(std::span<const MobileSegment> segments) {
    transient_segments_.assign(segments.begin(), segments.end());
}

bool Navigator::transient_owner_moving(EntityId owner) const noexcept {
    return owner != no_entity && std::binary_search(transient_active_owners_.begin(),
                                                    transient_active_owners_.end(), owner);
}

void Navigator::prepare_bodies(const World& world) {
    const auto& map = world.map;
    if (body_cells_.size() != map.tiles.size()) body_cells_.assign(map.tiles.size(), {});
    else for (const int index : body_touched_) body_cells_[index] = {};
    body_touched_.clear();
    for (const auto& body : world.entities) if (body.hp && map.contains(body.position)) {
        const int index = map.index(body.position);
        auto& cell = body_cells_[index];
        cell.slots.add(body.subcell, body.side);
        if (cell.owner == no_entity && !cell.ambiguous) body_touched_.push_back(index);
        if (body.id == no_entity || cell.owner != no_entity || cell.ambiguous) cell.ambiguous = true;
        else cell.owner = body.id;
    }
}

EntityId Navigator::prepared_body_owner(Cell cell) const noexcept {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_ || body_cells_.empty())
        return no_entity;
    const auto& body = body_cells_[cell.y * width_ + cell.x];
    return body.ambiguous ? no_entity : body.owner;
}

void Navigator::set_prepared_move_steps(std::span<const PreparedMoveStep> rows) {
    prepared_move_steps_.assign(rows.begin(), rows.end());
    std::stable_sort(prepared_move_steps_.begin(), prepared_move_steps_.end(),
                     [](const auto& a, const auto& b) { return a.owner < b.owner; });
}

bool Navigator::prepared_step_allowed(EntityId owner, Cell next) const noexcept {
    auto row = std::lower_bound(prepared_move_steps_.begin(), prepared_move_steps_.end(), owner,
                               [](const auto& entry, EntityId value) { return entry.owner < value; });
    // Duplicate constraints remain conservative even if an invalid producer
    // supplies different steps for one owner. No hidden winner is selected.
    for (; row != prepared_move_steps_.end() && row->owner == owner; ++row)
        if (!row->next || *row->next != next) return false;
    return true;
}

void Navigator::prepare_transient(const Map& map) {
    if (transient_cells_.size() != map.tiles.size()) {
        transient_cells_.assign(map.tiles.size(), {});
    } else {
        for (const int index : transient_touched_) transient_cells_[index] = {};
    }
    transient_touched_.clear();
    for (const auto& occupant : transient_occupants_) {
        const int index = map.index(occupant.cell);
        auto& cell = transient_cells_[index];
        cell.slots.add(occupant.owner ? occupant.subcell : whole_cell, occupant.side);
        if (cell.owner == no_entity && !cell.blocked_for_all) transient_touched_.push_back(index);
        cell.blocks_completion = cell.blocks_completion || occupant.blocks_completion;
        if (occupant.owner == no_entity) cell.blocked_for_all = true;
        else if (cell.owner == no_entity && !cell.blocked_for_all) cell.owner = occupant.owner;
        else if (cell.owner != occupant.owner) cell.blocked_for_all = true;
    }
    if (transient_segment_cells_.size() != map.tiles.size()) {
        transient_segment_cells_.assign(map.tiles.size(), {});
    } else {
        for (const int index : transient_segment_touched_) transient_segment_cells_[index] = {};
    }
    transient_segment_touched_.clear();
    for (const auto& segment : transient_segments_) {
        const auto key = diagonal_key(map.width, segment.from, segment.to);
        if (key.cell < 0) continue;
        auto& cell = transient_segment_cells_[key.cell];
        if (cell.owner[0] == no_entity && cell.owner[1] == no_entity &&
            !cell.blocked_for_all[0] && !cell.blocked_for_all[1])
            transient_segment_touched_.push_back(key.cell);
        auto& owner = cell.owner[key.direction];
        auto& blocked = cell.blocked_for_all[key.direction];
        if (segment.owner == no_entity) blocked = true;
        else if (owner == no_entity && !blocked) owner = segment.owner;
        else if (owner != segment.owner) blocked = true;
    }
    // Publish ownership at the same successful prepare boundary as geometric
    // segment crossings. Staging or rejecting a new segment list leaves the
    // last prepared owner snapshot intact for every owner/worker query.
    transient_active_owners_.clear();
    transient_active_owners_.reserve(transient_segments_.size());
    for (const auto& segment : transient_segments_)
        if (segment.owner != no_entity) transient_active_owners_.push_back(segment.owner);
    if (!std::is_sorted(transient_active_owners_.begin(), transient_active_owners_.end()))
        std::sort(transient_active_owners_.begin(), transient_active_owners_.end());
    transient_active_owners_.erase(std::unique(transient_active_owners_.begin(), transient_active_owners_.end()),
                                   transient_active_owners_.end());
}

bool Navigator::transient_crossed(Cell from, Cell to, EntityId ignore) const noexcept {
    const auto contains = [&](Cell cell) {
        return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_;
    };
    if (!contains(from) || !contains(to) || transient_segment_cells_.empty()) return false;
    const auto key = diagonal_key(width_, from, to);
    if (key.cell < 0) return false;
    const auto& cell = transient_segment_cells_[key.cell];
    const auto direction = key.direction ^ 1U;
    return cell.blocked_for_all[direction] ||
           (cell.owner[direction] != no_entity && cell.owner[direction] != ignore);
}

bool Navigator::transient_occupied(Cell cell, EntityId ignore) const noexcept {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_ ||
        transient_cells_.empty()) return false;
    const auto& occupancy = transient_cells_[cell.y * width_ + cell.x];
    return occupancy.blocked_for_all || (occupancy.owner != no_entity && occupancy.owner != ignore);
}

SubcellOccupancy Navigator::prepared_slots(Cell cell) const noexcept {
    if (cell.x < 0 || cell.y < 0 || cell.x >= width_ || cell.y >= height_ || body_cells_.empty())
        return {};
    const auto index = cell.y * width_ + cell.x;
    auto slots = body_cells_[index].slots;
    slots.merge(transient_cells_[index].slots);
    return slots;
}

bool Navigator::can_finish_move(Cell from, Cell goal, bool goal_has_body, const Entity* mover) const noexcept {
    const auto contains = [&](Cell cell) {
        return cell.x >= 0 && cell.y >= 0 && cell.x < width_ && cell.y < height_;
    };
    if (!contains(from) || !contains(goal) || transient_cells_.empty()) return false;
    if (from != goal && (!goal_has_body || grid_distance(from, goal) != 1)) return false;
    const auto& occupancy = transient_cells_[goal.y * width_ + goal.x];
    if (mover && mover->subcell != whole_cell) {
        if (from == goal) return !transient_owner_moving(mover->id);
        if (prepared_slots(goal).reserve(mover->subcell, mover->side)) return false;
        if (!occupancy.slots.reserve(mover->subcell, mover->side) && occupancy.slots.slots != 31)
            return !occupancy.blocks_completion;
    }
    return !occupancy.blocked_for_all && !occupancy.blocks_completion;
}

void Navigator::prepare(const World& world) {
    const Map& map = world.map;
    if (map.width <= 0 || map.height <= 0 ||
        map.width > Map::max_dimension || map.height > Map::max_dimension ||
        map.tiles.size() != std::size_t(map.width) * map.height ||
        (!map.travel_edges.empty() && map.travel_edges.size()!=map.tiles.size()) ||
        (!obstacle_mask_.empty() && obstacle_mask_.size()!=map.tiles.size())) {
        throw std::invalid_argument("The navigation map does not match its dimensions.");
    }
    const auto houses = map.house_count();
    if (std::any_of(transient_occupants_.begin(), transient_occupants_.end(),
                    [&](const MobileOccupant& occupant) {
                        return !map.contains(occupant.cell) || side_index(occupant.side) >= houses ||
                               (occupant.subcell != whole_cell && occupant.subcell >= subcell_offsets.size());
                    })) {
        throw std::invalid_argument("A transient navigation occupant has an invalid cell, house or foot position.");
    }
    if (std::any_of(transient_segments_.begin(), transient_segments_.end(),
                    [&](const MobileSegment& segment) {
                        return !map.contains(segment.from) || !map.contains(segment.to) ||
                               grid_distance(segment.from, segment.to) != 1;
                    })) {
        throw std::invalid_argument("A transient navigation segment is not a map-contained one-cell move.");
    }
    if (std::any_of(prepared_move_steps_.begin(), prepared_move_steps_.end(),
                    [&](const PreparedMoveStep& row) {
                        return row.owner == no_entity || (row.next && !map.contains(*row.next));
                    })) {
        throw std::invalid_argument("A prepared navigation step has no owner or lies outside the map.");
    }

    prepare_bodies(world);
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
    if(!obstacle_mask_.empty())for(int y=0;y<map.height;++y)for(int x=0;x<map.width;++x)
        if(obstacle_mask_[y*map.width+x])blocked[y*flow_width+x]=1;
    for (const Entity& unit : world.entities) {
        if (unit.hp && is_building(unit.kind)) {
            blocked[unit.position.y * flow_width + unit.position.x] = 1;
        }
    }

    std::vector<unsigned char> edges;
    if (!map.travel_edges.empty()) {
        edges.assign(blocked.size(),0);
        for (int y=0;y<map.height;++y)
            std::copy_n(map.travel_edges.begin()+y*map.width,map.width,edges.begin()+y*flow_width);
    }
    const bool size_changed = width_ != map.width || height_ != map.height;
    if (!size_changed && blocked == blocked_ && edges==edges_) {
        prepare_transient(map);
        return;
    }
    std::string error;
    const auto revision = revision_ + 1;
    const bool updated = size_changed ? cache_.configure(flow_width, flow_height, blocked, revision, error,edges)
                                      : cache_.set_grid(blocked, revision, error,edges);
    if (!updated) throw std::runtime_error(error);
    width_ = map.width;
    height_ = map.height;
    revision_ = revision;
    blocked_ = std::move(blocked);
    edges_ = std::move(edges);
    prepare_transient(map);
}

std::optional<Cell> Navigator::next(const World& world, Cell from, Cell goal, EntityId owner) {
    const Map& map = world.map;
    if (blocked_.empty() || map.width != width_ || map.height != height_) return std::nullopt;

    const FlowGrid grid{map, blocked_, std::max(minimum_flow_extent, width_),
                        std::max(minimum_flow_extent, height_)};
    const auto* mover = owner ? world.find(owner) : nullptr;
    const auto occupied = [&](Cell cell) {
        if (mover && mover->subcell != whole_cell && map.walkable(cell)) {
            auto slots = world.cell_occupancy(cell, owner);
            slots.merge(transient_cells_[map.index(cell)].slots);
            return !slots.reserve(mover->subcell, mover->side);
        }
        return !world.free_at(cell) || transient_occupied(cell, owner);
    };
    const auto routes = route_targets(grid, from, goal, occupied);
    const auto sample = [&](int handle, std::span<const int> cells, auto& values) {
        return sample_route(cache_, handle, cells, values);
    };
    const auto crossed = [&](Cell a, Cell b) {
        return transient_crossed(a, b, owner) || !prepared_step_allowed(owner, b);
    };
    auto choices = rank_moves(grid, from, routes, occupied, sample, crossed);
    if (!choices.count && !choices.incomplete) {
        const auto* requester = world.find(owner);
        if (requester && requester->position == from && requester->destination == goal &&
            leads_closed_goal_cycle(world, grid, *requester, *this))
            choices = rank_moves(grid, from, routes, occupied, sample, crossed, true);
    }
    if (!choices.count) return std::nullopt;
    return choices.cells[0];
}

std::size_t Navigator::ranked_static_steps(const World& world, Cell from, Cell goal,
                                          std::array<Cell, 8>& out) {
    const Map& map = world.map;
    if (blocked_.empty() || map.width != width_ || map.height != height_) return 0;

    const FlowGrid grid{map, blocked_, std::max(minimum_flow_extent, width_),
                        std::max(minimum_flow_extent, height_)};
    const auto mobile_occupied = [](Cell) { return false; };
    const auto routes = route_targets(grid, from, goal, mobile_occupied);
    const auto choices = rank_moves(grid, from, routes, mobile_occupied,
        [&](int handle, std::span<const int> cells, auto& values) {
            return sample_route(cache_, handle, cells, values);
        }, [](Cell, Cell) { return false; });
    std::copy_n(choices.cells.begin(), choices.count, out.begin());
    return choices.count;
}

bool apply_combat(World& world, const Command& command) {
    if (world.house_defeated(command.side)) return false;
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
        if (order != OrderKind::AttackMove || !target || target->side == command.side || world.house_defeated(target->side)) return false;
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
    navigator.visible_houses_ = world.house_count();
    auto vision = std::span{navigator.visible_}.first(navigator.visible_houses_);
    for (auto& mask : vision) mask.resize(snapshot.map.tiles.size());
    std::vector<Intent> intents(snapshot.entities.size());
    if (intents.empty()) {
        refresh_visible_rows(snapshot, vision, 0, snapshot.map.height);
        prepare_routes(navigator.cache_, intents, navigator.workers_.count());
        return;
    }

    const auto vision_rows = vision_extent(snapshot);
    const auto lanes = static_cast<unsigned>(std::min<std::size_t>(navigator.workers_.count(), intents.size()));
    const auto& prepared_cache = std::as_const(navigator.cache_);
    const auto evaluate_move = [&](Intent& intent, const Entity& unit) {
        const auto destination_occupied = [&](Cell cell) {
            if (unit.subcell == whole_cell) return occupied(cell) || navigator.transient_occupied(cell, unit.id);
            return !navigator.prepared_slots(cell).reserve(unit.subcell, unit.side);
        };
        const auto sample = [&](int handle, std::span<const int> cells, auto& values) {
            return sample_field(prepared_cache, handle, cells, values);
        };
        const auto crossed = [&](Cell a, Cell b) {
            return navigator.transient_crossed(a, b, unit.id) || !navigator.prepared_step_allowed(unit.id, b);
        };
        intent.moves = rank_moves(grid, unit.position, intent.goals, destination_occupied, sample, crossed);
        if (!intent.moves.count && !intent.moves.incomplete && leads_closed_goal_cycle(snapshot, grid, unit, navigator))
            intent.moves = rank_moves(grid, unit.position, intent.goals, destination_occupied, sample, crossed, true);
    };
    {
        profiling::Scope workers(navigator.profile_, Phase::WorkersWall);
        navigator.workers_.run(intents.size(), [&](unsigned lane, std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const auto& unit = snapshot.entities[i];
                auto& intent = intents[i];
                intent = plan_unit(snapshot, grid, navigator.spatial_, unit, occupied, navigator);
                if (intent.goals.count) evaluate_move(intent, unit);
            }
            // Split the region that contains visibility, then clear and fill
            // disjoint row ranges. Outside rows belong to the end lanes.
            const auto split = [&](unsigned index) {
                return vision_rows[0] + (vision_rows[1] - vision_rows[0]) *
                           static_cast<int>(index) / static_cast<int>(lanes);
            };
            refresh_visible_rows(snapshot, vision, lane ? split(lane) : 0,
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
        constexpr std::size_t chunk_size = 32;
        std::atomic<std::size_t> next{0};
        navigator.workers_.run_shared_queue(lanes, [&](unsigned, std::size_t, std::size_t) {
            for (;;) {
                const auto begin = next.fetch_add(chunk_size, std::memory_order_relaxed);
                if (begin >= intents.size()) break;
                const auto end = std::min(intents.size(), begin + chunk_size);
                for (auto i = begin; i < end; ++i) {
                    if (intents[i].moves.incomplete) evaluate_move(intents[i], snapshot.entities[i]);
                }
            }
        });
    }

    // Capped fields can be sampled exactly by the owner once all shared
    // readers have joined, even if that cold query evicts another field.
    for (std::size_t i = 0; i < intents.size(); ++i) {
        auto& intent = intents[i];
        if (!intent.moves.incomplete) continue;
        const auto& unit = snapshot.entities[i];
        const auto destination_occupied = [&](Cell cell) {
            if (unit.subcell == whole_cell) return occupied(cell) || navigator.transient_occupied(cell, unit.id);
            return !navigator.prepared_slots(cell).reserve(unit.subcell, unit.side);
        };
        const auto sample = [&](int handle, std::span<const int> cells, auto& values) {
            return sample_route(navigator.cache_, handle, cells, values);
        };
        const auto crossed = [&](Cell a, Cell b) {
            return navigator.transient_crossed(a, b, unit.id) || !navigator.prepared_step_allowed(unit.id, b);
        };
        intent.moves = rank_moves(grid, unit.position, intent.goals, destination_occupied, sample, crossed);
        if (!intent.moves.count && !intent.moves.incomplete && leads_closed_goal_cycle(snapshot, grid, unit, navigator))
            intent.moves = rank_moves(grid, unit.position, intent.goals, destination_occupied, sample, crossed, true);
    }
    profiling::Scope commit(navigator.profile_, Phase::Commit);
    commit_intents(world, intents, navigator.spatial_, navigator.work_, navigator);
    navigator.profile_.add(profiling::Counter::Ticks);
    navigator.profile_.add(profiling::Counter::LivingCalculated, intents.size());
    navigator.profile_.add(profiling::Counter::NewLaunches, navigator.work_.fired);
}

}  // namespace coastal::rts
