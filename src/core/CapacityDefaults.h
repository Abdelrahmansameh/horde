#pragma once

#include "core/Types.h"

namespace immune {

// Shared reservation defaults. These are storage limits, not encounter counts
// or damage/attack-rate tuning. The shipped sim.json must match them.
inline constexpr u32 kDefaultChaffCapacity = 65536;
inline constexpr u32 kDefaultSquadCapacity = 4096;
inline constexpr u32 kDefaultToxinShotCapacity = 16384;
inline constexpr u32 kDefaultHarpoonCapacity = 4096;

} // namespace immune
