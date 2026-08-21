// sim/damage/DamageField.cpp — aggregate damage evaluation. Owner: Wave 2A.
// DamageField.h is a frozen contract; this file implements it only.
//
// SHAPE RESOLUTION
// Every shape resolves to a SpatialHash query (conservative at cell
// granularity) followed by an exact per-agent test done here. Chain is the
// odd one out: it has no direct SpatialHash query, so it's evaluated as a
// walk of up to kMaxChainLinks small circle queries, jumping tick-by-tick to
// the nearest not-yet-hit matching agent. That's a judgment call — the header
// only specifies "a sequence of small circles built at evaluation time"; see
// the report for the exact semantics chosen.
//
// DETERMINISM FOR ProbabilisticRemoval
// Each field forks its own Rng stream from the sim Rng using the field's
// index in fields_ (stable within a tick — clear_transient() only runs
// between ticks). Each *candidate agent* then forks its own sub-stream keyed
// on its ChaffBuffers array index (also stable within a tick — compact() is
// deliberately deferred until after all damage sources have applied, per the
// header). Keying per-agent draws this way means an agent's roll is a pure
// function of (field index, agent index), never of the order in which the
// spatial hash happened to enumerate candidates — so results are reproducible
// under any candidate traversal order, not just a fixed one. See
// tests/test_damage_field.cpp for a direct test of that property (same agent
// set, two different SpatialHash cell sizes -> two different candidate
// orderings -> identical removal set).
#include "sim/damage/DamageField.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <algorithm>
#include <cmath>

