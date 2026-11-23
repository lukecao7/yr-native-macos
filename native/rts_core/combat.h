// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "navigation/flow_cache.h"
#include "profiling/tick_profile.h"
#include "spatial_index.h"
#include "worker_pool.h"
#include "world.h"

#include <array>
#include <cstddef>
#include <span>

namespace coastal::rts {

struct StepWork {
    unsigned moved = 0;
    unsigned engaged = 0; // Has an enemy in weapon range.
    unsigned fired = 0;
    unsigned active = 0; // Unique entities that moved or engaged this step.
};

// Derived physical source/tail occupancy. This is a destination-cell filter,
// separate from static topology. no_entity means an unknown owner and blocks
// every mover; repeated equal owners may share one cell without conflict.
struct MobileOccupant {
    Cell cell;
    EntityId owner = no_entity;
    // Admission remains blocked even when a settled body permits an adjacent
    // Move to complete. Producers may clear this only after physical motion
    // and body turning have settled. Unknown producers default to waiting.
    bool blocks_completion = true;
};

// Derived active one-cell centre segment. It keeps a moving owner's whole
// source-to-next diagonal exclusive until physical interpolation ends. It is
// separate from cell admission and from static topology; no save field is added.
struct MobileSegment {
    Cell from;
    Cell to;
    EntityId owner = no_entity;
};

// One prepare's actual non-turret PrimaryFacing choice. Empty next means
// no movement was prepared. This derives from live motion and is never saved.
struct PreparedMoveStep {
    EntityId owner = no_entity;
    std::optional<Cell> next;
};

class Navigator {
public:
    explicit Navigator(unsigned threads = 1, bool measure = false);
    WorkerPool& workers() noexcept { return workers_; }
    const StepWork& last_work() const noexcept { return work_; }
    std::span<const std::uint8_t> visibility(Side side) const noexcept {
        return visible_[side_index(side)];
    }
    navigation::FlowCache::Stats flow_stats() const noexcept { return cache_.stats(); }

    // Owner-thread entry points retained for path queries and focused tests.
    void prepare(const World& world);
    // Copies a row-major Map-sized overlay; any nonzero byte adds a blocker.
    // An empty span removes the overlay, preserving terrain/building blockers.
    // The setter only stages it: prepare(world) validates the size and makes
    // changes visible to next(). World::tick prepares before its path queries.
    // After a bad-size rejection, replace/clear the mask and prepare again.
    void set_obstacle_mask(std::span<const std::uint8_t>);
    // Queries the last prepared grid. A masked goal without a world entity
    // can be approached, but its Move order waits until the mask is cleared.
    std::optional<Cell> next(const World& world, Cell from, Cell goal,
                             EntityId owner = no_entity);

    // Replaces the complete transient snapshot with an owned copy. Empty
    // clears it. prepare(world) publishes it before queries/worker dispatch.
    // Transient cells only filter move destinations, never FlowCache topology
    // or diagonal side cells. Distinct owners sharing a cell block all movers;
    // a unique known owner can ignore its own entry. Owner-thread staging and
    // prepare must not overlap worker reads. Entries must be inside the map.
    void set_transient_occupancy(std::span<const MobileOccupant>);
    bool transient_occupied(Cell cell, EntityId ignore = no_entity) const noexcept;
    // Replaces derived active segments. prepare() validates one-cell adjacent
    // endpoints and publishes them. Only strict interior line crossings are
    // blocked; mobile side cells retain ordinary side-around semantics.
    void set_transient_segments(std::span<const MobileSegment>);
    bool transient_crossed(Cell from, Cell to, EntityId ignore = no_entity) const noexcept;
    // Owner stages the actual next step prepared by an external body-turn
    // controller. Unlisted actors keep all ranked alternatives; listed actors
    // may commit only that next step. Empty next admits none. Re-derived each
    // preparation; prepare() validates it before shared worker reads.
    void set_prepared_move_steps(std::span<const PreparedMoveStep>);
    bool prepared_step_allowed(EntityId owner, Cell next) const noexcept;
    // Shared owner/worker predicate over the last prepared snapshot. The
    // caller supplies live World-body occupancy, never a terrain-only mask.
    // A Move may finish at its goal, or adjacent to an occupied goal, only
    // when every entry there has settled. Own entries count for completion;
    // unknown/multiple owners remain conservative even if their flags clear.
    // This query allocates nothing and never changes the World or FlowCache.
    bool can_finish_move(Cell from, Cell goal, bool goal_has_body) const noexcept;

    // Owner-thread query over the last prepared static grid. Returns all
    // downhill route steps in next()'s stable cost order, ignoring mobile
    // and transient occupancy. Terrain, obstacle/building blockers, imported
    // travel edges and static diagonal side cells retain normal constraints.
    // The world is unchanged; cold goals may populate the shared FlowCache.
    // Call prepare(world) after any topology change and outside worker reads.
    // Only out[0..returned_count) is written; the count is at most out.size().
    std::size_t ranked_static_steps(const World& world, Cell from, Cell goal,
                                    std::array<Cell, 8>& out);

private:
    friend void tick_combat(World& world, Navigator& navigator);
    struct TransientCell {
        EntityId owner = no_entity;
        bool blocked_for_all = false;
        bool blocks_completion = false;
    };
    struct SegmentCell {
        std::array<EntityId, 2> owner{};
        std::array<bool, 2> blocked_for_all{};
    };
    void prepare_transient(const Map& map);
    navigation::FlowCache cache_;
    std::vector<MobileOccupant> transient_occupants_;
    std::vector<TransientCell> transient_cells_;
    std::vector<int> transient_touched_;
    std::vector<MobileSegment> transient_segments_;
    std::vector<SegmentCell> transient_segment_cells_;
    std::vector<int> transient_segment_touched_;
    std::vector<PreparedMoveStep> prepared_move_steps_;
    std::vector<unsigned char> blocked_;
    std::vector<unsigned char> edges_;
    std::vector<std::uint8_t> obstacle_mask_;
    int width_ = 0;
    int height_ = 0;
    std::uint32_t revision_ = 0;
    WorkerPool workers_;
    SpatialIndex spatial_;
    StepWork work_{};
    std::array<std::vector<std::uint8_t>, 2> visible_;
    profiling::Recorder profile_;
};

bool apply_combat(World& world, const Command& command);
void tick_combat(World& world, Navigator& navigator);

}  // namespace coastal::rts
