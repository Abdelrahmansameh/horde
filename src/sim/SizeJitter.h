// sim/SizeJitter.h — per-unit spawn size variation.
//
// Every chaff agent and every swarmer is born with a `size_scale` around 1,
// drawn uniformly from [1 - jitter, 1 + jitter], and keeps it for life. It
// multiplies BOTH the drawn body and the collision body, so a big one looks
// big and also takes up more room; nothing about a unit's size can disagree
// between what is seen and what collides.
//
// WHY A HASH, NOT AN Rng DRAW
// The scale is a pure function of the unit's generation -- a globally unique,
// never-reissued id both stores already hand out in spawn order. That keeps
// it deterministic (the generation sequence is) without threading an Rng into
// every spawner, and, more importantly, without consuming draws from the sim
// generator: adding the stream shifted no existing random sequence, so a run
// with jitter 0 is bit-identical to one from before the feature existed.
#pragma once

#include "core/Types.h"

namespace immune::sim {

/// Scale for the unit with unique id `generation`, uniform in
/// [1 - jitter, 1 + jitter]. jitter <= 0 returns exactly 1. Clamped below 1
/// so a mistyped config cannot produce a zero or negative body.
inline f32 spawn_size_scale(u32 generation, f32 jitter) {
    if (!(jitter > 0.0f)) return 1.0f;
    if (jitter > 0.9f) jitter = 0.9f;
    // lowbias32 (Wellons): a full-avalanche integer mix, so consecutive ids
    // land far apart instead of making a burst grow in a visible ramp.
    u32 h = generation ^ 0x9e3779b9u;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    const f32 u = static_cast<f32>(h >> 8u) * 0x1.0p-24f;   // [0, 1)
    return 1.0f + jitter * (2.0f * u - 1.0f);
}

} // namespace immune::sim
