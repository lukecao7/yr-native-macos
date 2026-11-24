// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/snapshot.h"

#include "rts_core/combat.h"
#include "rts_core/economy.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace coastal::rts {
namespace {

constexpr std::array<std::uint8_t, 4> signature{'C', 'R', 'T', 'S'};
constexpr std::uint16_t legacy_version = 1;
constexpr std::uint16_t ai_version = 2;
constexpr std::uint16_t topology_version = 3;
constexpr std::uint16_t subcell_version = 4;
constexpr std::size_t snapshot_limit = 16 * 1024 * 1024;
constexpr std::size_t tile_bytes = 4;
constexpr std::size_t production_bytes = 7;
constexpr std::size_t entity_bytes = 52;
constexpr std::size_t projectile_bytes = 28;
constexpr std::size_t focus_bytes = 2 * sizeof(EntityId);
constexpr std::size_t seen_tile_bytes = 4;
constexpr std::size_t contact_bytes = 23;

std::uint64_t checksum(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class Writer {
public:
    template <class... T>
    void operator()(T... values) {
        (put(values), ...);
    }

    template <class Value>
    void count(const std::vector<Value>& sequence) {
        put(static_cast<std::uint32_t>(sequence.size()));
    }

    std::vector<std::uint8_t> bytes;

private:
    template <class T>
    void put(T value) {
        if constexpr (std::is_enum_v<T>) {
            put(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            put(static_cast<std::uint8_t>(value));
        } else {
            const auto bits = static_cast<std::make_unsigned_t<T>>(value);
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                bytes.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
            }
        }
    }
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : remaining_(bytes) {}

    template <class... T>
    void operator()(T&... values) {
        ((values = get<T>()), ...);
    }

    std::size_t count(std::size_t limit, std::size_t entry_bytes) {
        const auto value = get<std::uint32_t>();
        if (value > limit || value > remaining_.size() / entry_bytes) {
            throw std::invalid_argument("Save contains an invalid record count.");
        }
        return value;
    }

    void require_entries(std::size_t count, std::size_t entry_bytes) const {
        if (count > remaining_.size() / entry_bytes) {
            throw std::invalid_argument("Save ended inside a record list.");
        }
    }

    bool empty() const noexcept { return remaining_.empty(); }

private:
    template <class T>
    T get() {
        if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(get<std::underlying_type_t<T>>());
        } else if constexpr (std::is_same_v<T, bool>) {
            const auto value = get<std::uint8_t>();
            if (value > 1) throw std::invalid_argument("Save contains an invalid Boolean.");
            return value == 1;
        } else {
            require_entries(1, sizeof(T));
            std::uint64_t bits = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i) {
                bits |= std::uint64_t{remaining_[i]} << (i * 8);
            }
            remaining_ = remaining_.subspan(sizeof(T));
            return std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
        }
    }

    std::span<const std::uint8_t> remaining_;
};

// Record field order and widths are shared by the encoder and decoder.
template <class Archive>
void cell_fields(Archive& archive, auto& cell) {
    static_assert(sizeof(int) == sizeof(std::int32_t));
    archive(cell.x, cell.y);
}

template <class Archive>
void world_fields(Archive& archive, auto& world) {
    archive(world.step, world.rng_state, world.next_id, world.result);
}

template <class Archive>
void rules_fields(Archive& archive, auto& rules) {
    archive(rules.initial_funds, rules.cargo_capacity, rules.harvest_ticks);
    for (auto& spec : rules.units) {
        archive(spec.hp, spec.cost, spec.train_ticks, spec.move_period, spec.vision,
                spec.range, spec.damage, spec.armor, spec.shot_period, spec.power);
    }
}

template <class Archive>
void side_fields(Archive& archive, auto& side) {
    archive(side.funds, side.income, side.deliveries, side.power_percent, side.production_paused);
    cell_fields(archive, side.rally);
}

template <class Archive>
void entity_fields(Archive& archive, auto& entity) {
    archive(entity.id, entity.kind, entity.side);
    cell_fields(archive, entity.position);
    archive(entity.hp, entity.order);
    cell_fields(archive, entity.destination);
    cell_fields(archive, entity.facing);
    archive(entity.move_wait, entity.shot_wait, entity.gather);
    cell_fields(archive, entity.deposit);
    archive(entity.depot, entity.cargo, entity.work);
}

template <class Archive>
void projectile_fields(Archive& archive, auto& projectile) {
    archive(projectile.shooter, projectile.target);
    cell_fields(archive, projectile.from);
    cell_fields(archive, projectile.to);
    archive(projectile.remaining, projectile.travel_ticks, projectile.damage);
}

