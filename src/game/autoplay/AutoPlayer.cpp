// game/autoplay/AutoPlayer.cpp — see the header for scope and constraints.
//
// THE PLAN, IN ONE PARAGRAPH
// Walk every vessel spline at a fixed step; at each sample, take the
// centerline point as a candidate site. Score it on four things the level
// already knows about itself -- how narrow the lumen is there (a choke
// concentrates the horde), how late in the path it sits (measured with the
// flow field's own cost-to-goal, so it follows the route agents actually
// take), how many lanes it can reach, and whether the author tagged the area
// as buildable margin. Sort, then accept greedily with a spacing rule so the
// plan spreads instead of stacking six towers on the single best cell. Assign
// a tower type per site from the level's own wave table.
#include "game/autoplay/AutoPlayer.h"

#include "core/Log.h"
#include "core/Math.h"
#include "game/economy/Economy.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"
#include "sim/flowfield/TissueRaster.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace immune::game {
namespace {

/// World units between spline samples. Half a tower footprint: fine enough
/// that a short choke is not stepped over, coarse enough that a long level
/// does not produce tens of thousands of candidates to sort.
constexpr f32 kSampleStep = 1.0f;

/// Minimum spacing between accepted sites, as a multiple of the type's tier-1
/// range. Below ~0.6 the plan stacks towers into one blob whose fields overlap
/// almost completely, which reads as "this tower is weak" when what actually
/// happened is that four copies fought over the same agents.
constexpr f32 kSpacingRangeFraction = 0.7f;

bool mask_covers(u8 family_mask, u32 family) {
    return (family_mask & (1u << family)) != 0;
}

/// Total chaff the level's wave table will throw, per family. The type
/// assignment's only input about what the bot is going to be fighting.
void level_family_mix(const WaveDirector& waves, f64 out[kFamilyCount]) {
    for (u32 f = 0; f < kFamilyCount; ++f) out[f] = 0.0;
    for (const WaveDef& w : waves.waves()) {
        for (const SpawnEntry& e : w.spawns) {
            const u32 f = static_cast<u32>(e.family);
            if (f < kFamilyCount) out[f] += static_cast<f64>(e.count);
        }
    }
    f64 total = 0.0;
    for (u32 f = 0; f < kFamilyCount; ++f) total += out[f];
    if (total <= 0.0) {
        // A level with no authored spawns cannot happen (the loader rejects
        // one), but a caller may hand us a director that was never given the
        // table. Flat weights beat a division by zero.
        for (u32 f = 0; f < kFamilyCount; ++f) out[f] = 1.0 / static_cast<f64>(kFamilyCount);
        return;
    }
    for (u32 f = 0; f < kFamilyCount; ++f) out[f] /= total;
}

/// Author's own "build here" hint, if the site falls inside a tagged zone.
f32 zone_bonus(const LevelDef& level, Vec2 p) {
    f32 best = 0.0f;
    for (usize i = 0; i < level.placement_zones.size(); ++i) {
        if (!level.placement_zones[i].contains(p)) continue;
        f32 bonus = 1.0f;
        if (i < level.placement_zone_tags.size()) {
            const PlacementZoneTag& tag = level.placement_zone_tags[i];
            bonus = tag.priority * (tag.concentrated ? 2.0f : 1.0f);
        }
        best = math::max(best, bonus);
    }
    return best;
}

/// How many distinct lanes a tower here could reach. A site covering a
/// bifurcation is worth more than two sites covering one branch each.
u32 lanes_covered(const LaneOwnershipMap& lanes, Vec2 p, f32 range) {
    if (lanes.lane_ids.empty() || lanes.width <= 0) return 0;
    bool seen[256] = {};
    u32 count = 0;
    const f32 step = math::max(lanes.cell_size, 0.25f);
    for (f32 dy = -range; dy <= range; dy += step) {
        for (f32 dx = -range; dx <= range; dx += step) {
            if (dx * dx + dy * dy > range * range) continue;
            const i32 lane = lanes.lane_at(Vec2{p.x + dx, p.y + dy});
            if (lane < 0 || lane >= 256) continue;
            if (seen[lane]) continue;
            seen[lane] = true;
            ++count;
        }
    }
    return count;
}

} // namespace

