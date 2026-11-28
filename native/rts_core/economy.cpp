// SPDX-License-Identifier: GPL-3.0-only
#include "economy.h"

#include "navigation/reachability.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace coastal::rts {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(UnitKind::Count)> unit_names{
    "Pioneer", "Hub", "Generator", "Depot", "Workshop", "Collector",
    "Skirmisher", "Sentinel", "Artillery",
};
constexpr std::string_view columns =
    "kind\thp\tcost\ttrain_ticks\tmove_period\tvision\trange\tdamage\tarmor\tshot_period\tpower";
constexpr std::size_t max_rules_bytes = 8192;
constexpr int build_radius = 10;
constexpr std::array<Cell, 4> cardinal{{{0, -1}, {-1, 0}, {1, 0}, {0, 1}}};
constexpr std::array<Cell, 8> exits{{
    {0, -1}, {-1, 0}, {1, 0}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
}};

int distance(Cell a, Cell b) noexcept {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

Cell neighbor(Cell from, Cell offset) noexcept {
    return {from.x + offset.x, from.y + offset.y};
}

bool row(std::ifstream& file, std::string& line) {
    if (!std::getline(file, line)) return false;
    if (line.ends_with('\r')) line.pop_back();
    return true;
}

template<std::size_t N>
bool fields(std::string_view line, std::array<std::string_view, N>& out) {
    for (std::size_t i = 0; i < N; ++i) {
        const auto tab = line.find('\t');
        if ((tab == line.npos) != (i == N - 1)) return false;
        out[i] = line.substr(0, tab);
        if (out[i].empty()) return false;
        if (tab != line.npos) line.remove_prefix(tab + 1);
    }
    return true;
}

template<class Integer>
bool number(std::string_view value, Integer& out) {
    const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), out);
    return status == std::errc{} && end == value.data() + value.size();
}

bool unit_row(std::string_view line, std::string_view name, UnitSpec& spec) {
    std::array<std::string_view, 11> v;
    return fields(line, v) && v[0] == name &&
        number(v[1], spec.hp) && number(v[2], spec.cost) && number(v[3], spec.train_ticks) &&
        number(v[4], spec.move_period) && number(v[5], spec.vision) && number(v[6], spec.range) &&
        number(v[7], spec.damage) && number(v[8], spec.armor) && number(v[9], spec.shot_period) &&
        number(v[10], spec.power);
}

bool owned(const Entity& entity, Side side, UnitKind unit) noexcept {
    return entity.side == side && entity.kind == unit && entity.hp != 0;
}

const Entity* owned_building(const World& world, Side side, UnitKind kind) noexcept {
    const auto found = std::find_if(world.entities.begin(), world.entities.end(),
        [=](const Entity& entity) { return owned(entity, side, kind); });
    return found == world.entities.end() ? nullptr : &*found;
}

void update_power(World& world) noexcept {
    std::array<std::uint64_t, max_houses> supply{}, demand{};
    for (const auto& entity : world.entities) {
        if (entity.hp == 0 || !is_building(entity.kind)) continue;
        const auto side = side_index(entity.side);
        const auto power = world.rules.spec(entity.kind).power;
        if (power > 0) supply[side] += power;
        else demand[side] += -power;
    }
    for (std::size_t i = 0; i < world.house_count(); ++i) {
        world.sides[i].power_percent = demand[i] == 0 ? 100 :
            static_cast<std::uint16_t>(std::min<std::uint64_t>(100, 100 * supply[i] / demand[i]));
    }
}

void add_funds(std::uint64_t& value, std::uint64_t amount) noexcept {
    value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}

bool deploy(World& world, const Command& command) {
    const auto& selected = command.units.empty() ? world.side(command.side).selected : command.units;
    const auto candidate = std::find_if(world.entities.begin(), world.entities.end(), [&](const Entity& entity) {
        return owned(entity, command.side, UnitKind::Pioneer) &&
            (selected.empty() || std::find(selected.begin(), selected.end(), entity.id) != selected.end()) &&
            (command.target == Cell{} || command.target == entity.position) &&
            world.map.at(entity.position).effective_terrain() == Terrain::Ground;
    });
    if (candidate == world.entities.end()) return false;
    const auto current_hp = candidate->hp;
    const auto pioneer_hp = world.rules.spec(UnitKind::Pioneer).hp;
    const auto hub_hp = world.rules.spec(UnitKind::Hub).hp;
    const auto scaled_hp = (std::uint32_t(current_hp) * hub_hp + pioneer_hp - 1) / pioneer_hp;
    const auto hp = static_cast<std::uint16_t>(std::min<std::uint32_t>(hub_hp, scaled_hp));
    *candidate = Entity{.id = candidate->id, .kind = UnitKind::Hub, .side = candidate->side,
        .position = candidate->position, .hp = hp, .destination = {}, .deposit = {}};
    update_power(world);
    return true;
}

