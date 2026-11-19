// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "world.h"

namespace coastal::rts {

// Synthetic, reproducible 512-square battle used by the headless and Mac tests.
World make_stress_match();
void drive_stress(World& world);

}  // namespace coastal::rts
