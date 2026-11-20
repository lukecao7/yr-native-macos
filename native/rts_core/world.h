// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace coastal::rts {

struct Cell {
    int x = -1;
    int y = -1;
    friend bool operator==(Cell, Cell) = default;
};

enum class Terrain : std::uint8_t { Ground, Ramp, Water, Rock, Deposit };
struct Tile {
    Terrain terrain = Terrain::Ground;
    std::uint8_t height = 0;
    std::uint16_t resource = 0;
};

struct Map {
    static constexpr int max_dimension = 512;
    int width = 0;
    int height = 0;
    std::vector<Tile> tiles;
    std::array<Cell, 2> starts;
    // Optional imported adjacency: NW,N,NE,W,E,SW,S,SE. Empty permits
    // ordinary ground adjacency; both endpoint bits must admit an edge.
    std::vector<std::uint8_t> travel_edges;

    bool contains(Cell cell) const noexcept {
        return cell.x >= 0 && cell.x < width && cell.y >= 0 && cell.y < height;
    }
    int index(Cell cell) const noexcept { return cell.y * width + cell.x; }
    Cell cell(int index) const noexcept { return {index % width, index / width}; }
    const Tile& at(Cell cell) const noexcept { return tiles[index(cell)]; }
    Tile& at(Cell cell) noexcept { return tiles[index(cell)]; }
    bool walkable(Cell cell) const noexcept {
        return contains(cell) && at(cell).terrain != Terrain::Water && at(cell).terrain != Terrain::Rock;
    }
    bool can_travel(Cell from, Cell to) const noexcept {
        if (!walkable(from) || !walkable(to)) return false;
        const int dx = to.x - from.x, dy = to.y - from.y;
        if ((!dx && !dy) || dx < -1 || dx > 1 || dy < -1 || dy > 1) return false;
        if (!travel_edges.empty() && travel_edges.size() != tiles.size()) return false;
        const auto open = [&](Cell a, Cell b) {
            if (travel_edges.empty()) return true;
            const int raw = (b.y-a.y+1)*3 + b.x-a.x+1;
            const int bit = raw < 4 ? raw : raw-1;
            return (travel_edges[index(a)] & (1U << bit)) &&
                   (travel_edges[index(b)] & (1U << (7-bit)));
        };
        if (!open(from, to)) return false;
        if (dx && dy) {
            const Cell x{from.x+dx,from.y}, y{from.x,from.y+dy};
            return walkable(x) && walkable(y) && open(from,x) && open(from,y) && open(to,x) && open(to,y);
        }
        return true;
    }
};

enum class Side : std::uint8_t { Blue, Copper };
constexpr std::size_t side_index(Side side) noexcept { return static_cast<std::size_t>(side); }
using EntityId = std::uint32_t;
constexpr EntityId no_entity = 0;

enum class UnitKind : std::uint8_t {
    Pioneer, Hub, Generator, Depot, Workshop, Collector, Skirmisher, Sentinel, Artillery, Count
};

constexpr bool is_building(UnitKind kind) noexcept {
    return kind >= UnitKind::Hub && kind <= UnitKind::Workshop;
}

constexpr bool is_producible(UnitKind kind) noexcept {
    return kind >= UnitKind::Collector && kind < UnitKind::Count;
}

struct UnitSpec {
    std::uint16_t hp = 0;
    std::uint16_t cost = 0;
    std::uint16_t train_ticks = 0;
    std::uint8_t move_period = 0; // Zero denotes an immobile structure.
    std::uint8_t vision = 0;
    std::uint8_t range = 0;
    std::uint8_t damage = 0;
    std::uint8_t armor = 0;
    std::uint8_t shot_period = 0;
    std::int16_t power = 0; // A negative value is consumption.
};

struct Rules {
    std::array<UnitSpec, static_cast<std::size_t>(UnitKind::Count)> units{};
    std::uint32_t initial_funds = 0;
    std::uint16_t cargo_capacity = 0;
    std::uint16_t harvest_ticks = 0;

