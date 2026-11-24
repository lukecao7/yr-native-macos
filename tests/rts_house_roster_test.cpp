// SPDX-License-Identifier: GPL-3.0-only
#include "rts_ai/strategy.h"
#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/match.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace coastal::rts;

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

World opening(const Rules& rules, std::size_t count) {
    World world;
    world.rules = rules;
    world.map.width = 120;
    world.map.height = 12;
    world.map.tiles.resize(world.map.width * world.map.height);
    for (std::size_t i = 0; i < count; ++i) world.map.starts[i] = {4 + 15 * int(i), 5};
    for (std::size_t i = 0; i < count; ++i) {
        const auto side = static_cast<Side>(i);
        const auto start = world.map.starts[i];
        auto& state = world.side(side);
        state.funds = rules.initial_funds + i * 100;
        state.ai.emplace().survey_index = static_cast<std::uint32_t>(i + 1);
        const auto hub = world.spawn(UnitKind::Hub, side, start);
        const auto unit = world.spawn(UnitKind::Skirmisher, side, {start.x + 1, start.y});
        const auto shop = world.spawn(UnitKind::Workshop, side, {start.x, start.y + 1});
        require(hub && unit && shop, "Every participating faction fields its own opening.");
        state.selected = {*unit};
        state.groups[2] = {*unit};
        Command rally{};
        rally.kind = CommandKind::Rally;
        rally.side = side;
        rally.target = {start.x + 4, start.y + 2};
        require(world.apply(rally), "Every house owns an independent rally point.");
    }
    return world;
}

void check_open_match(const std::filesystem::path& directory) {
    std::string error;
    auto world = open_match(directory, "EightHarbors.map", error);
    require(world && world->house_count() == 8, "Open all eight starts through the normal map reader.");
    const auto deployed = [](const World& world, Side side) {
        return std::ranges::any_of(world.entities, [=](const Entity& e) { return e.side == side && e.kind == UnitKind::Hub; });
    };
    auto adaptive = *world;
    ai::drive_opponents(*world, Side::Blue, false);
    ai::drive_opponents(adaptive, Side::Blue, true);
    for (std::size_t i = 0; i < world->house_count(); ++i) {
        const auto side = static_cast<Side>(i);
        require(deployed(*world, side) == (side != Side::Blue), "The host drives every active computer house.");
        require(adaptive.side(side).ai.has_value() == (side != Side::Blue), "Each adaptive opponent keeps its own memory.");
    }
}

void save_round_trip(const World& world) {
    const auto saved = encode_snapshot(world);
    require(saved[4] == 5, "A multi-house game selects snapshot format 5.");
    std::string error;
    const auto restored = decode_snapshot(saved, error);
    require(restored && error.empty() && restored->house_count() == world.house_count() &&
            restored->defeated_houses == world.defeated_houses && encode_snapshot(*restored) == saved,
            "House slots, production queues, funds, groups, AI memory, vision sources and result round-trip.");
}

