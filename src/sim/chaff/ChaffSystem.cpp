// Batch chaff movement kernel. Owner: Wave 1B. Fluid-feel rules and wall
// contact added by the movement overhaul (see ChaffSystem.h's tuning fields).
//
// FOUR PASSES, ONE TICK
//
//   A. accumulate  (parallel, per-range Rng)   — flow/drift, then the three
//      local crowd rules (separation, alignment, crowd pressure) from ONE
//      neighbour gather, then jitter, into vel_x/vel_y (UNCLAMPED); plus the
//      per-agent effective max speed and replication rolls into scratch. This
//      is the pass that touches the spatial hash, so it is inherently
//      gather-heavy and does not vectorize — that cost is unavoidable and is
//      what the spatial hash's O(1) cell lookup exists to minimize.
//   B. integrate   (serial, straight-line)     — clamp_length + p += v*dt over
//      contiguous vel_x/vel_y/scratch_max_speed/old_pos_{x,y}/pos_{x,y}. No
//      branches on flags, no hash access, no gather: this is the loop
//      docs/ARCHITECTURE.md means by "the movement loop is written so MSVC
//      auto-vectorizes it" (verified below).
//   B2. wall contact (parallel, SDF gather)    — projects agents out of tissue
//      they overlap and cancels the inbound part of their velocity. Separate
//      from B so B stays vectorizable.
//   C. despawn     (serial)                    — bounds/goal test against the
//      now-final position, flags kPendingKill. Cheap, branchy, not perf-critical.
//
// Splitting out B is also what makes the result scheduling-independent: pass A
// writes only the index each range owns and reads neighbours through snapshots
// of positions AND velocities taken *before* pass A runs (old_pos_*, old_vel_*),
// never through the live arrays another range may already have overwritten.
//
// WHY THIS LOOKS LIKE A FLUID WITHOUT ANY FLUID MATH
// There is no density solve, no pressure projection, no smoothing kernel —
// nothing from SPH or continuum crowds, both of which would mean replacing the
// flow field rather than extending it. Every agent runs the same three cheap
// local rules against its own neighbourhood, and the collective motion (waves
// travelling back through a jam, a mass splashing along a wall and rejoining
// downstream) is emergent, exactly as in Reynolds' boids. Cohesion is
// deliberately omitted: the flow field already supplies "everyone go that way",
// so a cohesion term would only fight it and ball the horde up.
#include "sim/chaff/ChaffSystem.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Rng.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace immune::sim {
namespace {

constexpr f32 kSeparationEpsSq = 1e-8f;

// DESIGN.md leaves the exact NET/snare slow factor as a feel-pass number (no
// data table exists yet — Wave 2C owns tuning). This is the one clearly-named
// constant standing in for it so kSlowed has an observable effect now instead
// of silently doing nothing.
constexpr f32 kSlowedSpeedMultiplier = 0.4f;

/// Everything one agent learns about its neighbourhood, from ONE walk of the
/// 3x3 cells around it.
struct NeighbourSample {
    Vec2 separation{0.0f, 0.0f};   ///< Mean normalized push-away, weighted by closeness.
    Vec2 avg_velocity{0.0f, 0.0f}; ///< Mean neighbour velocity, for alignment.
    /// SUMMED (not averaged) positional correction that resolves actual
    /// interpenetration. Summed on purpose: this is a geometric constraint, not
    /// a preference, so being crowded by ten agents must push ten times as hard
    /// as being crowded by one. Averaging it -- which is right for `separation`,
    /// a steering preference -- is precisely what let dense crowds overlap.
    Vec2 contact_push{0.0f, 0.0f};
    u32 crowd = 0;                 ///< Neighbours inside alignment_radius; drives pressure.
    bool has_alignment = false;
};

/// 3x3-cell neighbourhood gather for agent `i`, reading positions and
/// velocities through the pre-tick snapshots (see file header for why the
/// snapshots exist at all).
///
/// WHY ONE FUNCTION AND NOT THREE
/// Separation, alignment and crowd-pressure each need "the agents near me".
/// Fetching that three times would triple the only genuinely expensive thing in
/// this kernel -- the gather -- to compute three cheap sums. So the walk happens
/// once and all three accumulate off it. The per-neighbour body below is a
/// handful of multiply-adds; the loop around it is the cost.
///
/// Uses the raw cell accessors per SpatialHash.h's guidance: this runs once per
/// agent per tick and must not touch a std::vector.
NeighbourSample gather_neighbours(const SpatialHash& hash,
                                  const f32* px, const f32* py,
                                  const f32* vx, const f32* vy,
                                  usize i, f32 sep_radius, f32 align_radius,
                                  f32 contact_radius, f32 contact_stiffness,
                                  u32 max_sampled) {
    NeighbourSample out;
    const f32 scan_radius = math::max(math::max(sep_radius, align_radius), contact_radius);
    if (scan_radius <= 0.0f) return out;

    const Vec2 p{px[i], py[i]};
    const IVec2 c = hash.cell_coord(p);
    const IVec2 dims = hash.grid_dims();
    const f32 sep_r2 = sep_radius * sep_radius;
    const f32 align_r2 = align_radius * align_radius;
    const f32 contact_r2 = contact_radius * contact_radius;
    const u32* indices = hash.indices();

    const i32 y0 = math::max(0, c.y - 1);
    const i32 y1 = math::min(dims.y - 1, c.y + 1);
    const i32 x0 = math::max(0, c.x - 1);
    const i32 x1 = math::min(dims.x - 1, c.x + 1);

    Vec2 sep{0.0f, 0.0f};
    Vec2 vel{0.0f, 0.0f};
    Vec2 contact{0.0f, 0.0f};
    u32 sep_count = 0;
    u32 align_count = 0;
    u32 sampled = 0;

    for (i32 cy = y0; cy <= y1; ++cy) {
        const u32 row = static_cast<u32>(cy) * static_cast<u32>(dims.x);
        u32 begin, end;
        // A row of cells is contiguous in the CSR array (SpatialHash.cpp lays
        // cells out row-major), so this could be one range instead of per-cell
        // ranges; kept per-cell here for clarity since 3 cells/row is already
        // tiny. See SpatialHash::gather_cells for the row-span version used by
        // the box/circle/cone queries.
        for (i32 cx = x0; cx <= x1; ++cx) {
            hash.cell_range(row + static_cast<u32>(cx), begin, end);
            for (u32 k = begin; k < end; ++k) {
                // The density cap. Truncating in CSR order keeps this a pure
                // function of positions (see ChaffTuning::max_neighbors_sampled)
                // while bounding the worst case a single packed cell can cost.
                if (sampled >= max_sampled) goto done;
                const u32 j = indices[k];
                if (j == i) continue;
                const f32 dx = p.x - px[j];
                const f32 dy = p.y - py[j];
                const f32 d2 = dx * dx + dy * dy;
                if (d2 < kSeparationEpsSq) continue;
                if (d2 >= align_r2 && d2 >= sep_r2 && d2 >= contact_r2) continue;
                ++sampled;

                // One square root, shared by the two rules that need a real
                // distance. Contact is deliberately NOT capped by max_sampled's
                // spirit even though it shares the counter: geometry already
                // bounds how many agents can physically be inside contact_radius
                // at once, so this term is self-limiting in a way the
                // alignment/pressure sums are not.
                const f32 d = std::sqrt(d2);
                const f32 inv_d = 1.0f / d;
                const f32 nx = dx * inv_d;   // unit vector from neighbour to me
                const f32 ny = dy * inv_d;

                if (d2 < contact_r2) {
                    // Half the overlap, because the neighbour independently
                    // computes and applies the other half.
                    const f32 correction = (contact_radius - d) * 0.5f * contact_stiffness;
                    contact.x += nx * correction;
                    contact.y += ny * correction;
                }
                if (d2 < sep_r2) {
                    const f32 push = (sep_radius - d) / sep_radius;  // 1 at d=0, 0 at edge
                    sep.x += nx * push;
                    sep.y += ny * push;
                    ++sep_count;
                }
                if (d2 < align_r2) {
                    vel.x += vx[j];
                    vel.y += vy[j];
                    ++align_count;
                }
            }
        }
    }
done:
    out.contact_push = contact;
    if (sep_count > 0) {
        const f32 inv = 1.0f / static_cast<f32>(sep_count);
        out.separation = Vec2{sep.x * inv, sep.y * inv};
    }
    if (align_count > 0) {
        const f32 inv = 1.0f / static_cast<f32>(align_count);
        out.avg_velocity = Vec2{vel.x * inv, vel.y * inv};
        out.has_alignment = true;
    }
    out.crowd = align_count;
    return out;
}

/// Pushes one agent out of tissue it is overlapping and cancels the part of its
/// velocity heading further in. This is the actual "bumps into the lane wall"
/// behaviour, and it deliberately replaces nothing -- the SDF-gradient term in
/// pass A is still the recovery net for agents that end up fully outside the
/// mask with no flow guidance; this is contact response for agents at the edge.
///
/// Position-based, in the sense the crowd-simulation literature means: rather
/// than adding a repulsive force and hoping it is strong enough before the next
/// tick, the position is projected back onto the legal side immediately, so an
/// agent can never be seen inside a wall regardless of how fast it arrived or
/// how hard the crowd behind it is pushing. That property is exactly what makes
/// a dense jam against a wall hold its shape instead of squeezing through.
void resolve_wall_contact(const DistanceField& sdf, f32& px, f32& py,
                          f32& vx, f32& vy, f32 radius, f32 restitution, f32 splash) {
    const Vec2 p{px, py};
    const f32 clearance = sdf.sample(p);
    if (clearance >= radius) return;   // not touching anything

    Vec2 n = sdf.gradient(p);          // points toward more open tissue
    const f32 n2 = math::length_sq(n);
    if (n2 <= math::kEpsilon) return;  // no usable normal (unbaked field, or a
                                       // perfectly flat plateau) -- leave pass
                                       // A's flow/SDF steering to handle it
    const f32 inv = 1.0f / std::sqrt(n2);
    n.x *= inv;
    n.y *= inv;

    // Positional projection. `clearance` is signed, so an agent that has ended
    // up well inside solid tissue gets a correspondingly large push and is
    // recovered in one tick rather than crawling out over many.
    const f32 penetration = radius - clearance;
    px += n.x * penetration;
    py += n.y * penetration;

    const f32 vn = vx * n.x + vy * n.y;
    if (vn >= 0.0f) return;            // already heading away; nothing to resolve

    // Split the velocity into "into the wall" and "along the wall".
    const f32 tx = vx - n.x * vn;
    const f32 ty = vy - n.y * vn;

    // Cancel the inbound part (optionally bouncing a little of it back).
    const f32 j = vn * (1.0f + restitution);
    vx -= n.x * j;
    vy -= n.y * j;

    // THE SPLASH. Cancelling the inbound component alone just deletes that
    // momentum, so an agent arriving head-on stops dead against the wall and
    // the horde reads as piling up rather than flowing around. A fluid does the
    // opposite: what cannot continue forward is redirected sideways, and the
    // mass keeps moving. So the blocked speed is re-injected along the wall
    // tangent.
    //
    // Direction: follow whatever sideways motion the agent already had, so a
    // crowd sweeping along a wall keeps sweeping the same way instead of
    // scattering. Only when the impact is dead-on (no tangential component at
    // all) is a side chosen arbitrarily -- and then it is chosen from the
    // agent's own position bits, which splits an incoming column to BOTH sides
    // of an obstacle. Picking a fixed side there would send every agent the
    // same way and read as a conveyor belt, not a splash.
    const f32 blocked = -vn * splash;
    if (blocked <= 0.0f) return;

    const f32 t2 = tx * tx + ty * ty;
    f32 ux, uy;
    if (t2 > 1e-6f) {
        const f32 inv_t = 1.0f / std::sqrt(t2);
        ux = tx * inv_t;
        uy = ty * inv_t;
    } else {
        // Deterministic per-agent coin flip from the position bits: same input
        // always gives the same side, so this stays reproducible, but adjacent
        // agents disagree and the column splits.
        u32 bits;
        std::memcpy(&bits, &px, sizeof(bits));
        u32 bits_y;
        std::memcpy(&bits_y, &py, sizeof(bits_y));
        bits ^= bits_y * 2654435761u;
        const f32 sign = (bits & 1u) ? 1.0f : -1.0f;
        ux = -n.y * sign;
        uy = n.x * sign;
    }
    vx += ux * blocked;
    vy += uy * blocked;
}

/// Pass B: branch-free clamp_length + p += v*dt over six contiguous streams.
/// Pulled out into its own small free function on purpose — MSVC's
/// auto-vectorizer has a complexity/size budget per function, and this loop
/// sitting inline inside ChaffSystem::update (a large function with a capturing
/// lambda, atomics, and three other passes) was enough to make it bail and fall
/// back to a scalar 4x-unrolled loop despite being branch-free and __restrict-
/// qualified. Isolated like this it vectorizes cleanly (confirmed with
/// `/Qvec-report:2`: "loop vectorized", packed `mulps`/`addps`/`divps` in the
/// generated code instead of the unrolled `mulss`/`addss`/`divss` it emitted
/// inline). See tests/test_chaff_system.cpp for the behavioural coverage this
/// function must keep passing.
void integrate_and_clamp(f32* __restrict pos_x, f32* __restrict pos_y,
                         f32* __restrict vel_x, f32* __restrict vel_y,
                         const f32* __restrict old_pos_x, const f32* __restrict old_pos_y,
                         const f32* __restrict push_x, const f32* __restrict push_y,
                         const f32* __restrict max_speed, u32 n, f32 dt) {
    for (u32 i = 0; i < n; ++i) {
        const f32 ms = max_speed[i];
        const f32 vxi = vel_x[i];
        const f32 vyi = vel_y[i];
        const f32 l2 = vxi * vxi + vyi * vyi;
        // Branch-free clamp: scale is 1 when already under the limit (or
        // stationary), ms/|v| otherwise. No `if`, no early-out — a straight
        // arithmetic sequence over contiguous memory is what lets the
        // auto-vectorizer pack four agents per SIMD lane.
        const f32 l2c = l2 > math::kEpsilon ? l2 : math::kEpsilon;
        const f32 inv_len = 1.0f / std::sqrt(l2c);
        const f32 raw_scale = ms * inv_len;
        const f32 scale = raw_scale < 1.0f ? raw_scale : 1.0f;
        const f32 nvx = vxi * scale;
        const f32 nvy = vyi * scale;
        vel_x[i] = nvx;
        vel_y[i] = nvy;
        // The contact push is added as DISPLACEMENT, not as another force, and
        // deliberately after the speed clamp: un-overlapping is a geometric
        // correction, so it must not be rationed by max_speed the way steering
        // is. That is the whole reason overlap survived a velocity-only
        // separation impulse. Two extra adds; the loop still vectorizes.
        pos_x[i] = old_pos_x[i] + nvx * dt + push_x[i];
        pos_y[i] = old_pos_y[i] + nvy * dt + push_y[i];
    }
}

} // namespace

