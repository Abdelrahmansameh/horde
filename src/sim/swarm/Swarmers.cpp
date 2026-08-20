// sim/swarm/Swarmers.cpp — the per-tick seek / attach / drain kernel.
// Swarmers.h states the contract and the cost model; this file implements it.
//
// SHAPE OF THE TICK, and why it is one branchy pass instead of three clean ones
// ProjectileSystem splits integration (vectorizable, branch-free) from
// collision (branchy) precisely so the compiler can vectorize the first half.
// That split does not pay here and is not attempted: a swarmer's velocity for
// this tick depends on whether it has a host, where that host is, and whether
// it just latched, so there is no position update that can run ahead of the
// decision. Faking the split would mean walking the streams twice to save
// nothing. One pass, stated plainly, is the honest structure.
//
// THE FOUR STATES a swarmer moves through, in the order this file handles them:
//   1. HOSTLESS  — no valid target. Pays one spatial query to find the nearest
//                  targetable agent, then steers at it.
//   2. SEEKING   — has a host, not yet in range. Steers toward it with a wander
//                  term; this is where the cloud gets its shape.
//   3. ATTACHED  — inside attach_radius. Rides the host and drains it.
//   4. RETIRED   — lifetime exhausted, or out of world. Dissolves.
// A swarmer can traverse 1 -> 2 -> 3 in a single tick, and drops from 3 back to
// 1 the tick after its host dies.
//
// WHY THE WANDER IS NOT FROM THE SHARED Rng
// Swarmers.h explains the determinism reason. The visual reason is just as
// binding: a shared draw would give every swarmer alive on the same tick a
// correlated nudge, and a cloud that jitters in unison reads as one object
// vibrating rather than as many small things moving independently. The private
// per-swarmer stream is what makes it a swarm.
#include "sim/swarm/Swarmers.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <cmath>

