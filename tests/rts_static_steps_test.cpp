// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace coastal::rts;

void require(bool okay, const std::string& message) {
    if (!okay) throw std::runtime_error(message);
}

// Self-authored rules and cells only. No original assets or private session
// entry points are needed to exercise the public navigation query.
World open_world(int width = 16, int height = 16) {
    World world;
    world.map.width = width;
    world.map.height = height;
    world.map.tiles.resize(std::size_t(width) * height);
    world.map.starts = {Cell{0, 0}, Cell{width - 1, height - 1}};
    world.rules.initial_funds = 1000;
    world.rules.cargo_capacity = 8;
    world.rules.harvest_ticks = 2;
    for (unsigned i = 0; i < static_cast<unsigned>(UnitKind::Count); ++i) {
        const auto kind = static_cast<UnitKind>(i);
        auto& spec = world.rules.spec(kind);
        spec.hp = 100;
        spec.cost = kind == UnitKind::Pioneer || kind == UnitKind::Hub ? 0 : 10;
        spec.train_ticks = is_producible(kind) ? 2 : 0;
        spec.move_period = is_building(kind) ? 0 : 1;
        spec.vision = 3;
    }
    world.rules.spec(UnitKind::Hub).power = 2;
    world.rules.spec(UnitKind::Generator).power = 10;
    world.rules.spec(UnitKind::Depot).power = -1;
    world.rules.spec(UnitKind::Workshop).power = -1;
    for (std::size_t i = 0; i < world.sides.size(); ++i) {
        world.sides[i].rally = world.map.starts[i];
        world.sides[i].funds = world.rules.initial_funds;
    }
    require(valid_rules(world.rules), "Invalid self-authored test rules");
    // World::tick includes match completion. A passive, zero-damage opposing
    // Pioneer keeps both sides alive while the public simulation API runs.
    // No side has AI, a production queue, a collector or an active weapon.
    require(world.spawn(UnitKind::Pioneer, Side::Copper, world.map.starts[1]).has_value(),
            "Could not create the passive synthetic opponent");
    return world;
}

EntityId spawn(World& world, UnitKind kind, Cell at, Side side = Side::Blue) {
    const auto id = world.spawn(kind, side, at);
    require(id.has_value(), "Could not spawn a self-authored test actor");
    return *id;
}

// Keep public World::tick's economy/combat/match policies active and prove
// that this movement-only fixture does not accidentally exercise those paths.
void tick_public(World& world, Navigator& nav) {
    const auto before_step = world.step;
    const auto before_rng = world.rng_state;
    const auto before_entities = world.entities.size();
    std::array<std::array<std::uint64_t, 3>, 2> economy{};
    for (std::size_t side = 0; side < world.sides.size(); ++side) {
        const auto& state = world.sides[side];
        require(state.queue.empty() && !state.ai, "Movement fixture acquired production or AI");
        economy[side] = {state.funds, state.income, state.deliveries};
    }
    require(world.projectiles.empty(), "Movement fixture acquired combat projectiles");
    world.tick(nav);
    require(world.step == before_step + 1 && world.result == MatchResult::Running,
            "Public tick did not advance once or ended the controlled two-side match");
    require(world.rng_state == before_rng && world.entities.size() == before_entities &&
            world.projectiles.empty(), "Public tick introduced combat, RNG work or entity changes");
    for (std::size_t side = 0; side < world.sides.size(); ++side) {
        const auto& state = world.sides[side];
        require(state.queue.empty() && !state.ai && state.funds == economy[side][0] &&
                state.income == economy[side][1] && state.deliveries == economy[side][2],
                "Public tick introduced an unintended economy or AI side effect");
    }
}

