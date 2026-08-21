// sim/Attribution.h — opt-in per-owner damage accounting. NEW MODULE.
//
// RATIONALE
// Damage in this game is aggregate by construction: sim/damage thins a density
// field, sim/projectile takes a bite out of one agent, and neither has any
// reason to care which tower caused it. That is right for the sim and useless
// for balance, where the only question that matters is "what did each tower
// actually earn for what it cost".
//
// Both damage paths already carry the answer -- DamageField::owner and
// ProjectileBuffers::owner have said "for kill attribution" since Wave 2A --
// so this header adds the sink they were waiting for and nothing else.
//
// COST WHEN OFF
// The sink is a nullable pointer on DamageSystem/ProjectileSystem, null by
// default and set only by the balance harness. Every recording site is behind
// `if (attribution_ != nullptr)`, so normal play and --bench pay one
// perfectly-predicted branch per damaging hit and nothing else. Nothing in the
// sim ever reads what it records, so a run with the sink attached is
// bit-identical to one without: tests/test_autoplay.cpp asserts exactly that.
//
// IDENTITY
// Keyed by owning EntityId. `entries` is a flat vector scanned linearly, which
// is the right shape at this size -- a level has tens of towers, not
// thousands, and the scan stays in one cache line's worth of ids.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune::sim {

/// Everything one owner (a tower, in practice) did to the horde.
///
/// Chaff damage is a density, not a hit count, because that is what the chaff
/// store actually stores; named-agent damage is a hit point total. Keeping the
/// two separate is deliberate -- summing them would produce a number in no
/// unit at all.
struct AttributionEntry {
    EntityId owner{};
    /// Chaff density removed, split by the victim's family.
    f64 density_removed[kFamilyCount] = {};
    /// Chaff agents whose density hit zero on this owner's hit.
    u32 chaff_killed[kFamilyCount] = {};
    /// Hit points taken off named agents (elites/bosses).
    f64 named_damage = 0.0;
    u32 named_killed = 0;
    /// Ticks on which this owner removed anything at all -- the numerator of
    /// the uptime figure the report prints. A tower that is in range of
    /// nothing for half a level is over-priced for where it can be built,
    /// which is invisible in a raw damage total.
    u32 active_ticks = 0;
    /// Set within a tick by record(); folded into active_ticks by mark_tick().
    bool touched_this_tick = false;
};

/// Accumulating sink. Not thread-safe: every recording site runs on the tick
/// thread (damage fields and projectile impacts are both serial passes), and a
/// sink that needed a lock would be a sink that changed the sim's timing.
class DamageAttribution {
public:
    /// Finds or appends the entry for `owner`. Never returns null.
    AttributionEntry& slot(EntityId owner) {
        for (AttributionEntry& e : entries_) {
            if (e.owner == owner) return e;
        }
        entries_.push_back(AttributionEntry{owner});
        return entries_.back();
    }

    void record_chaff(EntityId owner, u8 family, f32 density_removed, bool killed) {
        if (family >= kFamilyCount) return;
        AttributionEntry& e = slot(owner);
        e.density_removed[family] += static_cast<f64>(density_removed);
        if (killed) ++e.chaff_killed[family];
        e.touched_this_tick = true;
    }

    void record_named(EntityId owner, f32 damage, bool killed) {
        AttributionEntry& e = slot(owner);
        e.named_damage += static_cast<f64>(damage);
        if (killed) ++e.named_killed;
        e.touched_this_tick = true;
    }

    /// Call once per tick, after every damage source has run. Rolls the
    /// per-tick "did anything" flag into the uptime counter.
    void mark_tick() {
        for (AttributionEntry& e : entries_) {
            if (e.touched_this_tick) ++e.active_ticks;
            e.touched_this_tick = false;
        }
    }

    const std::vector<AttributionEntry>& entries() const { return entries_; }
    void clear() { entries_.clear(); }

private:
    std::vector<AttributionEntry> entries_;
};

} // namespace immune::sim