bool can_stand(const World& world, const Entity& collector, Cell cell) noexcept {
    return world.map.walkable(cell) && (cell == collector.position || world.free_at(cell));
}

void idle(Entity& collector) noexcept {
    collector.order = OrderKind::Idle;
    collector.destination = {};
}

void gather_at(Entity& collector, Cell goal) noexcept {
    collector.order = OrderKind::Gather;
    collector.destination = goal;
}

class GatherRoutes {
public:
    explicit GatherRoutes(const World& world) : world_(world) {}

    std::optional<Cell> best_deposit(const Entity& collector) {
        if (!deposits_) {
            deposits_.emplace();
            for (std::size_t i = 0; i < world_.map.tiles.size(); ++i) {
                const auto& tile = world_.map.tiles[i];
                if (tile.effective_terrain() == Terrain::Deposit) {
                    deposits_->push_back(world_.map.cell(static_cast<int>(i)));
                }
            }
        }
        std::optional<Cell> best;
        int best_distance = std::numeric_limits<int>::max();
        for (const Cell cell : *deposits_) {
            const int separation = distance(collector.position, cell);
            if (separation >= best_distance || world_.map.at(cell).resource == 0 ||
                !can_stand(world_, collector, cell) || !connected(collector.position, cell)) continue;
            best = cell;
            best_distance = separation;
        }
        return best;
    }

    bool connected(Cell from, Cell to) {
        if (!reachability_) {
            reachability_.emplace();
            std::vector<unsigned char> blocked(world_.map.tiles.size());
            for (std::size_t i = 0; i < blocked.size(); ++i) {
                blocked[i] = !world_.map.walkable(world_.map.cell(static_cast<int>(i)));
            }
            for (const auto& entity : world_.entities) {
                if (entity.hp != 0 && is_building(entity.kind)) blocked[world_.map.index(entity.position)] = 1;
            }
            std::string error;
            reachability_->update(world_.map.width, world_.map.height, blocked, error);
        }
        return reachability_->connected(world_.map.index(from), world_.map.index(to));
    }

    std::optional<std::pair<EntityId, Cell>> best_depot(const Entity& collector) {
        std::optional<std::pair<EntityId, Cell>> best;
        int best_distance = std::numeric_limits<int>::max();
        for (const auto& depot : world_.entities) {
            if (!owned(depot, collector.side, UnitKind::Depot)) continue;
            for (const auto offset : cardinal) {
                const auto cell = neighbor(depot.position, offset);
                const int separation = distance(collector.position, cell);
                if (separation >= best_distance || !can_stand(world_, collector, cell) ||
                    !connected(collector.position, cell)) continue;
                best = {depot.id, cell};
                best_distance = separation;
            }
        }
        return best;
    }

private:
    const World& world_;
    std::optional<navigation::ReachabilityCache> reachability_;
    std::optional<std::vector<Cell>> deposits_;
};

void seek(World& world, Entity& collector, GatherRoutes& routes) {
    const auto valid = [&](Cell cell) {
        return can_stand(world, collector, cell) &&
            world.map.at(cell).effective_terrain() == Terrain::Deposit;
    };
    // Occupancy can change while a collector approaches. Replan only when the
    // previous target is lost or the collector needs a new route.
    const bool recheck = world.step % 60 == 0;
    if (!valid(collector.deposit) || collector.order != OrderKind::Gather ||
        (recheck && !routes.connected(collector.position, collector.deposit))) {
        collector.deposit = routes.best_deposit(collector).value_or(Cell{});
    }
    if (collector.deposit == Cell{}) {
        idle(collector);
        return;
    }
    gather_at(collector, collector.deposit);
    if (collector.position == collector.deposit) collector.gather = GatherPhase::Harvest;
}

