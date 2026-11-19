// SPDX-License-Identifier: GPL-3.0-only
#include "session.h"

#include "rts_ai/strategy.h"
#include "rts_3d/scene.h"

#include "rts_core/combat.h"
#include "rts_core/economy.h"
#include "rts_core/iso.h"
#include "rts_core/match.h"
#include "rts_core/snapshot.h"
#include "rts_core/stress.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace coastal::macos_host {
namespace {

using namespace coastal::rts;

constexpr int width = 1024;
constexpr int height = 768;
constexpr auto frame_size = std::size_t(width) * height * 4;
constexpr Side player = Side::Blue;
constexpr Side rival = Side::Copper;

Command order(CommandKind kind, Side side) {
    Command command{};
    command.kind = kind;
    command.side = side;
    return command;
}

bool has_modifier(InputModifiers value, Modifier flag) {
    return (value & static_cast<InputModifiers>(flag)) != 0;
}

std::optional<PixelPoint> pixel(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    return PixelPoint{int(std::clamp(x, -double(width), 2.0 * width)),
                      int(std::clamp(y, -double(height), 2.0 * height))};
}

bool is_drag(const SelectionBox& box) noexcept {
    return std::abs(box.start.x - box.end.x) >= 4 || std::abs(box.start.y - box.end.y) >= 4;
}

class EngineSession final : public Session {
public:
    EngineSession(World world, const LaunchOptions& options)
        : world_(std::move(world)), navigator_(options.threads, options.strict_audit),
          save_path_(options.data_directory / "CoastalHost.snapshot"),
          threads_(options.threads), audit_(options.strict_audit), stress_(options.stress),
          adaptive_(options.adaptive_ai), cycle_views_(options.cycle_views) {
        if (stress_) {
            camera_.zoom = .55;
            const Cell center{world_.map.width / 2, world_.map.height / 2};
            camera_.center_on(center, world_.map);
            orbit_.distance = orbit_.goal_distance = 116;
            orbit_.center_on(world_.map, center);
        } else {
            camera_.center_on(world_.map.starts[0], world_.map);
            orbit_.center_on(world_.map, world_.map.starts[0]);
        }
    }

    ~EngineSession() override {
        if (!audit_) return;
        std::cout << "COASTAL_RTS_WORLD {\"step\":" << world_.step
                  << ",\"units\":" << world_.entities.size()
                  << ",\"blueDeliveries\":" << world_.side(player).deliveries
                  << ",\"copperDeliveries\":" << world_.side(rival).deliveries
                  << ",\"result\":" << static_cast<unsigned>(world_.result) << "}\n";
        if (stress_) write_workload_metrics();
        if (view_frames_ || cycle_views_) {
            try {
                write_view_metrics();
            } catch (const std::exception& error) {
                std::cerr << "CoastalHost view audit: " << error.what() << '\n';
            }
        }
    }

    Viewport viewport() const noexcept override { return {width, height}; }
    FrameClock::duration frame_interval() const noexcept override { return std::chrono::nanoseconds(16'666'667); }
    bool supports_shared_frames() const noexcept override { return true; }

    std::shared_ptr<const view3d::TerrainMesh> terrain_for_metal() override {
        scene_.terrain = view3d::make_terrain_mesh(world_.map);
        return scene_.terrain;
    }

    void scene_available(bool available) noexcept override { metal_scene_ready_ = available; }


    std::optional<Frame> advance_frame() override {
        if (!software_pixels_ || !software_pixels_.unique()) {
            software_pixels_ = std::make_shared<std::vector<std::uint8_t>>(frame_size);
        }
        auto frame = advance_frame_into(*software_pixels_);
        if (frame) frame->rgba_owner = software_pixels_;
        return frame;
    }

    std::optional<Frame> advance_frame_into(std::span<std::uint8_t> rgba) override {
        if (rgba.size() != frame_size) return std::nullopt;
        if (cycle_views_ && metal_scene_ready_) view_3d_ = world_.step >= 150 && world_.step < 930;
        if (stress_) {
            drive_stress(world_);
        } else if (adaptive_ || world_.side(rival).ai) {
            ai::drive_adaptive(world_, rival, audit_ ? &std::cout : nullptr);
        } else {
            ai::drive_scripted(world_, rival);
        }
        const auto started = FrameClock::now();
        world_.tick(navigator_);
        if (stress_ && audit_) sample_workload(started);
        advance_camera();

        ViewOverlay overlay;
        overlay.drag = drag_ && is_drag(*drag_) ? drag_ : std::nullopt;
        if (view_3d_) {
            view3d::update_scene(scene_, world_, orbit_.pose(width, height), overlay.drag);
            ++view_frames_;
            minimum_visible_ = std::min(minimum_visible_, scene_.visible_units);
            return Frame{{frame_++, world_.entities.size(), false}, {}, {}, &scene_};
        }
        if (build_kind_ && hover_) {
            overlay.build_cell = pick_cell(world_.map, camera_, *hover_, width, height);
            if (overlay.build_cell) overlay.can_build = can_build(world_, player, *build_kind_, *overlay.build_cell);
        }
        if (!render_isometric(world_, camera_, overlay, rgba, width, height, &navigator_.workers())) {
            throw std::runtime_error("Could not render the isometric world.");
        }
        return Frame{{frame_++, world_.entities.size(), false}, rgba};
    }

