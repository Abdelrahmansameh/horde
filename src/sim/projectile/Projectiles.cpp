// sim/projectile/Projectiles.cpp — SoA projectile store + the per-tick
// integrate/collide/retire kernel. Owner: Wave 6A.
// Projectiles.h is a frozen contract; this file implements it only.
//
// SHAPE OF THE TICK
//   1. Three straight-line loops integrate pos_x, pos_y and decrement life over
//      [0, count). Every slot in that range is alive (invariant P1), so there is
//      no branch inside them and MSVC auto-vectorizes all three.
//   2. One pass per round resolves expiry, out-of-bounds, and collision. This is
//      the branchy pass and it is kept separate from (1) on purpose: mixing the
//      cell lookup into the integration loop would kill vectorization of the
//      part that is trivially vectorizable.
//   3. compact() swap-removes the retired rounds.
//
// WHY COLLISION LOOKS "WRONG" AND IS NOT
// Projectiles.h states the design decision: a round tests ONLY the spatial-hash
// cell it currently occupies, and damages at most ONE agent per tick. Do not
// "fix" this into a 3x3 neighbourhood, a swept segment, or a nearest-candidate
// search. The cost model is the point — one cell lookup per live round per
// tick, so the Gunner's cost tracks round count and is independent of the 10k
// chaff around it. At 60 Hz the player reads a stream of impacts; the missed
// grazes are invisible and the budget is not.
//
// Consequences accepted by that decision, spelled out so they are not read as
// bugs later:
//   - A fast round can tunnel: if one dt of travel steps it clean over a cell,
//     the agents in that cell are never tested. Callers bound this by not
//     firing rounds whose per-tick step greatly exceeds the hash cell size.
//   - The victim is the first family-matching agent inside hit_radius in
//     ascending chaff-index order, not the nearest one. "First in index order"
//     is what makes it deterministic; "nearest" would cost a full scan of the
//     cell for a difference nobody can see.
//   - A piercing round may re-hit the same agent on a later tick. Remembering
//     victims would mean a per-round hit list, i.e. a per-round allocation,
//     which CONVENTIONS.md §3 forbids outright.
//
// DETERMINISM
// This pass draws nothing from `rng` (see the note at the head of update()) and
// reads no clock. Its only ordering dependency is the ascending CSR order the
// SpatialHash build guarantees, so the result is a pure function of the store,
// the chaff state, and dt — identical at any thread count.
//
// EVENTS ARE AN OUTPUT
// Every CombatEvent is built from values that are already decided by the time
// it is pushed, inside an `if (events)` that touches no sim state. Attaching or
// detaching the sink cannot move one bit of state_hash(); test_projectiles.cpp
// asserts exactly that by running the same scenario twice.
#include "sim/projectile/Projectiles.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <cassert>

