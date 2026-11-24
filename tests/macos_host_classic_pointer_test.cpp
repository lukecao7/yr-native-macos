// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/session.h"
#include "rts_core/iso.h"
#include "rts_core/match.h"
#include "rts_core/snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace coastal::macos_host;
using namespace coastal::rts;
namespace fs = std::filesystem;

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

using Phase = PointerEvent::Phase;

void pointer(Session& session, Phase phase, unsigned button, PixelPoint p, InputModifiers modifiers = {}) {
    session.pointer_event({phase, button, double(p.x), double(p.y), modifiers, 1});
}

void click(Session& session, unsigned button, PixelPoint p, InputModifiers modifiers = {}) {
    pointer(session, Phase::Down, button, p, modifiers);
    pointer(session, Phase::Up, button, p, modifiers);
}

void key(Session& session, std::uint16_t code, std::string_view letter = {}) {
    session.key_event({KeyEvent::Phase::Down, code, 0, false, letter, letter});
}

std::uint64_t file_identity(const fs::path& path) {
    struct stat record{};
    require(::stat(path.c_str(), &record) == 0, "The real save file is present.");
    return record.st_ino;
}

class Replay {
public:
    explicit Replay(const fs::path& directory) {
        options_.data_directory = directory;
        options_.skirmish_map = "Harbor.map";
        options_.threads = 1;
        save_ = directory / "CoastalHost.snapshot";
        fs::copy_file("tests/fixtures/coastal_rules.tsv", directory / "coastal_rules.tsv");
        std::ofstream map(directory / options_.skirmish_map);
        map << "COASTAL_MAP 1\n32 20 100\n";
        for (int y = 0; y < 20; ++y) {
            std::string row(32, '.');
            if (y == 6) row[7] = 'a';
            if (y == 15) row[26] = 'b';
            if (y == 12) row[8] = '#';
            map << row << '\n';
        }
        map.close();
        std::string error;
        auto loaded = open_match(directory, options_.skirmish_map, error);
        if (!loaded) throw std::runtime_error(error);
        start = std::move(*loaded);
        start.step = 101;
        camera_.center_on(start.map.starts[0], start.map);
    }

    PixelPoint point(Cell cell) const { return project_cell(start.map, camera_, cell, 1024, 768); }

    bool blue_body_at(Cell cell) const {
        const auto center = point(cell);
        for (int y = center.y - 8; y < center.y; ++y) {
            for (int x = center.x - 4; x <= center.x + 4; ++x) {
                const auto offset = (std::size_t(y) * 1024 + x) * 4;
                if (last_frame_[offset] == 96 && last_frame_[offset + 1] == 201 && last_frame_[offset + 2] == 240)
                    return true;
            }
        }
        return false;
    }

    void picture(std::string_view name) const {
        std::ofstream out(options_.data_directory / (std::string(name) + ".ppm"), std::ios::binary);
        out << "P6\n1024 768\n255\n";
        for (std::size_t i = 0; i < last_frame_.size(); i += 4)
            out.write(reinterpret_cast<const char*>(&last_frame_[i]), 3);
        require(out.good(), "Write the actual frame as a portable RGB image.");
    }