void ChaffSystem::ensure_scratch(usize capacity) {
    if (old_pos_x_.size() == capacity) return;   // already sized; reserved-once
    old_pos_x_.assign(capacity, 0.0f);
    old_pos_y_.assign(capacity, 0.0f);
    old_vel_x_.assign(capacity, 0.0f);
    old_vel_y_.assign(capacity, 0.0f);
    contact_push_x_.assign(capacity, 0.0f);
    contact_push_y_.assign(capacity, 0.0f);
    scratch_max_speed_.assign(capacity, 0.0f);
    replicate_wanted_.assign(capacity, 0u);
}

ChaffUpdateStats ChaffSystem::update(ChaffBuffers& buffers, const FlowField& flow,
                                     const DistanceField& sdf, const SpatialHash& hash,
                                     Rng& rng, f32 dt, JobSystem* jobs) {
    ChaffUpdateStats stats{};
    const usize count = buffers.count();
    if (count == 0) return stats;

    ensure_scratch(buffers.capacity());

    // Snapshot pre-tick positions for pass A's separation reads (see rationale
    // in the file header). Plain contiguous copies — trivially vectorizable and
    // cheap relative to the hash-gather pass that follows.
    std::memcpy(old_pos_x_.data(), buffers.pos_x.data(), count * sizeof(f32));
    std::memcpy(old_pos_y_.data(), buffers.pos_y.data(), count * sizeof(f32));
    // Velocities need the same treatment for alignment -- see old_vel_x_'s
    // declaration in the header.
    std::memcpy(old_vel_x_.data(), buffers.vel_x.data(), count * sizeof(f32));
    std::memcpy(old_vel_y_.data(), buffers.vel_y.data(), count * sizeof(f32));

    f32* vx = buffers.vel_x.data();
    f32* vy = buffers.vel_y.data();
    const u8* fam = buffers.family.data();
    const u8* flg = buffers.flags.data();
    const f32* old_px = old_pos_x_.data();
    const f32* old_py = old_pos_y_.data();
    const f32* old_vx = old_vel_x_.data();
    const f32* old_vy = old_vel_y_.data();
    f32* push_x = contact_push_x_.data();
    f32* push_y = contact_push_y_.data();
    f32* max_speed_scratch = scratch_max_speed_.data();
    u8* replicate_wanted = replicate_wanted_.data();
    const ChaffTuning& tuning = tuning_;

    // ---- Pass A: accumulate (parallel, gather-heavy, not vectorized) --------
    std::atomic<u32> replication_rolls{0};

    auto accumulate = [&](usize begin, usize end, u32 range_index) {
        // Exactly one fork per range, as the frozen header requires.
        Rng local_rng = rng.fork(static_cast<u64>(range_index));
        u32 rolls = 0;
        for (usize i = begin; i < end; ++i) {
            const u8 flags_i = flg[i];
            const u32 f = fam[i];
            const ChaffFamilyParams& fp = tuning.family[f < kFamilyCount ? f : 0];

            const bool hidden = (flags_i & chaff_flags::kHidden) != 0;
            if (hidden) {
                // Burrowed: no flow, no separation, no jitter, no replication —
                // reads as the agent going still while only NK Cells can find
                // it (DESIGN.md §5). Zero velocity so pass B is a no-op integrate.
                vx[i] = 0.0f;
                vy[i] = 0.0f;
                max_speed_scratch[i] = 0.0f;
                push_x[i] = 0.0f;
                push_y[i] = 0.0f;
                continue;
            }

            const bool drifting = (flags_i & chaff_flags::kDrifting) != 0;
            const bool slowed = (flags_i & chaff_flags::kSlowed) != 0;

            const Vec2 p{old_px[i], old_py[i]};
            Vec2 dir;
            if (drifting) {
                dir = tuning.ambient_drift;
            } else {
                dir = flow.sample(p);
                if (math::length_sq(dir) <= math::kEpsilon) {
                    // Off the baked walkable region (or a genuine dead
                    // pocket): recover toward higher tissue clearance instead
                    // of leaving the agent with zero net guidance. See this
                    // file's header comment for why this exists.
                    dir = sdf.gradient(p);
                }
            }

            Vec2 v{vx[i], vy[i]};
            v += dir * fp.acceleration * dt;

            // ONE gather feeds all four local rules: separation, alignment,
            // crowd pressure, and the positional contact correction.
            const f32 contact_radius = fp.radius * fp.contact_spacing;
            const NeighbourSample nb =
                gather_neighbours(hash, old_px, old_py, old_vx, old_vy, i,
                                  fp.separation_radius, fp.alignment_radius,
                                  contact_radius, fp.contact_stiffness,
                                  tuning.max_neighbors_sampled);
            push_x[i] = nb.contact_push.x;
            push_y[i] = nb.contact_push.y;

            // Crowd pressure amplifies separation rather than adding a
            // second independent force. Separation already points "away
            // from where everyone is"; when the neighbourhood is packed,
            // that same direction is exactly where the mass needs to
            // relieve into, so scaling it keeps the release coherent
            // instead of adding noise on top.
            f32 push = fp.separation_strength;
            if (fp.pressure_gain > 0.0f &&
                static_cast<f32>(nb.crowd) > fp.pressure_threshold) {
                const f32 excess = static_cast<f32>(nb.crowd) - fp.pressure_threshold;
                const f32 mul = 1.0f + excess * fp.pressure_gain;
                push *= mul < fp.pressure_max ? mul : fp.pressure_max;
            }
            v += nb.separation * push;

            // Alignment: steer toward the neighbourhood's mean velocity.
            // Written as a difference (a steering term, not a velocity
            // assignment) so it can never overrule the flow field -- it
            // biases how the agent gets where it is already going, which is
            // what makes the crowd move as a body without losing the
            // objective.
            if (nb.has_alignment && fp.alignment_strength > 0.0f) {
                v += (nb.avg_velocity - v) * fp.alignment_strength * dt;
            }
            if (fp.jitter > 0.0f) {
                v += local_rng.unit_disc() * fp.jitter;
            }

            vx[i] = v.x;
            vy[i] = v.y;
            max_speed_scratch[i] = fp.max_speed * (slowed ? kSlowedSpeedMultiplier : 1.0f);

            if (fp.replication_rate > 0.0f && local_rng.chance(fp.replication_rate * dt)) {
                replicate_wanted[i] = 1u;
                ++rolls;
            }
        }
        if (rolls != 0) replication_rolls.fetch_add(rolls, std::memory_order_relaxed);
    };

    if (jobs) {
        jobs->parallel_for(count, accumulate);
    } else {
        accumulate(0, count, 0);
    }

    // ---- Pass B: integrate (serial, branch-free, auto-vectorizes) ----------
    // See integrate_and_clamp()'s doc comment for why this is its own function
    // rather than an inline loop.
    f32* px = buffers.pos_x.data();
    f32* py = buffers.pos_y.data();
    integrate_and_clamp(px, py, vx, vy, old_px, old_py, push_x, push_y,
                        max_speed_scratch, static_cast<u32>(count), dt);

    // ---- Pass B2: wall contact (parallel, gather-light) ---------------------
    // Must run after B, because contact is decided against the position the
    // agent actually ended up at, not the one it started from. Kept out of B so
    // B stays the branch-free vectorizable loop described above -- an SDF
    // sample is a bilinear gather and would sink it.
    //
    // Embarrassingly parallel and deterministic: every agent reads only the
    // immutable DistanceField and writes only its own slot. No RNG, no
    // cross-agent reads, so no snapshot needed and no scheduling sensitivity.
    //
    // Skipped entirely when the field was never baked (width 0). Most unit
    // tests pass a default-constructed DistanceField, and its sample() returns
    // 0, which would otherwise read as "every agent is buried in a wall".
    if (sdf.width() > 0 && sdf.height() > 0) {
        auto resolve_range = [&](usize begin, usize end, u32) {
            for (usize i = begin; i < end; ++i) {
                if ((flg[i] & chaff_flags::kHidden) != 0) continue;   // burrowed: not in the world
                const u32 f = fam[i];
                const ChaffFamilyParams& fp = tuning.family[f < kFamilyCount ? f : 0];
                resolve_wall_contact(sdf, px[i], py[i], vx[i], vy[i], fp.radius,
                                     fp.wall_restitution, fp.wall_splash);
            }
        };
        if (jobs) {
            jobs->parallel_for(count, resolve_range);
        } else {
            resolve_range(0, count, 0);
        }
    }

    // ---- Pass C: despawn + replication resolve (serial, deterministic) -----
    u32 despawned_goal = 0;
    u32 despawned_bounds = 0;
    const bool has_goal = goal_radius_ > 0.0f;
    const f32 goal_r2 = goal_radius_ * goal_radius_;
    for (usize i = 0; i < count; ++i) {
        bool killed = false;
        if (has_goal) {
            const f32 dx = px[i] - goal_.x;
            const f32 dy = py[i] - goal_.y;
            if (dx * dx + dy * dy <= goal_r2) {
                buffers.kill(i);
                ++despawned_goal;
                const u32 f = fam[i];
                if (f < kFamilyCount) ++stats.despawned_at_goal_by_family[f];
                killed = true;
            }
        }
        if (!killed && !bounds_.contains(Vec2{px[i], py[i]})) {
            buffers.kill(i);
            ++despawned_bounds;
            const u32 f = fam[i];
            if (f < kFamilyCount) ++stats.despawned_out_of_bounds_by_family[f];
        }
    }

    // Deterministic serial replication resolve, index order — independent of
    // how pass A's ranges were scheduled. spawn() is not safe to call from
    // inside pass A (it mutates buffers.count()), which is why the roll was
    // deferred to `replicate_wanted` instead of spawning inline.
    u32 replicated = 0;
    const u32 cap = tuning_.max_replications_per_tick;
    for (usize i = 0; i < count; ++i) {
        if (!replicate_wanted[i]) continue;
        replicate_wanted[i] = 0;
        if (replicated >= cap || buffers.full()) continue;
        const u32 f = fam[i];
        const ChaffFamilyParams& fp = tuning.family[f < kFamilyCount ? f : 0];
        ChaffSpawnParams sp;
        sp.position = Vec2{px[i], py[i]};
        sp.velocity = Vec2{vx[i], vy[i]};
        sp.family = static_cast<PathogenFamily>(f);
        sp.density = fp.base_density > 0.0f ? fp.base_density : 1.0f;
        sp.flags = chaff_flags::kReplicated;
        if (buffers.spawn(sp).valid()) ++replicated;
    }

    stats.moved = static_cast<u32>(count);
    stats.replicated = replicated;
    stats.despawned_at_goal = despawned_goal;
    stats.despawned_out_of_bounds = despawned_bounds;
    return stats;
}