std::string autoplay_profile_name(AutoPlayProfile profile, TowerType single_type) {
    switch (profile) {
        case AutoPlayProfile::GreedyCheapest: return "greedy-cheapest";
        case AutoPlayProfile::SpreadCoverage: return "spread-coverage";
        case AutoPlayProfile::SaveForTier3: return "save-for-tier3";
        case AutoPlayProfile::SingleType:
            return std::string("single-type:") + tower_type_name(single_type);
    }
    return "greedy-cheapest";
}

bool parse_autoplay_profile(std::string_view text, AutoPlayProfile& out_profile,
                            TowerType& out_single_type) {
    if (text.empty() || text == "greedy-cheapest" || text == "greedy") {
        out_profile = AutoPlayProfile::GreedyCheapest;
        return true;
    }
    if (text == "spread-coverage" || text == "spread") {
        out_profile = AutoPlayProfile::SpreadCoverage;
        return true;
    }
    if (text == "save-for-tier3" || text == "tier3") {
        out_profile = AutoPlayProfile::SaveForTier3;
        return true;
    }
    const std::string_view prefix = "single-type:";
    if (text.size() > prefix.size() && text.substr(0, prefix.size()) == prefix) {
        TowerType type{};
        if (!parse_tower_type(text.substr(prefix.size()), type)) return false;
        out_profile = AutoPlayProfile::SingleType;
        out_single_type = type;
        return true;
    }
    return false;
}

