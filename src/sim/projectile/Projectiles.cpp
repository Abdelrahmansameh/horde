// sim/projectile/Projectiles.cpp — SoA projectile store + the per-tick
// integrate/collide/retire kernel. Owner: Wave 6A.
// Projectiles.h is a frozen contract; this file implements it only.
//
// SHAPE OF THE TICK
//   1. Three straight-line loops integrate pos_x, pos_y and decrement life over
//      [0, count). Every slot in that range is alive (invariant P1), so there is
//      no branch inside them and MSVC auto-vectorizes all three.
//   2. One pass per round resolves expiry, swept wall collision, out-of-bounds,
//      and agent collision. This is the branchy pass and it is kept separate
//      from (1) on purpose: mixing the lookups into the integration loop would
//      kill vectorization of the part that is trivially vectorizable.
//   3. compact() swap-removes the retired rounds.
//
// WHY COLLISION LOOKS "WRONG" AND IS NOT
// Projectiles.h states the design decision: for AGENTS a round tests ONLY the
// spatial-hash cell it currently occupies, and damages at most ONE per tick.
// Do not "fix" this into a 3x3 neighbourhood or nearest-candidate search. Walls
// are different: the tissue grid is traversed along the round's tick segment,
// which is still independent of the 10k chaff around it and prevents obvious
// tunnelling through thin obstacles.
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
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <cassert>
#include <cmath>
#include <limits>

namespace immune::sim {

namespace {

bool family_matches(u8 mask, u8 family) {
    return (mask & static_cast<u8>(1u << family)) != 0;
}

/// Walks a round's centre line through the tissue grid and returns the first
/// wall crossing. This is a grid DDA rather than an endpoint test so a fast
/// round cannot skip across a one-cell obstacle between ticks. An unconfigured
/// mask is the headless/test convention for "no walls".
///
/// AUTHORED walls only (TissueMask::authored_wall): a vessel boundary or a
/// level obstacle stops a round; a runtime block the player laid -- a Fibrin
/// Clot, a Fibroblast's collagen scar -- does not. Those are walls to the
/// horde and to nothing on the player's side (sim/flowfield/RuntimeBlock.h),
/// and a Neutrophil firing over its own team's wall is the point of having
/// one.
bool first_wall_hit(const TissueMask& tissue, Vec2 from, Vec2 to, Vec2& hit) {
    if (tissue.width() <= 0 || tissue.height() <= 0) return false;

    IVec2 cell = tissue.world_to_cell(from);
    const IVec2 end = tissue.world_to_cell(to);
    if (tissue.authored_wall(cell.x, cell.y)) {
        hit = from;
        return true;
    }

    const Vec2 delta = to - from;
    const i32 step_x = delta.x > 0.0f ? 1 : (delta.x < 0.0f ? -1 : 0);
    const i32 step_y = delta.y > 0.0f ? 1 : (delta.y < 0.0f ? -1 : 0);
    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 cell_size = tissue.cell_size();
    const Vec2 origin = tissue.world_origin();

    const f32 next_x = origin.x + static_cast<f32>(cell.x + (step_x > 0 ? 1 : 0)) * cell_size;
    const f32 next_y = origin.y + static_cast<f32>(cell.y + (step_y > 0 ? 1 : 0)) * cell_size;
    f32 t_max_x = step_x == 0 ? inf : (next_x - from.x) / delta.x;
    f32 t_max_y = step_y == 0 ? inf : (next_y - from.y) / delta.y;
    const f32 t_delta_x = step_x == 0 ? inf : cell_size / std::abs(delta.x);
    const f32 t_delta_y = step_y == 0 ? inf : cell_size / std::abs(delta.y);

    while (cell.x != end.x || cell.y != end.y) {
        f32 t = 0.0f;
        if (t_max_x < t_max_y) {
            cell.x += step_x;
            t = t_max_x;
            t_max_x += t_delta_x;
        } else {
            cell.y += step_y;
            t = t_max_y;
            t_max_y += t_delta_y;
        }
        if (t > 1.0f) break;
        if (tissue.authored_wall(cell.x, cell.y)) {
            hit = from + delta * math::clamp(t, 0.0f, 1.0f);
            return true;
        }
    }
    return false;
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
    // Neutrophil shooters are the only source of real projectiles. Carrying
    // that identity makes impacts and fizzles use the same warm yellow as the
    // bullet renderer rather than the environmental-event fallback palette.
    e.source = TowerType::Neutrophil;
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
                                         const TissueMask& tissue,
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
    const EntityId* __restrict powner = projectiles.owner.data();

    for (usize i = 0; i < entry_count; ++i) {
        const Vec2 p{px[i], py[i]};

        // Timeout retires before collision: a round with no life left cannot
        // land a hit on the same tick.
        if (life[i] <= 0.0f) {
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

        // Vessel boundaries and authored obstacles stop a round; the player's
        // own runtime blocks (clots, scars) do not -- see first_wall_hit.
        // Sweep from the previous position so a round cannot tunnel through
        // a thin wall.
        Vec2 wall_hit;
        const Vec2 previous = p - Vec2{vx[i], vy[i]} * dt;
        if (first_wall_hit(tissue, previous, p, wall_hit)) {
            pflags[i] |= projectile_flags::kPendingKill;
            ++stats.wall_impacts;
            if (events) {
                CombatEvent e = make_event(CombatEventType::ProjectileImpact, wall_hit,
                                           Vec2{vx[i], vy[i]}, pvisual[i], pradius[i]);
                e.magnitude = 1.0f;
                events->push(e);
            }
            continue;
        }

        // Leaving the simulation rectangle is still an expiry, unless the
        // swept path met a tissue wall first (handled above).
        if (!world_bounds.contains(p)) {
            pflags[i] |= projectile_flags::kPendingKill;
            ++stats.expired;
            if (events) {
                CombatEvent e = make_event(CombatEventType::ProjectileExpired, p,
                                           Vec2{vx[i], vy[i]}, pvisual[i], pradius[i]);
                e.magnitude = pdamage[i];
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

            // A round landing on an agent the Goblet Cell has already weakened
            // (chaff_flags::kMarked) hits harder, exactly like every other
            // damage path in the sim reads the same flag (DamageField.cpp,
            // Swarmers.cpp, and strike_named's comp::Marked check).
            const f32 dmg = (af & chaff_flags::kMarked) != 0
                                ? pdamage[i] * chaff_flags::kMarkedDamageMultiplier
                                : pdamage[i];
            const f32 before = chaff.density[idx];
            chaff.apply_density_loss(idx, dmg);
            const f32 removed = before - chaff.density[idx];
            if (removed <= 0.0f) continue;   // nothing left to take; keep looking

            ++stats.impacts;
            stats.density_removed += removed;

            // Off by default; see sim/Attribution.h. `removed` is the clamped
            // effect, not the nominal damage, so an over-killing round is not
            // credited with density that was never there.
            if (attribution_ != nullptr && powner[i].valid()) {
                const bool killed = (chaff.flags[idx] & chaff_flags::kPendingKill) != 0;
                attribution_->record_chaff(powner[i], cfamily[idx], removed, killed);
            }

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