std::vector<Cell> query(Navigator& nav, const World& world, Cell from, Cell goal) {
    std::array<Cell, 8> result;
    result.fill(Cell{-20, -30});
    const auto count = nav.ranked_static_steps(world, from, goal, result);
    require(count <= result.size(), "Static query exceeded eight candidates");
    for (std::size_t i = count; i < result.size(); ++i) {
        require(result[i] == Cell{-20, -30}, "Static query wrote beyond its returned prefix");
    }
    return {result.begin(), result.begin() + count};
}

bool has(const std::vector<Cell>& steps, Cell at) {
    return std::find(steps.begin(), steps.end(), at) != steps.end();
}

bool static_blocked(const World& world, const std::vector<std::uint8_t>& mask, Cell at) {
    if (!world.map.walkable(at)) return true;
    if (!mask.empty() && mask[world.map.index(at)]) return true;
    return std::any_of(world.entities.begin(), world.entities.end(), [&](const Entity& entity) {
        return entity.hp && is_building(entity.kind) && entity.position == at;
    });
}

void require_legal(const World& world, const std::vector<std::uint8_t>& mask,
                   Cell from, const std::vector<Cell>& steps) {
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto to = steps[i];
        const int dx = to.x - from.x, dy = to.y - from.y;
        require((dx || dy) && std::abs(dx) <= 1 && std::abs(dy) <= 1,
                "Static query returned a nonadjacent cell");
        require(!static_blocked(world, mask, to), "Static query entered a static blocker");
        require(world.map.can_travel(from, to), "Static query crossed an imported terrain edge");
        require(std::find(steps.begin(), steps.begin() + i, to) == steps.begin() + i,
                "Static query duplicated a candidate");
        if (dx && dy) {
            require(!static_blocked(world, mask, {to.x, from.y}) &&
                    !static_blocked(world, mask, {from.x, to.y}),
                    "Static query cut a blocked diagonal corner");
        }
    }
}

void mobile_ring_and_stable_order() {
    auto world = open_world();
    const Cell from{4, 4}, goal{12, 6};
    const auto mover = spawn(world, UnitKind::Pioneer, from);
    // NW is the only empty neighbor, and increases the static distance. E
    // and SE have the same best full-route cost. All mobile side cells stay
    // occupied, including those used by diagonal candidates.
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        if ((!dx && !dy) || (dx == -1 && dy == -1)) continue;
        spawn(world, UnitKind::Skirmisher, {from.x + dx, from.y + dy});
    }
    Command command{};
    command.kind = CommandKind::Move;
    command.side = Side::Blue;
    command.units = {mover};
    command.target = goal;
    require(world.apply(command), "Could not issue the ordinary Move fixture");
    const auto before = encode_snapshot(world);
    const std::vector<Cell> expected{{5, 4}, {5, 5}, {4, 5}, {5, 3}};
    for (const unsigned threads : {1U, 4U, 8U}) {
        Navigator nav(threads);
        nav.prepare(world);
        require(!nav.next(world, from, goal), "Occupied ring did not block the ordinary query");
        require(query(nav, world, from, goal) == expected,
                "Static query lost a mobile blocker, a descending alternative or its stable cost order");
        require_legal(world, {}, from, expected);
        const auto built = nav.flow_stats().fields_built;
        require(built == 1, "Unblocked single-goal query did not reuse one shared flow field");
        for (unsigned repeat = 0; repeat < 64; ++repeat) {
            require(query(nav, world, from, goal) == expected, "Warm query changed candidate order");
        }
        require(nav.flow_stats().fields_built == built, "Repeated query rebuilt a resident goal field");
        require(nav.flow_stats().active_goals == 0 && nav.flow_stats().active_references == 0,
                "Read-only route query changed active movement references");
        require(encode_snapshot(world) == before,
                "Static query changed actors, destinations, orders, selection, RNG or simulation time");
    }
}

