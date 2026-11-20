// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "navigation/flow_cache.h"
#include "profiling/tick_profile.h"
#include "spatial_index.h"
#include "worker_pool.h"
#include "world.h"

#include <span>

namespace coastal::rts {

struct StepWork {
    unsigned moved = 0;
    unsigned engaged = 0; // Has an enemy in weapon range.
    unsigned fired = 0;
    unsigned active = 0; // Unique entities that moved or engaged this step.
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
    std::optional<Cell> next(const World& world, Cell from, Cell goal);

private:
    friend void tick_combat(World& world, Navigator& navigator);
    navigation::FlowCache cache_;
    std::vector<unsigned char> blocked_;
    std::vector<unsigned char> edges_;
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