namespace immune::sim {

namespace {

constexpr u32 kInvalidIndex = static_cast<u32>(-1);
/// Chain-jump link cap. Not specified by the header; chosen as "enough to
/// feel like a cascade without an unbounded per-tick cost per field".
constexpr u32 kMaxChainLinks = 8;

/// Result of an exact per-shape point test: whether the point is inside, and
/// its normalized distance from the shape's "centre" in [0, 1] for falloff.
struct ShapeHit {
    bool inside = false;
    f32 t = 0.0f;
};

ShapeHit test_circle(Vec2 origin, f32 radius, Vec2 p) {
    if (radius <= 0.0f) return {};
    const Vec2 d = p - origin;
    const f32 d2 = d.x * d.x + d.y * d.y;
    if (d2 > radius * radius) return {};
    return ShapeHit{true, std::sqrt(d2) / radius};
}

ShapeHit test_rect(const Rect& rect, Vec2 p) {
    if (!rect.contains(p)) return {};
    const Vec2 c = rect.center();
    const Vec2 half = rect.size() * 0.5f;
    const f32 tx = half.x > math::kEpsilon ? std::abs(p.x - c.x) / half.x : 0.0f;
    const f32 ty = half.y > math::kEpsilon ? std::abs(p.y - c.y) / half.y : 0.0f;
    return ShapeHit{true, math::saturate(math::max(tx, ty))};
}

ShapeHit test_cone(Vec2 origin, Vec2 direction, f32 radius, f32 half_angle, Vec2 p) {
    if (radius <= 0.0f) return {};
    const Vec2 d = p - origin;
    const f32 d2 = d.x * d.x + d.y * d.y;
    if (d2 > radius * radius) return {};
    if (d2 < 1e-8f) return ShapeHit{true, 0.0f};   // agent sits on the apex
    const f32 dist = std::sqrt(d2);
    const f32 cos_theta = (d.x * direction.x + d.y * direction.y) / dist;
    if (cos_theta < std::cos(half_angle)) return {};
    return ShapeHit{true, dist / radius};
}

/// Falloff exponent applied to normalized distance (DamageField::falloff doc):
/// 0 -> flat multiplier of 1 everywhere inside the shape, 1 -> linear falloff
/// to 0 at the boundary, 2 -> quadratic. pow(x, 0) == 1 for any x in [0, 1],
/// so a single formula covers all three without a branch on "flat".
f32 falloff_multiplier(f32 t, f32 falloff) {
    if (falloff <= 0.0f) return 1.0f;
    const f32 x = 1.0f - math::saturate(t);
    return std::pow(x, falloff);
}

bool family_matches(u8 family_mask, u8 family) {
    return (family_mask & (1u << family)) != 0;
}

/// Coarse "how many cells did this query span" estimate for DamageStats, from
/// the query's own bounding box rather than SpatialHash internals (gather_cells
/// is private). Not exact (a circle's bbox touches corner cells outside the
/// circle) but a fair proxy for the debug/HUD signal it feeds.
u32 estimate_cells_touched(const SpatialHash& hash, Vec2 lo, Vec2 hi) {
    const IVec2 dims = hash.grid_dims();
    if (dims.x <= 0 || dims.y <= 0) return 0;
    const IVec2 c0 = hash.cell_coord(lo);
    const IVec2 c1 = hash.cell_coord(hi);
    const i32 x0 = math::max(0, math::min(c0.x, c1.x));
    const i32 x1 = math::min(dims.x - 1, math::max(c0.x, c1.x));
    const i32 y0 = math::max(0, math::min(c0.y, c1.y));
    const i32 y1 = math::min(dims.y - 1, math::max(c0.y, c1.y));
    if (x1 < x0 || y1 < y0) return 0;
    return static_cast<u32>((x1 - x0 + 1) * (y1 - y0 + 1));
}

/// Applies a final (already falloff/marked-adjusted) density amount to one
/// agent and folds the result into `stats`. This is the single place that
/// reads `apply_density_loss`'s actual clamped effect (never the nominal
/// request) and decides whether it was a kill *this call*.
void apply_and_record(ChaffBuffers& chaff, u32 idx, f32 amount, DamageStats& stats,
                     EntityId owner, DamageAttribution* attribution) {
    if (amount <= 0.0f) return;
    const bool already_dead = (chaff.flags[idx] & chaff_flags::kPendingKill) != 0;
    const f32 before = chaff.density[idx];
    chaff.apply_density_loss(idx, amount);
    const f32 removed = before - chaff.density[idx];
    if (removed <= 0.0f) return;
    stats.density_removed += removed;
    const u8 fam = chaff.family[idx];
    if (fam < kFamilyCount) stats.density_removed_by_family[fam] += removed;
    const bool killed = !already_dead && (chaff.flags[idx] & chaff_flags::kPendingKill) != 0;
    if (killed) ++stats.agents_killed;
    // Off by default; see sim/Attribution.h. Environmental hazards carry no
    // owner and are deliberately not booked against anyone.
    if (attribution != nullptr && owner.valid()) {
        attribution->record_chaff(owner, fam, removed, killed);
    }
}

/// Runs the family/alive/mode gate for one already-shape-tested candidate and
/// applies damage per `mode`. `field_rng` is the field's own forked stream;
/// this function derives a per-agent sub-stream from it keyed on `idx` so the
/// roll never depends on candidate traversal order (see file header comment).
void apply_to_agent(ChaffBuffers& chaff, u32 idx, const DamageField& field, f32 t, f32 dt,
                     ThinningMode mode, const Rng& field_rng, DamageStats& stats,
                     DamageAttribution* attribution) {
    const u8 agent_flags = chaff.flags[idx];
    if ((agent_flags & chaff_flags::kAlive) == 0) return;
    if ((agent_flags & chaff_flags::kPendingKill) != 0) return;   // already dying this tick
    if (!family_matches(field.family_mask, chaff.family[idx])) return;

    const bool marked = (agent_flags & chaff_flags::kMarked) != 0;
    const f32 mult = falloff_multiplier(t, field.falloff) * (marked ? field.marked_multiplier : 1.0f);

    if (mode == ThinningMode::DensityThinning) {
        apply_and_record(chaff, idx, field.kill_rate * dt * mult, stats, field.owner, attribution);
        return;
    }

    // ProbabilisticRemoval: kill_rate is reinterpreted as a per-second removal
    // probability (mirrors DensityThinning's "density removed per second"
    // reading of the same knob). Whole-agent removal on success.
    const f32 rate = field.kill_rate * mult;
    const f32 probability = math::saturate(rate * dt);
    if (probability <= 0.0f) return;
    Rng roll = field_rng.fork(static_cast<u64>(idx));
    if (roll.chance(probability)) {
        apply_and_record(chaff, idx, chaff.density[idx], stats, field.owner, attribution);
    }
}

/// Finds the nearest live, family-matching, not-yet-visited agent within
/// `radius` of `from`. Returns kInvalidIndex if none.
u32 find_nearest_unvisited(const ChaffBuffers& chaff, const std::vector<u32>& candidates,
                            const u32* visited, u32 visited_count, Vec2 from, f32 radius,
                            u8 family_mask, f32& out_t) {
    u32 best = kInvalidIndex;
    f32 best_d2 = radius * radius;
    for (u32 idx : candidates) {
        if (idx >= chaff.count()) continue;
        bool seen = false;
        for (u32 v = 0; v < visited_count; ++v) {
            if (visited[v] == idx) { seen = true; break; }
        }
        if (seen) continue;
        if ((chaff.flags[idx] & chaff_flags::kAlive) == 0) continue;
        if ((chaff.flags[idx] & chaff_flags::kPendingKill) != 0) continue;
        if (!family_matches(family_mask, chaff.family[idx])) continue;
        const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
        const Vec2 d = p - from;
        const f32 d2 = d.x * d.x + d.y * d.y;
        if (d2 > radius * radius) continue;
        if (best == kInvalidIndex || d2 < best_d2) {
            best = idx;
            best_d2 = d2;
        }
    }
    out_t = radius > 0.0f ? std::sqrt(best_d2) / radius : 0.0f;
    return best;
}

/// Chain-jump walk: start at field.origin, repeatedly damage the nearest
/// not-yet-hit matching agent within field.radius and jump to it, up to
/// kMaxChainLinks hops. Stops early once no further candidate is in range.
void apply_chain(ChaffBuffers& chaff, const SpatialHash& hash, const DamageField& field,
                 ThinningMode mode, const Rng& field_rng, f32 dt, DamageStats& stats,
                 std::vector<u32>& scratch, DamageAttribution* attribution) {
    if (field.radius <= 0.0f) return;

    u32 visited[kMaxChainLinks];
    u32 visited_count = 0;
    Vec2 current = field.origin;

    for (u32 link = 0; link < kMaxChainLinks; ++link) {
        scratch.clear();
        hash.query_circle(current, field.radius, scratch);
        stats.cells_touched += estimate_cells_touched(
            hash, current - Vec2{field.radius, field.radius}, current + Vec2{field.radius, field.radius});

        f32 t = 0.0f;
        const u32 best = find_nearest_unvisited(chaff, scratch, visited, visited_count, current,
                                                field.radius, field.family_mask, t);
        if (best == kInvalidIndex) break;

        apply_to_agent(chaff, best, field, t, dt, mode, field_rng, stats, attribution);
        visited[visited_count++] = best;
        current = Vec2{chaff.pos_x[best], chaff.pos_y[best]};
    }
}

/// Read-only counterpart of apply_chain for measure_density: same walk, no
/// mutation, no stats.
f32 measure_chain(const ChaffBuffers& chaff, const SpatialHash& hash, const DamageField& region,
                  std::vector<u32>& scratch) {
    if (region.radius <= 0.0f) return 0.0f;

    u32 visited[kMaxChainLinks];
    u32 visited_count = 0;
    Vec2 current = region.origin;
    f32 total = 0.0f;

    for (u32 link = 0; link < kMaxChainLinks; ++link) {
        scratch.clear();
        hash.query_circle(current, region.radius, scratch);

        f32 t = 0.0f;
        const u32 best = find_nearest_unvisited(chaff, scratch, visited, visited_count, current,
                                                region.radius, region.family_mask, t);
        if (best == kInvalidIndex) break;

        total += chaff.density[best];
        visited[visited_count++] = best;
        current = Vec2{chaff.pos_x[best], chaff.pos_y[best]};
    }
    return total;
}

} // namespace

u32 DamageSystem::submit(const DamageField& field) {
    fields_.push_back(field);
    return static_cast<u32>(fields_.size() - 1);
}

void DamageSystem::clear_transient(f32 dt) {
    // Snapshot BEFORE the cull, for rendered_fields(). This is the only moment
    // the full set of fields that were actually live this tick still exists:
    // the loop below is about to drop every persistent one. assign() into a
    // member reuses the capacity reserve() already took, so the steady state is
    // a memcpy of a few hundred structs and no allocation.
    rendered_.assign(fields_.begin(), fields_.end());

    usize write = 0;
    for (usize i = 0; i < fields_.size(); ++i) {
        DamageField f = fields_[i];
        if (f.lifetime <= 0.0f) continue; // persistent: owner re-submits
        f.lifetime -= dt;
        if (f.lifetime <= 0.0f) continue; // expired
        fields_[write++] = f;
    }
    fields_.resize(write);
}

DamageStats DamageSystem::apply(ChaffBuffers& chaff, const SpatialHash& hash, Rng& rng, f32 dt) {
    DamageStats stats;
    stats.fields_evaluated = static_cast<u32>(fields_.size());

    // Reused across calls so apply()'s hot loop allocates nothing beyond what
    // the spatial hash's own out_indices growth needs on first use.
    static thread_local std::vector<u32> candidates;

    for (usize field_idx = 0; field_idx < fields_.size(); ++field_idx) {
        const DamageField& field = fields_[field_idx];

        // Friendly-fire fields damage the player's own units/objective;
        // chaff-facing behaviour doesn't depend on them, so those fields are
        // counted as evaluated but skipped here rather than guessed at.
        if (field.friendly_fire) continue;

        const Rng field_rng = rng.fork(static_cast<u64>(field_idx));

        switch (field.shape) {
        case FieldShape::Circle: {
            candidates.clear();
            hash.query_circle(field.origin, field.radius, candidates);
            stats.cells_touched += estimate_cells_touched(
                hash, field.origin - Vec2{field.radius, field.radius},
                field.origin + Vec2{field.radius, field.radius});
            for (u32 idx : candidates) {
                if (idx >= chaff.count()) continue;
                const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
                const ShapeHit hit = test_circle(field.origin, field.radius, p);
                if (!hit.inside) continue;
                apply_to_agent(chaff, idx, field, hit.t, dt, mode_, field_rng, stats, attribution_);
            }
            break;
        }
        case FieldShape::Rect: {
            candidates.clear();
            hash.query_rect(field.rect, candidates);
            stats.cells_touched += estimate_cells_touched(hash, field.rect.min, field.rect.max);
            for (u32 idx : candidates) {
                if (idx >= chaff.count()) continue;
                const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
                const ShapeHit hit = test_rect(field.rect, p);
                if (!hit.inside) continue;
                apply_to_agent(chaff, idx, field, hit.t, dt, mode_, field_rng, stats, attribution_);
            }
            break;
        }
        case FieldShape::Cone: {
            candidates.clear();
            hash.query_cone(field.origin, field.direction, field.radius, field.arc_radians, candidates);
            stats.cells_touched += estimate_cells_touched(
                hash, field.origin - Vec2{field.radius, field.radius},
                field.origin + Vec2{field.radius, field.radius});
            for (u32 idx : candidates) {
                if (idx >= chaff.count()) continue;
                const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
                const ShapeHit hit =
                    test_cone(field.origin, field.direction, field.radius, field.arc_radians, p);
                if (!hit.inside) continue;
                apply_to_agent(chaff, idx, field, hit.t, dt, mode_, field_rng, stats, attribution_);
            }
            break;
        }
        case FieldShape::Chain: {
            apply_chain(chaff, hash, field, mode_, field_rng, dt, stats, candidates, attribution_);
            break;
        }
        }
    }

    return stats;
}

f32 DamageSystem::measure_density(const ChaffBuffers& chaff, const SpatialHash& hash,
                                  const DamageField& region) const {
    static thread_local std::vector<u32> candidates;
    f32 total = 0.0f;

    switch (region.shape) {
    case FieldShape::Circle: {
        candidates.clear();
        hash.query_circle(region.origin, region.radius, candidates);
        for (u32 idx : candidates) {
            if (idx >= chaff.count()) continue;
            if ((chaff.flags[idx] & chaff_flags::kAlive) == 0) continue;
            if (!family_matches(region.family_mask, chaff.family[idx])) continue;
            const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
            if (test_circle(region.origin, region.radius, p).inside) total += chaff.density[idx];
        }
        break;
    }
    case FieldShape::Rect: {
        candidates.clear();
        hash.query_rect(region.rect, candidates);
        for (u32 idx : candidates) {
            if (idx >= chaff.count()) continue;
            if ((chaff.flags[idx] & chaff_flags::kAlive) == 0) continue;
            if (!family_matches(region.family_mask, chaff.family[idx])) continue;
            const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
            if (test_rect(region.rect, p).inside) total += chaff.density[idx];
        }
        break;
    }
    case FieldShape::Cone: {
        candidates.clear();
        hash.query_cone(region.origin, region.direction, region.radius, region.arc_radians, candidates);
        for (u32 idx : candidates) {
            if (idx >= chaff.count()) continue;
            if ((chaff.flags[idx] & chaff_flags::kAlive) == 0) continue;
            if (!family_matches(region.family_mask, chaff.family[idx])) continue;
            const Vec2 p{chaff.pos_x[idx], chaff.pos_y[idx]};
            if (test_cone(region.origin, region.direction, region.radius, region.arc_radians, p).inside)
                total += chaff.density[idx];
        }
        break;
    }
    case FieldShape::Chain: {
        total = measure_chain(chaff, hash, region, candidates);
        break;
    }
    }
    return total;
}

} // namespace immune::sim
