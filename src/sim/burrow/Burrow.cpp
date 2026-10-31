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

/// True if `p` is at least `margin` inside `area`, or `area` is empty.
bool inside_play_area(const Rect& area, Vec2 p, f32 margin) {
    const Vec2 size = area.size();
    if (size.x <= 0.0f || size.y <= 0.0f) return true;
    return p.x >= area.min.x + margin && p.x <= area.max.x - margin &&
           p.y >= area.min.y + margin && p.y <= area.max.y - margin;
}

/// Open lane: walkable in the live mask (a clot or scar is not) and at least
/// `clearance` from any wall. An unbaked mask or field checks nothing.
bool on_open_lane(const TissueMask& mask, const DistanceField& sdf, Vec2 p, f32 clearance) {
    if (mask.width() > 0 && mask.height() > 0) {
        const IVec2 c = mask.world_to_cell(p);
        if (!mask.walkable(c.x, c.y)) return false;
    }
    if (sdf.width() > 0 && sdf.height() > 0 && sdf.sample(p) < clearance) return false;
    return true;
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
                              f32& out_score, const Rect& play_area, f32 body_half) const {
    (void)from;
    if (!inside_play_area(play_area, exit, bp.edge_margin)) return false;
    // Standing ground with room for the body.
    if (!on_open_lane(mask, sdf, exit, bp.wall_clearance)) return false;
    if (!flow.reachable(exit)) return false;
    // It comes up facing down the lane, so the whole body -- and the exit
    // hole under its tail -- lie along the flow either side of the exit.
    // Each end has to be on open lane too, or the hole is dug in the wall.
    if (body_half > 0.0f) {
        const Vec2 fwd = math::normalize_safe(flow.sample(exit));
        if (math::length_sq(fwd) <= math::kEpsilon) return false;
        const Vec2 tail = exit - fwd * body_half;
        const Vec2 head = exit + fwd * body_half;
        if (!on_open_lane(mask, sdf, tail, bp.wall_clearance)) return false;
        if (!on_open_lane(mask, sdf, head, bp.wall_clearance)) return false;
        if (!inside_play_area(play_area, tail, bp.edge_margin)) return false;
    }

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

void BurrowSystem::update_slither(ChaffBuffers& chaff, const FlowField& flow, f32 dt) {
    if (!active_ || dt <= 0.0f) return;
    // Heading noise must not become an instantaneous bend of the whole body.
    const f32 heading_response = 1.0f - std::exp(-dt / 0.10f);
    const f32 bend_response = 1.0f - std::exp(-dt / 0.20f);
    for (usize i = 0; i < chaff.count(); ++i) {
        const u32 f = chaff.family[i] < kFamilyCount ? chaff.family[i] : 0u;
        const SlitherParams& sp = tuning_.slither[f];
        if (!sp.enabled || (chaff.flags[i] & chaff_flags::kPendingKill) != 0) continue;
        const Vec2 p{chaff.pos_x[i], chaff.pos_y[i]};
        const Vec2 displacement = p - Vec2{chaff.prev_pos_x[i], chaff.prev_pos_y[i]};
        const f32 distance = math::length(displacement);
        const f32 speed = distance / dt;
        if (chaff.slither_phase[i] < 0.0f) {
            u32 h = chaff.generation[i] * 2654435761u;
            h ^= h >> 15u;
            chaff.slither_phase[i] = static_cast<f32>(h & 0xFFFFu) * (kSlitherPhasePeriod / 65536.0f);
            Vec2 dir = displacement;
            if (math::length_sq(dir) <= math::kEpsilon) dir = flow.sample(p);
            if (math::length_sq(dir) <= math::kEpsilon) dir = {chaff.vel_x[i], chaff.vel_y[i]};
            chaff.body_heading[i] = math::length_sq(dir) > math::kEpsilon ? std::atan2(dir.y, dir.x) : 0.0f;
        }
        const bool was_reversing = chaff.slither_phase_delta[i] < 0.0f;
        chaff.body_heading_delta[i] = 0.0f;
        chaff.slither_phase_delta[i] = 0.0f;
        chaff.slither_curvature_delta[i] = 0.0f;
        const bool frozen = (chaff.flags[i] & (chaff_flags::kHidden | chaff_flags::kLatched)) != 0;
        // The first dive frame still includes this tick's final surface step.
        // Later burrow frames and bodies carried by cells hold their pose.
        const bool starting_dive = chaff.burrow_state[i] == burrow_state::kDiving &&
                                   chaff.burrow_anim[i] == 0.0f;
        if (frozen && !starting_dive) continue;
        const f32 old_heading = chaff.body_heading[i];
        const f32 old_curvature = chaff.slither_curvature[i];
        f32 travel_heading = std::atan2(displacement.y, displacement.x);
        const f32 heading_error = std::fabs(wrap_angle(travel_heading - old_heading));
        const bool reverse = heading_error > 0.75f * math::kPi ||
                             (was_reversing && heading_error > 0.5f * math::kPi);
        if (speed > sp.min_speed) {
            // Only a clearly backwards shove runs the track in reverse. A
            // near-perpendicular turn must not alternate between +/-90deg.
            if (reverse) travel_heading = wrap_angle(travel_heading + math::kPi);
            const f32 sign = reverse ? -1.0f : 1.0f;
            // Estimate the endpoint tangent with the already filtered bend;
            // feeding last tick's raw turn back here amplified steering noise.
            const f32 want = travel_heading + 0.5f * old_curvature * sign * distance;
            const f32 step = math::max(sp.turn_rate, 0.0f) * dt;
            const f32 turn = math::clamp(wrap_angle(want - old_heading) * heading_response, -step, step);
            chaff.body_heading_delta[i] = turn;
            chaff.body_heading[i] = wrap_angle(old_heading + turn);
        }
        const f32 turn = chaff.body_heading_delta[i];
        const f32 half_turn = 0.5f * turn;
        // Actual distance, not its projection onto a lagging heading: that
        // projection approaches zero on turns and made turn/distance explode.
        const f32 signed_distance = reverse ? -distance : distance;
        const f32 arc = std::fabs(half_turn) > 1e-4f
                            ? signed_distance * half_turn / std::sin(half_turn) : signed_distance;
        if (speed > sp.min_speed && std::fabs(arc) > 1e-5f) {
            const f32 half_body = tuning_.body_half_length[f] > 0.0f
                                     ? tuning_.body_half_length[f] * chaff.size_scale[i]
                                     : 0.5f * math::max(sp.wavelength, 1.0f);
            const f32 max_curvature = 0.6f / half_body;
            const f32 target = math::clamp(turn / arc, -max_curvature, max_curvature);
            const f32 wave = std::fabs(sp.amplitude * sp.head_amplitude) +
                             0.5f * std::fabs(sp.curl) + std::fabs(sp.probe);
            const f32 max_side = wave * 2.0f * half_body / math::max(sp.body_length, 0.01f);
            // Bound how far a bend change can move either end, including its
            // sideways wave offset. A tiny root step cannot swing the head by
            // metres. Cap the budget during large external position corrections.
            const f32 gain = half_body * (0.5f * half_body + max_side);
            const f32 budget = 0.25f * math::min(distance, math::max(sp.wavelength, 1.0f) * dt);
            const f32 limit = budget / math::max(gain, 1e-5f);
            const f32 change = math::clamp((target - old_curvature) * bend_response, -limit, limit);
            chaff.slither_curvature_delta[i] = change;
            chaff.slither_curvature[i] = old_curvature + change;
        }
        const f32 idle = sp.min_speed > 0.0f
                             ? 1.0f - math::smoothstep01(speed / sp.min_speed)
                             : (distance <= 1e-6f ? 1.0f : 0.0f);
        const f32 per_unit = sp.wavelength > math::kEpsilon ? math::kTwoPi / sp.wavelength : 0.0f;
        const f32 advance = arc * per_unit + idle * sp.idle_rate * math::kTwoPi * dt;
        chaff.slither_phase_delta[i] = advance;
        f32 phase = std::fmod(chaff.slither_phase[i] + advance, kSlitherPhasePeriod);
        if (phase < 0.0f) phase += kSlitherPhasePeriod;
        chaff.slither_phase[i] = phase;
    }
}

BurrowStats BurrowSystem::update(ChaffBuffers& chaff, const FlowField& flow,
                                 const DistanceField& sdf, const TissueMask& mask,
                                 const SpatialHash& hash, const std::vector<BurrowThreat>& threats,
                                 Rng& rng, f32 dt, const Rect& play_area, bool animate) {
    BurrowStats stats{};
    if (!active_ || dt <= 0.0f) return stats;
    if (animate) update_slither(chaff, flow, dt);

    // Index defender positions once, then examine only discs that could reach
    // the immediate path or a candidate exit. The maximum radius keeps the
    // broad phase conservative even for unusually large tower ranges.
    const Rect bounds = hash.bounds();
    const Rect old_bounds = threat_hash_.bounds();
    if (threat_hash_.grid_dims().x <= 0 ||
        bounds.min.x != old_bounds.min.x || bounds.min.y != old_bounds.min.y ||
        bounds.max.x != old_bounds.max.x || bounds.max.y != old_bounds.max.y) {
        threat_hash_.configure(SpatialHashDesc{bounds, 32.0f});
    }
    threat_x_.resize(threats.size());
    threat_y_.resize(threats.size());
    f32 max_threat_radius = 0.0f;
    for (usize j = 0; j < threats.size(); ++j) {
        threat_x_[j] = threats[j].position.x;
        threat_y_[j] = threats[j].position.y;
        max_threat_radius = math::max(max_threat_radius, threats[j].radius);
    }
    threat_hash_.rebuild(threat_x_.data(), threat_y_.data(), threats.size());

    const usize n = chaff.count();
    for (usize i = 0; i < n; ++i) {
        const u32 f = chaff.family[i] < kFamilyCount ? chaff.family[i] : 0u;
        const BurrowParams& bp = tuning_.burrow[f];
        if (!bp.enabled) continue;
        u8& flags = chaff.flags[i];
        if ((flags & chaff_flags::kPendingKill) != 0) continue;

        const Vec2 p{chaff.pos_x[i], chaff.pos_y[i]};
        const bool frozen = (flags & (chaff_flags::kHidden | chaff_flags::kLatched)) != 0;
        u8& state = chaff.burrow_state[i];

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
                // A ready burrow is held in reserve until the approach is
                // dangerous. Zero is distinct from the uninitialised -1.
                timer = math::max(0.0f, timer - dt);
                if (timer > 0.0f) break;

                // Due -- but only where the player can see it: off-screen
                // (a spawn point outside the play area) it waits until it
                // has walked in.
                if (!inside_play_area(play_area, p, bp.edge_margin)) {
                    timer = math::max(bp.retry_delay, dt);
                    break;
                }
                // The dive's hole is dug under the head. A worm shoved against
                // a wall, or rounding a bend with its head over the lining,
                // waits until its head is back over open lane.
                const f32 body_half = tuning_.body_half_length[f];
                if (body_half > 0.0f) {
                    const f32 h = chaff.body_heading[i];
                    const Vec2 head = p + Vec2{std::cos(h), std::sin(h)} * body_half;
                    if (!on_open_lane(mask, sdf, head, bp.wall_clearance) ||
                        !inside_play_area(play_area, head, bp.edge_margin)) {
                        timer = math::max(bp.retry_delay, dt);
                        break;
                    }
                }
                // Look down the lane from here.
                Vec2 fwd = flow.sample(p);
                const f32 from_cost = flow.sample_cost(p);
                if (math::length_sq(fwd) <= math::kEpsilon || !std::isfinite(from_cost)) {
                    timer = math::max(bp.retry_delay, dt);
                    ++stats.failed;
                    break;
                }
                fwd = math::normalize_safe(fwd);
                const f32 threat_reach = max_threat_radius * math::max(bp.tower_range_scale, 0.0f);
                const f32 query_radius = math::max(bp.min_range, 0.0f) + threat_reach;
                threat_hash_.query_circle(p, math::max(query_radius, 0.0f), threat_indices_);
                nearby_threats_.clear();
                for (u32 index : threat_indices_) nearby_threats_.push_back(threats[index]);
                // Watch the immediate approach as well as the ground under
                // the parasite, so it can dive before reaching defenders.
                // Defenders behind it do not consume a ready burrow unless
                // their coverage still reaches it.
                bool threatened = false;
                const f32 lookahead = math::max(bp.min_range, 0.0f);
                for (const BurrowThreat& threat : nearby_threats_) {
                    const f32 radius = math::max(0.0f, threat.radius * bp.tower_range_scale);
                    if (radius <= 0.0f) continue;
                    const Vec2 offset = threat.position - p;
                    const f32 along = math::clamp(offset.x * fwd.x + offset.y * fwd.y,
                                                  0.0f, lookahead);
                    if (math::length_sq(offset - fwd * along) <= radius * radius) {
                        threatened = true;
                        break;
                    }
                }
                if (!threatened) break;
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
                    threat_hash_.query_circle(exit, threat_reach, threat_indices_);
                    nearby_threats_.clear();
                    for (u32 index : threat_indices_) nearby_threats_.push_back(threats[index]);
                    f32 score = 0.0f;
                    if (!score_exit(bp, p, from_cost, exit, chaff, flow, sdf, mask, hash, nearby_threats_,
                                    score, play_area, body_half)) {
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
                chaff.body_heading_delta[i] = 0.0f;
                chaff.slither_curvature[i] = 0.0f;
                chaff.slither_curvature_delta[i] = 0.0f;
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