namespace immune::sim {

namespace {

bool family_matches(u8 mask, u8 family) {
    return (mask & static_cast<u8>(1u << family)) != 0;
}

/// A swarmer may latch onto anything alive, of a matching family, and not
/// already dying this tick. Hidden (burrowed) agents are deliberately NOT
/// excluded: a granule that has already been released does not care that its
/// host tucked itself into the tissue.
bool targetable(const ChaffBuffers& chaff, u32 idx, u8 mask) {
    if (idx >= chaff.count()) return false;
    const u8 f = chaff.flags[idx];
    if ((f & chaff_flags::kAlive) == 0) return false;
    if ((f & chaff_flags::kPendingKill) != 0) return false;
    return family_matches(mask, chaff.family[idx]);
}

/// PCG-flavoured step on a per-swarmer state word. Same algorithm family as
/// core/Rng.h so the quality is known, but a private stream per swarmer — see
/// the note at the head of this file for why that matters twice over.
u32 next_rand(u32& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

f32 signed_unit(u32& state) {
    return static_cast<f32>(next_rand(state) >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

CombatEvent make_event(CombatEventType type, Vec2 pos, Vec2 dir, u16 visual_id) {
    CombatEvent e;
    e.type = type;
    e.visual_id = visual_id;
    // Named as the tower that owns every swarmer in the game today, so the VFX
    // layer picks the Cytotoxic T palette without an ECS lookup on the hot path.
    e.source = TowerType::CytotoxicT;
    e.target_family = PathogenFamily::Count;
    e.origin = pos;
    e.secondary = pos;
    e.direction = math::normalize_safe(dir);
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

void SwarmerBuffers::reserve(usize max_swarmers) {
    capacity_ = max_swarmers;
    pos_x.assign(max_swarmers, 0.0f);
    pos_y.assign(max_swarmers, 0.0f);
    vel_x.assign(max_swarmers, 0.0f);
    vel_y.assign(max_swarmers, 0.0f);
    dps.assign(max_swarmers, 0.0f);
    life.assign(max_swarmers, 0.0f);
    attach_radius.assign(max_swarmers, 0.0f);
    search_radius.assign(max_swarmers, 0.0f);
    speed.assign(max_swarmers, 0.0f);
    target_index.assign(max_swarmers, 0u);
    target_generation.assign(max_swarmers, 0u);
    family_mask.assign(max_swarmers, 0u);
    flags.assign(max_swarmers, 0u);
    visual_id.assign(max_swarmers, 0u);
    seed.assign(max_swarmers, 0u);
    owner.assign(max_swarmers, EntityId{});
    clear();
}

void SwarmerBuffers::clear() {
    count_ = 0;
    for (usize i = 0; i < capacity_; ++i) flags[i] = 0;
}

bool SwarmerBuffers::spawn(const SwarmerSpawnParams& p) {
    if (count_ >= capacity_) return false;   // S3: never grows.
    const usize i = count_++;
    pos_x[i] = p.position.x;
    pos_y[i] = p.position.y;
    vel_x[i] = p.velocity.x;
    vel_y[i] = p.velocity.y;
    dps[i] = p.damage_per_second;
    life[i] = p.lifetime;
    attach_radius[i] = p.attach_radius;
    search_radius[i] = p.search_radius;
    speed[i] = p.speed;
    target_index[i] = 0u;
    target_generation[i] = 0u;      // invalid handle => starts hostless
    family_mask[i] = p.family_mask;
    flags[i] = swarmer_flags::kAlive;
    visual_id[i] = p.visual_id;
    // Never zero: a zero state word makes the LCG produce a fixed sequence
    // shared by every swarmer that happened to get it.
    seed[i] = p.seed | 1u;
    owner[i] = p.owner;
    return true;
}

void SwarmerBuffers::kill(usize index) {
    if (index >= count_) return;
    flags[index] |= swarmer_flags::kPendingKill;
}

usize SwarmerBuffers::compact() {
    usize removed = 0;
    usize i = 0;
    while (i < count_) {
        if ((flags[i] & swarmer_flags::kPendingKill) == 0) { ++i; continue; }
        const usize last = count_ - 1;
        if (i != last) {
            pos_x[i] = pos_x[last];
            pos_y[i] = pos_y[last];
            vel_x[i] = vel_x[last];
            vel_y[i] = vel_y[last];
            dps[i] = dps[last];
            life[i] = life[last];
            attach_radius[i] = attach_radius[last];
            search_radius[i] = search_radius[last];
            speed[i] = speed[last];
            target_index[i] = target_index[last];
            target_generation[i] = target_generation[last];
            family_mask[i] = family_mask[last];
            flags[i] = flags[last];
            visual_id[i] = visual_id[last];
            seed[i] = seed[last];
            owner[i] = owner[last];
        }
        flags[last] = 0;
        --count_;
        ++removed;
        // The survivor swapped down into slot i has not been tested yet, so i
        // deliberately does not advance. Same pattern as ChaffBuffers::compact.
    }
    return removed;
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

SwarmerStats SwarmerSystem::update(SwarmerBuffers& sw,
                                   ChaffBuffers& chaff,
                                   const SpatialHash& hash,
                                   const Rect& world_bounds,
                                   Rng& rng,
                                   f32 dt,
                                   CombatEventSink* events) {
    // Threaded through for signature symmetry with ProjectileSystem, and
    // deliberately unused: every stochastic choice a swarmer makes comes from
    // its own seed stream (see the file header).
    (void)rng;

    SwarmerStats stats;

    const usize entry_count = sw.count();
    const usize prev_live = static_cast<usize>(last_.live);
    stats.spawned_this_tick =
        static_cast<u32>(entry_count > prev_live ? entry_count - prev_live : 0);

    if (entry_count == 0 || dt <= 0.0f) {
        stats.live = static_cast<u32>(entry_count);
        last_ = stats;
        return last_;
    }

    for (usize i = 0; i < entry_count; ++i) {
        sw.life[i] -= dt;

        Vec2 p{sw.pos_x[i], sw.pos_y[i]};

        // ---- 4. Retirement, checked first so a spent swarmer never gets to
        // drain anything on the tick it dies.
        if (sw.life[i] <= 0.0f || !world_bounds.contains(p)) {
            sw.flags[i] |= swarmer_flags::kPendingKill;
            ++stats.expired;
            if (events) {
                events->push(make_event(CombatEventType::ProjectileExpired, p,
                                        Vec2{sw.vel_x[i], sw.vel_y[i]}, sw.visual_id[i]));
            }
            continue;
        }

        const u8 mask = sw.family_mask[i];

        // ---- Resolve the current host. A handle that no longer resolves, or
        // resolves to something no longer targetable, means the host died —
        // which is the ordinary end of a successful attachment, not an error.
        usize host = ChaffBuffers::npos;
        const ChaffHandle held = sw.target(i);
        if (held.valid()) {
            host = chaff.resolve(held);
            if (host != ChaffBuffers::npos && !targetable(chaff, static_cast<u32>(host), mask)) {
                host = ChaffBuffers::npos;
            }
            if (host == ChaffBuffers::npos) {
                // Only count a finished host for a swarmer that was actually
                // latched: a seeking swarmer whose quarry died to something
                // else has not finished anything.
                if ((sw.flags[i] & swarmer_flags::kAttached) != 0) ++stats.hosts_finished;
                sw.target_generation[i] = 0u;
                sw.flags[i] &= static_cast<u8>(~swarmer_flags::kAttached);
            }
        }

        // ---- 1. HOSTLESS: pay for a search. The only expensive path here.
        if (host == ChaffBuffers::npos) {
            ++stats.searching;
            scratch_.clear();
            hash.query_circle(p, sw.search_radius[i], scratch_);

            u32 best = 0;
            bool found = false;
            f32 best_d2 = 0.0f;
            const f32 r2 = sw.search_radius[i] * sw.search_radius[i];
            for (const u32 idx : scratch_) {
                if (!targetable(chaff, idx, mask)) continue;
                const f32 dx = chaff.pos_x[idx] - p.x;
                const f32 dy = chaff.pos_y[idx] - p.y;
                const f32 d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;
                // Strictly-less keeps the tie-break at "lowest chaff index",
                // which is what makes the choice reproducible.
                if (!found || d2 < best_d2) {
                    found = true;
                    best = idx;
                    best_d2 = d2;
                }
            }

            if (found) {
                host = best;
                sw.target_index[i] = best;
                sw.target_generation[i] = chaff.generation[best];
            }
        }

        // ---- No host anywhere in reach: drift on, slowing, and let the
        // lifetime run out. Deliberately not killed early — a granule that
        // vanishes the instant its lane is clear makes the cloud pop out of
        // existence between waves instead of dispersing.
        if (host == ChaffBuffers::npos) {
            const f32 damp = 1.0f / (1.0f + 1.5f * dt);
            sw.vel_x[i] *= damp;
            sw.vel_y[i] *= damp;
            sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
            sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
            continue;
        }

        const Vec2 host_pos{chaff.pos_x[host], chaff.pos_y[host]};
        const Vec2 to_host = host_pos - p;
        const f32 dist = math::length(to_host);
        const f32 attach_r = sw.attach_radius[i];

        // ---- 3. ATTACHED: ride the host and drain it.
        if (dist <= attach_r) {
            const bool was_attached = (sw.flags[i] & swarmer_flags::kAttached) != 0;
            sw.flags[i] |= swarmer_flags::kAttached;
            ++stats.attached;

            // Sit just off the host's centre rather than exactly on it, so a
            // dozen swarmers on one agent form a visible clump instead of
            // stacking into a single brighter dot. The offset is derived from
            // the private seed, so it is stable for this swarmer.
            const f32 ang = static_cast<f32>(sw.seed[i] & 0xFFFFu) * (math::kTwoPi / 65536.0f);
            const f32 ring = attach_r * 0.6f;
            sw.pos_x[i] = host_pos.x + std::cos(ang) * ring;
            sw.pos_y[i] = host_pos.y + std::sin(ang) * ring;
            // Carry the host's motion so the clump travels with it.
            sw.vel_x[i] = chaff.vel_x[host];
            sw.vel_y[i] = chaff.vel_y[host];

            // A granule feeding on an agent the Goblet Cell has already
            // weakened (chaff_flags::kMarked) drains it faster, same flag every
            // other damage path in the sim reads (DamageField.cpp,
            // Projectiles.cpp, strike_named's comp::Marked check).
            const f32 drain = (chaff.flags[host] & chaff_flags::kMarked) != 0
                                  ? sw.dps[i] * dt * chaff_flags::kMarkedDamageMultiplier
                                  : sw.dps[i] * dt;
            const f32 before = chaff.density[host];
            chaff.apply_density_loss(host, drain);
            const f32 removed = before - chaff.density[host];
            stats.density_removed += removed;

            if (events && !was_attached) {
                CombatEvent e = make_event(CombatEventType::ProjectileImpact, host_pos,
                                           to_host, sw.visual_id[i]);
                e.target_family = static_cast<PathogenFamily>(chaff.family[host]);
                e.magnitude = removed;
                events->push(e);
            }
            continue;
        }

        // ---- 2. SEEKING: steer at the host, with a wander term.
        sw.flags[i] &= static_cast<u8>(~swarmer_flags::kAttached);

        const Vec2 dir = dist > math::kEpsilon ? to_host / dist : Vec2{1.0f, 0.0f};
        u32 s = sw.seed[i];
        const Vec2 wander{signed_unit(s), signed_unit(s)};
        sw.seed[i] = s;

        // Wander fades out as the swarmer closes, so the cloud is loose and
        // organic on approach but converges cleanly instead of orbiting its
        // target forever.
        const f32 wander_gain = 0.45f * math::saturate(dist / math::max(attach_r * 6.0f, 0.01f));
        Vec2 want = dir + wander * wander_gain;
        want = math::normalize_safe(want);
        if (want.x == 0.0f && want.y == 0.0f) want = dir;

        const f32 spd = sw.speed[i];
        const Vec2 desired = want * spd;

        // Steer rather than snap: the turn rate is what makes a swarmer arc in
        // and overshoot slightly, and the overshoot is most of the "alive" read.
        const f32 turn = math::saturate(9.0f * dt);
        sw.vel_x[i] += (desired.x - sw.vel_x[i]) * turn;
        sw.vel_y[i] += (desired.y - sw.vel_y[i]) * turn;

        sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
        sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
    }

    // Chaff compaction is the CALLER's job, once, after every damage source.
    sw.compact();

    stats.live = static_cast<u32>(sw.count());
    last_ = stats;
    return last_;
}

} // namespace immune::sim