    void pointer_event(PointerEvent event) noexcept override {
        try {
            handle_pointer(event);
        } catch (const std::exception& error) {
            std::cerr << "CoastalHost input: " << error.what() << '\n';
        }
    }

    void scroll_event(ScrollEvent event) noexcept override {
        if (!std::isfinite(event.scrolling_delta_x) || !std::isfinite(event.scrolling_delta_y)) return;
        if (view_3d_) {
            orbit_.zoom_by(std::pow(1.06, std::clamp(event.scrolling_delta_y, -16.0, 16.0)));
        } else if (has_modifier(event.modifiers, Modifier::Control)) {
            camera_.zoom_by(std::pow(1.05, std::clamp(event.scrolling_delta_y, -16.0, 16.0)));
        } else {
            const auto size = event.precise ? 1.0 : 10.0;
            camera_.pan(event.scrolling_delta_x * size, event.scrolling_delta_y * size);
        }
    }

    void key_event(KeyEvent event) noexcept override {
        try {
            if (event.phase != KeyEvent::Phase::Down) return;
            const auto characters = event.characters_ignoring_modifiers;
            if (!event.repeat && (characters == "v" || characters == "V") && metal_scene_ready_ && !cycle_views_) {
                view_3d_ = !view_3d_;
                drag_.reset();
                orbit_drag_.reset();
                return;
            }
            if (pan_key(event.key_code) || event.repeat || stress_) return;
            if (event.key_code == 96 || event.key_code == 101) {
                save_or_load(event.key_code == 96);
            } else if (event.characters_ignoring_modifiers.size() == 1) {
                handle_character(event.characters_ignoring_modifiers.front(), event.modifiers);
            }
        } catch (const std::exception& error) {
            std::cerr << "CoastalHost input: " << error.what() << '\n';
        }
    }

    void input_focus_lost() noexcept override {
        hover_.reset();
        drag_.reset();
        orbit_drag_.reset();
    }

private:
    void write_view_metrics() const {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto byte : encode_snapshot(world_)) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        std::ostringstream fingerprint;
        fingerprint << std::hex << std::setw(16) << std::setfill('0') << hash;
        std::cout << "COASTAL_RTS_VIEW {\"step\":" << world_.step
                  << ",\"submitted3dFrames\":" << view_frames_
                  << ",\"minimumVisibleUnits\":" << (view_frames_ ? minimum_visible_ : 0)
                  << ",\"worldHash\":\"" << fingerprint.str() << "\"}\n";
    }

    std::optional<Cell> pick(PixelPoint point) const noexcept {
        if (view_3d_) return view3d::pick_terrain(world_.map, orbit_.pose(width, height), point);
        return pick_cell(world_.map, camera_, point, width, height);
    }

