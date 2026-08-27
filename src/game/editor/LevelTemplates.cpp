// game/editor/LevelTemplates.cpp — see LevelTemplates.h.
//
// Every template follows the same recipe, which is also the recipe a level has
// to satisfy to be playable at all: lanes wide enough to hold squads, a spawn
// point ON the lumen at the head of each lane, an objective ON the lumen at the
// end, placement zones straddling the lane, and a wave table that ramps.
#include "game/editor/LevelTemplates.h"

#include "core/Math.h"

#include <algorithm>
#include <cmath>

namespace immune::game {
namespace {

/// Placement zone straddling a run of the lane, generous enough to actually
/// build in. `concentrated` marks the bends, per DESIGN.md 4.3/4.7's "generous
/// at bends, tight along straights".
///
/// CLAMPED TO THE WORLD on both corners. The padding that makes a zone generous
/// is exactly what pushes it past the edge on a lane that runs near one, and a
/// zone hanging outside the level is buildable area that does not exist -- the
/// validator warns about it, and File > New should never produce a level that
/// warns on its first frame.
void add_zone(LevelDef& d, Vec2 a, Vec2 b, f32 pad, bool concentrated, f32 priority = 1.0f) {
    Rect r;
    r.min = Vec2{math::min(a.x, b.x) - pad, math::min(a.y, b.y) - pad};
    r.max = Vec2{math::max(a.x, b.x) + pad, math::max(a.y, b.y) + pad};
    r.min = Vec2{math::max(r.min.x, d.world_bounds.min.x),
                 math::max(r.min.y, d.world_bounds.min.y)};
    r.max = Vec2{math::min(r.max.x, d.world_bounds.max.x),
                 math::min(r.max.y, d.world_bounds.max.y)};
    if (r.max.x <= r.min.x || r.max.y <= r.min.y) return;   // fully outside
    d.placement_zones.push_back(r);
    d.placement_zone_tags.push_back(PlacementZoneTag{concentrated, priority});
}

Vessel make_vessel(const std::string& id, const std::string& lane, VesselType type,
                   const std::vector<Vec2>& pts, f32 width) {
    Vessel v;
    v.id = id;
    v.lane_id = lane;
    v.type = type;
    v.points.reserve(pts.size());
    for (Vec2 p : pts) v.points.push_back(VesselPoint{p, width});
    return v;
}

} // namespace

const char* level_template_to_string(LevelTemplate t) {
    switch (t) {
    case LevelTemplate::StraightLane: return "Straight lane";
    case LevelTemplate::Fork: return "Fork";
    case LevelTemplate::Switchback: return "Switchback";
    case LevelTemplate::ConvergentChamber: return "Convergent chamber";
    case LevelTemplate::MultiLaneTrunk: return "Multi-lane trunk";
    }
    return "?";
}

std::vector<LevelTemplate> all_level_templates() {
    return {LevelTemplate::StraightLane, LevelTemplate::Fork, LevelTemplate::Switchback,
            LevelTemplate::ConvergentChamber, LevelTemplate::MultiLaneTrunk};
}

LevelDef make_template(LevelTemplate t, const TemplateParams& params) {
    LevelDef d;
    d.schema = 1;
    d.name = params.name;
    d.region = params.region;
    d.cell_size = params.cell_size > 0.0f ? params.cell_size : 0.5f;

    const f32 w = math::max(params.world_size.x, 64.0f);
    const f32 h = math::max(params.world_size.y, 64.0f);
    d.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{w, h}};

    // Keep the lumen inside the world with room for its own wall: a lane whose
    // half-width runs off the edge rasterizes clipped and reads as a mistake.
    const f32 lane = math::clamp(params.lane_width, 4.0f, math::min(w, h) * 0.5f);
    const f32 half = lane * 0.5f;
    const f32 margin = half + 8.0f;