void all_static_blocker_sources_keep_corners_closed() {
    // Each blocker source independently closes E and both diagonals using E
    // as a side cell. Other legal detours must remain available.
    for (unsigned kind = 0; kind < 4; ++kind) {
        auto world = open_world();
        const Cell from{4, 4}, goal{12, 6}, blocked{5, 4};
        std::vector<std::uint8_t> mask(world.map.tiles.size(), 0);
        if (kind == 0) world.map.at(blocked).terrain = Terrain::Rock;
        if (kind == 1) world.map.at(blocked).terrain = Terrain::Water;
        if (kind == 2) spawn(world, UnitKind::Hub, blocked);
        if (kind == 3) mask[world.map.index(blocked)] = 9;
        Navigator nav;
        nav.set_obstacle_mask(mask);
        nav.prepare(world);
        const auto steps = query(nav, world, from, goal);
        require(!steps.empty(), "Single static blocker erased every legal detour");
        require(!has(steps, blocked) && !has(steps, {5, 3}) && !has(steps, {5, 5}),
                "Static blocker or one of its diagonal side corners was ignored");
        require_legal(world, mask, from, steps);
        require(nav.next(world, from, goal) == steps.front(),
                "Static and ordinary queries disagree when mobiles do not block the candidates");
        require(query(nav, world, blocked, goal).empty(), "Blocked origin produced a route");
    }
}

int edge_bit(Cell from, Cell to) {
    const int raw = (to.y - from.y + 1) * 3 + to.x - from.x + 1;
    return raw < 4 ? raw : raw - 1;
}

void imported_edges_and_diagonal_sides_repair_warm_cache() {
    auto world = open_world();
    const Cell from{4, 4}, east{5, 4}, south{4, 5}, diagonal{5, 5}, goal{10, 10};
    // The source importer owns elevation/ramp legality and publishes it as
    // travel_edges. Tile height alone is not an extra rule in this API.
    world.map.travel_edges.assign(world.map.tiles.size(), 255);
    world.map.at(east).height = 2;
    world.map.at(south).terrain = Terrain::Ramp;
    Navigator nav;
    nav.prepare(world);
    const auto initial = query(nav, world, from, goal);
    require(!initial.empty() && initial.front() == diagonal, "Open diagonal fixture is invalid");
    const std::array<std::array<Cell, 2>, 5> edges{{
        {from, diagonal}, {from, east}, {from, south}, {diagonal, east}, {diagonal, south}
    }};
    for (const auto& edge : edges) {
        const auto a = edge[0], b = edge[1];
        const auto bit = static_cast<std::uint8_t>(1U << edge_bit(a, b));
        const auto reverse = static_cast<std::uint8_t>(1U << edge_bit(b, a));
        const auto previous_revision = nav.flow_stats().grid_invalidations;
        world.map.travel_edges[world.map.index(a)] &= static_cast<std::uint8_t>(~bit);
        require(world.map.travel_edges[world.map.index(b)] & reverse,
                "Fixture accidentally closed both endpoints");
        require(!world.map.can_travel(from, diagonal), "One-endpoint closure did not invalidate diagonal");
        nav.prepare(world);
        require(nav.flow_stats().grid_invalidations > previous_revision,
                "Imported edge change did not invalidate the warm grid");
        const auto steps = query(nav, world, from, goal);
        require(!steps.empty() && !has(steps, diagonal), "Static query bypassed a closed terrain/side edge");
        require_legal(world, {}, from, steps);
        world.map.travel_edges[world.map.index(a)] |= bit;
        nav.prepare(world);
        const auto reopened = query(nav, world, from, goal);
        require(!reopened.empty() && reopened.front() == diagonal,
                "Reopened edge did not restore the cached best diagonal");
    }
}

