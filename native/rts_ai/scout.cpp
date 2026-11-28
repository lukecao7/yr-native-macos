// SPDX-License-Identifier: GPL-3.0-only
#include "strategy.h"

#include "navigation/flow_repair.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <tuple>

namespace coastal::rts::ai {
namespace {

int separation(Cell a, Cell b) noexcept {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool passable(const SeenTile& tile) noexcept {
    return tile.known && tile.terrain != Terrain::Water && tile.terrain != Terrain::Rock;
}

bool current_contact(const Observation& observation, const EnemyContact& contact) noexcept {
    return observation.contains(contact.position) &&
           contact.seen_at <= observation.step &&
           observation.step - contact.seen_at < AiMemory::contact_lifetime;
}

Cell survey_waypoint(const Observation& observation, AiMemory& memory) {
    // These are board sectors inferred from friendly spawn and public dimensions.
    // Start toward the middle of the far side, then sweep the flanks.
    const int width = observation.width;
    const int height = observation.height;
    const Cell home = observation.home;
    const bool across_x = std::abs(2 * home.x + 1 - width) * height >=
                          std::abs(2 * home.y + 1 - height) * width;
    constexpr int sector_scale = 16;
    constexpr std::array sectors{
        Cell{12, 8}, Cell{12, 4}, Cell{12, 12},
        Cell{15, 8}, Cell{15, 4}, Cell{15, 12},
        Cell{8, 4}, Cell{8, 12}, Cell{4, 4}, Cell{4, 12}
    };
    const auto position = [&](std::size_t index) {
        const Cell sector = sectors[index];
        if (across_x) {
            const int column = home.x < width / 2 ? sector.x : sector_scale - sector.x;
            return Cell{(width - 1) * column / sector_scale, (height - 1) * sector.y / sector_scale};
        }
        const int row = home.y < height / 2 ? sector.x : sector_scale - sector.x;
        return Cell{(width - 1) * sector.y / sector_scale, (height - 1) * row / sector_scale};
    };
    const auto start = memory.survey_index % sectors.size();
    for (std::size_t offset = 0; offset < sectors.size(); ++offset) {
        const auto index = (start + offset) % sectors.size();
        const auto anchor = position(index);
        if (!memory.chart[observation.index(anchor)].known) {
            memory.survey_index = static_cast<std::uint32_t>(index);
            return anchor;
        }
    }
    return position(start);
}

// An integral image lets the route search count newly visible tiles in
// constant time at each candidate destination.
class UnseenTiles {
public:
    UnseenTiles(int width, int height, std::span<const SeenTile> chart)
        : width_(width), height_(height),
          sums_(std::size_t(width + 1) * (height + 1)) {
        for (int y = 0; y < height; ++y) {
            int row = 0;
            for (int x = 0; x < width; ++x) {
                row += !chart[y * width + x].known;
                sums_[index(x + 1, y + 1)] = row + sums_[index(x + 1, y)];
            }
        }
    }

    int within(Cell position, int radius) const noexcept {
        const int left = std::max(0, position.x - radius);
        const int top = std::max(0, position.y - radius);
        const int right = std::min(width_, position.x + radius + 1);
        const int bottom = std::min(height_, position.y + radius + 1);
        return sums_[index(right, bottom)] - sums_[index(left, bottom)] -
               sums_[index(right, top)] + sums_[index(left, top)];
    }

private:
    int index(int x, int y) const noexcept { return y * (width_ + 1) + x; }
    int width_;
    int height_;
    std::vector<int> sums_;
};

const Entity* choose_scout(const Observation& observation, Cell previous_target) {
    const Entity* best = nullptr;
    for (const auto& unit : observation.allies) {
        const auto& spec = observation.rules.spec(unit.kind);
        if (unit.hp * 2 < spec.hp || !spec.damage || !spec.move_period || !spec.vision) continue;
        if (unit.order == OrderKind::Move && unit.destination == previous_target &&
            observation.contains(previous_target)) return &unit;
        if (!best || std::tie(spec.move_period, unit.id) <
                     std::tie(observation.rules.spec(best->kind).move_period, best->id)) {
            best = &unit;
        }
    }
    return best;
}

std::optional<Cell> route_to_frontier(const Observation& observation, const AiMemory& memory,
                                      const ThreatGrid& threat, const Entity& unit, Cell anchor) {
    std::vector<unsigned char> blocked(memory.chart.size());
    std::transform(memory.chart.begin(), memory.chart.end(), blocked.begin(),
                   [](const SeenTile& tile) { return !passable(tile); });
    std::vector<bool> occupied(memory.chart.size());
    for (const auto& ally : observation.allies) {
        const int index = observation.index(ally.position);
        occupied[index] = true;
        if (is_building(ally.kind)) blocked[index] = true;
    }
    for (const auto& contact : memory.enemies) {
        if (is_building(contact.kind) && current_contact(observation, contact)) {
            blocked[observation.index(contact.position)] = true;
        }
    }

    const auto& spec = observation.rules.spec(unit.kind);
    const int diameter = 2 * spec.vision + 1;
    const UnseenTiles unseen(observation.width, observation.height, memory.chart);
    const int start = observation.index(unit.position);
    std::vector<int> dist(memory.chart.size(), -1);
    std::vector<int> frontier{start};
    dist[start] = 0;
    std::optional<Cell> best;
    int best_score = std::numeric_limits<int>::min();
    for (std::size_t head = 0; head < frontier.size(); ++head) {
        const int index = frontier[head];
        const Cell cell{index % observation.width, index / observation.width};
        if (!occupied[index]) {
            const int reveal = unseen.within(cell, spec.vision);
            if (reveal > 0) {
                const int score = 32 * reveal / (diameter * diameter) -
                                  6 * separation(cell, anchor) - dist[index] - 3 * threat.at(cell);
                if (score > best_score) {
                    best_score = score;
                    best = cell;
                }
            }
        }
        navigation::detail::for_flow_neighbour(observation.width, observation.height, blocked, index,
            [&](int next, std::int32_t) {
                if (dist[next] < 0) {
                    dist[next] = dist[index] + 1;
                    frontier.push_back(next);
                }
            });
    }
    return best;
}

}  // namespace

Observation observe(const World& world, Side side) {
    Observation observation;
    observation.side = side;
    observation.step = world.step;
    const auto& map = world.map;
    if (world.house_defeated(side) || map.width <= 0 || map.height <= 0 ||
        map.width > Map::max_dimension || map.height > Map::max_dimension ||
        map.tiles.size() != std::size_t(map.width) * map.height) return observation;

    observation.width = map.width;
    observation.height = map.height;
    observation.home = map.starts[side_index(side)];
    observation.rules = world.rules;
    const auto& own = world.side(side);
    observation.funds = own.funds;
    observation.power = own.power_percent;
    observation.deliveries = own.deliveries;
    observation.queue = own.queue;
    observation.visible.resize(map.tiles.size());

    for (const auto& unit : world.entities) {
        if (unit.side != side || !unit.hp || !map.contains(unit.position)) continue;
        observation.allies.push_back(unit);
        const int radius = world.rules.spec(unit.kind).vision;
        const int left = std::max(0, unit.position.x - radius);
        const int right = std::min(map.width - 1, unit.position.x + radius);
        const int top = std::max(0, unit.position.y - radius);
        const int bottom = std::min(map.height - 1, unit.position.y + radius);
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                const int index = y * map.width + x;
                const Tile& tile = map.tiles[index];
                const auto terrain = tile.effective_terrain();
                const auto resource = terrain == Terrain::Deposit ? tile.resource : std::uint16_t{0};
                observation.visible[index] = {true, terrain, resource};
            }
        }
    }
    for (const auto& unit : world.entities) {
        if (unit.side != side && unit.hp && !world.house_defeated(unit.side) && map.contains(unit.position) &&
            observation.visible[map.index(unit.position)].known) {
            observation.enemies.push_back({unit.id, unit.kind, unit.position, unit.hp, world.step});
        }
    }
    return observation;
}