std::vector<std::uint8_t> play(const Rules& rules, std::size_t count, unsigned lanes) {
    auto world = opening(rules, count);
    const auto last = static_cast<Side>(count - 1);
    const auto neighbor = static_cast<Side>(count - 2);
    require(world.active_sides().size() == count && world.house_count() == count, "The start roster is compact.");
    require(world.house_defeated(static_cast<Side>(count)) && world.house_defeated(static_cast<Side>(255)),
            "Inactive or invalid owners cannot issue world commands.");
    const auto last_funds = world.side(last).funds;
    const auto neighbor_funds = world.side(neighbor).funds;
    Command train{};
    train.kind = CommandKind::Queue;
    train.side = last;
    train.unit = UnitKind::Skirmisher;
    require(world.apply(train) && world.side(last).funds + rules.spec(train.unit).cost == last_funds &&
            world.side(neighbor).funds == neighbor_funds && world.side(neighbor).queue.empty(),
            "The third or eighth house pays for its queue without using another house's resources.");
    SpatialIndex spatial;
    spatial.rebuild(world);
    const auto& unit = *world.find(world.side(last).selected.front());
    const auto* hostile = spatial.nearest_enemy(world, unit, 30);
    require(hostile && hostile->side == neighbor, "The closest independently owned opponent is targetable.");

    auto scripted = world;
    scripted.step = 720;
    const auto unit_id = scripted.side(last).selected.front();
    ai::drive_scripted(scripted, last, UnitKind::Skirmisher);
    require(scripted.find(unit_id)->destination == scripted.map.starts[count - 2],
            "A scripted AI in slot three or eight attacks its closest surviving opponent.");
    scripted.mark_house_defeated(neighbor);
    ai::drive_scripted(scripted, last, UnitKind::Skirmisher);
    require(scripted.find(unit_id)->destination == scripted.map.starts[count - 3],
            "The scripted AI switches targets when its nearest rival is eliminated.");
    ai::drive_scripted(scripted, static_cast<Side>(255), UnitKind::Skirmisher);
    ai::drive_adaptive(scripted, static_cast<Side>(255));
    require(ai::observe(scripted, neighbor).visible.empty(), "Eliminated observers receive no view.");

    Navigator routes(lanes);
    for (int tick = 0; tick != 12; ++tick) world.tick(routes);
    require(world.result == MatchResult::Running && world.defeated_houses == 0, "The opening preserves every house.");
    for (std::size_t i = 0; i < count; ++i) {
        const auto vision = routes.visibility(static_cast<Side>(i));
        require(vision.size() == world.map.tiles.size(), "Each active house has a complete visibility mask.");
        for (std::size_t j = 0; j < count; ++j)
            require(bool(vision[world.map.index(world.map.starts[j])]) == (i == j), "Sight belongs to its source house.");
    }
    require(routes.visibility(static_cast<Side>(count)).empty(), "Closed houses obtain no visibility mask.");
    save_round_trip(world);

    world.mark_house_defeated(neighbor);
    world.tick(routes);
    require(world.result == MatchResult::Running &&
            std::ranges::none_of(routes.visibility(neighbor), [](auto seen) { return seen != 0; }) &&
            !world.spawn(UnitKind::Pioneer, neighbor, {0, 0}), "One elimination leaves the others playing.");
    train.side = neighbor;
    require(!world.apply(train), "An eliminated house may not spend or produce.");
    spatial.rebuild(world);
    hostile = spatial.nearest_enemy(world, *world.find(world.side(last).selected.front()), 60);
    require(hostile && hostile->side != neighbor && hostile->side != last, "Combat acquires another surviving enemy.");
    save_round_trip(world);

    for (std::size_t i = 0; i + 1 < count; ++i) world.mark_house_defeated(static_cast<Side>(i));
    world.tick(routes);
    require(winning_house(world.result) == last && world.result == victory_for(last) &&
            !world.house_defeated(last) && !valid_match_result(world.result, count - 1),
            "The actual surviving house wins, including slots three and eight.");
    save_round_trip(world);
    return encode_snapshot(world);
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string error;
        const std::filesystem::path rules_path = argc > 1 ? argv[1] : "tests/fixtures/coastal_rules.tsv";
        check_open_match(rules_path.parent_path());
        const auto rules = load_rules(rules_path, error);
        if (!rules) throw std::runtime_error(error);
        for (auto count : {3U, 8U}) {
            const auto reference = play(*rules, count, 1);
            require(reference == play(*rules, count, 4) && reference == play(*rules, count, 8),
                    "House results, queues, authority and snapshots match at 1, 4 and 8 threads.");
            std::cout << count << " houses: economy/vision/targeting/scripted-AI/elimination/winner/save; 1/4/8 workers identical\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "rts_house_roster_test: " << error.what() << '\n';
        return 1;
    }
}