void harvest(World& world, Entity& collector) noexcept {
    if (collector.position != collector.deposit || !world.map.contains(collector.deposit) ||
        world.map.at(collector.deposit).effective_terrain() != Terrain::Deposit) {
        collector.gather = GatherPhase::Seek;
        collector.work = 0;
        collector.deposit = {};
        idle(collector);
        return;
    }
    gather_at(collector, collector.deposit);
    if (++collector.work < world.rules.harvest_ticks) return;
    auto& deposit = world.map.at(collector.deposit);
    collector.cargo = std::min(world.rules.cargo_capacity, deposit.resource);
    deposit.resource -= collector.cargo;
    collector.deposit = {};
    collector.work = 0;
    collector.gather = GatherPhase::Return;
    idle(collector);
}

void unload(World& world, Entity& collector, GatherRoutes& routes) {
    const auto* depot = world.find(collector.depot);
    const bool berth_valid = depot && owned(*depot, collector.side, UnitKind::Depot) &&
        collector.order == OrderKind::Gather && distance(depot->position, collector.destination) == 1 &&
        can_stand(world, collector, collector.destination) &&
        (world.step % 60 != 0 || routes.connected(collector.position, collector.destination));
    if (!berth_valid) {
        const auto berth = routes.best_depot(collector);
        if (!berth) {
            collector.depot = no_entity;
            idle(collector);
            return;
        }
        collector.depot = berth->first;
        gather_at(collector, berth->second);
    }
    if (collector.position != collector.destination) return;
    auto& side = world.side(collector.side);
    add_funds(side.funds, collector.cargo);
    add_funds(side.income, collector.cargo);
    if (side.deliveries < std::numeric_limits<std::uint32_t>::max()) ++side.deliveries;
    collector.depot = no_entity;
    collector.cargo = 0;
    collector.gather = GatherPhase::Seek;
    idle(collector);
}

void train(World& world, Side owner) {
    auto& side = world.side(owner);
    if (side.production_paused || side.queue.empty()) return;
    if (!owned_building(world, owner, UnitKind::Workshop)) return;
    auto& order = side.queue.front();
    const auto work = std::min<std::uint32_t>(order.remaining_work, side.power_percent);
    order.remaining_work -= work;
    if (order.remaining_work != 0) return;

    std::optional<Cell> spawn_cell;
    for (const auto& workshop : world.entities) {
        if (!owned(workshop, owner, UnitKind::Workshop)) continue;
        const auto position = std::find_if(exits.begin(), exits.end(), [&](Cell offset) {
            return world.map.walkable(neighbor(workshop.position, offset)) &&
                world.free_at(neighbor(workshop.position, offset));
        });
        if (position == exits.end()) continue;
        spawn_cell = neighbor(workshop.position, *position);
        break;
    }
    if (!spawn_cell || world.entities.size() >= World::entity_limit) return;
    const auto id = world.spawn(order.kind, owner, *spawn_cell);
    if (!id) return;
    if (world.map.walkable(side.rally) && side.rally != *spawn_cell) {
        auto* unit = world.find(*id);
        unit->order = OrderKind::Move;
        unit->destination = side.rally;
    }
    side.queue.erase(side.queue.begin());
}

}  // namespace

bool valid_rules(const Rules& rules) noexcept {
    if (rules.initial_funds > 10'000'000 || rules.cargo_capacity == 0 || rules.cargo_capacity > 4096 ||
        rules.harvest_ticks == 0) return false;
    for (std::size_t i = 0; i < unit_names.size(); ++i) {
        const auto kind = static_cast<UnitKind>(i);
        const auto& spec = rules.spec(kind);
        if (spec.hp == 0 || spec.vision == 0 || spec.vision > 64 || spec.range > spec.vision ||
            ((spec.move_period == 0) != is_building(kind)) ||
            ((spec.range == 0) != (spec.damage == 0)) || ((spec.range == 0) != (spec.shot_period == 0)) ||
            ((spec.train_ticks != 0) != is_producible(kind)) ||
            ((spec.cost == 0) != (kind == UnitKind::Pioneer || kind == UnitKind::Hub))) return false;
        if (!is_building(kind) && spec.power != 0) return false;
    }
    return rules.spec(UnitKind::Hub).power >= 0 && rules.spec(UnitKind::Generator).power > 0 &&
        rules.spec(UnitKind::Depot).power < 0 && rules.spec(UnitKind::Workshop).power < 0;
}