void AutoPlayer::plan(const LevelDef& level, const LaneOwnershipMap& lanes,
                      const sim::SimWorld& world, const TowerSystem& towers,
                      const WaveDirector& waves) {
    sites_.clear();
    next_site_ = 0;

    f64 mix[kFamilyCount];
    level_family_mix(waves, mix);

    // --- 1. Candidates: every spline sample, with the level's own geometry.
    struct Candidate {
        Vec2 pos;
        f32 width;
        f32 cost_to_goal;
        f32 score;
        u32 lanes;
    };
    std::vector<Candidate> candidates;

    // Reference range for the coverage term. Using one type's range (rather
    // than each candidate's eventual type, which is not chosen yet) keeps the
    // ordering independent of the assignment that follows it.
    const f32 reference_range = towers.stats(TowerType::Macrophage, 1).range;

    f32 max_cost = 0.0f;
    for (const Vessel& v : level.vessels) {
        if (v.points.size() < 2) continue;
        sim::VesselSpline spline;
        spline.points.reserve(v.points.size());
        for (const VesselPoint& p : v.points) {
            spline.points.push_back(sim::VesselPoint{p.position, p.width, 1.0f});
        }
        const f32 max_u = static_cast<f32>(spline.points.size() - 1);
        // Step in world units by converting through a coarse arc estimate: the
        // spline is close enough to uniform in u over one segment that a fixed
        // sub-division per segment is within a sample of the real spacing, and
        // being exactly on arc length buys nothing a scoring pass can use.
        const f32 samples_per_segment = math::max(2.0f, 8.0f / kSampleStep);
        for (f32 u = 0.0f; u <= max_u; u += 1.0f / samples_per_segment) {
            const sim::VesselPoint sample = sim::eval_spline(spline, u);
            const f32 cost = world.flow().sample_cost(sample.pos);
            if (!std::isfinite(cost)) continue;   // unreachable pocket
            Candidate c;
            c.pos = sample.pos;
            c.width = sample.width;
            c.cost_to_goal = cost;
            c.lanes = lanes_covered(lanes, sample.pos, reference_range);
            candidates.push_back(c);
            max_cost = math::max(max_cost, cost);
        }
    }
    if (candidates.empty()) {
        IMMUNE_LOG_WARN("autoplay: level '%s' produced no candidate sites", level.name.c_str());
        return;
    }

    // --- 2. Score. Every term is normalized to roughly [0,1] so the weights
    // below are readable as relative importance rather than as unit
    // conversions.
    f32 max_width = 0.0f;
    for (const Candidate& c : candidates) max_width = math::max(max_width, c.width);
    if (max_width <= 0.0f) max_width = 1.0f;
    if (max_cost <= 0.0f) max_cost = 1.0f;

    for (Candidate& c : candidates) {
        const f32 choke = 1.0f - (c.width / max_width);          // narrow = high
        const f32 lateness = 1.0f - (c.cost_to_goal / max_cost);  // near goal = high
        const f32 coverage = c.lanes > 1 ? static_cast<f32>(c.lanes - 1) : 0.0f;
        c.score = 1.0f * choke                //
                  + 0.6f * lateness           //
                  + 0.5f * coverage           //
                  + 0.8f * zone_bonus(level, c.pos);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    // --- 3. Accept greedily with spacing, choosing the type per site as we
    // go. Type and legality have to be decided together: footprints differ by
    // type, so a site accepted on one tower's clearance is not necessarily a
    // site another tower fits in, and deciding the type afterwards would
    // either invalidate accepted sites or force a probe so conservative that
    // most of a tight level goes unbuilt.
    const f32 spacing = reference_range * kSpacingRangeFraction;
    u32 placed_of_type[kTowerTypeCount] = {};

    // Each type's share of the horde it can actually touch. A type that cannot
    // hit the dominant family is not a considered choice, it is a wasted site.
    f32 coverage[kTowerTypeCount] = {};
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const TowerStats& st = towers.stats(static_cast<TowerType>(t), 1);
        for (u32 f = 0; f < kFamilyCount; ++f) {
            if (mask_covers(st.family_mask, f)) coverage[t] += static_cast<f32>(mix[f]);
        }
    }

    for (const Candidate& c : candidates) {
        if (sites_.size() >= cfg_.max_sites) break;
        bool too_close = false;
        for (const PlannedSite& s : sites_) {
            if (math::length_sq(s.position - c.pos) < spacing * spacing) {
                too_close = true;
                break;
            }
        }
        if (too_close) continue;

        // Try types best-first and take the first one that can legally stand
        // here. `available_atp` is maximal: the plan is about geometry, and
        // affordability is re-decided at every purchase.
        u8 tried = 0;
        bool placed = false;
        while (tried < kTowerTypeCount && !placed) {
            TowerType best = TowerType::Neutrophil;
            f32 best_score = -std::numeric_limits<f32>::max();
            bool have_candidate = false;
            for (u32 t = 0; t < kTowerTypeCount; ++t) {
                if ((tried & (1u << t)) != 0) continue;
                if (cfg_.profile == AutoPlayProfile::SingleType &&
                    static_cast<TowerType>(t) != cfg_.single_type) {
                    continue;
                }
                // Diminishing returns per type, so one strong type does not
                // take every site and leave five towers untested in the report.
                const f32 score = coverage[t] - 0.18f * static_cast<f32>(placed_of_type[t]);
                if (!have_candidate || score > best_score) {
                    best_score = score;
                    best = static_cast<TowerType>(t);
                    have_candidate = true;
                }
            }
            if (!have_candidate) break;
            tried |= static_cast<u8>(1u << static_cast<u32>(best));

            const PlacementQuery q =
                towers.validate(world, best, c.pos, std::numeric_limits<u32>::max());
            if (!q.valid() && q.result != PlacementResult::CannotAfford) continue;

            PlannedSite site;
            site.position = q.snapped_position;
            site.type = best;
            site.score = c.score;
            site.lumen_width = c.width;
            site.cost_to_goal = c.cost_to_goal;
            sites_.push_back(site);
            ++placed_of_type[static_cast<u32>(best)];
            placed = true;
        }
    }

    IMMUNE_LOG_INFO("autoplay: planned %zu sites on '%s' (profile %s)", sites_.size(),
                    level.name.c_str(),
                    autoplay_profile_name(cfg_.profile, cfg_.single_type).c_str());
}

u32 AutoPlayer::rejected_sites() const {
    u32 n = 0;
    for (const PlannedSite& s : sites_) {
        if (s.rejected) ++n;
    }
    return n;
}

