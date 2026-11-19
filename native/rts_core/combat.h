// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "navigation/flow_cache.h"
#include "world.h"

namespace coastal::rts {

class Navigator {
public:
    // Rebuild derived grid only when changed; used at the start of a simulation step.
    void prepare(const World& world);
    std::optional<Cell> next(const World& world, Cell from, Cell goal);

private:
    navigation::FlowCache cache_;
    std::vector<unsigned char> blocked_;
    int width_ = 0;
    int height_ = 0;
    std::uint32_t revision_ = 0;
};

bool apply_combat(World& world, const Command& command);
void tick_combat(World& world, Navigator& navigator);

}  // namespace coastal::rts