template <class Archive>
void ai_fields(Archive& archive, auto& memory) {
    cell_fields(archive, memory.scout_target);
    cell_fields(archive, memory.expansion);
    archive(memory.survey_index, memory.preference);
}

template <class Archive>
void seen_tile_fields(Archive& archive, auto& tile) {
    archive(tile.known, tile.terrain, tile.resource);
}

template <class Archive>
void contact_fields(Archive& archive, auto& contact) {
    archive(contact.id, contact.kind);
    cell_fields(archive, contact.position);
    archive(contact.hp, contact.seen_at);
}

void write_extensions(Writer& writer, const World& world, std::uint32_t focus_count) {
    writer(focus_count);
    for (const auto& entity : world.entities) {
        if (entity.focus != no_entity) writer(entity.id, entity.focus);
    }
    for (const auto& side : world.sides) {
        writer(side.ai.has_value());
        if (!side.ai) continue;
        const auto& memory = *side.ai;
        ai_fields(writer, memory);
        writer.count(memory.chart);
        for (const auto& tile : memory.chart) seen_tile_fields(writer, tile);
        writer.count(memory.enemies);
        for (const auto& contact : memory.enemies) contact_fields(writer, contact);
    }
}

void read_extensions(Reader& reader, World& world, bool allow_empty = false) {
    const auto focus_count = reader.count(world.entities.size(), focus_bytes);
    auto actor = world.entities.begin();
    for (std::size_t i = 0; i < focus_count; ++i) {
        EntityId owner, focus;
        reader(owner, focus);
        // Advance through the sorted entity list. Each owner can occur once.
        while (actor != world.entities.end() && actor->id < owner) ++actor;
        if (actor == world.entities.end() || actor->id != owner || focus == no_entity) {
            throw std::invalid_argument("Invalid focus owner or target in save.");
        }
        actor->focus = focus;
        ++actor;
    }

    for (auto& side : world.sides) {
        bool present;
        reader(present);
        if (!present) continue;
        auto& memory = side.ai.emplace();
        ai_fields(reader, memory);
        const auto chart_size = reader.count(world.map.tiles.size(), seen_tile_bytes);
        if (chart_size != 0 && chart_size != world.map.tiles.size()) {
            throw std::invalid_argument("AI chart does not match the map size.");
        }
        memory.chart.resize(chart_size);
        for (auto& tile : memory.chart) seen_tile_fields(reader, tile);
        memory.enemies.resize(reader.count(World::entity_limit, contact_bytes));
        for (auto& contact : memory.enemies) contact_fields(reader, contact);
    }
    if (!allow_empty && focus_count == 0 && !world.sides[0].ai && !world.sides[1].ai) {
        throw std::invalid_argument("Save has an empty AI extension.");
    }
}

void write_ids(Writer& writer, const std::vector<EntityId>& ids) {
    writer.count(ids);
    for (const EntityId id : ids) writer(id);
}

void read_ids(Reader& reader, std::vector<EntityId>& ids, std::size_t limit) {
    ids.resize(reader.count(limit, sizeof(EntityId)));
    for (auto& id : ids) reader(id);
}

bool dimensions_valid(int width, int height) {
    return width > 0 && height > 0 && width <= Map::max_dimension && height <= Map::max_dimension;
}

bool optional_cell(const Map& map, Cell cell) {
    return cell == Cell{} || map.contains(cell);
}

bool valid_id_list(const World& world, const std::vector<EntityId>& ids, Side side) {
    if (ids.size() > world.entities.size()) return false;
    EntityId previous = no_entity;
    for (const EntityId id : ids) {
        if (id <= previous) return false;
        const auto* entity = world.find(id);
        if (!entity || entity->side != side) return false;
        previous = id;
    }
    return true;
}

void check_ai_memory(const World& world, Side side, const AiMemory& memory) {
    if ((!memory.chart.empty() && memory.chart.size() != world.map.tiles.size()) ||
        memory.enemies.size() > World::entity_limit ||
        memory.preference > UnitKind::Count ||
        !optional_cell(world.map, memory.scout_target) ||
        !optional_cell(world.map, memory.expansion)) {
        throw std::invalid_argument("Invalid AI memory in save.");
    }
    for (const auto& tile : memory.chart) {
        if (tile.terrain > Terrain::Deposit || (tile.terrain != Terrain::Deposit && tile.resource) ||
            (!tile.known && (tile.terrain != Terrain::Ground || tile.resource))) {
            throw std::invalid_argument("Invalid remembered terrain or resource in save.");
        }
    }

    EntityId previous = no_entity;
    for (const auto& contact : memory.enemies) {
        if (contact.id <= previous || contact.id >= world.next_id ||
            contact.kind >= UnitKind::Count || contact.hp == 0 ||
            contact.hp > world.rules.spec(contact.kind).hp ||
            !world.map.contains(contact.position) || contact.seen_at > world.step) {
            throw std::invalid_argument("Invalid remembered contact in save.");
        }
        const auto* current = world.find(contact.id);
        if (current && current->side == side) {
            throw std::invalid_argument("Remembered enemy ID belongs to the same faction.");
        }
        previous = contact.id;
    }
}