namespace immune::sim {

namespace {

bool family_matches(u8 mask, u8 family) {
    return (mask & static_cast<u8>(1u << family)) != 0;
}

/// Fills the fields every projectile-raised event shares. `source` stays at
/// TowerType::Count: a round carries an owning EntityId, not a tower type, and
/// resolving one to the other would need an ECS lookup that sim/projectile has
/// no business doing on the hot path. The VFX layer keys a projectile's look on
/// `visual_id`, which is exactly what that field is for.
CombatEvent make_event(CombatEventType type, Vec2 pos, Vec2 vel, u16 visual_id, f32 radius) {
    CombatEvent e;
    e.type = type;
    e.visual_id = visual_id;
    e.source = TowerType::Count;
    e.target_family = PathogenFamily::Count;
    e.origin = pos;
    e.secondary = pos;
    e.direction = math::normalize_safe(vel);
    e.radius = radius;
    return e;
}

} // namespace

void ProjectileBuffers::reserve(usize max_projectiles) {
    capacity_ = max_projectiles;
    pos_x.assign(max_projectiles, 0.0f);
    pos_y.assign(max_projectiles, 0.0f);
    vel_x.assign(max_projectiles, 0.0f);
    vel_y.assign(max_projectiles, 0.0f);
    damage.assign(max_projectiles, 0.0f);
    life.assign(max_projectiles, 0.0f);
    hit_radius.assign(max_projectiles, 0.0f);
    family_mask.assign(max_projectiles, 0u);
    flags.assign(max_projectiles, 0u);
    visual_id.assign(max_projectiles, 0u);
    owner.assign(max_projectiles, EntityId{});
    clear();
}

void ProjectileBuffers::clear() {
    count_ = 0;
    for (usize i = 0; i < capacity_; ++i) flags[i] = 0;
    assert_invariants();
}

bool ProjectileBuffers::spawn(const ProjectileSpawnParams& p) {
    if (count_ >= capacity_) return false;   // P3: never grows.
    const usize i = count_++;
    pos_x[i] = p.position.x;
    pos_y[i] = p.position.y;
    vel_x[i] = p.velocity.x;
    vel_y[i] = p.velocity.y;
    damage[i] = p.damage;
    life[i] = p.lifetime;
    hit_radius[i] = p.hit_radius;
    family_mask[i] = p.family_mask;
    flags[i] = static_cast<u8>((p.flags | projectile_flags::kAlive) &
                               ~projectile_flags::kPendingKill);
    visual_id[i] = p.visual_id;
    owner[i] = p.owner;
    return true;
}

void ProjectileBuffers::kill(usize index) {
    if (index >= count_) return;
    flags[index] |= projectile_flags::kPendingKill;
}

usize ProjectileBuffers::compact() {
    usize removed = 0;
    usize i = 0;
    while (i < count_) {
        if ((flags[i] & projectile_flags::kPendingKill) == 0) { ++i; continue; }
        const usize last = count_ - 1;
        if (i != last) {
            pos_x[i] = pos_x[last];
            pos_y[i] = pos_y[last];
            vel_x[i] = vel_x[last];
            vel_y[i] = vel_y[last];
            damage[i] = damage[last];
            life[i] = life[last];
            hit_radius[i] = hit_radius[last];
            family_mask[i] = family_mask[last];
            flags[i] = flags[last];
            visual_id[i] = visual_id[last];
            owner[i] = owner[last];
        }
        --count_;
        flags[count_] = 0;
        ++removed;
        // Do not advance i: the swapped-in round must be tested too.
    }
    assert_invariants();
    return removed;
}

void ProjectileBuffers::assert_invariants() const {
#ifndef NDEBUG
    assert(count_ <= capacity_);                                  // P3
    assert(pos_x.size() == capacity_ && pos_y.size() == capacity_);  // P2
    for (usize i = 0; i < count_; ++i) {
        assert((flags[i] & projectile_flags::kAlive) != 0);        // P1
    }
#endif
}

ProjectileStats ProjectileSystem::update(ProjectileBuffers& projectiles,
                                         ChaffBuffers& chaff,
                                         const SpatialHash& hash,
                                         const Rect& world_bounds,
                                         Rng& rng,
                                         f32 dt,
                                         CombatEventSink* events) {
    // `rng` is threaded through because the frozen signature reserves the right
    // to add stochastic collision behaviour later, but this pass deliberately
    // draws NOTHING from it. Muzzle spread is applied by the firing tower when
    // it builds ProjectileSpawnParams; consuming a draw here would advance the
    // shared sim stream every tick and shift every downstream system's numbers
    // as a side effect of how many rounds happened to be in flight.
    (void)rng;

    ProjectileStats stats;

    // Rounds the towers appended since the previous update(). The system has no
    // spawn entry point of its own -- callers push straight into the store --
    // so "spawned this tick" is the growth of count() since we last looked.
    // Saturating, because an external clear() can shrink it.
    const usize entry_count = projectiles.count();
    const usize prev_live = static_cast<usize>(last_.live);
    stats.spawned_this_tick =
        static_cast<u32>(entry_count > prev_live ? entry_count - prev_live : 0);

    if (entry_count == 0 || dt <= 0.0f) {
        stats.live = static_cast<u32>(entry_count);
        last_ = stats;
        return last_;
    }

    // ---- 1. Integration. Branch-free over [0, count) by invariant P1. -------
    f32* __restrict px = projectiles.pos_x.data();
    f32* __restrict py = projectiles.pos_y.data();
    const f32* __restrict vx = projectiles.vel_x.data();
    const f32* __restrict vy = projectiles.vel_y.data();
    f32* __restrict life = projectiles.life.data();

    for (usize i = 0; i < entry_count; ++i) px[i] += vx[i] * dt;
    for (usize i = 0; i < entry_count; ++i) py[i] += vy[i] * dt;
    for (usize i = 0; i < entry_count; ++i) life[i] -= dt;

    // ---- 2. Expiry, bounds, and cell-local collision. -----------------------
    const f32* __restrict cpx = chaff.pos_x.data();
    const f32* __restrict cpy = chaff.pos_y.data();
    const u8* __restrict cflags = chaff.flags.data();
    const u8* __restrict cfamily = chaff.family.data();
    const u32* __restrict cell_indices = hash.indices();
    const usize chaff_count = chaff.count();
    const usize indexed = hash.indexed_count();

    const f32* __restrict pdamage = projectiles.damage.data();
    const f32* __restrict pradius = projectiles.hit_radius.data();
    const u8* __restrict pmask = projectiles.family_mask.data();
    u8* __restrict pflags = projectiles.flags.data();
    const u16* __restrict pvisual = projectiles.visual_id.data();

    for (usize i = 0; i < entry_count; ++i) {
        const Vec2 p{px[i], py[i]};

        // Timeout and "left the world" are the same outcome to the sim and the
        // same event to the VFX layer; both retire the round without a hit.
        if (life[i] <= 0.0f || !world_bounds.contains(p)) {
            pflags[i] |= projectile_flags::kPendingKill;
            ++stats.expired;
            if (events) {
                CombatEvent e = make_event(CombatEventType::ProjectileExpired, p,
                                           Vec2{vx[i], vy[i]}, pvisual[i], pradius[i]);
                e.magnitude = pdamage[i];   // the damage this round never delivered
                events->push(e);
            }
            continue;
        }

        if (indexed == 0 || chaff_count == 0) continue;

        u32 begin = 0, end = 0;
        hash.cell_range(hash.cell_index(hash.cell_coord(p)), begin, end);
        if (begin >= end) continue;
        if (end > indexed) end = static_cast<u32>(indexed);

        const f32 r = pradius[i];
        const f32 r2 = r * r;
        const u8 mask = pmask[i];

        for (u32 slot = begin; slot < end; ++slot) {
            const u32 idx = cell_indices[slot];
            if (idx >= chaff_count) continue;
            const u8 af = cflags[idx];
            // Skip agents already dying this tick: a round spent on a corpse is
            // a round that visibly fails to kill anything, and it matches the
            // gate sim/damage applies for the same reason.
            if ((af & chaff_flags::kAlive) == 0) continue;
            if ((af & chaff_flags::kPendingKill) != 0) continue;
            if (!family_matches(mask, cfamily[idx])) continue;

            const f32 dx = cpx[idx] - p.x;
            const f32 dy = cpy[idx] - p.y;
            if (dx * dx + dy * dy > r2) continue;

            const f32 before = chaff.density[idx];
            chaff.apply_density_loss(idx, pdamage[i]);
            const f32 removed = before - chaff.density[idx];
            if (removed <= 0.0f) continue;   // nothing left to take; keep looking

            ++stats.impacts;
            stats.density_removed += removed;

            if (events) {
                CombatEvent e = make_event(CombatEventType::ProjectileImpact, p,
                                           Vec2{vx[i], vy[i]}, pvisual[i], r);
                e.target_family = static_cast<PathogenFamily>(cfamily[idx]);
                e.magnitude = removed;
                events->push(e);
            }

            // A piercing round passes through and keeps flying; everything else
            // is spent on its first victim.
            if ((pflags[i] & projectile_flags::kPiercing) == 0) {
                pflags[i] |= projectile_flags::kPendingKill;
            }
            break;   // one agent per round per tick, by design.
        }
    }

    // ---- 3. Retire. Chaff compaction is the CALLER's job, once, after every
    // damage source has run (Projectiles.h and DamageField.h both say so).
    projectiles.compact();

    stats.live = static_cast<u32>(projectiles.count());
    last_ = stats;
    return last_;
}

} // namespace immune::sim
