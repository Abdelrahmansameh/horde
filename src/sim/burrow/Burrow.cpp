// Burrowing and slithering chaff. See Burrow.h for the design.
#include "sim/burrow/Burrow.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <cmath>

namespace immune::sim {
namespace {

struct FamilyTables {
    BurrowParams burrow[kFamilyCount];
    SlitherParams slither[kFamilyCount];
};

FamilyTables& tables() {
    static FamilyTables t;
    return t;
}

u32 slot(PathogenFamily family) {
    const u32 i = static_cast<u32>(family);
    return i < kFamilyCount ? i : 0u;
}

/// Wraps an angle into (-pi, pi].
f32 wrap_angle(f32 a) {
    while (a > math::kPi) a -= math::kTwoPi;
    while (a <= -math::kPi) a += math::kTwoPi;
    return a;
}

/// A fresh cooldown: base +- uniform jitter, never below one tick so a
/// zero-cooldown config still takes at least a tick on the surface.
f32 roll_cooldown(const BurrowParams& bp, Rng& rng, f32 dt) {
    const f32 jitter = bp.cooldown_jitter > 0.0f ? rng.range_f(-bp.cooldown_jitter, bp.cooldown_jitter)
                                                 : 0.0f;
    return math::max(bp.cooldown + jitter, dt);
}

/// Surface pathogens (other than `self`) within `radius` of `p`.
u32 count_crowd(const ChaffBuffers& chaff, const SpatialHash& hash, Vec2 p, f32 radius,
                usize self) {
    if (radius <= 0.0f || hash.cell_size() <= 0.0f) return 0;
    const IVec2 dims = hash.grid_dims();
    if (dims.x <= 0 || dims.y <= 0) return 0;
    const IVec2 lo = hash.cell_coord(p - Vec2{radius, radius});
    const IVec2 hi = hash.cell_coord(p + Vec2{radius, radius});
    const u32* indices = hash.indices();
    const usize n = chaff.count();
    const f32 r2 = radius * radius;
    u32 found = 0;
    for (i32 cy = math::max(lo.y, 0); cy <= math::min(hi.y, dims.y - 1); ++cy) {
        for (i32 cx = math::max(lo.x, 0); cx <= math::min(hi.x, dims.x - 1); ++cx) {
            u32 begin = 0, end = 0;
            hash.cell_range(static_cast<u32>(cy) * static_cast<u32>(dims.x) + static_cast<u32>(cx),
                            begin, end);
            for (u32 k = begin; k < end; ++k) {
                const u32 j = indices[k];
                if (j >= n || j == self) continue;
                if (chaff.burrow_state[j] != burrow_state::kSurface) continue;
                const f32 dx = chaff.pos_x[j] - p.x;
                const f32 dy = chaff.pos_y[j] - p.y;
                if (dx * dx + dy * dy > r2) continue;
                ++found;
            }
        }
    }
    return found;
}

} // namespace

const BurrowParams& family_burrow(PathogenFamily family) { return tables().burrow[slot(family)]; }
void set_family_burrow(PathogenFamily family, const BurrowParams& params) {
    tables().burrow[slot(family)] = params;
}
const SlitherParams& family_slither(PathogenFamily family) { return tables().slither[slot(family)]; }
void set_family_slither(PathogenFamily family, const SlitherParams& params) {
    tables().slither[slot(family)] = params;
}

void BurrowSystem::set_tuning(const BurrowTuning& tuning) {
    tuning_ = tuning;
    active_ = false;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        if (tuning_.burrow[f].enabled || tuning_.slither[f].enabled) active_ = true;
    }
}