void check_world(const World& world) {
    const auto& map = world.map;
    if (!dimensions_valid(map.width, map.height) ||
        map.tiles.size() != static_cast<std::size_t>(map.width) * map.height ||
        (!map.travel_edges.empty() && map.travel_edges.size()!=map.tiles.size())) {
        throw std::invalid_argument("Invalid map dimensions in save.");
    }
    for (const auto& tile : map.tiles) {
        if (tile.terrain > Terrain::Deposit ||
            (tile.terrain != Terrain::Deposit && tile.resource != 0)) {
            throw std::invalid_argument("Invalid terrain or resource in save.");
        }
    }
    if (!map.walkable(map.starts[0]) || !map.walkable(map.starts[1]) ||
        map.starts[0] == map.starts[1]) {
        throw std::invalid_argument("Invalid start cells in save.");
    }
    if (!valid_rules(world.rules) || world.rng_state == 0 || world.next_id == no_entity ||
        world.result > MatchResult::Draw) {
        throw std::invalid_argument("Invalid rules or simulation state in save.");
    }
    if (world.entities.size() > World::entity_limit ||
        world.projectiles.size() > World::projectile_limit) {
        throw std::invalid_argument("Save exceeds the world record limits.");
    }

    EntityId previous = no_entity;
    for (const Entity& entity : world.entities) {
        if (entity.id <= previous || entity.id >= world.next_id ||
            entity.side > Side::Copper || entity.kind >= UnitKind::Count ||
            entity.hp == 0 || entity.hp > world.rules.spec(entity.kind).hp ||
            !map.walkable(entity.position) || !optional_cell(map, entity.destination) ||
            !optional_cell(map, entity.deposit) || entity.order > OrderKind::Gather ||
            entity.gather > GatherPhase::Return || entity.depot >= world.next_id ||
            entity.focus >= world.next_id ||
            (entity.subcell == whole_cell ? entity.previous_subcell != whole_cell :
                entity.subcell >= subcell_offsets.size() || entity.previous_subcell >= subcell_offsets.size()) ||
            entity.cargo > world.rules.cargo_capacity || entity.work >= world.rules.harvest_ticks ||
            entity.facing.x < -1 || entity.facing.x > 1 ||
            entity.facing.y < -1 || entity.facing.y > 1) {
            throw std::invalid_argument("Invalid entity state in save.");
        }
        previous = entity.id;
    }

    for (const auto& entity : world.entities) {
        const auto* target = entity.focus == no_entity ? nullptr : world.find(entity.focus);
        if (target && target->side == entity.side) {
            throw std::invalid_argument("A focus target belongs to the same faction.");
        }
    }

    for (const Projectile& projectile : world.projectiles) {
        if (projectile.shooter == no_entity || projectile.shooter >= world.next_id ||
            projectile.target == no_entity || projectile.target >= world.next_id ||
            !map.contains(projectile.from) || !map.contains(projectile.to) ||
            projectile.remaining == 0 || projectile.remaining > projectile.travel_ticks ||
            projectile.damage == 0) {
            throw std::invalid_argument("Invalid projectile state in save.");
        }
    }

    for (const Side value : {Side::Blue, Side::Copper}) {
        const auto& side = world.side(value);
        if (side.power_percent > 100 || !optional_cell(map, side.rally) ||
            side.queue.size() > SideState::queue_limit ||
            !valid_id_list(world, side.selected, value)) {
            throw std::invalid_argument("Invalid faction state in save.");
        }
        if (side.ai) check_ai_memory(world, value, *side.ai);
        for (const auto& group : side.groups) {
            if (!valid_id_list(world, group, value)) {
                throw std::invalid_argument("Invalid control group in save.");
            }
        }
        for (const auto& item : side.queue) {
            if (!is_producible(item.kind) || item.paid > world.rules.spec(item.kind).cost ||
                item.remaining_work > std::uint32_t{world.rules.spec(item.kind).train_ticks} * 100) {
                throw std::invalid_argument("Invalid production entry in save.");
            }
        }
    }
}