    void sample_workload(FrameClock::time_point started) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(FrameClock::now() - started);
        simulation_ns_ += static_cast<std::uint64_t>(elapsed.count());
        const auto& work = navigator_.last_work();
        minimum_units_ = std::min(minimum_units_, world_.entities.size());
        minimum_active_ = std::min(minimum_active_, work.active);
        minimum_moved_ = std::min(minimum_moved_, work.moved);
        minimum_fired_ = std::min(minimum_fired_, work.fired);
        moves_ += work.moved;
        launches_ += work.fired;
        if (work.moved && work.engaged) ++mixed_steps_;
    }

    void write_workload_metrics() const {
        std::cout << "COASTAL_RTS_WORK {\"threads\":" << threads_
                  << ",\"steps\":" << world_.step
                  << ",\"minimumUnits\":" << minimum_units_
                  << ",\"minimumActive\":" << minimum_active_
                  << ",\"minimumMoved\":" << minimum_moved_
                  << ",\"minimumFired\":" << minimum_fired_
                  << ",\"millionMoves\":" << double(moves_) / 1'000'000
                  << ",\"projectilesLaunched\":" << launches_
                  << ",\"stepsWithMovementAndCombat\":" << mixed_steps_
                  << ",\"meanSimulationNs\":" << (world_.step ? simulation_ns_ / world_.step : 0)
                  << ",\"peakFlowBytes\":" << navigator_.flow_stats().peak_total_distance_bytes
                  << "}\n";
    }

    void handle_pointer(PointerEvent event) {
        using Phase = PointerEvent::Phase;
        if (event.phase == Phase::Leave) {
            hover_.reset();
            return;
        }
        const auto point = pixel(event.x, event.y);
        if (!point) return;
        if (point->x >= 0 && point->x < width && point->y >= 0 && point->y < height) hover_ = point;
        else hover_.reset();
        if (view_3d_ && event.button == 2) {
            if (event.phase == Phase::Down) orbit_drag_ = point;
            if (event.phase == Phase::Drag && orbit_drag_) {
                orbit_.orbit(point->x - orbit_drag_->x, point->y - orbit_drag_->y);
                orbit_drag_ = point;
            }
            if (event.phase == Phase::Up) orbit_drag_.reset();
            return;
        }
        if (stress_) return;

        if (event.button == 1 && event.phase == Phase::Up) {
            if (build_kind_ || rally_mode_) {
                build_kind_.reset();
                rally_mode_ = false;
                return;
            }
            if (const auto cell = pick(*point)) {
                auto command = order(has_modifier(event.modifiers, Modifier::Shift) ?
                                     CommandKind::AttackMove : CommandKind::Move, player);
                command.target = *cell;
                world_.apply(command);
            }
            return;
        }

        if (event.button != 0) return;
        if (event.phase == Phase::Down && !build_kind_ && !rally_mode_) drag_ = SelectionBox{*point, *point};
        if (event.phase == Phase::Drag && drag_) drag_->end = *point;
        if (event.phase == Phase::Up) {
            if (build_kind_ || rally_mode_) {
                if (const auto cell = pick(*point)) {
                    auto command = order(rally_mode_ ? CommandKind::Rally : CommandKind::Build, player);
                    command.target = *cell;
                    command.unit = build_kind_.value_or(UnitKind::Pioneer);
                    if (world_.apply(command)) {
                        build_kind_.reset();
                        rally_mode_ = false;
                    }
                }
            } else if (drag_) {
                drag_->end = *point;
                select(*drag_, has_modifier(event.modifiers, Modifier::Shift));
            }
            drag_.reset();
        }
    }

    void select(const SelectionBox& box, bool add) {
        auto command = order(CommandKind::Select, player);
        command.add = add;
        const auto [left, right] = std::minmax(box.start.x, box.end.x);
        const auto [top, bottom] = std::minmax(box.start.y, box.end.y);
        const bool dragged = is_drag(box);
        const auto clicked_cell = dragged ? std::nullopt : pick(box.end);
        EntityId nearest = no_entity;
        int best = 19 * 19;
        const auto orbit_pose = view_3d_ ? orbit_.pose(width, height) : view3d::CameraPose{};
        for (const auto& entity : world_.entities) {
            if (entity.side != player) continue;
            if (clicked_cell && entity.position == *clicked_cell) {
                nearest = entity.id;
                break;
            }
            const auto foot = view_3d_
                ? view3d::project_point(orbit_pose, view3d::tile_anchor(world_.map, entity.position))
                : std::optional<PixelPoint>(project_cell(world_.map, camera_, entity.position, width, height));
            if (!foot) continue;
            if (dragged) {
                if (foot->x >= left && foot->x <= right && foot->y >= top && foot->y <= bottom) {
                    command.units.push_back(entity.id);
                }
            } else {
                const int dx = foot->x - box.end.x;
                const int dy = foot->y - 6 - box.end.y;
                if (std::abs(dx) <= 18 && std::abs(dy) <= 18 && dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    nearest = entity.id;
                }
            }
        }
        if (!dragged && nearest != no_entity) command.units.push_back(nearest);
        world_.apply(command);
    }

    void advance_camera() {
        if (view_3d_) {
            orbit_.advance();
            return;
        }
        if (hover_ && !drag_) {
            constexpr int border = 14;
            const auto scroll = [](int pixel, int extent) {
                return pixel < border ? 8 : pixel >= extent - border ? -8 : 0;
            };
            camera_.pan(scroll(hover_->x, width), scroll(hover_->y, height));
        }
        camera_.advance();
    }

    bool pan_key(std::uint16_t key) {
        if (view_3d_) {
            switch (key) {
            case 123: orbit_.orbit(-18, 0); return true;
            case 124: orbit_.orbit(18, 0); return true;
            case 125: orbit_.orbit(0, -18); return true;
            case 126: orbit_.orbit(0, 18); return true;
            default: return false;
            }
        }
        switch (key) {
        case 123: camera_.pan(28, 0); return true;   // Left
        case 124: camera_.pan(-28, 0); return true;  // Right
        case 125: camera_.pan(0, -28); return true;  // Down
        case 126: camera_.pan(0, 28); return true;   // Up
        default: return false;
        }
    }

    void handle_character(char key, InputModifiers modifiers) {
        if (key >= '1' && key <= '9') {
            auto command = order(has_modifier(modifiers, Modifier::Control) ?
                                 CommandKind::AssignGroup : CommandKind::RecallGroup, player);
            command.index = static_cast<std::uint32_t>(key - '1');
            world_.apply(command);
            return;
        }
        if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
        switch (key) {
        case 'd': world_.apply(order(CommandKind::Deploy, player)); return;
        case 'g': choose_build(UnitKind::Generator); return;
        case 'm': choose_build(UnitKind::Depot); return;
        case 'w': choose_build(UnitKind::Workshop); return;
        case 'q': train(UnitKind::Skirmisher); return;
        case 'e': train(UnitKind::Sentinel); return;
        case 'r': train(UnitKind::Artillery); return;
        case 'c': train(UnitKind::Collector); return;
        case 'p': world_.apply(order(CommandKind::TogglePause, player)); return;
        case 'h': world_.apply(order(CommandKind::Hold, player)); return;
        case 'u': {
            auto cancel = order(CommandKind::Cancel, player);
            const auto size = world_.side(player).queue.size();
            if (size) {
                cancel.index = static_cast<std::uint32_t>(size - 1);
                world_.apply(cancel);
            }
            return;
        }
        case 't': build_kind_.reset(); rally_mode_ = true; return;
        case ' ': {
            const auto& selected = world_.side(player).selected;
            const auto* selected_entity = selected.empty() ? nullptr : world_.find(selected.front());
            const auto center = selected_entity ? selected_entity->position : world_.map.starts[0];
            camera_.center_on(center, world_.map);
            orbit_.center_on(world_.map, center);
            return;
        }
        case '\x1b': build_kind_.reset(); rally_mode_ = false; drag_.reset(); return;
        default: return;
        }
    }

    void choose_build(UnitKind kind) noexcept {
        rally_mode_ = false;
        build_kind_ = kind;
    }

    void train(UnitKind kind) {
        auto command = order(CommandKind::Queue, player);
        command.unit = kind;
        world_.apply(command);
    }

    void save_or_load(bool save) {
        std::string error;
        if (save) {
            if (!save_world(save_path_, world_, error)) std::cerr << "CoastalHost save: " << error << '\n';
            return;
        }
        auto loaded = load_world(save_path_, error);
        if (!loaded) {
            std::cerr << "CoastalHost load: " << error << '\n';
            return;
        }
        auto navigation = Navigator(threads_, audit_);
        auto terrain = metal_scene_ready_ ? view3d::make_terrain_mesh(loaded->map) : nullptr;
        world_ = std::move(*loaded);
        navigator_ = std::move(navigation);
        scene_.terrain = std::move(terrain);
        camera_.center_on(world_.map.starts[0], world_.map);
        orbit_.center_on(world_.map, world_.map.starts[0]);
        build_kind_.reset();
        rally_mode_ = false;
        input_focus_lost();
    }

    World world_;
    Navigator navigator_;
    Camera camera_;
    view3d::OrbitCamera orbit_;
    view3d::SceneFrame scene_;
    std::filesystem::path save_path_;
    unsigned threads_;
    bool audit_;
    bool stress_;
    bool adaptive_;
    bool cycle_views_;
    bool metal_scene_ready_ = false;
    bool view_3d_ = false;
    std::uint64_t view_frames_ = 0;
    std::size_t minimum_visible_ = World::entity_limit;
    std::size_t minimum_units_ = World::entity_limit;
    unsigned minimum_active_ = World::entity_limit;
    unsigned minimum_moved_ = World::entity_limit;
    unsigned minimum_fired_ = World::entity_limit;
    std::uint64_t simulation_ns_ = 0;
    std::uint64_t moves_ = 0;
    std::uint64_t launches_ = 0;
    std::uint64_t mixed_steps_ = 0;
    std::optional<PixelPoint> hover_;
    std::optional<PixelPoint> orbit_drag_;
    std::optional<SelectionBox> drag_;
    std::optional<UnitKind> build_kind_;
    bool rally_mode_ = false;
    std::shared_ptr<std::vector<std::uint8_t>> software_pixels_;
    std::uint64_t frame_ = 0;
};

}  // namespace

std::unique_ptr<Session> create_session(const LaunchOptions& options, std::string& error) {
    if (options.stress) return std::make_unique<EngineSession>(make_stress_match(), options);
    auto world = open_match(options.data_directory,
                            options.skirmish_map.empty() ? "Basin.map" : options.skirmish_map, error);
    if (!world) return nullptr;
    return std::make_unique<EngineSession>(std::move(*world), options);
}

}  // namespace coastal::macos_host