u32 ChaffSystem::spawn_burst(ChaffBuffers& buffers, PathogenFamily family, Vec2 portal,
                             f32 portal_radius, u32 count, Rng& rng) const {
    const ChaffFamilyParams& fp = tuning_.family[static_cast<u32>(family)];
    if (count == 0) return 0;

    // SPAWN PACKING (was: rng.unit_disc() * portal_radius, i.e. uniform random
    // inside the disc). Uniform random placement puts agents on top of each
    // other by construction -- with n points in a disc the expected number of
    // overlapping pairs is not small, it is the birthday problem, so a burst
    // reliably spawned a knot of interpenetrating agents that then had to be
    // shoved apart by the contact pass over the following ticks. That looked
    // exactly like the thing the contact pass was added to prevent.
    //
    // Replaced with a golden-angle (phyllotaxis) spiral: the arrangement
    // sunflower seeds use. Points land at
    //     r = R * sqrt((i + 0.5) / n),  theta = i * golden_angle
    // which fills the disc at uniform DENSITY -- the sqrt keeps area per point
    // constant -- while the irrational angle guarantees no two points ever line
    // up. Spacing is even by construction, so no rejection sampling, no
    // retries, and cost stays O(1) per agent.
    const f32 kGoldenAngle = 2.39996323f;   // pi * (3 - sqrt(5))

    // Grow the disc if the requested radius cannot hold `count` agents at
    // contact distance. Growing the portal is the right trade: a burst that
    // does not fit has to go somewhere, and spilling slightly wider reads far
    // better than spawning a solid interpenetrating plug in the middle.
    //
    // For a phyllotaxis disc the closest pair sits about 1.55*R/sqrt(n) apart
    // (nearest neighbours are Fibonacci-index offsets, not adjacent indices,
    // which is why the naive uniform-density estimate R*sqrt(pi/n) is too
    // optimistic and let small bursts overlap). Inverting that and keeping a
    // little margin gives the 0.75 below; tests/test_chaff_system.cpp measures
    // the worst pair at several burst sizes so this constant cannot rot.
    const f32 contact_d = fp.radius * fp.contact_spacing;
    const f32 needed = contact_d * std::sqrt(static_cast<f32>(count)) * 0.75f;
    const f32 radius = math::max(portal_radius, needed);

    // One draw, for the whole burst: a random spiral phase so successive waves
    // out of the same portal are not stamped identically. Rotating the pattern
    // cannot disturb the spacing, whereas per-agent jitter would reintroduce
    // exactly the overlap this is here to remove.
    const f32 phase = rng.range_f(0.0f, math::kTwoPi);

    u32 spawned = 0;
    for (u32 i = 0; i < count; ++i) {
        if (buffers.full()) break;
        const f32 t = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(count);
        const f32 r = radius * std::sqrt(t);
        const f32 a = phase + static_cast<f32>(i) * kGoldenAngle;
        ChaffSpawnParams p;
        p.position = portal + Vec2{std::cos(a), std::sin(a)} * r;
        p.family = family;
        p.density = fp.base_density > 0.0f ? fp.base_density : 1.0f;
        if (buffers.spawn(p).valid()) ++spawned;
    }
    return spawned;
}

} // namespace immune::sim