class Descriptor {
public:
    explicit Descriptor(int fd) : fd_(fd) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    ~Descriptor() { if (fd_ >= 0) ::close(fd_); }

    int get() const noexcept { return fd_; }
    bool close() noexcept { return ::close(std::exchange(fd_, -1)) == 0; }

private:
    int fd_;
};

struct RemoveTemporary {
    const std::string& path;
    bool active = true;
    ~RemoveTemporary() { if (active) ::unlink(path.c_str()); }
};

bool file_error(std::string& error, const char* operation, int cause) {
    error = std::string(operation) + ": " + std::error_code(cause, std::generic_category()).message();
    return false;
}

ssize_t read_some(int fd, void* output, std::size_t size) {
    ssize_t count;
    do {
        count = ::read(fd, output, size);
    } while (count < 0 && errno == EINTR);
    return count;
}

}  // namespace

std::vector<std::uint8_t> encode_snapshot(const World& world) {
    check_world(world);

    Writer writer;
    writer.bytes.insert(writer.bytes.end(), signature.begin(), signature.end());
    const auto focus_count = static_cast<std::uint32_t>(std::ranges::count_if(
        world.entities, [](const Entity& entity) { return entity.focus != no_entity; }));
    const bool has_extension = focus_count != 0 || world.sides[0].ai || world.sides[1].ai;
    const bool has_topology = !world.map.travel_edges.empty();
    const bool has_subcells = std::ranges::any_of(world.entities, [](const Entity& unit) { return unit.subcell != whole_cell; });
    writer(has_subcells ? subcell_version : has_topology ? topology_version : has_extension ? ai_version : legacy_version);
    world_fields(writer, world);
    writer(static_cast<std::uint16_t>(world.map.width), static_cast<std::uint16_t>(world.map.height));
    for (const Cell start : world.map.starts) cell_fields(writer, start);
    for (const Tile& tile : world.map.tiles) writer(tile.terrain, tile.height, tile.resource);
    rules_fields(writer, world.rules);
    for (const SideState& side : world.sides) {
        side_fields(writer, side);
        writer.count(side.queue);
        for (const Production& item : side.queue) writer(item.kind, item.paid, item.remaining_work);
        write_ids(writer, side.selected);
        for (const auto& group : side.groups) write_ids(writer, group);
    }
    writer.count(world.entities);
    for (const Entity& entity : world.entities) entity_fields(writer, entity);
    writer.count(world.projectiles);
    for (const Projectile& projectile : world.projectiles) projectile_fields(writer, projectile);
    if (has_extension || has_topology || has_subcells) write_extensions(writer, world, focus_count);
    if (has_topology || has_subcells) {
        writer.count(world.map.travel_edges);
        for (auto edge:world.map.travel_edges) writer(edge);
    }
    if (has_subcells) for (const auto& unit : world.entities) writer(unit.subcell, unit.previous_subcell);
    writer(checksum(writer.bytes));
    return std::move(writer.bytes);
}

std::optional<World> decode_snapshot(std::span<const std::uint8_t> bytes, std::string& error) {
    error.clear();
    if (bytes.size() < signature.size() + sizeof(legacy_version) + sizeof(std::uint64_t) ||
        bytes.size() > snapshot_limit || !std::ranges::equal(bytes.first(signature.size()), signature)) {
        error = "Unrecognized save size or signature.";
        return std::nullopt;
    }
    std::uint64_t stored_checksum;
    Reader suffix(bytes.last(sizeof(stored_checksum)));
    suffix(stored_checksum);
    const auto body = bytes.first(bytes.size() - sizeof(stored_checksum));
    if (stored_checksum != checksum(body)) {
        error = "Save checksum mismatch.";
        return std::nullopt;
    }

    try {
        Reader reader(body.subspan(signature.size()));
        std::uint16_t version;
        reader(version);
        if (version != legacy_version && version != ai_version && version != topology_version && version != subcell_version) {
            throw std::invalid_argument("Unsupported save version.");
        }

        World world;
        world_fields(reader, world);
        std::uint16_t width;
        std::uint16_t height;
        reader(width, height);
        if (!dimensions_valid(width, height)) throw std::invalid_argument("Invalid map dimensions in save.");
        world.map.width = width;
        world.map.height = height;
        for (Cell& start : world.map.starts) cell_fields(reader, start);
        const auto area = static_cast<std::size_t>(width) * height;
        reader.require_entries(area, tile_bytes);
        world.map.tiles.resize(area);
        for (Tile& tile : world.map.tiles) reader(tile.terrain, tile.height, tile.resource);

        rules_fields(reader, world.rules);
        for (SideState& side : world.sides) {
            side_fields(reader, side);
            side.queue.resize(reader.count(SideState::queue_limit, production_bytes));
            for (Production& item : side.queue) reader(item.kind, item.paid, item.remaining_work);
            read_ids(reader, side.selected, World::entity_limit);
            for (auto& group : side.groups) read_ids(reader, group, World::entity_limit);
        }
        world.entities.resize(reader.count(World::entity_limit, entity_bytes));
        for (Entity& entity : world.entities) entity_fields(reader, entity);
        world.projectiles.resize(reader.count(World::projectile_limit, projectile_bytes));
        for (Projectile& projectile : world.projectiles) projectile_fields(reader, projectile);
        if (version >= ai_version) read_extensions(reader, world, version >= topology_version);
        if (version >= topology_version) {
            const auto count=reader.count(area,1);
            if (count!=area && !(version==subcell_version && !count)) throw std::invalid_argument("Save topology does not match its map.");
            world.map.travel_edges.resize(count);
            for (auto& edge:world.map.travel_edges) reader(edge);
        }
        if (version == subcell_version) for (auto& unit : world.entities) reader(unit.subcell, unit.previous_subcell);
        if (!reader.empty()) throw std::invalid_argument("Unexpected bytes at the end of the save.");
        check_world(world);
        world.refresh_entity_index();
        return world;
    } catch (const std::invalid_argument& invalid) {
        error = invalid.what();
        return std::nullopt;
    }
}