void remember(const Observation& observation, AiMemory& memory) {
    if (memory.chart.size() != observation.visible.size()) {
        memory.chart.assign(observation.visible.size(), SeenTile{});
        memory.enemies.clear();
        memory.scout_target = {};
        memory.survey_index = 0;
    }
    for (std::size_t i = 0; i < observation.visible.size(); ++i) {
        if (observation.visible[i].known) memory.chart[i] = observation.visible[i];
    }

    // A revisited position can disprove a stale contact. Current observations
    // replace all contacts confirmed within sight regardless of their old cell.
    std::erase_if(memory.enemies, [&](const EnemyContact& contact) {
        return !current_contact(observation, contact) ||
               observation.visible[observation.index(contact.position)].known;
    });
    for (const auto& enemy : observation.enemies) {
        auto place = std::lower_bound(memory.enemies.begin(), memory.enemies.end(), enemy.id,
                                      [](const auto& contact, EntityId id) { return contact.id < id; });
        if (place != memory.enemies.end() && place->id == enemy.id) *place = enemy;
        else memory.enemies.insert(place, enemy);
    }
}

ThreatGrid estimate_threat(const Observation& observation, const AiMemory& memory) {
    ThreatGrid grid{observation.width, observation.height,
                    std::vector<std::uint16_t>(observation.visible.size())};
    for (const auto& enemy : memory.enemies) {
        if (!current_contact(observation, enemy)) continue;
        const auto& spec = observation.rules.spec(enemy.kind);
        const int radius = spec.range + 2;
        const int strength = spec.damage ? 28 + spec.damage + 4 * spec.range :
                             is_building(enemy.kind) ? 12 : 5;
        const int life = static_cast<int>(AiMemory::contact_lifetime);
        const int freshness = life - static_cast<int>(observation.step - enemy.seen_at);
        for (int y = std::max(0, enemy.position.y - radius);
             y <= std::min(grid.height - 1, enemy.position.y + radius); ++y) {
            for (int x = std::max(0, enemy.position.x - radius);
                 x <= std::min(grid.width - 1, enemy.position.x + radius); ++x) {
                const int weight = radius + 1 - separation(enemy.position, {x, y});
                auto& value = grid.cells[observation.index({x, y})];
                value = static_cast<std::uint16_t>(std::min<int>(
                    std::numeric_limits<std::uint16_t>::max(),
                    value + strength * weight * freshness / ((radius + 1) * life)));
            }
        }
    }
    return grid;
}

std::optional<Command> scout(const Observation& observation, AiMemory& memory, const ThreatGrid& threat) {
    if (!observation.enemies.empty() || !observation.contains(observation.home) ||
        memory.chart.size() != observation.visible.size()) {
        memory.scout_target = {};
        return std::nullopt;
    }
    const auto* unit = choose_scout(observation, memory.scout_target);
    if (!unit) {
        memory.scout_target = {};
        return std::nullopt;
    }
    const auto anchor = survey_waypoint(observation, memory);
    const auto goal = route_to_frontier(observation, memory, threat, *unit, anchor);
    memory.scout_target = goal.value_or(Cell{});
    if (!goal) return std::nullopt;
    Command result{};
    result.kind = CommandKind::Move;
    result.side = observation.side;
    result.target = *goal;
    result.units.push_back(unit->id);
    return result;
}

}  // namespace coastal::rts::ai
