// sim/zone/SlowZones.h — timed slow circles on the ground.
// Owner: the swarmer-roster redesign (Interferon).
//
// WHAT A SLOW ZONE IS
// The Interferon's swarmers are suicide bombers that deal no damage: where one
// detonates it leaves a circle on the tissue for `duration` seconds, and
// anything walking through that circle is slowed. The slow OUTLIVES the
// contact: an agent that steps out keeps its slow for `slow_duration` more
// seconds, and an agent that stays inside has that clock refreshed every tick,
// so "still standing in it" and "just left it" both read the same way — slow —
// and only "left it a while ago" reads as recovered.
//
// WHY THIS IS NOT A DamageField
// A DamageField publishes DAMAGE, and the damage system's whole cost model
// (sim/damage/DamageField.h) is built on never testing agent pairs and never
// writing state. A slow is state: it has to land on a specific agent's slot
// and stay there after the zone is gone. So this layer does the one thing that
// file refuses to, on a budget it can afford: one spatial-hash circle query per
// live zone per tick, and one O(n) sweep over the chaff flags to expire slows
// that have run out. Zones are few (tens at most), so the query side is cheap;
// the sweep is a flags scan and vectorizes.
//
// It touches two things a zone must own together, because the flag alone is
// not the debuff: chaff_flags::kSlowed says "slowed", and the parallel
// ChaffBuffers::slow_remaining / slow_factor streams say for how long and how
// hard. Named agents get the same treatment through comp::Slowed, which this
// layer cannot see (it knows nothing about the ECS) — SimWorld applies that
// half after calling update(), from the same zone list.
//
// DETERMINISM
// Runs inside the fixed tick and contributes to state_hash() through the chaff
// streams it writes. Zones are walked in insertion order; an agent under two
// zones takes the LAST one's factor and the longer of the two clocks.
//
// This layer is SIMULATION. The frost ring on the ground is drawn by the
// renderer's field pass from zones(); the detonation puff is a CombatEvent.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune::sim {

class ChaffBuffers;
class SpatialHash;

struct SlowZone {
    Vec2 origin{0.0f, 0.0f};
    f32 radius = 3.0f;
    /// Seconds the circle itself persists. <= 0 retires it on the next update.
    f32 remaining = 0.0f;
    /// What `remaining` started at, so the renderer can fade it honestly.
    f32 duration = 0.0f;
    /// Seconds an agent stays slowed after its last tick inside the circle.
    f32 slow_duration = 1.5f;
    /// Max-speed multiplier applied while slowed. 0.4 = 40% speed.
    f32 slow_factor = 0.4f;
    u8 family_mask = 0xFF;
    /// Owning tower, so the renderer can tint by source and telemetry can
    /// attribute the crowd control (nothing does yet).
    EntityId owner{};
    TowerType source = TowerType::Interferon;
    u16 visual_id = 0;
};

struct SlowZoneStats {
    u32 live = 0;
    u32 expired = 0;
    u32 agents_slowed = 0;    ///< Slot writes this tick (refreshes included).
    u32 slows_expired = 0;    ///< kSlowed bits cleared this tick.
};

class SlowZoneSystem {
public:
    /// Reserves zone storage so spawn() never allocates during a tick.
    void reserve(usize max_zones);

    /// Adds a zone. Silently dropped (returns false) when full — a missing
    /// circle is a cosmetic loss, a mid-tick reallocation is not acceptable.
    bool spawn(const SlowZone& zone);

    /// One tick: expire finished slows on chaff, then let every live zone
    /// refresh the slow on what it covers, then retire spent zones.
    SlowZoneStats update(ChaffBuffers& chaff, const SpatialHash& hash, f32 dt);

    const std::vector<SlowZone>& zones() const { return zones_; }
    usize count() const { return zones_.size(); }
    usize capacity() const { return capacity_; }

    void clear() { zones_.clear(); }

    const SlowZoneStats& last_stats() const { return last_; }

private:
    std::vector<SlowZone> zones_;
    usize capacity_ = 0;
    SlowZoneStats last_{};
    /// Scratch for the per-zone hash queries; allocated once, reused.
    std::vector<u32> scratch_;
};

} // namespace immune::sim