    const UnitSpec& spec(UnitKind kind) const noexcept { return units[static_cast<std::size_t>(kind)]; }
    UnitSpec& spec(UnitKind kind) noexcept { return units[static_cast<std::size_t>(kind)]; }
};

enum class OrderKind : std::uint8_t { Idle, Move, AttackMove, Hold, Gather };
enum class GatherPhase : std::uint8_t { Seek, Harvest, Return };

struct Entity {
    EntityId id;
    UnitKind kind;
    Side side;
    Cell position;
    std::uint16_t hp;
    OrderKind order = OrderKind::Idle;
    Cell destination;
    Cell facing{1, 0};
    std::uint8_t move_wait = 0;
    std::uint8_t shot_wait = 0;
    GatherPhase gather = GatherPhase::Seek;
    Cell deposit;
    EntityId depot = no_entity;
    std::uint16_t cargo = 0;
    std::uint16_t work = 0;
    EntityId focus = no_entity;
};

struct Projectile {
    EntityId shooter;
    EntityId target;
    Cell from;
    Cell to;
    std::uint8_t remaining;
    std::uint8_t travel_ticks;
    std::uint16_t damage;
};

struct Production {
    UnitKind kind;
    std::uint16_t paid;
    std::uint32_t remaining_work; // Hundredths of a fully powered simulation step.
};

struct SeenTile {
    bool known = false;
    Terrain terrain = Terrain::Ground;
    std::uint16_t resource = 0;
};

struct EnemyContact {
    EntityId id = no_entity;
    UnitKind kind = UnitKind::Pioneer;
    Cell position;
    std::uint16_t hp = 0;
    std::uint64_t seen_at = 0;
};

struct AiMemory {
    static constexpr std::uint64_t contact_lifetime = 480;
    std::vector<SeenTile> chart;
    std::vector<EnemyContact> enemies; // Increasing entity IDs.
    Cell scout_target;
    Cell expansion;
    std::uint32_t survey_index = 0;
    UnitKind preference = UnitKind::Count;
};

struct SideState {
    static constexpr std::size_t queue_limit = 64;
    std::uint64_t funds = 0;
    std::uint64_t income = 0;
    std::uint32_t deliveries = 0;
    std::uint16_t power_percent = 100;
    bool production_paused = false;
    Cell rally;
    std::vector<Production> queue;
    std::vector<EntityId> selected;
    std::array<std::vector<EntityId>, 9> groups;
    std::optional<AiMemory> ai;
};

enum class MatchResult : std::uint8_t { Running, BlueWon, CopperWon, Draw };
enum class CommandKind : std::uint8_t {
    Select, AssignGroup, RecallGroup, Move, AttackMove, Hold,
    Deploy, Build, Queue, TogglePause, Cancel, Rally
};

struct Command {
    CommandKind kind;
    Side side;
    Cell target;
    UnitKind unit = UnitKind::Pioneer;
    std::uint32_t index = 0; // Group (zero-based) or queue entry.
    std::vector<EntityId> units;
    bool add = false;
    EntityId focus = no_entity; // Optional enemy focus for an attack-move command.
};

class Navigator;

struct World {
    static constexpr std::size_t entity_limit = 32'768;
    static constexpr std::size_t projectile_limit = 262'144;
    Map map;
    Rules rules;
    std::array<SideState, 2> sides;
    std::vector<Entity> entities; // Stable increasing IDs.
    std::vector<Projectile> projectiles;
    std::uint64_t step = 0;
    std::uint64_t rng_state = 1;
    EntityId next_id = 1;
    MatchResult result = MatchResult::Running;

    SideState& side(Side value) noexcept { return sides[side_index(value)]; }
    const SideState& side(Side value) const noexcept { return sides[side_index(value)]; }
    Entity* find(EntityId id) noexcept;
    const Entity* find(EntityId id) const noexcept;
    bool free_at(Cell cell) const noexcept;
    std::optional<EntityId> spawn(UnitKind kind, Side side, Cell position);
    bool apply(const Command& command);
    void tick(Navigator& navigator);
    std::uint32_t random();
};

}  // namespace coastal::rts