void masked_goal_fallback_ignores_mobile_occupants() {
    auto world = open_world();
    const Cell from{2, 7}, goal{12, 7};
    std::vector<std::uint8_t> mask(world.map.tiles.size(), 0);
    mask[world.map.index(goal)] = 1;
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        if (dx || dy) spawn(world, UnitKind::Pioneer, {goal.x + dx, goal.y + dy});
    }
    const auto before = encode_snapshot(world);
    Navigator nav;
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(!nav.next(world, from, goal), "All occupied fallback goals did not block next()");
    const auto steps = query(nav, world, from, goal);
    require(!steps.empty() && steps.front() == Cell{3, 7},
            "Static query dropped occupied fallback cells around a masked goal");
    require_legal(world, mask, from, steps);
    require(nav.flow_stats().fields_built <= 8, "Masked goal built more than eight fallback fields");
    require(query(nav, world, {11, 7}, goal).empty(), "Already reached fallback goal did not wait");
    require(encode_snapshot(world) == before, "Masked-goal query rewrote the user destination or actors");
}

void padded_map_and_live_footprint_mask() {
    auto world = open_world(3, 5);
    const Cell start{0, 0}, goal{2, 0};
    std::vector<std::uint8_t> mask(world.map.tiles.size(), 0);
    for (int y = 0; y < 4; ++y) mask[world.map.index({1, y})] = 1;
    Navigator nav;
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(query(nav, world, start, goal) == std::vector<Cell>{{0, 1}},
            "Padded flow grid used the wrong source-map stride");
    Cell cursor = start;
    bool bottom = false;
    for (unsigned step = 0; step < 24 && cursor != goal; ++step) {
        const auto steps = query(nav, world, cursor, goal);
        require(!steps.empty(), "Open footprint passage became unreachable");
        require_legal(world, mask, cursor, steps);
        cursor = steps.front();
        bottom |= cursor.y == 4;
    }
    require(cursor == goal && bottom, "Footprint route did not use the real bottom-row passage");
    mask[world.map.index({1, 4})] = 1;
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    require(query(nav, world, start, goal).empty(), "Closed footprint reused a stale reachable field");
    nav.set_obstacle_mask({});
    nav.prepare(world);
    const auto reopened = query(nav, world, start, goal);
    require(!reopened.empty() && reopened.front() == Cell{1, 0},
            "Clearing a warm footprint mask did not restore the direct route");
}

void unavailable_queries_return_empty() {
    auto world = open_world();
    Navigator nav;
    require(query(nav, world, {2, 2}, {12, 6}).empty(), "Unprepared query returned a route");
    world.map.at({1, 1}).terrain = Terrain::Rock;
    nav.prepare(world);
    for (const auto& cells : std::array<std::array<Cell, 2>, 5>{{
             {Cell{-1, 2}, Cell{12, 6}}, {Cell{2, 2}, Cell{16, 6}},
             {Cell{2, 2}, Cell{2, 2}}, {Cell{1, 1}, Cell{12, 6}},
             {Cell{2, 2}, Cell{1, 1}}
         }}) {
        require(query(nav, world, cells[0], cells[1]).empty(), "Invalid/no-op query produced a route");
    }
    const auto other_size = open_world(3, 5);
    require(query(nav, other_size, {0, 0}, {2, 0}).empty(), "Mismatched prepared dimensions were accepted");
}

World corridor_world() {
    auto world = open_world();
    for (auto& tile : world.map.tiles) tile.terrain = Terrain::Rock;
    for (int x = 1; x < 15; ++x) world.map.at({x, 2}).terrain = Terrain::Ground;
    world.map.starts = {Cell{1, 2}, Cell{14, 2}};
    for (std::size_t i = 0; i < world.sides.size(); ++i) world.sides[i].rally = world.map.starts[i];
    // open_world's opposing Pioneer must remain on the corridor's real ground.
    for (auto& entity : world.entities) {
        if (entity.side == Side::Copper) entity.position = world.map.starts[1];
    }
    return world;
}

void move(World& world, EntityId id, Cell goal) {
    Command command{};
    command.kind = CommandKind::Move;
    command.side = world.find(id)->side;
    command.units = {id};
    command.target = goal;
    require(world.apply(command), "Could not issue the transient-occupancy Move fixture");
}

