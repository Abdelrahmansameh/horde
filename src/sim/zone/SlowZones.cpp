// sim/zone/SlowZones.cpp — slow-zone upkeep. SlowZones.h states the contract.
#include "sim/zone/SlowZones.h"

#include "core/Math.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <algorithm>

namespace immune::sim {

void SlowZoneSystem::reserve(usize max_zones) {
    capacity_ = max_zones;
    zones_.clear();
    zones_.reserve(max_zones);
    if (scratch_.capacity() < 2048) scratch_.reserve(2048);
}

bool SlowZoneSystem::spawn(const SlowZone& zone) {
    if (zones_.size() >= capacity_) return false;
    zones_.push_back(zone);
    return true;
}

SlowZoneStats SlowZoneSystem::update(ChaffBuffers& chaff, const SpatialHash& hash, f32 dt) {
    SlowZoneStats stats;

    // ---- 1. Expire. Runs BEFORE the zones refresh, so an agent still inside
    // one never sees its clock hit zero: the refresh below tops it back up on
    // the same tick. Nothing else in the sim clears kSlowed, so this is the
    // only place the bit ever goes away.
    const usize n = chaff.count();
    u8* flags = chaff.flags.data();
    f32* remaining = chaff.slow_remaining.data();
    f32* factor = chaff.slow_factor.data();
    for (usize i = 0; i < n; ++i) {
        if ((flags[i] & chaff_flags::kSlowed) == 0) continue;
        remaining[i] -= dt;
        if (remaining[i] > 0.0f) continue;
        remaining[i] = 0.0f;
        factor[i] = chaff_flags::kDefaultSlowFactor;
        flags[i] &= static_cast<u8>(~chaff_flags::kSlowed);
        ++stats.slows_expired;
    }

    // ---- 2. Refresh. One circle query per zone; the zone writes the slot of
    // every matching agent it covers. Hidden agents are NOT exempt — a
    // burrowed pathogen is untargetable, not untouchable, and the aggregate
    // paths (fields, fluid) touch it too.
    for (SlowZone& z : zones_) {
        z.remaining -= dt;
        if (z.remaining <= 0.0f) continue;   // spent: retired below, applies nothing
        scratch_.clear();
        hash.query_circle(z.origin, z.radius, scratch_);
        const f32 r2 = z.radius * z.radius;
        for (const u32 idx : scratch_) {
            if (idx >= n) continue;
            const u8 f = flags[idx];
            if ((f & chaff_flags::kAlive) == 0 || (f & chaff_flags::kPendingKill) != 0) continue;
            if ((z.family_mask & static_cast<u8>(1u << chaff.family[idx])) == 0) continue;
            const f32 dx = chaff.pos_x[idx] - z.origin.x;
            const f32 dy = chaff.pos_y[idx] - z.origin.y;
            if (dx * dx + dy * dy > r2) continue;
            flags[idx] |= chaff_flags::kSlowed;
            // Longer of the two clocks, so a zone about to expire cannot
            // shorten a slow a fresher zone just granted.
            remaining[idx] = math::max(remaining[idx], z.slow_duration);
            factor[idx] = math::clamp(z.slow_factor, 0.0f, 1.0f);
            ++stats.agents_slowed;
        }
    }

    // ---- 3. Retire. Order-preserving erase, so surviving zones keep their
    // insertion order and the "last zone wins" rule above stays reproducible.
    const usize before = zones_.size();
    zones_.erase(std::remove_if(zones_.begin(), zones_.end(),
                                [](const SlowZone& z) { return z.remaining <= 0.0f; }),
                 zones_.end());
    stats.expired = static_cast<u32>(before - zones_.size());
    stats.live = static_cast<u32>(zones_.size());
    last_ = stats;
    return last_;
}

} // namespace immune::sim
