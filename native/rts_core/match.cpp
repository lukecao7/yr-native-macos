// SPDX-License-Identifier: GPL-3.0-only
#include "match.h"

#include "economy.h"
#include "iso.h"

#include <utility>

namespace coastal::rts {

std::optional<World> open_match(const std::filesystem::path& directory, std::string_view map_name,
                                std::string& error) {
    const auto name = std::filesystem::path(map_name);
    if (name.filename() != name || name.extension() != ".map") {
        error = "Choose a self-authored .map file with --skirmish-map.";
        return std::nullopt;
    }
    auto map = load_map(directory / name, error);
    if (!map) return std::nullopt;
    auto rules = load_rules(directory / "coastal_rules.tsv", error);
    if (!rules) return std::nullopt;

    World world{};
    world.map = std::move(*map);
    world.rules = *rules;
    world.rng_state = 0x295F732D18C2CB5DULL;
    for (const Side team : {Side::Blue, Side::Copper}) {
        world.side(team).funds = rules->initial_funds;
        if (!world.spawn(UnitKind::Pioneer, team, world.map.starts[side_index(team)])) {
            error = "Invalid starting position in selected map.";
            return std::nullopt;
        }
    }
    constexpr std::array offsets{Cell{1, 0}, Cell{0, 1}, Cell{-1, 0}, Cell{0, -1}};
    for (const Side team : {Side::Blue, Side::Copper}) {
        const auto origin = world.map.starts[side_index(team)];
        bool spawned = false;
        for (const auto offset : offsets) {
            if (world.spawn(UnitKind::Collector, team, {origin.x + offset.x, origin.y + offset.y})) {
                spawned = true;
                break;
            }
        }
        if (!spawned) {
            error = "Each starting position needs an adjacent free cell for a Collector.";
            return std::nullopt;
        }
    }
    return world;
}

}  // namespace coastal::rts
