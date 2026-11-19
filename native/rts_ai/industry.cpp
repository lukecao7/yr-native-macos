// SPDX-License-Identifier: GPL-3.0-only
#include "strategy.h"

#include "navigation/reachability.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace coastal::rts::ai {
namespace {

constexpr int build_radius = 10;
constexpr int safe_expansion_threat = 14;
constexpr std::array<Cell, 4> adjacent{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
constexpr auto kinds = static_cast<std::size_t>(UnitKind::Count);

int distance(Cell a, Cell b) noexcept {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

Cell offset(Cell cell, Cell delta) noexcept {
    return {cell.x + delta.x, cell.y + delta.y};
}

std::size_t kind_index(UnitKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

SeenTile known_tile(const Observation& observation, const AiMemory& memory, Cell cell) {
    if (!observation.contains(cell)) return {};
    const auto index = static_cast<std::size_t>(observation.index(cell));
    if (index < observation.visible.size() && observation.visible[index].known) {
        return observation.visible[index];
    }
    return index < memory.chart.size() ? memory.chart[index] : SeenTile{};
}

bool known_passable(SeenTile tile) {
    return tile.known && tile.terrain != Terrain::Water && tile.terrain != Terrain::Rock;
}

enum class ThreatClass : std::size_t { Light, Armored, LongRange, Fortified };

void score_contact(const Observation& observation, const EnemyContact& contact, int confidence,
                   std::array<int, 3>& scores) {
    if (!contact.id || !contact.hp || contact.kind >= UnitKind::Count) return;
    // Rows are Skirmisher, Sentinel and Artillery. Columns are enemy roles.
    // Speed punishes artillery, armor absorbs light fire, and range lets
    // artillery attack armor and stationary defenses from farther away.
    constexpr std::array<std::array<int, 4>, 3> counters{{
        {{1, -4, 12, 0}},
        {{12, 2, -3, 1}},
        {{3, 13, 1, 3}},
    }};
    const auto& hostile = observation.rules.spec(contact.kind);
    ThreatClass role;
    if (is_building(contact.kind)) role = ThreatClass::Fortified;
    else if (!hostile.damage) return;
    else if (hostile.range >= 7) role = ThreatClass::LongRange;
    else if (hostile.armor >= 5) role = ThreatClass::Armored;
    else role = ThreatClass::Light;
    const auto column = static_cast<std::size_t>(role);
    for (std::size_t i = 0; i < scores.size(); ++i) {
        scores[i] += confidence * counters[i][column];
    }
}

std::array<int, 3> production_scores(const Observation& observation, const AiMemory& memory) {
    std::array<int, 3> scores{12, 10, 8};
    std::vector<EntityId> visible;
    visible.reserve(observation.enemies.size());
    for (const auto& contact : observation.enemies) {
        score_contact(observation, contact, 3, scores);
        visible.push_back(contact.id);
    }
    std::sort(visible.begin(), visible.end());
    for (const auto& contact : memory.enemies) {
        if (std::binary_search(visible.begin(), visible.end(), contact.id) ||
            contact.seen_at > observation.step) continue;
        const auto age = observation.step - contact.seen_at;
        if (age < AiMemory::contact_lifetime) {
            score_contact(observation, contact, age < AiMemory::contact_lifetime / 2 ? 2 : 1, scores);
        }
    }
    return scores;
}

struct Industry {
    const Observation& observation;
    const AiMemory& memory;
    const ThreatGrid& threat;
    IndustryPlan plan;
    std::uint64_t budget;
    std::array<unsigned, kinds> inventory{};
    std::vector<Cell> hubs;
    std::vector<Cell> depots;
    std::vector<Cell> reserved;
    int supply = 0;
    int demand = 0;

    Industry(const Observation& observation, const AiMemory& memory, const ThreatGrid& threat)
        : observation(observation), memory(memory), threat(threat), budget(observation.funds) {
        for (const auto& ally : observation.allies) {
            if (!ally.hp || ally.side != observation.side || ally.kind >= UnitKind::Count) continue;
            account(ally.kind);
            if (ally.kind == UnitKind::Hub) hubs.push_back(ally.position);
            if (ally.kind == UnitKind::Depot) depots.push_back(ally.position);
        }
    }

    unsigned count(UnitKind kind) const noexcept { return inventory[kind_index(kind)]; }

    void account(UnitKind kind) {
        ++inventory[kind_index(kind)];
        const int power = observation.rules.spec(kind).power;
        if (power > 0) supply += power;
        else demand -= power;
    }

    int danger(Cell cell) const noexcept {
        if (cell.x < 0 || cell.y < 0 || cell.x >= threat.width || cell.y >= threat.height) return 0;
        const auto index = std::size_t(cell.y) * threat.width + cell.x;
        return index < threat.cells.size() ? threat.cells[index] : 0;
    }

    bool occupied(Cell cell) const {
        return std::find(reserved.begin(), reserved.end(), cell) != reserved.end() ||
            std::any_of(observation.allies.begin(), observation.allies.end(),
                [cell](const Entity& ally) { return ally.hp && ally.position == cell; }) ||
            std::any_of(observation.enemies.begin(), observation.enemies.end(),
                [cell](const EnemyContact& contact) { return contact.hp && contact.position == cell; });
    }

    std::optional<Cell> site(Cell toward, int maximum_threat = std::numeric_limits<int>::max(),
                             int maximum_distance = std::numeric_limits<int>::max()) const {
        std::optional<Cell> best;
        std::tuple<int, int, int, int, int> best_rank;
        for (const Cell hub : hubs) {
            for (int dy = -build_radius; dy <= build_radius; ++dy) {
                const int extent = build_radius - std::abs(dy);
                for (int dx = -extent; dx <= extent; ++dx) {
                    const int from_hub = std::abs(dx) + std::abs(dy);
                    if (from_hub < 2) continue; // Keep room for units around the Hub.
                    const Cell cell{hub.x + dx, hub.y + dy};
                    const int separation = distance(cell, toward);
                    if (separation > maximum_distance) continue;
                    const auto tile = known_tile(observation, memory, cell);
                    if (!tile.known || tile.terrain != Terrain::Ground || occupied(cell)) continue;
                    const int risk = danger(cell);
                    if (risk > maximum_threat) continue;
                    if (std::count_if(adjacent.begin(), adjacent.end(), [&](Cell delta) {
                            return known_passable(known_tile(observation, memory, offset(cell, delta)));
                        }) < 2) continue;
                    const auto rank = std::tuple{risk, separation, from_hub, cell.y, cell.x};
                    if (!best || rank < best_rank) {
                        best = cell;
                        best_rank = rank;
                    }
                }
            }
        }
        return best;
    }

    bool build(UnitKind kind, std::optional<Cell> cell) {
        if (!cell) return false;
        const auto cost = observation.rules.spec(kind).cost;
        if (budget < cost) return false;
        plan.commands.push_back({.kind = CommandKind::Build, .side = observation.side,
                                 .target = *cell, .unit = kind, .units = {}});
        budget -= cost;
        reserved.push_back(*cell);
        account(kind);
        if (kind == UnitKind::Depot) depots.push_back(*cell);
        return true;
    }

    bool queue(UnitKind kind) {
        const auto cost = observation.rules.spec(kind).cost;
        if (budget < cost) return false;
        plan.commands.push_back({.kind = CommandKind::Queue, .side = observation.side,
                                 .target = {}, .unit = kind, .units = {}});
        budget -= cost;
        account(kind);
        return true;
    }
};

struct Deposit {
    Cell cell;
    int resource;
};

std::vector<Deposit> accessible_deposits(const Industry& industry, std::optional<Cell> proposed_depot = {}) {
    const auto& o = industry.observation;
    std::vector<Deposit> deposits;
    std::vector<unsigned char> blocked(static_cast<std::size_t>(o.width) * o.height);
    for (int y = 0; y < o.height; ++y) {
        for (int x = 0; x < o.width; ++x) {
            const Cell cell{x, y};
            const auto tile = known_tile(o, industry.memory, cell);
            blocked[o.index(cell)] = !known_passable(tile);
            if (tile.known && tile.terrain == Terrain::Deposit && tile.resource) {
                deposits.push_back({cell, tile.resource});
            }
        }
    }
    if (deposits.empty()) return deposits;

    for (const auto& ally : o.allies) {
        if (ally.hp && is_building(ally.kind) && o.contains(ally.position)) blocked[o.index(ally.position)] = 1;
    }
    for (const auto& contact : o.enemies) {
        if (contact.hp && is_building(contact.kind) && o.contains(contact.position)) {
            blocked[o.index(contact.position)] = 1;
        }
    }
    for (const auto cell : industry.reserved) blocked[o.index(cell)] = 1;
    if (proposed_depot) blocked[o.index(*proposed_depot)] = 1;
    navigation::ReachabilityCache routes;
    std::string error;
    if (!routes.update(o.width, o.height, blocked, error)) return {};

    std::vector<Cell> paths;
    for (const auto& ally : o.allies) {
        if (ally.hp && ally.kind == UnitKind::Collector && o.contains(ally.position)) {
            paths.push_back(ally.position);
        }
    }
    if (paths.empty()) {
        for (const auto hub : industry.hubs) {
            for (const auto delta : adjacent) {
                const auto cell = offset(hub, delta);
                if (o.contains(cell)) paths.push_back(cell);
            }
        }
    }
    const auto reachable = [&](Cell start, Cell destination) {
        return o.contains(start) && routes.connected(o.index(start), o.index(destination));
    };
    std::erase_if(deposits, [&](const Deposit& deposit) {
        if (std::none_of(paths.begin(), paths.end(), [&](Cell start) {
                return reachable(start, deposit.cell);
            })) return true;
        return proposed_depot && std::none_of(adjacent.begin(), adjacent.end(), [&](Cell delta) {
            return reachable(offset(*proposed_depot, delta), deposit.cell);
        });
    });
    return deposits;
}

std::optional<Cell> depot_site(const Industry& industry, const std::vector<Deposit>& deposits) {
    const auto home = industry.observation.home;
    auto nearest = std::min_element(deposits.begin(), deposits.end(), [&](const Deposit& a, const Deposit& b) {
        const auto rank = [&](const Deposit& value) {
            return std::tuple{industry.danger(value.cell), distance(value.cell, home), -value.resource};
        };
        return rank(a) < rank(b);
    });
    return industry.site(nearest != deposits.end() ? nearest->cell : home);
}

std::optional<Cell> expansion_site(const Industry& industry, const std::vector<Deposit>& deposits) {
    const unsigned cargo = std::max(1U, static_cast<unsigned>(industry.observation.rules.cargo_capacity));
    std::optional<Cell> result;
    Cell resource_target;
    int best_return = 0;
    for (const auto& deposit : deposits) {
        if (deposit.resource < static_cast<int>(2 * cargo) ||
            industry.danger(deposit.cell) > safe_expansion_threat) continue;
        const int old_route = distance(industry.depots.front(), deposit.cell);
        if (old_route < 6) continue;
        const auto candidate = industry.site(deposit.cell, safe_expansion_threat, 3);
        if (!candidate) continue;
        const int route = distance(*candidate, deposit.cell);
        if (old_route - route < 4) continue;
        const int savings = (old_route - route) * static_cast<int>(deposit.resource / cargo);
        if (savings > best_return) {
            result = candidate;
            resource_target = deposit.cell;
            best_return = savings;
        }
    }
    if (!result) return std::nullopt;
    const auto confirmed = accessible_deposits(industry, result);
    if (std::none_of(confirmed.begin(), confirmed.end(), [&](const Deposit& deposit) {
            return deposit.cell == resource_target;
        })) return std::nullopt;
    return result;
}

bool deploy(Industry& industry) {
    const auto& o = industry.observation;
    const auto pioneer = std::find_if(o.allies.begin(), o.allies.end(), [&](const Entity& unit) {
        const auto tile = known_tile(o, industry.memory, unit.position);
        return unit.side == o.side && unit.hp && unit.kind == UnitKind::Pioneer &&
               tile.known && tile.terrain == Terrain::Ground;
    });
    if (pioneer == o.allies.end()) return false;
    industry.plan.commands.push_back({.kind = CommandKind::Deploy, .side = o.side,
                                      .target = pioneer->position, .units = {pioneer->id}});
    industry.hubs.push_back(pioneer->position);
    industry.reserved.push_back(pioneer->position);
    industry.account(UnitKind::Hub);
    return true;
}

unsigned army_size(const Industry& industry) {
    unsigned size = 0;
    for (const auto kind : combat_kinds) size += industry.count(kind);
    return size;
}

UnitKind choose_recruit(const Industry& industry, bool any_combat_observed) {
    const auto frontline = industry.count(UnitKind::Skirmisher) + industry.count(UnitKind::Sentinel);
    if (frontline >= 2 && industry.count(UnitKind::Artillery) < std::max(1U, frontline / 4)) {
        return UnitKind::Artillery;
    }
    if (!any_combat_observed && army_size(industry) == 0) return UnitKind::Skirmisher;
    if (!any_combat_observed && army_size(industry) < 3) {
        if (!industry.count(UnitKind::Sentinel)) return UnitKind::Sentinel;
        if (!industry.count(UnitKind::Artillery)) return UnitKind::Artillery;
    }

    // Pay attention to the counter, but retain a screen for fragile long-range units.
    const auto& scores = industry.plan.scores;
    std::size_t strongest = 0;
    for (std::size_t i = 1; i < combat_kinds.size(); ++i) {
        if (scores[i] > scores[strongest]) strongest = i;
    }
    if (combat_kinds[strongest] == UnitKind::Artillery &&
        industry.count(UnitKind::Artillery) > industry.count(UnitKind::Sentinel) + 1) {
        return UnitKind::Sentinel;
    }
    return combat_kinds[strongest];
}

}  // namespace

IndustryPlan plan_industry(const Observation& observation, const AiMemory& memory, const ThreatGrid& threat) {
    Industry industry(observation, memory, threat);
    auto& plan = industry.plan;
    plan.scores = production_scores(observation, memory);
    const auto best = std::max_element(plan.scores.begin(), plan.scores.end());
    plan.preference = combat_kinds[std::distance(plan.scores.begin(), best)];
    if (observation.width <= 0 || observation.width > Map::max_dimension ||
        observation.height <= 0 || observation.height > Map::max_dimension ||
        !observation.contains(observation.home)) return std::move(plan);

    if (industry.hubs.empty()) deploy(industry);
    const auto needs_depot = !industry.count(UnitKind::Depot);
    const auto needs_workshop = !industry.count(UnitKind::Workshop);
    const auto& rules = observation.rules;

    const int opening_demand = industry.demand - (needs_depot ? rules.spec(UnitKind::Depot).power : 0)
                             - (needs_workshop ? rules.spec(UnitKind::Workshop).power : 0);
    if (industry.supply < opening_demand) {
        industry.build(UnitKind::Generator, industry.site(observation.home));
    }
    const bool expand = industry.depots.size() == 1 && observation.deliveries >= 4 &&
        army_size(industry) >= 2 && industry.danger(observation.home) <= safe_expansion_threat;
    std::vector<Deposit> deposits;
    if (needs_depot || expand) deposits = accessible_deposits(industry);
    if (needs_depot) industry.build(UnitKind::Depot, depot_site(industry, deposits));
    if (needs_workshop) industry.build(UnitKind::Workshop, industry.site(observation.home));

    const auto reserve = rules.spec(UnitKind::Skirmisher).cost;
    const auto depot_cost = rules.spec(UnitKind::Depot).cost;
    std::uint64_t investment_reserve = 0;
    if (expand) {
        auto target = expansion_site(industry, deposits);
        const int new_demand = industry.demand - rules.spec(UnitKind::Depot).power;
        const bool needs_power = industry.supply < new_demand;
        const std::uint64_t total_cost = depot_cost + reserve + (needs_power ? rules.spec(UnitKind::Generator).cost : 0);
        if (target && industry.budget >= total_cost) {
            if (needs_power && industry.build(UnitKind::Generator, industry.site(observation.home))) {
                target = expansion_site(industry, deposits);
            }
            if (industry.supply >= new_demand && industry.build(UnitKind::Depot, target)) {
                plan.expansion = target;
            }
        } else if (target && army_size(industry) >= 3 && industry.budget >= depot_cost / 2U) {
            // Cash already close to a useful expansion can accumulate while
            // the current mining route and screen are still operating.
            investment_reserve = total_cost;
        }
    }

    if (!industry.count(UnitKind::Workshop)) return std::move(plan);
    auto queue_size = observation.queue.size();
    for (const auto& waiting : observation.queue) {
        if (waiting.kind < UnitKind::Count) ++industry.inventory[kind_index(waiting.kind)];
    }
    const unsigned desired_collectors = plan.expansion ? 2 : 1;
    if (industry.count(UnitKind::Collector) < desired_collectors && industry.count(UnitKind::Depot) &&
        queue_size < 2 && industry.queue(UnitKind::Collector)) {
        ++queue_size;
    }
    const auto armed = [&](const EnemyContact& contact) {
        return contact.hp && contact.kind < UnitKind::Count && rules.spec(contact.kind).damage > 0;
    };
    const bool saw_combat = std::any_of(observation.enemies.begin(), observation.enemies.end(), armed) ||
                            std::any_of(memory.enemies.begin(), memory.enemies.end(), [&](const auto& contact) {
                                return contact.seen_at <= observation.step &&
                                       observation.step - contact.seen_at < AiMemory::contact_lifetime &&
                                       armed(contact);
                            });
    for (; queue_size < 2; ++queue_size) {
        const auto recruit = choose_recruit(industry, saw_combat);
        if (industry.budget < rules.spec(recruit).cost + investment_reserve || !industry.queue(recruit)) break;
    }
    return std::move(plan);
}

}  // namespace coastal::rts::ai
