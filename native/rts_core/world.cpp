// SPDX-License-Identifier: GPL-3.0-only
#include "world.h"

#include "combat.h"
#include "economy.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <utility>

namespace coastal::rts {
namespace {

constexpr auto empty_entity_index = std::numeric_limits<std::uint16_t>::max();
static_assert(World::entity_limit < empty_entity_index);

auto entity_position(const std::vector<Entity>& entities, EntityId id) noexcept {
    return std::lower_bound(entities.begin(), entities.end(), id,
                            [](const Entity& entity, EntityId key) { return entity.id < key; });
}

}  // namespace

const Entity* World::find(EntityId id) const noexcept {
    if (entities.empty()) return nullptr;
    const auto& slots = entity_index_.slots;
    if (!slots.empty()) {
        const auto index = slots[id & (slots.size() - 1)];
        if (index < entities.size() && entities[index].id == id) return &entities[index];
    }
    if (id >= entities.front().id) {
        const auto index = std::size_t(id - entities.front().id);
        if (index < entities.size() && entities[index].id == id) return &entities[index];
    }
    const auto found = entity_position(entities, id);
    return found == entities.end() || found->id != id ? nullptr : &*found;
}

Entity* World::find(EntityId id) noexcept {
    return const_cast<Entity*>(std::as_const(*this).find(id));
}

void World::refresh_entity_index() noexcept {
    const auto size = entities.size();
    auto& slots = entity_index_.slots;
    if (!size || size > entity_limit || entities.back().id - entities.front().id == size - 1) {
        slots.clear();
        return; // The contiguous-ID lookup already covers this roster.
    }
    // Size by population, up to 4*entity_limit slots, regardless of how far
    // apart IDs are. Old slots may remain since a lookup verifies the ID.
    const auto target_size = std::bit_ceil(std::max<std::size_t>(16, size * 4));
    if (slots.size() < target_size) {
        try {
            slots.resize(target_size, empty_entity_index);
        } catch (const std::bad_alloc&) {
            return; // The sorted lookup still works.
        }
    }
    const auto mask = slots.size() - 1;
    for (std::size_t i = 0; i < size; ++i)
        slots[entities[i].id & mask] = static_cast<std::uint16_t>(i);
}

void World::index_appended_entity() noexcept {
    auto& slots = entity_index_.slots;
    if (slots.size() < entities.size() * 2) {
        refresh_entity_index();
        return;
    }
    const auto index = entities.size() - 1;
    slots[entities[index].id & (slots.size() - 1)] = static_cast<std::uint16_t>(index);
}

SubcellOccupancy World::cell_occupancy(Cell cell, EntityId ignore) const noexcept {
    SubcellOccupancy occupied;
    for (const auto& body : entities)
        if (body.hp && body.id != ignore && body.position == cell) occupied.add(body.subcell, body.side);
    return occupied;
}

bool World::free_at(Cell cell, std::uint8_t preferred, Side team) const noexcept {
    if (!map.walkable(cell) || side_index(team) >= house_count()) return false;
    if (preferred == whole_cell) return std::none_of(entities.begin(), entities.end(), [cell](const Entity& body) {
        return body.hp && body.position == cell;
    });
    return preferred < subcell_offsets.size() && cell_occupancy(cell).reserve(preferred, team).has_value();
}

std::optional<EntityId> World::spawn(UnitKind kind, Side team, Cell position, std::uint8_t preferred) {
    if (kind >= UnitKind::Count || house_defeated(team) || !rules.spec(kind).hp ||
        entities.size() >= entity_limit || next_id == std::numeric_limits<EntityId>::max() ||
        !map.walkable(position) || (preferred != whole_cell && preferred >= subcell_offsets.size())) return std::nullopt;
    const auto slot = cell_occupancy(position).reserve(preferred, team);
    if (!slot) return std::nullopt;
    const EntityId id = next_id;
    Entity value{};
    value.id = id;
    value.kind = kind;
    value.side = team;
    value.position = position;
    value.subcell = value.previous_subcell = *slot;
    value.hp = rules.spec(kind).hp;
    if (team == Side::Copper) value.facing = {-1, 0};
    entities.push_back(value);
    ++next_id;
    index_appended_entity();
    return id;
}

bool World::apply(const Command& command) {
    if (result != MatchResult::Running || house_defeated(command.side)) return false;
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
        std::uint8_t alive = 0;
        for (const auto& entity : entities) if (entity.hp)
            alive |= static_cast<std::uint8_t>(1U << side_index(entity.side));
        alive &= static_cast<std::uint8_t>(~defeated_houses);
        const auto count = house_count();
        if (count > 2) defeated_houses = static_cast<std::uint8_t>(((1U << count) - 1U) & ~alive);
        if (!alive) result = MatchResult::Draw;
        else if (std::has_single_bit(alive)) result = victory_for(static_cast<Side>(std::countr_zero(alive)));
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