    switch (t) {
    case LevelTemplate::StraightLane: {
        const f32 y = h * 0.5f;
        d.vessels.push_back(make_vessel("main", "main", VesselType::Artery,
                                        {Vec2{margin, y}, Vec2{w * 0.5f, y}, Vec2{w - margin, y}},
                                        lane));
        d.spawn_points.push_back(SpawnPoint{"p0", Vec2{margin, y}, half * 0.7f, "main"});
        d.objectives.push_back(ObjectivePoint{"organ", Vec2{w - margin, y},
                                              Vec2{half * 0.4f, half * 0.4f}, 0.0f, 100.0f});
        add_zone(d, Vec2{margin, y}, Vec2{w - margin, y}, half + 20.0f, false);
        break;
    }
    case LevelTemplate::Fork: {
        const f32 y = h * 0.5f;
        const f32 split = w * 0.45f;
        d.vessels.push_back(make_vessel("trunk", "main", VesselType::Artery,
                                        {Vec2{margin, y}, Vec2{split, y}}, lane));
        d.vessels.push_back(make_vessel("branch_a", "main", VesselType::Artery,
                                        {Vec2{split, y}, Vec2{w * 0.72f, y - h * 0.22f},
                                         Vec2{w - margin, y - h * 0.18f}},
                                        lane * 0.8f));
        d.vessels.push_back(make_vessel("branch_b", "main", VesselType::Artery,
                                        {Vec2{split, y}, Vec2{w * 0.72f, y + h * 0.22f},
                                         Vec2{w - margin, y + h * 0.18f}},
                                        lane * 0.8f));
        // `children` is what welds a bifurcation together when the parent's
        // terminal point is dragged.
        d.vessels[0].children = {"branch_a", "branch_b"};
        d.spawn_points.push_back(SpawnPoint{"p0", Vec2{margin, y}, half * 0.7f, "main"});
        d.objectives.push_back(
            ObjectivePoint{"organ_a", Vec2{w - margin, y - h * 0.18f},
                           Vec2{half * 0.35f, half * 0.35f}, 0.0f, 100.0f});
        d.objectives.push_back(
            ObjectivePoint{"organ_b", Vec2{w - margin, y + h * 0.18f},
                           Vec2{half * 0.35f, half * 0.35f}, 0.0f, 100.0f});
        add_zone(d, Vec2{split, y}, Vec2{split, y}, half + 30.0f, true, 1.5f);
        add_zone(d, Vec2{margin, y}, Vec2{w - margin, y}, half + 24.0f, false);
        break;
    }
    case LevelTemplate::Switchback: {
        // A hairpin. The shape that makes tower range and lane length matter,
        // and the one whose flow field is most worth looking at while editing.
        const f32 top = margin;
        const f32 mid = h * 0.5f;
        const f32 bot = h - margin;
        d.vessels.push_back(make_vessel(
            "main", "main", VesselType::Artery,
            {Vec2{margin, top}, Vec2{w - margin, top}, Vec2{w - margin, mid}, Vec2{margin, mid},
             Vec2{margin, bot}, Vec2{w - margin, bot}},
            lane));
        d.spawn_points.push_back(SpawnPoint{"p0", Vec2{margin, top}, half * 0.7f, "main"});
        d.objectives.push_back(ObjectivePoint{"organ", Vec2{w - margin, bot},
                                              Vec2{half * 0.4f, half * 0.4f}, 0.0f, 100.0f});
        add_zone(d, Vec2{w - margin, top}, Vec2{w - margin, mid}, half + 26.0f, true, 1.6f);
        add_zone(d, Vec2{margin, mid}, Vec2{margin, bot}, half + 26.0f, true, 1.6f);
        add_zone(d, Vec2{margin, top}, Vec2{w - margin, bot}, half + 10.0f, false);
        break;
    }
    case LevelTemplate::ConvergentChamber: {
        const f32 goal_x = w - margin;
        const f32 goal_y = h * 0.5f;
        d.vessels.push_back(make_vessel("upper", "upper", VesselType::Artery,
                                        {Vec2{margin, margin}, Vec2{w * 0.5f, h * 0.32f},
                                         Vec2{goal_x, goal_y}},
                                        lane));
        d.vessels.push_back(make_vessel("lower", "lower", VesselType::Lymphatic,
                                        {Vec2{margin, h - margin}, Vec2{w * 0.5f, h * 0.68f},
                                         Vec2{goal_x, goal_y}},
                                        lane));
        d.spawn_points.push_back(SpawnPoint{"p_upper", Vec2{margin, margin}, half * 0.7f, "upper"});
        d.spawn_points.push_back(
            SpawnPoint{"p_lower", Vec2{margin, h - margin}, half * 0.7f, "lower"});
        d.objectives.push_back(ObjectivePoint{"organ", Vec2{goal_x, goal_y},
                                              Vec2{half * 0.5f, half * 0.5f}, 0.0f, 100.0f});
        add_zone(d, Vec2{goal_x, goal_y}, Vec2{goal_x, goal_y}, half + 40.0f, true, 1.8f);
        add_zone(d, Vec2{margin, margin}, Vec2{goal_x, h - margin}, 8.0f, false);
        break;
    }
    case LevelTemplate::MultiLaneTrunk: {
        // Three NAMED, TYPED lanes converging on one objective -- DESIGN.md
        // 4.1/9.2's whole point, and the case the lane hue and threat readout
        // exist for.
        const f32 goal_x = w - margin;
        const f32 goal_y = h * 0.5f;
        const f32 narrow = lane * 0.75f;
        struct LaneSpec {
            const char* id;
            VesselType type;
            f32 y;
        };
        const LaneSpec lanes[3] = {{"artery", VesselType::Artery, margin},
                                   {"lymph", VesselType::Lymphatic, h * 0.5f},
                                   {"nerve", VesselType::NerveAdjacent, h - margin}};
        for (const LaneSpec& l : lanes) {
            d.vessels.push_back(make_vessel(
                l.id, l.id, l.type,
                {Vec2{margin, l.y}, Vec2{w * 0.55f, (l.y + goal_y) * 0.5f}, Vec2{goal_x, goal_y}},
                narrow));
            d.spawn_points.push_back(
                SpawnPoint{std::string("p_") + l.id, Vec2{margin, l.y}, narrow * 0.4f, l.id});
        }
        d.objectives.push_back(ObjectivePoint{"organ", Vec2{goal_x, goal_y},
                                              Vec2{narrow * 0.5f, narrow * 0.5f}, 0.0f, 100.0f});
        add_zone(d, Vec2{goal_x, goal_y}, Vec2{goal_x, goal_y}, narrow + 40.0f, true, 1.8f);
        add_zone(d, Vec2{margin, margin}, Vec2{goal_x, h - margin}, 8.0f, false);
        break;
    }
    }