bool BurrowSystem::score_exit(const BurrowParams& bp, Vec2 from, f32 from_cost, Vec2 exit,
                              const ChaffBuffers& chaff, const FlowField& flow,
                              const DistanceField& sdf, const TissueMask& mask,
                              const SpatialHash& hash, const std::vector<BurrowThreat>& threats,
                              f32& out_score) const {
    (void)from;
    // Standing ground: walkable in the live mask (a clot or scar is not), and
    // with room for the body.
    if (mask.width() > 0 && mask.height() > 0) {
        const IVec2 c = mask.world_to_cell(exit);
        if (!mask.walkable(c.x, c.y)) return false;
    }
    if (sdf.width() > 0 && sdf.height() > 0 && sdf.sample(exit) < bp.wall_clearance) return false;
    if (!flow.reachable(exit)) return false;

    // Further along the lane, measured along the lane: the cone decides which
    // way to LOOK, cost-to-goal decides what counts as forward, so a cone that
    // sweeps across a hairpin into the leg behind cannot take it backwards.
    const f32 cost = flow.sample_cost(exit);
    if (!std::isfinite(cost)) return false;
    const f32 progress = from_cost - cost;
    if (progress < bp.min_progress) return false;
    if (cost < bp.goal_standoff) return false;

    f32 covered = 0.0f;
    for (const BurrowThreat& t : threats) {
        const f32 r = t.radius * bp.tower_range_scale;
        if (math::length_sq(exit - t.position) <= r * r) covered += 1.0f;
    }
    const f32 crowd =
        static_cast<f32>(count_crowd(chaff, hash, exit, bp.crowd_radius, ChaffBuffers::npos));

    out_score = bp.progress_weight * progress - bp.tower_weight * covered - bp.crowd_weight * crowd;
    return true;
}