void transient_source_waits_and_releases_original_move() {
    for (const unsigned threads : {1U, 4U, 8U}) {
        auto world = corridor_world();
        const Cell from{2, 2}, goal{3, 2};
        const auto mover = spawn(world, UnitKind::Pioneer, from);
        const auto buddy = spawn(world, UnitKind::Pioneer, goal);
        move(world, mover, goal); // The only command issued during this case.
        Navigator nav(threads);
        nav.prepare(world);
        {
            std::vector<MobileOccupant> source{{goal, buddy}};
            nav.set_transient_occupancy(source);
            // The setter owns its staged entries, even if the caller releases
            // or mutates the source vector before publication.
            source.front().owner = mover;
            source.clear();
        }
        require(!nav.transient_occupied(goal, mover), "Staging published before prepare()");
        nav.prepare(world);
        require(nav.transient_occupied(goal, mover) && !nav.transient_occupied(goal, buddy),
                "Unique physical-source owner filtering is incorrect");
        // An adjacent occupied goal normally completes Move. A transient
        // physical source must preserve this original order instead.
        tick_public(world, nav);
        require(world.find(mover)->position == from && world.find(mover)->order == OrderKind::Move &&
                world.find(mover)->destination == goal, "Temporary source completed Move as Idle");

        // Model a reservation already committed to a new target while its
        // interpolated physical tail is still in the old cell. This fixture
        // change represents the producer's two distinct coordinate domains.
        world.find(buddy)->position = {4, 2};
        nav.prepare(world);
        const auto before_queries = encode_snapshot(world);
        require(!nav.next(world, from, goal, mover), "Reserved new target released the physical tail early");
        require(query(nav, world, from, goal) == std::vector<Cell>{goal},
                "Static query treated a physical tail as topology");
        require(encode_snapshot(world) == before_queries,
                "Tail/static queries changed commands, actors, RNG or the simulation clock");
        for (unsigned wait = 0; wait < 3; ++wait) {
            tick_public(world, nav);
            const auto* actor = world.find(mover);
            require(actor->position == from && actor->order == OrderKind::Move && actor->destination == goal,
                    "Core reservation entered a transient source or forgot the original Move");
            require(nav.last_work().moved == 0, "Waiting source committed an unexpected movement");
        }
        const auto warm = nav.flow_stats();
        nav.set_transient_occupancy({});
        require(nav.transient_occupied(goal, mover), "Clearing stage changed the published snapshot early");
        nav.prepare(world);
        require(!nav.transient_occupied(goal, mover) && nav.next(world, from, goal, mover) == goal,
                "Physical-source release did not reopen the original destination");
        require(nav.flow_stats().grid_invalidations == warm.grid_invalidations &&
                nav.flow_stats().fields_built == warm.fields_built,
                "Transient release invalidated/rebuilt a static flow field");
        tick_public(world, nav);
        require(world.find(mover)->position == goal, "Original Move did not continue after release");
        tick_public(world, nav);
        require(world.find(mover)->order == OrderKind::Idle,
                "Move did not complete after actually reaching its original destination");
    }
}

