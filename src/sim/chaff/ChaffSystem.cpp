// Batch chaff movement kernel. Owner: Wave 1B.
//
// THREE PASSES, ONE TICK
// The frozen four-line kernel (flow sample, separation, clamp, integrate) is
// split into three passes rather than one fused per-agent loop:
//
//   A. accumulate  (parallel, per-range Rng)   — flow/drift + separation + jitter
//      into vel_x/vel_y (UNCLAMPED), plus the per-agent effective max speed and
//      replication rolls into scratch. This is the pass that touches the
//      spatial hash, so it is inherently gather-heavy and does not vectorize —
//      that cost is unavoidable and is what the spatial hash's O(1) cell lookup
//      exists to minimize.
//   B. integrate   (serial, straight-line)     — clamp_length + p += v*dt over
//      contiguous vel_x/vel_y/scratch_max_speed/old_pos_{x,y}/pos_{x,y}. No
//      branches on flags, no hash access, no gather: this is the loop
//      docs/ARCHITECTURE.md means by "the movement loop is written so MSVC
//      auto-vectorizes it" (verified below).
//   C. despawn     (serial)                    — bounds/goal test against the
//      now-final position, flags kPendingKill. Cheap, branchy, not perf-critical.
//
// Splitting out B is also what makes the result scheduling-independent: pass A
// writes only the index each range owns and reads neighbours through a snapshot
// of positions taken *before* pass A runs (old_pos_x_/old_pos_y_), never through
// the live arrays another range may already have overwritten.
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

/// 3x3-cell separation impulse for agent `i`, reading positions through the
/// pre-tick snapshot `px`/`py` (see file header). Uses the raw cell accessors
/// per SpatialHash.h's guidance — this runs up to 10,000 times a tick and must
/// not touch a std::vector per call.
Vec2 separation_impulse(const SpatialHash& hash, const f32* px, const f32* py,
                        usize i, f32 radius) {
    if (radius <= 0.0f) return Vec2{0.0f, 0.0f};
    const Vec2 p{px[i], py[i]};
    const IVec2 c = hash.cell_coord(p);
    const IVec2 dims = hash.grid_dims();
    const f32 r2 = radius * radius;
    const u32* indices = hash.indices();

    const i32 y0 = math::max(0, c.y - 1);
    const i32 y1 = math::min(dims.y - 1, c.y + 1);
    const i32 x0 = math::max(0, c.x - 1);
    const i32 x1 = math::min(dims.x - 1, c.x + 1);

    Vec2 accum{0.0f, 0.0f};
    u32 neighbours = 0;
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
                const u32 j = indices[k];
                if (j == i) continue;
                const f32 dx = p.x - px[j];
                const f32 dy = p.y - py[j];
                const f32 d2 = dx * dx + dy * dy;
                if (d2 >= r2 || d2 < kSeparationEpsSq) continue;
                const f32 d = std::sqrt(d2);
                const f32 push = (radius - d) / radius;   // 1 at d=0, 0 at d=radius
                accum.x += (dx / d) * push;
                accum.y += (dy / d) * push;
                ++neighbours;
            }
        }
    }
    if (neighbours > 0) {
        const f32 inv = 1.0f / static_cast<f32>(neighbours);
        accum.x *= inv;
        accum.y *= inv;
    }
    return accum;
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
        pos_x[i] = old_pos_x[i] + nvx * dt;
        pos_y[i] = old_pos_y[i] + nvy * dt;
    }
}

} // namespace

void ChaffSystem::ensure_scratch(usize capacity) {
    if (old_pos_x_.size() == capacity) return;   // already sized; reserved-once
    old_pos_x_.assign(capacity, 0.0f);
    old_pos_y_.assign(capacity, 0.0f);
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

    f32* vx = buffers.vel_x.data();
    f32* vy = buffers.vel_y.data();
    const u8* fam = buffers.family.data();
    const u8* flg = buffers.flags.data();
    const f32* old_px = old_pos_x_.data();
    const f32* old_py = old_pos_y_.data();
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
                continue;
            }

            const bool drifting = (flags_i & chaff_flags::kDrifting) != 0;
            const bool clumped = (flags_i & chaff_flags::kClumped) != 0;
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

            if (!clumped) {
                v += separation_impulse(hash, old_px, old_py, i, fp.separation_radius)
                     * fp.separation_strength;
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
    integrate_and_clamp(px, py, vx, vy, old_px, old_py, max_speed_scratch,
                        static_cast<u32>(count), dt);

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
                killed = true;
            }
        }
        if (!killed && !bounds_.contains(Vec2{px[i], py[i]})) {
            buffers.kill(i);
            ++despawned_bounds;
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
    u32 spawned = 0;
    for (u32 i = 0; i < count; ++i) {
        if (buffers.full()) break;
        ChaffSpawnParams p;
        p.position = portal + rng.unit_disc() * portal_radius;
        p.family = family;
        p.density = fp.base_density > 0.0f ? fp.base_density : 1.0f;
        if (buffers.spawn(p).valid()) ++spawned;
    }
    return spawned;
}

} // namespace immune::sim