bool save_world(const std::filesystem::path& path, const World& world, std::string& error) {
    error.clear();
    std::vector<std::uint8_t> bytes;
    try {
        bytes = encode_snapshot(world);
    } catch (const std::invalid_argument& invalid) {
        error = invalid.what();
        return false;
    }

    auto temporary_path = (path.parent_path() / ".coastal-save-XXXXXX").string();
    Descriptor temporary(::mkstemp(temporary_path.data()));
    if (temporary.get() < 0) return file_error(error, "Could not create the temporary save", errno);
    RemoveTemporary remove_temporary{temporary_path};

    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::write(temporary.get(), bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return file_error(error, "Could not write the temporary save", count < 0 ? errno : EIO);
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(temporary.get()) != 0) return file_error(error, "Could not sync the temporary save", errno);
    if (!temporary.close()) return file_error(error, "Could not close the temporary save", errno);
    if (::rename(temporary_path.c_str(), path.c_str()) != 0) {
        return file_error(error, "Could not replace the save", errno);
    }
    remove_temporary.active = false;
    return true;
}

std::optional<World> load_world(const std::filesystem::path& path, std::string& error) {
    error.clear();
    Descriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (file.get() < 0) {
        file_error(error, "Could not open the save", errno);
        return std::nullopt;
    }
    struct stat info {};
    if (::fstat(file.get(), &info) != 0) {
        file_error(error, "Could not read the save file information", errno);
        return std::nullopt;
    }
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uint64_t>(info.st_size) > snapshot_limit) {
        error = "Save must be a regular file within the supported size.";
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        const auto count = read_some(file.get(), bytes.data() + consumed, bytes.size() - consumed);
        if (count <= 0) {
            file_error(error, "Could not read the whole save", count < 0 ? errno : EIO);
            return std::nullopt;
        }
        consumed += static_cast<std::size_t>(count);
    }
    std::uint8_t extra;
    const auto trailing = read_some(file.get(), &extra, sizeof(extra));
    if (trailing < 0) {
        file_error(error, "Could not finish reading the save", errno);
        return std::nullopt;
    }
    if (trailing > 0) {
        error = "Save size changed while reading.";
        return std::nullopt;
    }
    return decode_snapshot(bytes, error);
}

bool replay(World& world, Navigator& navigator, std::span<const ReplayCommand> commands,
            std::uint64_t end_step, std::string& error) {
    error.clear();
    if (end_step < world.step || !std::ranges::is_sorted(commands, {}, &ReplayCommand::step)) {
        error = "Replay end predates the world or commands are out of order.";
        return false;
    }
    auto next = std::ranges::lower_bound(commands, world.step, {}, &ReplayCommand::step);
    while (world.step < end_step) {
        const auto step = world.step;
        // Previously accepted commands remain applied if a later command is rejected.
        while (next != commands.end() && next->step == step) {
            if (!world.apply(next->command)) {
                error = "Replay command rejected at step " + std::to_string(step) + ".";
                return false;
            }
            ++next;
        }
        world.tick(navigator);
        if (world.step != step + 1) {
            error = "Simulation failed to advance replay step " + std::to_string(step) + ".";
            return false;
        }
    }
    return true;
}

}  // namespace coastal::rts