std::optional<Rules> load_rules(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    std::ifstream file(path);
    if (ec || !file || size > max_rules_bytes) {
        error = "Cannot read a Coastal rules table of at most 8192 bytes.";
        return std::nullopt;
    }
    Rules rules;
    std::string line;
    std::array<std::string_view, 4> economy;
    if (!row(file, line) || line != "COASTAL_RULES\t1" || !row(file, line) ||
        !fields(line, economy) || economy[0] != "economy" ||
        !number(economy[1], rules.initial_funds) || !number(economy[2], rules.cargo_capacity) ||
        !number(economy[3], rules.harvest_ticks) || !row(file, line) || line != columns) {
        error = "Invalid Coastal rules table header or economy row.";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < unit_names.size(); ++i) {
        if (!row(file, line) || !unit_row(line, unit_names[i], rules.units[i])) {
            error = "Invalid Coastal rules row for " + std::string(unit_names[i]) + ".";
            return std::nullopt;
        }
    }
    if (row(file, line) || file.bad() || !valid_rules(rules)) {
        error = "Invalid Coastal rules values or extra rows.";
        return std::nullopt;
    }
    return rules;
}

bool can_build(const World& world, Side side, UnitKind kind, Cell position) noexcept {
    if (world.house_defeated(side) || !is_building(kind) || kind == UnitKind::Hub ||
        !world.map.contains(position) || world.map.at(position).effective_terrain() != Terrain::Ground ||
        !world.free_at(position) || world.side(side).funds < world.rules.spec(kind).cost) return false;
    return std::any_of(world.entities.begin(), world.entities.end(), [&](const Entity& entity) {
        return owned(entity, side, UnitKind::Hub) && distance(entity.position, position) <= build_radius;
    });
}

bool apply_economy(World& world, const Command& command) {
    if (world.house_defeated(command.side)) return false;
    auto& side = world.side(command.side);
    switch (command.kind) {
    case CommandKind::Deploy:
        return deploy(world, command);
    case CommandKind::Build:
        if (!can_build(world, command.side, command.unit, command.target) ||
            !world.spawn(command.unit, command.side, command.target)) return false;
        side.funds -= world.rules.spec(command.unit).cost;
        update_power(world);
        return true;
    case CommandKind::Queue: {
        if (!is_producible(command.unit) || side.queue.size() >= SideState::queue_limit ||
            !owned_building(world, command.side, UnitKind::Workshop)) return false;
        const auto& unit = world.rules.spec(command.unit);
        if (side.funds < unit.cost) return false;
        side.queue.push_back({command.unit, unit.cost, std::uint32_t(unit.train_ticks) * 100});
        side.funds -= unit.cost;
        return true;
    }
    case CommandKind::TogglePause:
        side.production_paused = !side.production_paused;
        return true;
    case CommandKind::Cancel:
        if (command.index >= side.queue.size()) return false;
        add_funds(side.funds, side.queue[command.index].paid);
        side.queue.erase(side.queue.begin() + command.index);
        return true;
    case CommandKind::Rally:
        if (!world.map.walkable(command.target)) return false;
        side.rally = command.target;
        return true;
    default:
        return false;
    }
}

void tick_economy(World& world) {
    update_power(world);
    GatherRoutes routes(world);
    for (auto& collector : world.entities) {
        if (collector.kind != UnitKind::Collector || collector.hp == 0 || world.house_defeated(collector.side) ||
            (collector.order != OrderKind::Idle && collector.order != OrderKind::Gather)) continue;
        switch (collector.gather) {
        case GatherPhase::Seek:
            seek(world, collector, routes);
            break;
        case GatherPhase::Harvest:
            harvest(world, collector);
            break;
        case GatherPhase::Return:
            unload(world, collector, routes);
            break;
        }
    }
    for (std::size_t i = 0; i < world.house_count(); ++i) {
        const auto house = static_cast<Side>(i);
        if (!world.house_defeated(house)) train(world, house);
    }
}

}  // namespace coastal::rts