AutoPlayAction AutoPlayer::choose(const sim::SimWorld& world, const TowerSystem& towers,
                                  const Economy& economy) {
    // --- The cheapest unbuilt site the plan still wants, in plan order.
    AutoPlayAction place;
    for (usize i = next_site_; i < sites_.size(); ++i) {
        const PlannedSite& s = sites_[i];
        if (s.built.valid() || s.rejected) continue;
        place.kind = AutoPlayAction::Kind::Placed;
        place.type = s.type;
        place.position = s.position;
        place.cost = towers.stats(s.type, 1).build_cost;
        place.tier = 1;
        break;
    }

    // --- The upgrade this profile wants most. Greedy profiles want the
    // cheapest; the saver wants the least-developed tower, so investment
    // deepens evenly instead of taking one tower to tier 3 and abandoning the
    // rest (which would measure a single tower, not a strategy).
    AutoPlayAction upgrade;
    u8 best_tier = 4;
    for (const PlannedSite& s : sites_) {
        if (!s.built.valid()) continue;
        const u32 cost = towers.upgrade_cost(world, s.built);
        if (cost == 0) continue;   // already tier 3, or sold out from under us
        bool better = upgrade.kind == AutoPlayAction::Kind::None;
        if (!better) {
            better = cfg_.profile == AutoPlayProfile::SaveForTier3
                         ? (s.tier < best_tier || (s.tier == best_tier && cost < upgrade.cost))
                         : cost < upgrade.cost;
        }
        if (!better) continue;
        best_tier = s.tier;
        upgrade.kind = AutoPlayAction::Kind::Upgraded;
        upgrade.tower = s.built;
        upgrade.type = s.type;
        upgrade.position = s.position;
        upgrade.cost = cost;
        upgrade.tier = static_cast<u8>(s.tier + 1);
    }

    const bool can_place =
        place.kind != AutoPlayAction::Kind::None && economy.can_afford(place.cost);
    const bool can_upgrade =
        upgrade.kind != AutoPlayAction::Kind::None && economy.can_afford(upgrade.cost);

    switch (cfg_.profile) {
        case AutoPlayProfile::SpreadCoverage:
            if (can_place) return place;
            return can_upgrade ? upgrade : AutoPlayAction{};
        case AutoPlayProfile::SaveForTier3:
            // Depth first, but never stall: with nothing left to upgrade, a
            // saver still builds rather than banking ATP forever.
            if (can_upgrade) return upgrade;
            return can_place ? place : AutoPlayAction{};
        case AutoPlayProfile::GreedyCheapest:
        case AutoPlayProfile::SingleType:
        default:
            if (can_place && can_upgrade) return place.cost <= upgrade.cost ? place : upgrade;
            if (can_place) return place;
            return can_upgrade ? upgrade : AutoPlayAction{};
    }
}

AutoPlayAction AutoPlayer::tick(sim::SimWorld& world, TowerSystem& towers, Economy& economy,
                                u64 tick) {
    const u32 interval = cfg_.decision_interval_ticks == 0 ? 1 : cfg_.decision_interval_ticks;
    if (tick % interval != 0) return AutoPlayAction{};

    AutoPlayAction action = choose(world, towers, economy);
    if (action.kind == AutoPlayAction::Kind::None) return action;

    if (action.kind == AutoPlayAction::Kind::Placed) {
        // Re-validate: the flow field and the neighbouring footprints have both
        // moved since the plan was made, and WouldBlockAllPaths in particular is
        // a function of what is already standing.
        for (usize i = next_site_; i < sites_.size(); ++i) {
            PlannedSite& s = sites_[i];
            if (s.built.valid() || s.rejected) {
                if (i == next_site_) ++next_site_;
                continue;
            }
            const PlacementQuery q = towers.validate(world, s.type, s.position, economy.atp());
            if (!q.valid()) {
                if (q.result == PlacementResult::CannotAfford) return AutoPlayAction{};
                // Anything else is permanent for this site: the thing that
                // invalidated it (a neighbour's footprint, a walled-off lane)
                // does not un-happen.
                s.rejected = true;
                continue;
            }
            const u32 cost = towers.stats(s.type, 1).build_cost;
            if (!economy.can_afford(cost)) return AutoPlayAction{};
            const EntityId placed = towers.place(world, s.type, q.snapped_position);
            if (!placed.valid()) {
                s.rejected = true;
                continue;
            }
            economy.spend(cost);
            s.built = placed;
            s.tier = 1;
            s.position = q.snapped_position;
            action.tower = placed;
            action.type = s.type;
            action.position = q.snapped_position;
            action.cost = cost;
            action.tier = 1;
            return action;
        }
        return AutoPlayAction{};
    }

    // Upgrade. upgrade() charges nothing by design (TowerSystem.h), so the
    // spend is this caller's, exactly as it is in app/.
    if (!economy.can_afford(action.cost)) return AutoPlayAction{};
    const u8 new_tier = towers.upgrade(world, action.tower);
    if (new_tier == 0) return AutoPlayAction{};
    economy.spend(action.cost);
    action.tier = new_tier;
    for (PlannedSite& s : sites_) {
        if (s.built == action.tower) s.tier = new_tier;
    }
    return action;
}

} // namespace immune::game