BurrowStats BurrowSystem::update(ChaffBuffers& chaff, const FlowField& flow,
                                 const DistanceField& sdf, const TissueMask& mask,
                                 const SpatialHash& hash, const std::vector<BurrowThreat>& threats,
                                 Rng& rng, f32 dt) {
    BurrowStats stats{};
    if (!active_ || dt <= 0.0f) return stats;

    const usize n = chaff.count();
    for (usize i = 0; i < n; ++i) {
        const u32 f = chaff.family[i] < kFamilyCount ? chaff.family[i] : 0u;
        const BurrowParams& bp = tuning_.burrow[f];
        const SlitherParams& sp = tuning_.slither[f];
        if (!bp.enabled && !sp.enabled) continue;
        u8& flags = chaff.flags[i];
        if ((flags & chaff_flags::kPendingKill) != 0) continue;

        const Vec2 p{chaff.pos_x[i], chaff.pos_y[i]};
        const Vec2 v{chaff.vel_x[i], chaff.vel_y[i]};
        u8& state = chaff.burrow_state[i];

        // ---- Slither (cosmetic) ---------------------------------------------
        // First sight: a per-agent phase so a horde does not undulate in
        // lockstep, and a heading already facing down the lane so a fresh
        // spawn does not visibly swing round from +x.
        if (sp.enabled && chaff.slither_phase[i] < 0.0f) {
            u32 h = chaff.generation[i] * 2654435761u;
            h ^= h >> 15u;
            chaff.slither_phase[i] = static_cast<f32>(h & 0xFFFFu) * (math::kTwoPi / 65536.0f);
            Vec2 dir = flow.sample(p);
            if (math::length_sq(dir) <= math::kEpsilon) dir = v;
            chaff.body_heading[i] =
                math::length_sq(dir) > math::kEpsilon ? std::atan2(dir.y, dir.x) : 0.0f;
        }
        const bool frozen = (flags & (chaff_flags::kHidden | chaff_flags::kLatched)) != 0;
        if (sp.enabled && !frozen) {
            const f32 speed = math::length(v);
            const f32 per_unit = sp.wavelength > math::kEpsilon ? math::kTwoPi / sp.wavelength : 0.0f;
            f32 ph = chaff.slither_phase[i] + speed * dt * per_unit + sp.idle_rate * math::kTwoPi * dt;
            ph = std::fmod(ph, math::kTwoPi);
            chaff.slither_phase[i] = ph;
            if (speed > sp.min_speed) {
                const f32 want = std::atan2(v.y, v.x);
                const f32 diff = wrap_angle(want - chaff.body_heading[i]);
                const f32 step = sp.turn_rate * dt;
                chaff.body_heading[i] =
                    wrap_angle(chaff.body_heading[i] + math::clamp(diff, -step, step));
            }
        }

        if (!bp.enabled) continue;

        // ---- Burrow state machine ---------------------------------------------
        f32& timer = chaff.burrow_timer[i];
        switch (state) {
            case burrow_state::kSurface: {
                chaff.burrow_anim[i] = 0.0f;
                // Held by something else (a Macrophage arm, a host it is
                // riding): not free to dig. The countdown waits too.
                if (frozen) break;
                if (timer < 0.0f) {
                    timer = roll_cooldown(bp, rng, dt);
                    break;
                }
                timer -= dt;
                if (timer > 0.0f) break;

                // Due. Look down the lane from here.
                Vec2 fwd = flow.sample(p);
                const f32 from_cost = flow.sample_cost(p);
                if (math::length_sq(fwd) <= math::kEpsilon || !std::isfinite(from_cost)) {
                    timer = math::max(bp.retry_delay, dt);
                    ++stats.failed;
                    break;
                }
                fwd = math::normalize_safe(fwd);
                const f32 base = std::atan2(fwd.y, fwd.x);
                const f32 half = bp.cone_half_angle * (math::kPi / 180.0f);
                bool found = false;
                f32 best_score = 0.0f;
                Vec2 best{0.0f, 0.0f};
                for (u32 c = 0; c < bp.candidate_count; ++c) {
                    // Both draws happen for every candidate, legal or not, so
                    // the sim Rng advances by a fixed amount per attempt.
                    const f32 a = base + (half > 0.0f ? rng.range_f(-half, half) : 0.0f);
                    const f32 d = rng.range_f(bp.min_range, math::max(bp.min_range, bp.max_range));
                    const Vec2 exit = p + Vec2{std::cos(a), std::sin(a)} * d;
                    f32 score = 0.0f;
                    if (!score_exit(bp, p, from_cost, exit, chaff, flow, sdf, mask, hash, threats,
                                    score)) {
                        continue;
                    }
                    if (!found || score > best_score) {
                        found = true;
                        best_score = score;
                        best = exit;
                    }
                }
                if (!found) {
                    timer = math::max(bp.retry_delay, dt);
                    ++stats.failed;
                    break;
                }

                // Dive. kHidden freezes it in the movement kernel and takes it
                // off every tower's target list; the state stream takes it out
                // of damage and the crowd.
                state = burrow_state::kDiving;
                timer = math::max(bp.dive_duration, dt);
                chaff.burrow_target_x[i] = best.x;
                chaff.burrow_target_y[i] = best.y;
                chaff.burrow_anim[i] = 0.0f;
                flags |= chaff_flags::kHidden;
                chaff.vel_x[i] = 0.0f;
                chaff.vel_y[i] = 0.0f;
                if (bp.leave_squad) chaff.squad_id[i] = kNoSquad;
                ++stats.dives;
                break;
            }
            case burrow_state::kDiving: {
                timer -= dt;
                chaff.burrow_anim[i] =
                    math::saturate(1.0f - timer / math::max(bp.dive_duration, dt));
                if (timer > 0.0f) break;
                // Under. Move to the exit now: nothing can see it, and parking
                // it there is what lets the renderer draw the exit telegraph
                // at the agent's own position. prev_pos too, so interpolation
                // does not smear a sprite across the jump.
                const Vec2 exit{chaff.burrow_target_x[i], chaff.burrow_target_y[i]};
                chaff.pos_x[i] = chaff.prev_pos_x[i] = exit.x;
                chaff.pos_y[i] = chaff.prev_pos_y[i] = exit.y;
                // Come up facing down the lane.
                const Vec2 fwd = flow.sample(exit);
                if (math::length_sq(fwd) > math::kEpsilon) {
                    chaff.body_heading[i] = std::atan2(fwd.y, fwd.x);
                }
                state = burrow_state::kUnderground;
                timer = math::max(bp.underground_duration, dt);
                chaff.burrow_anim[i] = 0.0f;
                break;
            }
            case burrow_state::kUnderground: {
                timer -= dt;
                const f32 tele = bp.telegraph_duration;
                chaff.burrow_anim[i] =
                    tele > 0.0f ? math::saturate(1.0f - timer / tele) : 0.0f;
                if (timer > 0.0f) break;
                state = burrow_state::kEmerging;
                timer = math::max(bp.emerge_duration, dt);
                chaff.burrow_anim[i] = 0.0f;
                break;
            }
            case burrow_state::kEmerging: {
                timer -= dt;
                chaff.burrow_anim[i] =
                    math::saturate(1.0f - timer / math::max(bp.emerge_duration, dt));
                if (timer > 0.0f) break;
                state = burrow_state::kSurface;
                flags &= static_cast<u8>(~chaff_flags::kHidden);
                chaff.burrow_anim[i] = 0.0f;
                timer = roll_cooldown(bp, rng, dt);
                ++stats.emerged;
                break;
            }
            default:
                state = burrow_state::kSurface;
                break;
        }
        if (state != burrow_state::kSurface) ++stats.burrowed;
    }
    return stats;
}

} // namespace immune::sim