    World run(const World& initial, std::string_view name,
              const std::function<void(Session&)>& gestures, unsigned ticks = 0) {
        std::string error;
        if (!save_world(save_, initial, error)) throw std::runtime_error(error);
        const auto before = file_identity(save_);
        auto runtime = create_session(options_, error);
        if (!runtime) throw std::runtime_error(error);
        key(*runtime, 101); // F9 restores initial state through the shipping session.
        gestures(*runtime);
        if (!ticks) key(*runtime, 96); // Queued before the first game tick.
        unsigned delivered = 0;
        bool saved = !ticks;
        const auto deadline = FrameClock::now() + std::chrono::seconds(8);
        while (FrameClock::now() < deadline && (!delivered || file_identity(save_) == before)) {
            if (auto frame = runtime->advance_frame()) {
                require(!frame->scene && frame->rgba.size() == 1024 * 768 * 4, "Receive a rendered 2D frame.");
                last_frame_.assign(frame->rgba.begin(), frame->rgba.end());
                ++delivered;
                if (!saved && delivered >= ticks) { key(*runtime, 96); saved = true; }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        require(delivered && file_identity(save_) != before, "F5 atomically wrote a fresh input checkpoint.");
        runtime.reset();
        auto restored = load_world(save_, error);
        if (!restored) throw std::runtime_error(error);
        fs::copy_file(save_, options_.data_directory / (std::string(name) + ".snapshot"));
        return std::move(*restored);
    }

    World start;
private:
    LaunchOptions options_;
    fs::path save_;
    Camera camera_;
    std::vector<std::uint8_t> last_frame_;
};

void check(Replay& replay) {
    auto& start = replay.start;
    const auto pioneer = start.entities[0].id;
    const auto collector = std::ranges::find_if(start.entities, [](const Entity& e) {
        return e.side == Side::Blue && e.kind == UnitKind::Collector;
    })->id;
    const Cell a{9, 8}, b{10, 10}, hostile{15, 8}, destination{11, 6}, rally{7, 9};
    const auto first = start.spawn(UnitKind::Skirmisher, Side::Blue, a).value();
    const auto second = start.spawn(UnitKind::Sentinel, Side::Blue, b).value();
    const auto enemy = start.spawn(UnitKind::Sentinel, Side::Copper, hostile).value();
    const auto shop = start.spawn(UnitKind::Workshop, Side::Blue, {5, 8}).value();
    const auto home = replay.point(start.find(pioneer)->position);
    const auto first_pixel = replay.point(a), second_pixel = replay.point(b);
    const auto enemy_pixel = replay.point(hostile), goal_pixel = replay.point(destination);
    const auto shift = static_cast<InputModifiers>(Modifier::Shift);
    const auto option = static_cast<InputModifiers>(Modifier::Option);

    auto selected = replay.run(start, "00-select", [&](Session& s) { click(s, 0, first_pixel); });
    require(selected.side(Side::Blue).selected == std::vector<EntityId>{first} &&
            replay.blue_body_at(a) && !replay.blue_body_at(destination), "The selected actor is drawn at its starting cell.");
    replay.picture("before-move");
    auto moving = replay.run(selected, "01-left-move", [&](Session& s) {
        click(s, 0, first_pixel); click(s, 0, goal_pixel);
    });
    require(moving.side(Side::Blue).selected == std::vector<EntityId>{first} &&
            moving.find(first)->order == OrderKind::Move && moving.find(first)->destination == destination,
            "Left ground click orders the selected actor.");
    auto advanced = replay.run(moving, "02-moved", [](Session&) {}, 8);
    require(advanced.find(first)->position != a && !replay.blue_body_at(a) &&
            replay.blue_body_at(advanced.find(first)->position), "The same actor leaves its starting pixels and appears at its new world position.");
    replay.picture("after-move");
    auto target = replay.run(moving, "03-enemy-focus", [&](Session& s) { click(s, 0, enemy_pixel); });
    require(target.find(first)->focus == enemy && target.find(first)->order == OrderKind::AttackMove &&
            target.find(first)->destination == hostile, "Clicking an enemy focuses that exact entity.");
    auto impact = replay.run(target, "16-attack-impact", [](Session&) {}, 40);
    require(!impact.find(enemy) || impact.find(enemy)->hp < start.find(enemy)->hp,
            "The selected shooter damages the clicked enemy through ordinary simulation.");
    auto switched = replay.run(moving, "04-shift-toggle", [&](Session& s) {
        click(s, 0, second_pixel, shift); click(s, 0, first_pixel, shift);
    });
    require(switched.side(Side::Blue).selected == std::vector<EntityId>{second}, "Shift adds and removes individual own units.");
    auto friendly = replay.run(switched, "05-own-switch", [&](Session& s) { click(s, 0, first_pixel); });
    require(friendly.side(Side::Blue).selected == std::vector<EntityId>{first}, "An unselected friendly receives selection.");
    auto cancelled = replay.run(moving, "06-right-clear", [&](Session& s) { click(s, 1, enemy_pixel); });
    require(cancelled.side(Side::Blue).selected.empty() && cancelled.find(first)->destination == destination &&
            cancelled.find(first)->focus == no_entity, "A right click clears selection and preserves the issued order.");
    auto dragged = replay.run(moving, "07-right-pan", [&](Session& s) {
        pointer(s, Phase::Down, 1, first_pixel);
        pointer(s, Phase::Drag, 1, {first_pixel.x + 48, first_pixel.y + 12});
        pointer(s, Phase::Up, 1, {first_pixel.x + 48, first_pixel.y + 12});
        click(s, 0, {second_pixel.x + 48, second_pixel.y + 12});
    });
    require(dragged.side(Side::Blue).selected == std::vector<EntityId>{second}, "Right drag pans the next pointer hit by its measured pixel delta.");
    auto out_and_back = replay.run(moving, "08-round-trip-drag", [&](Session& s) {
        pointer(s, Phase::Down, 1, first_pixel);
        pointer(s, Phase::Drag, 1, {first_pixel.x + 10, first_pixel.y});
        pointer(s, Phase::Up, 1, first_pixel);
    });
    require(out_and_back.side(Side::Blue).selected == std::vector<EntityId>{first}, "Returning a right drag to the press point retains selection.");
    auto deployed = replay.run(start, "09-deploy", [&](Session& s) { click(s, 0, home); click(s, 0, home); });
    require(deployed.find(pioneer)->kind == UnitKind::Hub && deployed.find(pioneer)->position == start.map.starts[0],
            "Clicking the selected Pioneer deploys the same entity in place.");
    auto no_build = replay.run(deployed, "10-cancel-construction", [&](Session& s) {
        click(s, 0, first_pixel); key(s, 0, "g"); click(s, 1, goal_pixel); click(s, 0, goal_pixel);
    });
    require(no_build.entities.size() == deployed.entities.size() && no_build.side(Side::Blue).funds == deployed.side(Side::Blue).funds &&
            no_build.side(Side::Blue).selected == std::vector<EntityId>{first} && no_build.find(first)->destination == destination,
            "Right click cancels placement, preserving selection and funds for the next ground order.");
    auto rallied = replay.run(start, "11-workshop-rally", [&](Session& s) {
        click(s, 0, replay.point(start.find(shop)->position)); click(s, 0, replay.point(rally));
    });
    require(rallied.side(Side::Blue).rally == rally, "The selected workshop sets its rally by left click.");
    auto no_rally = replay.run(moving, "17-cancel-rally", [&](Session& s) {
        key(s, 0, "t"); click(s, 1, goal_pixel); click(s, 0, replay.point(rally));
    });
    require(no_rally.side(Side::Blue).rally == moving.side(Side::Blue).rally &&
            no_rally.find(first)->destination == rally, "A right click cancels an armed rally and restores movement.");
    auto forced = replay.run(moving, "12-option-ground", [&](Session& s) { click(s, 0, enemy_pixel, option); });
    require(forced.find(first)->order == OrderKind::Move && forced.find(first)->destination == hostile &&
            forced.find(first)->focus == no_entity, "Option uses the ground under a target for a move order.");
    auto rectangle = replay.run(start, "13-box", [&](Session& s) {
        const PixelPoint from{std::min(first_pixel.x, second_pixel.x) - 1, std::min(first_pixel.y, second_pixel.y) - 1};
        const PixelPoint to{std::max(first_pixel.x, second_pixel.x) + 1, std::max(first_pixel.y, second_pixel.y) + 1};
        pointer(s, Phase::Down, 0, from); pointer(s, Phase::Drag, 0, to); pointer(s, Phase::Up, 0, to);
    });
    require(rectangle.side(Side::Blue).selected == std::vector<EntityId>{first,second}, "The left marquee selects both covered actors.");
    auto denied = replay.run(moving, "14-impassable", [&](Session& s) { click(s, 0, replay.point({8, 12})); });
    require(denied.find(first)->destination == destination && denied.side(Side::Blue).selected == std::vector<EntityId>{first},
            "Impassable terrain preserves selection and the previous valid order.");
    auto support = replay.run(start, "15-unarmed", [&](Session& s) {
        click(s, 0, replay.point(start.find(collector)->position)); click(s, 0, enemy_pixel);
    });
    require(support.find(collector)->focus == no_entity && support.find(collector)->order == OrderKind::Idle,
            "An unarmed selection cannot issue an attack.");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::runtime_error("Provide at most one new output directory.");
        fs::path directory;
        if (argc == 2) {
            directory = argv[1];
            require(fs::create_directory(directory), "Use a new evidence directory.");
        } else {
            auto name = (fs::temp_directory_path() / "coastal-pointer-XXXXXX").string();
            require(::mkdtemp(name.data()), "Create a scratch directory.");
            directory = name;
        }
        Replay replay(directory);
        check(replay);
        if (argc == 1) fs::remove_all(directory);
        std::cout << "PASS 18 public 2D pointer checkpoints through actual session events, F9, F5, rendered ticks and loaded world state\n";
    } catch (const std::exception& error) {
        std::cerr << "classic_pointer_test: " << error.what() << '\n';
        return 1;
    }
}