    WaveRampParams ramp;
    ramp.wave_count = math::max(params.wave_count, 1u);
    // Scale the opening counts to the number of lanes, so a three-lane template
    // does not arrive three times as hard as a one-lane one.
    const f32 lane_scale = 1.0f / static_cast<f32>(math::max<usize>(d.spawn_points.size(), 1));
    ramp.first_count[0] = static_cast<u32>(135.0f * lane_scale) + 10;
    ramp.last_count[0] = static_cast<u32>(600.0f * lane_scale) + 20;
    ramp.first_count[1] = 0;
    ramp.last_count[1] = static_cast<u32>(240.0f * lane_scale) + 10;
    d.waves = make_wave_ramp(ramp);

    // On a multi-spawn level the generated entries are duplicated per spawn
    // point, so every lane actually receives a horde. A single entry with an
    // empty spawn_point_id would put the whole wave down lane one and leave the
    // others empty all match -- which the validator warns about, but which is
    // better simply not to author.
    if (d.spawn_points.size() > 1) {
        for (WaveDef& wv : d.waves) {
            std::vector<SpawnEntry> spread;
            for (const SpawnEntry& e : wv.spawns) {
                for (const SpawnPoint& sp : d.spawn_points) {
                    SpawnEntry copy = e;
                    copy.spawn_point_id = sp.id;
                    spread.push_back(copy);
                }
            }
            wv.spawns = std::move(spread);
        }
    }
    return d;
}

