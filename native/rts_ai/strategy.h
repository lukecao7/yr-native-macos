// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "rts_core/world.h"

#include <iosfwd>
#include <span>

namespace coastal::rts::ai {

// A planner receives only its own state, currently visible enemies and tiles,
// and the map dimensions. Unknown terrain is represented by known == false.
struct Observation {
    Side side = Side::Blue;
    std::uint64_t step = 0;
    int width = 0;
    int height = 0;
    Cell home;
    Rules rules;
    std::uint64_t funds = 0;
    std::uint16_t power = 100;
    std::uint32_t deliveries = 0;
    std::vector<Production> queue;
    std::vector<Entity> allies;
    std::vector<EnemyContact> enemies;
    std::vector<SeenTile> visible;

    bool contains(Cell cell) const noexcept {
        return cell.x >= 0 && cell.x < width && cell.y >= 0 && cell.y < height;
    }
    int index(Cell cell) const noexcept { return cell.y * width + cell.x; }
};

Observation observe(const World& world, Side side);
void remember(const Observation& observation, AiMemory& memory);

struct ThreatGrid {
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> cells;
    int at(Cell cell) const noexcept {
        if (cell.x < 0 || cell.x >= width || cell.y < 0 || cell.y >= height) return 0;
        return cells[cell.y * width + cell.x];
    }
};

ThreatGrid estimate_threat(const Observation& observation, const AiMemory& memory);
// If scouting is useful, returns one move command for a single ally.
std::optional<Command> scout(const Observation& observation, AiMemory& memory, const ThreatGrid& threat);

inline constexpr std::array combat_kinds{UnitKind::Skirmisher, UnitKind::Sentinel, UnitKind::Artillery};

struct IndustryPlan {
    std::array<int, 3> scores{};
    UnitKind preference = UnitKind::Count;
    std::optional<Cell> expansion;
    std::vector<Command> commands;
};

IndustryPlan plan_industry(const Observation& observation, const AiMemory& memory, const ThreatGrid& threat);
std::vector<Command> plan_tactics(const Observation& observation, const AiMemory& memory,
                                  const ThreatGrid& threat, EntityId scout_id = no_entity);

void drive_opponents(World& world, Side player, bool adaptive, std::ostream* audit = nullptr);
void drive_adaptive(World& world, Side side, std::ostream* audit = nullptr);
void drive_scripted(World& world, Side side, UnitKind doctrine = UnitKind::Skirmisher);

}  // namespace coastal::rts::ai