void transient_owners_are_conservative_on_overlap() {
    auto world = corridor_world();
    const Cell from{2, 2}, goal{3, 2};
    const auto mover = spawn(world, UnitKind::Pioneer, from);
    const auto other = spawn(world, UnitKind::Pioneer, {8, 2});
    Navigator nav;
    auto publish = [&](std::initializer_list<MobileOccupant> entries) {
        nav.set_transient_occupancy(std::span<const MobileOccupant>(entries.begin(), entries.size()));
        nav.prepare(world);
    };
    publish({{goal, mover}, {goal, mover}});
    require(!nav.transient_occupied(goal, mover) && nav.transient_occupied(goal),
            "Duplicate same-owner entries became an overlap or an unknown-owner query ignored them");
    require(nav.next(world, from, goal, mover) == goal && !nav.next(world, from, goal),
            "next() did not consistently apply its explicit owner");
    const auto before = encode_snapshot(world);
    const auto warm = nav.flow_stats();
    publish({{goal, mover}, {goal, other}});
    require(nav.transient_occupied(goal, mover) && nav.transient_occupied(goal, other),
            "One owner bypassed a cell shared by multiple owners");
    require(!nav.next(world, from, goal, mover), "Multi-owner overlap did not block the destination");
    publish({{goal, no_entity}});
    require(nav.transient_occupied(goal, mover) && nav.transient_occupied(goal),
            "Unknown source owner was treated as a free cell");
    publish({{goal, mover}});
    require(nav.next(world, from, goal, mover) == goal, "Replacing overlap did not restore the unique owner");
    require(nav.flow_stats().grid_invalidations == warm.grid_invalidations &&
            nav.flow_stats().fields_built == warm.fields_built,
            "Owner-only transient changes rebuilt static topology or fields");
    require(encode_snapshot(world) == before, "Owner queries changed World authority");

    nav.set_transient_occupancy(std::array{MobileOccupant{{16, 2}, other}});
    bool rejected = false;
    try { nav.prepare(world); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Out-of-map transient entry was not rejected");
    require(encode_snapshot(world) == before, "Rejected transient prepare changed World authority");
    nav.set_transient_occupancy({});
    nav.prepare(world);
    require(nav.next(world, from, goal, mover) == goal, "Clear/prepare did not recover from invalid entries");
}

void transient_side_cells_preserve_static_diagonals() {
    auto world = open_world();
    const Cell from{4, 4}, east{5, 4}, south{4, 5}, diagonal{5, 5}, goal{10, 10};
    const auto mover = spawn(world, UnitKind::Pioneer, from);
    const auto first = spawn(world, UnitKind::Pioneer, {12, 1});
    const auto second = spawn(world, UnitKind::Pioneer, {13, 1});
    move(world, mover, goal);
    Navigator nav;
    const std::array sources{MobileOccupant{east, first}, MobileOccupant{south, second}};
    nav.set_transient_occupancy(sources);
    nav.prepare(world);
    require(nav.next(world, from, goal, mover) == diagonal,
            "Transient side occupancy was incorrectly added to static corner blockers");
    tick_public(world, nav);
    require(world.find(mover)->position == diagonal, "Core rank/commit did not admit the legal diagonal");

    world.find(mover)->position = from;
    world.find(mover)->move_wait = 0;
    std::vector<std::uint8_t> mask(world.map.tiles.size(), 0);
    mask[world.map.index(east)] = 1;
    nav.set_obstacle_mask(mask);
    nav.prepare(world);
    const auto steps = query(nav, world, from, goal);
    require(!steps.empty() && !has(steps, diagonal), "Transient API weakened a real static side blocker");
    require_legal(world, mask, from, steps);
    tick_public(world, nav);
    require(world.find(mover)->position != diagonal, "Core commit crossed a static blocked corner");
}

void completion_flags_gate_arrival_and_settled_neighbors() {
    {
        auto world = open_world();
        const Cell goal{4, 4};
        const auto mover = spawn(world, UnitKind::Pioneer, goal);
        move(world, mover, goal);
        Navigator nav;
        require(!nav.can_finish_move(goal, goal, true), "Unprepared completion query succeeded");
        nav.set_transient_occupancy(std::array{MobileOccupant{goal, mover, true}});
        nav.prepare(world);
        const auto before = encode_snapshot(world);
        const auto fields = nav.flow_stats().fields_built;
        require(!nav.can_finish_move(goal, goal, true),
                "Own unsettled body/turn was ignored after logical arrival");
        require(!nav.can_finish_move({-1, 4}, goal, true), "Invalid completion origin succeeded");
        require(encode_snapshot(world) == before && nav.flow_stats().fields_built == fields,
                "Completion query changed World authority or built a field");
        for (unsigned wait = 0; wait < 3; ++wait) {
            tick_public(world, nav);
            require(world.find(mover)->order == OrderKind::Move && world.find(mover)->position == goal,
                    "Logical arrival completed before the producer declared motion/turn settled");
        }
        nav.set_transient_occupancy(std::array{MobileOccupant{goal, mover, false}});
        nav.prepare(world);
        require(nav.can_finish_move(goal, goal, true), "Settled own arrival never became completable");
        require(nav.transient_occupied(goal), "Settled completion relaxed physical cell admission");
        tick_public(world, nav);
        require(world.find(mover)->order == OrderKind::Idle, "Settled own arrival did not complete Move");
    }
    {
        auto world = corridor_world();
        const Cell from{2, 2}, goal{3, 2};
        const auto mover = spawn(world, UnitKind::Pioneer, from);
        const auto buddy = spawn(world, UnitKind::Pioneer, goal);
        move(world, mover, goal);
        Navigator nav;
        nav.set_transient_occupancy(std::array{MobileOccupant{goal, buddy}});
        nav.prepare(world);
        require(!nav.can_finish_move(from, goal, true), "Default producer did not conservatively wait");
        tick_public(world, nav);
        require(world.find(mover)->order == OrderKind::Move, "Unsettled adjacent goal completed Move");
        nav.set_transient_occupancy(std::array{MobileOccupant{goal, buddy, false}});
        nav.prepare(world);
        require(nav.can_finish_move(from, goal, true), "Settled occupied neighbor never completed Move");
        require(!nav.can_finish_move(from, goal, false), "Terrain/mask occupancy was treated as a live body");
        require(!nav.can_finish_move({1, 2}, goal, true), "Distant occupied goal completed Move");
        require(nav.transient_occupied(goal, mover) && !nav.next(world, from, goal, mover),
                "Completion flag weakened foreign-body admission");
        tick_public(world, nav);
        require(world.find(mover)->position == from && world.find(mover)->order == OrderKind::Idle,
                "Adjacent settled completion moved into the occupied goal or stayed busy");
    }
}

void completion_flags_preserve_overlap_safety() {
    auto world = corridor_world();
    const Cell from{2, 2}, goal{3, 2};
    const auto mover = spawn(world, UnitKind::Pioneer, from);
    const auto buddy = spawn(world, UnitKind::Pioneer, goal);
    const auto other = spawn(world, UnitKind::Pioneer, {8, 2});
    Navigator nav;
    auto publish = [&](std::initializer_list<MobileOccupant> entries) {
        nav.set_transient_occupancy(std::span<const MobileOccupant>(entries.begin(), entries.size()));
        nav.prepare(world);
    };
    publish({{goal, buddy, false}, {goal, buddy, true}});
    require(!nav.can_finish_move(from, goal, true), "Same-owner flags were not conservatively ORed");
    publish({{goal, buddy, false}, {goal, other, false}});
    require(nav.transient_occupied(goal, buddy) && nav.transient_occupied(goal, other),
            "Settled flags weakened a multi-owner physical overlap");
    require(!nav.can_finish_move(from, goal, true) && !nav.can_finish_move(goal, goal, true),
            "Multiple physical owners became completable merely by clearing flags");
    publish({{goal, no_entity, false}});
    require(nav.transient_occupied(goal, mover) && !nav.can_finish_move(from, goal, true),
            "Unknown physical owner bypassed completion safety");
    publish({{goal, buddy, false}, {goal, buddy, false}});
    const auto before = encode_snapshot(world);
    require(nav.can_finish_move(from, goal, true) && nav.transient_occupied(goal, mover),
            "Settled same-owner duplicates did not separate completion from admission");
    require(encode_snapshot(world) == before, "Completion/overlap queries changed World authority");
}

std::vector<std::vector<std::uint8_t>> shared_goal_run(unsigned threads) {
    auto world = open_world();
    const Cell goal{11, 5};
    const std::vector<EntityId> ids{
        spawn(world, UnitKind::Pioneer, {2, 4}),
        spawn(world, UnitKind::Pioneer, {2, 5}),
        spawn(world, UnitKind::Pioneer, {2, 6})
    };
    Command command{};
    command.kind = CommandKind::Move;
    command.side = Side::Blue;
    command.target = goal;
    command.units = ids;
    require(world.apply(command), "Could not issue the shared-goal selected Move");
    Navigator nav(threads);
    std::vector<std::vector<std::uint8_t>> states;
    for (unsigned tick = 0; tick < 48; ++tick) {
        std::vector<MobileOccupant> bodies;
        for (const auto& entity : world.entities) {
            // Instantaneous synthetic actors publish actual current cells.
            // A strategic command destination is deliberately not occupancy.
            // A real interpolated producer must additionally retain its body
            // and source tail, and keep completion true until turning settles.
            const bool unfinished = entity.order == OrderKind::Move && entity.position != entity.destination;
            bodies.push_back({entity.position, entity.id, unfinished});
        }
        nav.set_transient_occupancy(bodies);
        nav.prepare(world);
        const auto before_queries = encode_snapshot(world);
        const bool goal_has_body = !world.free_at(goal);
        for (const auto id : ids) {
            const auto* entity = world.find(id);
            (void)nav.can_finish_move(entity->position, goal, goal_has_body);
        }
        require(encode_snapshot(world) == before_queries, "Shared-goal completion query changed the command/clock");
        tick_public(world, nav);
        for (std::size_t a = 0; a < world.entities.size(); ++a) {
            for (std::size_t b = a + 1; b < world.entities.size(); ++b) {
                require(world.entities[a].position != world.entities[b].position,
                        "Shared-goal completion admitted overlapping physical bodies");
            }
        }
        states.push_back(encode_snapshot(world));
    }
    unsigned at_goal = 0;
    for (const auto id : ids) {
        const auto* entity = world.find(id);
        require(entity->order == OrderKind::Idle, "Shared strategic goal remained permanently busy");
        require(std::max(std::abs(entity->position.x - goal.x), std::abs(entity->position.y - goal.y)) <= 1,
                "Shared-goal Move finished before reaching its goal neighborhood");
        at_goal += entity->position == goal;
    }
    require(at_goal == 1, "Existing deterministic cell claims did not choose one actual goal occupant");
    return states;
}

void shared_goals_finish_after_settlement() {
    const auto one = shared_goal_run(1);
    require(one == shared_goal_run(4), "Complete shared-goal snapshots diverged for 1/4 threads");
    require(one == shared_goal_run(8), "Complete shared-goal snapshots diverged for 1/8 threads");
}
} // namespace

int main() {
    try {
        mobile_ring_and_stable_order();
        all_static_blocker_sources_keep_corners_closed();
        imported_edges_and_diagonal_sides_repair_warm_cache();
        masked_goal_fallback_ignores_mobile_occupants();
        padded_map_and_live_footprint_mask();
        unavailable_queries_return_empty();
        transient_source_waits_and_releases_original_move();
        transient_owners_are_conservative_on_overlap();
        transient_side_cells_preserve_static_diagonals();
        completion_flags_gate_arrival_and_settled_neighbors();
        completion_flags_preserve_overlap_safety();
        shared_goals_finish_after_settlement();
        std::cout << "RTS static steps: mobile ring, stable alternatives, read-only World, shared warm fields, "
                     "all static blockers, reciprocal/slope edges, fallback goals, padded masks and "
                     "transient source/owner filtering, completion settlement and shared goals passed\n";
    } catch (const std::exception& error) {
        std::cerr << "RTS static steps failed: " << error.what() << '\n';
        return 1;
    }
}