std::vector<WaveDef> make_wave_ramp(const WaveRampParams& params) {
    std::vector<WaveDef> out;
    const u32 n = math::max(params.wave_count, 1u);
    out.reserve(n);

    for (u32 i = 0; i < n; ++i) {
        // t runs 0..1 across the table; a single-wave table sits at the start
        // of the ramp rather than dividing by zero.
        const f32 t = n > 1 ? static_cast<f32>(i) / static_cast<f32>(n - 1) : 0.0f;
        const f32 curved = std::pow(t, math::max(params.curve, 0.01f));

        WaveDef w;
        w.index = i;
        w.name = params.name_prefix + "_" + std::to_string(i + 1);
        w.prep_time = math::lerp(params.first_prep, params.last_prep, t);
        w.atp_reward = static_cast<u32>(
            math::lerp(static_cast<f32>(params.first_atp), static_cast<f32>(params.last_atp), t));

        for (u32 f = 0; f < kFamilyCount; ++f) {
            if (params.first_count[f] == 0 && params.last_count[f] == 0) continue;
            if (i < params.first_wave[f]) continue;
            // Re-normalise the ramp over the waves this family actually appears
            // in, so a family that joins at wave 2 still ramps from its own
            // first count rather than starting partway up someone else's curve.
            const u32 span = n > params.first_wave[f] ? n - params.first_wave[f] : 1;
            const f32 ft = span > 1 ? static_cast<f32>(i - params.first_wave[f]) /
                                          static_cast<f32>(span - 1)
                                    : 0.0f;
            const f32 fcurved = std::pow(ft, math::max(params.curve, 0.01f));

            SpawnEntry e;
            e.family = static_cast<PathogenFamily>(f);
            e.count = static_cast<u32>(math::lerp(static_cast<f32>(params.first_count[f]),
                                                  static_cast<f32>(params.last_count[f]), fcurved));
            // parse_spawn_entry rejects count 0? No -- but a zero-count entry is
            // an entry that does nothing, and the validator warns about a wave
            // that spawns nothing. Keep at least one.
            e.count = math::max(e.count, 1u);
            e.start_time = f == 0 ? 0.0f : 1.0f + static_cast<f32>(f);
            e.duration = math::max(params.duration, 0.1f);
            e.spawn_point_id = params.spawn_point_id;
            w.spawns.push_back(e);
        }
        if (w.spawns.empty()) {
            SpawnEntry e;
            e.family = PathogenFamily::Virus;
            e.count = math::max(params.first_count[0], 1u);
            e.duration = math::max(params.duration, 0.1f);
            e.spawn_point_id = params.spawn_point_id;
            w.spawns.push_back(e);
        }
        (void)curved;
        out.push_back(std::move(w));
    }
    return out;
}

WaveBudget wave_budget(const WaveDef& w) {
    WaveBudget b;
    if (w.spawns.empty()) return b;

    f32 first = 1e30f;
    f32 last = -1e30f;
    for (const SpawnEntry& e : w.spawns) {
        b.total_agents += e.count;
        first = math::min(first, e.start_time);
        last = math::max(last, e.start_time + e.duration);
    }
    b.span_seconds = math::max(last - first, 0.0f);

    // Peak release rate: entries overlap in time, so the honest number is the
    // highest SUM of concurrent per-second rates, not the largest single entry.
    // Sampled at every entry boundary, which is where the sum can change.
    std::vector<f32> marks;
    marks.reserve(w.spawns.size() * 2);
    for (const SpawnEntry& e : w.spawns) {
        marks.push_back(e.start_time);
        marks.push_back(e.start_time + e.duration);
    }
    std::sort(marks.begin(), marks.end());
    for (f32 m : marks) {
        f32 rate = 0.0f;
        for (const SpawnEntry& e : w.spawns) {
            // Half-open [start, end): an entry that ends exactly at `m` is no
            // longer releasing, and one that starts there is.
            if (m < e.start_time || m >= e.start_time + e.duration) continue;
            rate += static_cast<f32>(e.count) / math::max(e.duration, 0.01f);
        }
        b.peak_per_second = math::max(b.peak_per_second, rate);
    }
    return b;
}

} // namespace immune::game
