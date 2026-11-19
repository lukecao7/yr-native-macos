// SPDX-License-Identifier: GPL-3.0-only
#include "world.h"

#include "combat.h"
#include "economy.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace coastal::rts {
namespace {

auto entity_position(const std::vector<Entity>& entities, EntityId id) noexcept {
    return std::lower_bound(entities.begin(), entities.end(), id,
                            [](const Entity& entity, EntityId key) { return entity.id < key; });
}

}  // namespace

const Entity* World::find(EntityId id) const noexcept {
    const auto found = entity_position(entities, id);
    return found == entities.end() || found->id != id ? nullptr : &*found;
}

Entity* World::find(EntityId id) noexcept {
    return const_cast<Entity*>(std::as_const(*this).find(id));
}

bool World::free_at(Cell cell) const noexcept {
    return map.walkable(cell) &&
           std::none_of(entities.begin(), entities.end(), [cell](const Entity& entity) {
               return entity.position == cell && entity.hp > 0;
           });
}

std::optional<EntityId> World::spawn(UnitKind kind, Side team, Cell position) {
    if (kind >= UnitKind::Count || side_index(team) >= sides.size() || !rules.spec(kind).hp ||
        entities.size() >= entity_limit || next_id == std::numeric_limits<EntityId>::max() ||
        !free_at(position)) return std::nullopt;
    const EntityId id = next_id;
    Entity value{};
    value.id = id;
    value.kind = kind;
    value.side = team;
    value.position = position;
    value.hp = rules.spec(kind).hp;
    if (team == Side::Copper) value.facing = {-1, 0};
    entities.push_back(value);
    ++next_id;
    return id;
}

bool World::apply(const Command& command) {
    if (result != MatchResult::Running || side_index(command.side) >= sides.size()) return false;
    switch (command.kind) {
    case CommandKind::Select:
    case CommandKind::AssignGroup:
    case CommandKind::RecallGroup:
    case CommandKind::Move:
    case CommandKind::AttackMove:
    case CommandKind::Hold:
        return apply_combat(*this, command);
    case CommandKind::Deploy:
    case CommandKind::Build:
    case CommandKind::Queue:
    case CommandKind::TogglePause:
    case CommandKind::Cancel:
    case CommandKind::Rally:
        return apply_economy(*this, command);
    }
    return false;
}

void World::tick(Navigator& navigator) {
    if (result == MatchResult::Running) {
        tick_economy(*this);
        tick_combat(*this, navigator);
        std::array<bool, 2> alive{};
        for (const auto& entity : entities) alive[side_index(entity.side)] |= entity.hp > 0;
        if (!alive[0] || !alive[1]) {
            result = alive[0] ? MatchResult::BlueWon : alive[1] ? MatchResult::CopperWon : MatchResult::Draw;
        }
    }
    ++step;
}

std::uint32_t World::random() {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return static_cast<std::uint32_t>((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

}  // namespace coastal::rts
