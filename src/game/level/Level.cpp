// game/level/Level.cpp — spline JSON loader + sim wiring. Owner: Wave 2D,
// extended by Wave 4D for the multi-lane schema (see Level.h's header comment
// for the rationale and the JSON shape).
//
// Parses the JSON schema documented in Level.h, validates it, and rasterizes
// it into a SimWorld's TissueMask/DistanceField/FlowField. See TissueRaster.h
// and FlowField.h (Wave 1A) for the geometry -> grid pipeline this drives.
#include "game/level/Level.h"

#include "core/Math.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/TissueRaster.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace immune::game {

const char* vessel_type_to_string(VesselType t) {
    switch (t) {
        case VesselType::Artery: return "artery";
        case VesselType::Vein: return "vein";
        case VesselType::Lymphatic: return "lymphatic";
        case VesselType::NerveAdjacent: return "nerve_adjacent";
        case VesselType::MucosalFold: return "mucosal_fold";
    }
    return "artery";
}

VesselType vessel_type_from_string(const std::string& s) {
    if (s == "artery") return VesselType::Artery;
    if (s == "vein") return VesselType::Vein;
    if (s == "lymphatic") return VesselType::Lymphatic;
    if (s == "nerve_adjacent") return VesselType::NerveAdjacent;
    if (s == "mucosal_fold") return VesselType::MucosalFold;
    return VesselType::Artery; // unknown or omitted -> documented default.
}

IVec2 LaneOwnershipMap::world_to_cell(Vec2 p) const {
    const Vec2 local = (p - world_origin) / cell_size;
    return IVec2{static_cast<i32>(std::floor(local.x)), static_cast<i32>(std::floor(local.y))};
}

i32 LaneOwnershipMap::lane_at(Vec2 p) const {
    const IVec2 c = world_to_cell(p);
    if (!in_range(c.x, c.y)) return -1;
    const u8 o = owner[index(c.x, c.y)];
    return o == kNoLane ? -1 : static_cast<i32>(o);
}

namespace {

using json = nlohmann::json;

/// nlohmann exceptions carry a message but no field context useful to a level
/// author; every parse helper below throws std::runtime_error with a field
/// path instead, and load_string() is the single place that catches and turns
/// that into a LevelLoadResult.
Vec2 parse_vec2(const json& j, const char* field) {
    if (!j.is_array() || j.size() != 2) {
        throw std::runtime_error(std::string("'") + field + "' must be a 2-element array");
    }
    return Vec2{j.at(0).get<f32>(), j.at(1).get<f32>()};
}

Vessel parse_vessel(const json& j, usize index) {
    const std::string ctx = "vessels[" + std::to_string(index) + "]";
    Vessel v;
    if (!j.contains("id")) throw std::runtime_error(ctx + ": missing 'id'");
    v.id = j.at("id").get<std::string>();
    // lane_id defaults to id: every pre-existing single-vessel level JSON
    // (no "lane_id" field at all) is automatically a well-formed single-lane
    // level with no content changes required.
    v.lane_id = j.value("lane_id", v.id);
    v.type = vessel_type_from_string(j.value("vessel_type", std::string{}));
    if (!j.contains("points")) throw std::runtime_error(ctx + " ('" + v.id + "'): missing 'points'");
    const json& pts = j.at("points");
    if (!pts.is_array()) throw std::runtime_error(ctx + " ('" + v.id + "'): 'points' must be an array");
    v.points.reserve(pts.size());
    for (const auto& p : pts) {
        if (!p.contains("p")) throw std::runtime_error(ctx + " ('" + v.id + "'): point missing 'p'");
        VesselPoint vp;
        vp.position = parse_vec2(p.at("p"), "vessels[].points[].p");
        vp.width = p.value("w", 4.0f);
        v.points.push_back(vp);
    }
    if (j.contains("children")) {
        for (const auto& c : j.at("children")) v.children.push_back(c.get<std::string>());
    }
    return v;
}

SpawnPoint parse_spawn_point(const json& j, usize index) {
    const std::string ctx = "spawn_points[" + std::to_string(index) + "]";
    SpawnPoint p;
    if (!j.contains("id")) throw std::runtime_error(ctx + ": missing 'id'");
    p.id = j.at("id").get<std::string>();
    if (!j.contains("pos")) throw std::runtime_error(ctx + " ('" + p.id + "'): missing 'pos'");
    p.position = parse_vec2(j.at("pos"), "spawn_points[].pos");
    p.radius = j.value("radius", 3.0f);
    // Empty is a valid, expected value here (legacy/single-lane levels never
    // set it); callers resolve the effective lane via
    // LevelLoader::resolve_spawn_point_lane_id() rather than reading this raw.
    p.lane_id = j.value("lane_id", std::string{});
    return p;
}

ObjectivePoint parse_objective(const json& j, usize index) {
    const std::string ctx = "objectives[" + std::to_string(index) + "]";
    ObjectivePoint o;
    if (!j.contains("id")) throw std::runtime_error(ctx + ": missing 'id'");
    o.id = j.at("id").get<std::string>();
    if (!j.contains("pos")) throw std::runtime_error(ctx + " ('" + o.id + "'): missing 'pos'");
    o.position = parse_vec2(j.at("pos"), "objectives[].pos");
    o.radius = j.value("radius", 5.0f);
    o.integrity = j.value("integrity", 100.0f);
    return o;
}

Rect parse_rect(const json& j, usize index) {
    const std::string ctx = "placement_zones[" + std::to_string(index) + "]";
    if (!j.contains("min") || !j.contains("max")) {
        throw std::runtime_error(ctx + ": needs both 'min' and 'max'");
    }
    Rect r;
    r.min = parse_vec2(j.at("min"), "placement_zones[].min");
    r.max = parse_vec2(j.at("max"), "placement_zones[].max");
    return r;
}

/// One authored squad route. `lane_id` is left empty when omitted rather than
/// guessed here -- build_squad_paths() resolves it against the vessel geometry,
/// which this function cannot see.
SquadPathDef parse_squad_path(const json& j, usize index) {
    const std::string ctx = "squad_paths[" + std::to_string(index) + "]";
    SquadPathDef sp;
    if (!j.contains("id")) throw std::runtime_error(ctx + ": missing 'id'");
    sp.id = j.at("id").get<std::string>();
    sp.lane_id = j.value("lane_id", std::string{});
    if (!j.contains("points")) {
        throw std::runtime_error(ctx + " ('" + sp.id + "'): missing 'points'");
    }
    const json& pts = j.at("points");
    if (!pts.is_array()) {
        throw std::runtime_error(ctx + " ('" + sp.id + "'): 'points' must be an array");
    }
    // A one-point path has no direction, so it cannot carry an anchor anywhere.
    // Rejecting it here rather than degrading silently keeps the failure at the
    // level file, where the author can see it.
    if (pts.size() < 2) {
        throw std::runtime_error(ctx + " ('" + sp.id + "'): needs at least 2 points");
    }
    sp.points.reserve(pts.size());
    for (const auto& p : pts) sp.points.push_back(parse_vec2(p, "squad_paths[].points[]"));
    sp.half_width = j.value("half_width", 6.0f);
    if (sp.half_width <= 0.0f) {
        throw std::runtime_error(ctx + " ('" + sp.id + "'): 'half_width' must be > 0");
    }
    return sp;
}

/// DESIGN.md §4.3/§4.7 authoring hint, index-aligned with the Rect above.
/// Both default fields are the same as PlacementZoneTag's in-struct defaults,
/// so an existing placement_zones entry with neither field is unaffected.
PlacementZoneTag parse_placement_zone_tag(const json& j) {
    PlacementZoneTag tag;
    tag.concentrated = j.value("concentrated", false);
    tag.priority = j.value("priority", 1.0f);
    return tag;
}

/// Level JSON speaks the same family names as the enemy roster and the
/// sim-test scripts ("virus", "bacteria"). Unlike the
/// vessel_type case, an unrecognized family is an error rather than a silent
/// default: a typo'd family name changes which horde a wave sends, and that is
/// exactly the kind of mistake a level author needs told about.
PathogenFamily parse_family(const std::string& s, const std::string& ctx) {
    static const char* kNames[kFamilyCount] = {"virus", "bacteria"};
    for (u32 f = 0; f < kFamilyCount; ++f) {
        if (s == kNames[f]) return static_cast<PathogenFamily>(f);
    }
    throw std::runtime_error(ctx + ": unknown family '" + s + "'");
}

WaveModifier parse_wave_modifier(const std::string& s, const std::string& ctx) {
    if (s.empty() || s == "none") return WaveModifier::None;
    if (s == "fever") return WaveModifier::Fever;
    if (s == "swarm") return WaveModifier::Swarm;
    throw std::runtime_error(ctx + ": unknown modifier '" + s + "'");
}

SpawnEntry parse_spawn_entry(const json& j, const std::string& ctx) {
    if (!j.is_object()) throw std::runtime_error(ctx + " must be an object");
    SpawnEntry e;
    e.family = parse_family(j.value("family", std::string("virus")), ctx);
    e.elite_id = j.value("elite_id", u16{0});
    e.count = j.value("count", u32{0});
    e.start_time = j.value("start_time", 0.0f);
    e.duration = j.value("duration", 1.0f);
    e.spawn_point_id = j.value("spawn_point_id", std::string{});
    // A zero-length window would make the whole count due on the first tick of
    // the entry, which is a spawn spike, not a wave. Treated as an authoring
    // error rather than clamped, since the intent ("all at once") is better
    // expressed as a short duration the author picked.
    if (e.duration <= 0.0f) throw std::runtime_error(ctx + ": 'duration' must be > 0");
    if (e.start_time < 0.0f) throw std::runtime_error(ctx + ": 'start_time' must be >= 0");
    return e;
}

WaveDef parse_wave(const json& j, usize index) {
    const std::string ctx = "waves[" + std::to_string(index) + "]";
    if (!j.is_object()) throw std::runtime_error(ctx + " must be an object");
    WaveDef w;
    // Not authorable: array position IS the index the director walks.
    w.index = static_cast<u32>(index);
    w.name = j.value("name", std::string{});
    w.prep_time = j.value("prep_time", 20.0f);
    w.atp_reward = j.value("atp_reward", u32{0});
    w.modifier = parse_wave_modifier(j.value("modifier", std::string{}), ctx);
    if (!j.contains("spawns")) throw std::runtime_error(ctx + ": missing 'spawns'");
    const json& arr = j.at("spawns");
    if (!arr.is_array() || arr.empty()) {
        throw std::runtime_error(ctx + ": 'spawns' must be a non-empty array");
    }
    w.spawns.reserve(arr.size());
    for (usize i = 0; i < arr.size(); ++i) {
        w.spawns.push_back(parse_spawn_entry(arr[i], ctx + ".spawns[" + std::to_string(i) + "]"));
    }
    return w;
}

} // namespace

LevelLoadResult LevelLoader::load_string(const std::string& text, LevelDef& out) const {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        return LevelLoadResult{false, std::string("level JSON parse error: ") + e.what(), 0};
    }

    if (!j.is_object()) {
        return LevelLoadResult{false, "level JSON must be an object", 0};
    }
    // A missing "schema" is an error, not a default (Level.h schema comment).
    if (!j.contains("schema")) {
        return LevelLoadResult{false, "level JSON missing required field 'schema'", 0};
    }

    LevelDef def;
    try {
        def.schema = j.at("schema").get<i32>();
        if (def.schema != 1) {
            return LevelLoadResult{false,
                                   "level JSON has unsupported schema version " +
                                       std::to_string(def.schema) + " (expected 1)",
                                   0};
        }

        def.name = j.value("name", std::string{});
        def.region = j.value("region", std::string{});

        if (j.contains("world")) {
            const json& w = j.at("world");
            if (!w.contains("min") || !w.contains("max")) {
                throw std::runtime_error("'world' needs both 'min' and 'max'");
            }
            def.world_bounds.min = parse_vec2(w.at("min"), "world.min");
            def.world_bounds.max = parse_vec2(w.at("max"), "world.max");
            def.cell_size = w.value("cell_size", 0.5f);
        }

        if (j.contains("vessels")) {
            const json& arr = j.at("vessels");
            if (!arr.is_array()) throw std::runtime_error("'vessels' must be an array");
            def.vessels.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i) def.vessels.push_back(parse_vessel(arr[i], i));
        }
        if (j.contains("spawn_points")) {
            const json& arr = j.at("spawn_points");
            if (!arr.is_array()) throw std::runtime_error("'spawn_points' must be an array");
            def.spawn_points.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i) def.spawn_points.push_back(parse_spawn_point(arr[i], i));
        }
        if (j.contains("objectives")) {
            const json& arr = j.at("objectives");
            if (!arr.is_array()) throw std::runtime_error("'objectives' must be an array");
            def.objectives.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i) def.objectives.push_back(parse_objective(arr[i], i));
        }
        if (j.contains("placement_zones")) {
            const json& arr = j.at("placement_zones");
            if (!arr.is_array()) throw std::runtime_error("'placement_zones' must be an array");
            def.placement_zones.reserve(arr.size());
            def.placement_zone_tags.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i) {
                def.placement_zones.push_back(parse_rect(arr[i], i));
                def.placement_zone_tags.push_back(parse_placement_zone_tag(arr[i]));
            }
        }
        if (j.contains("squad_paths")) {
            const json& arr = j.at("squad_paths");
            if (!arr.is_array()) throw std::runtime_error("'squad_paths' must be an array");
            def.squad_paths.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i)
                def.squad_paths.push_back(parse_squad_path(arr[i], i));
        }
        if (j.contains("ambient_drift")) {
            def.ambient_drift = parse_vec2(j.at("ambient_drift"), "ambient_drift");
        }
        // Required, not optional. A level's wave table is authored here and
        // nowhere else -- there is no region-shaped generator to fall back to
        // any more -- so a level without one has no content, and saying so at
        // load time is the only place it can be said usefully. The check lives
        // in the parser rather than validate() because validate() is not on
        // the app's load path (App::load_level/Modes::build_world both go
        // straight from load_file() to instantiate()).
        if (!j.contains("waves")) throw std::runtime_error("level declares no 'waves'");
        {
            const json& arr = j.at("waves");
            if (!arr.is_array()) throw std::runtime_error("'waves' must be an array");
            if (arr.empty()) throw std::runtime_error("'waves' must not be empty");
            def.waves.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i) def.waves.push_back(parse_wave(arr[i], i));
        }
    } catch (const std::exception& e) {
        return LevelLoadResult{false, std::string("level JSON malformed: ") + e.what(), 0};
    }

    out = std::move(def);
    return LevelLoadResult{true, "", 0};
}

LevelLoadResult LevelLoader::load_file(const std::string& path, LevelDef& out) const {
    const auto text = platform::read_text_file(path);
    if (!text) {
        return LevelLoadResult{false, "cannot read level file '" + path + "'", 0};
    }
    return load_string(*text, out);
}

LevelLoadResult LevelLoader::validate(const LevelDef& def) const {
    if (def.schema != 1) return LevelLoadResult{false, "unsupported or missing level schema", 0};
    if (def.vessels.empty()) return LevelLoadResult{false, "level has no vessels", 0};
    if (def.spawn_points.empty()) return LevelLoadResult{false, "level has no spawn points", 0};
    if (def.objectives.empty()) return LevelLoadResult{false, "level has no objectives", 0};
    // Same rule load_string() enforces, repeated here so a LevelDef built in
    // code (a test fixture, default_test_level()) cannot skip it either.
    if (def.waves.empty()) return LevelLoadResult{false, "level declares no waves", 0};
    // A named spawn point that doesn't exist would make WaveDirector fall back
    // to the first spawn point at runtime, so the wave would silently come out
    // of the wrong lane instead of failing loudly here.
    for (const WaveDef& w : def.waves) {
        for (const SpawnEntry& e : w.spawns) {
            if (e.spawn_point_id.empty()) continue;
            bool found = false;
            for (const SpawnPoint& p : def.spawn_points) {
                if (p.id == e.spawn_point_id) { found = true; break; }
            }
            if (!found) {
                return LevelLoadResult{false,
                                       "wave '" + w.name + "' spawns from unknown spawn point '" +
                                           e.spawn_point_id + "'",
                                       0};
            }
        }
    }
    // An authored squad path naming a lane that does not exist would be
    // silently re-homed onto the nearest lane by build_squad_paths(), which is
    // the right behaviour for an OMITTED lane_id and the wrong one for a typo'd
    // one -- the author would get squads on a lane they never meant to touch.
    for (const SquadPathDef& sp : def.squad_paths) {
        if (sp.points.size() < 2) {
            return LevelLoadResult{false, "squad path '" + sp.id + "' needs at least 2 points", 0};
        }
        if (sp.half_width <= 0.0f) {
            return LevelLoadResult{false, "squad path '" + sp.id + "' has non-positive half_width",
                                   0};
        }
        if (sp.lane_id.empty()) continue;
        bool found = false;
        for (const Vessel& v : def.vessels) {
            if ((v.lane_id.empty() ? v.id : v.lane_id) == sp.lane_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return LevelLoadResult{
                false, "squad path '" + sp.id + "' names unknown lane '" + sp.lane_id + "'", 0};
        }
    }
    return LevelLoadResult{true, "", 0};
}

LevelLoadResult LevelLoader::instantiate(const LevelDef& def, sim::SimWorld& world) const {
    const f32 cell = def.cell_size > 0.0f ? def.cell_size : 0.5f;
    const Vec2 extent = def.world_bounds.size();
    world.tissue().resize(static_cast<i32>(extent.x / cell),
                          static_cast<i32>(extent.y / cell),
                          cell, def.world_bounds.min);

    // Splines -> TissueMask. game::VesselPoint (position/width) maps onto
    // sim::VesselPoint (pos/width/cost_mul); levels don't yet author per-point
    // cost, so every point gets the neutral multiplier.
    std::vector<sim::VesselSpline> splines;
    splines.reserve(def.vessels.size());
    for (const Vessel& v : def.vessels) {
        sim::VesselSpline spline;
        spline.points.reserve(v.points.size());
        for (const VesselPoint& p : v.points) {
            spline.points.push_back(sim::VesselPoint{p.position, p.width, 1.0f});
        }
        splines.push_back(std::move(spline));
    }
    sim::rasterize_vessels(world.tissue(), splines);

    // TissueMask -> DistanceField -> FlowField, per the pipeline in FlowField.h.
    world.sdf().bake(world.tissue());

    sim::FlowFieldBakeDesc flow_desc;
    flow_desc.goal_cells.reserve(def.objectives.size());
    for (const ObjectivePoint& o : def.objectives) {
        flow_desc.goal_cells.push_back(world.tissue().world_to_cell(o.position));
    }
    world.flow().bake(world.tissue(), flow_desc);

    // ECS objective entities: the structural, potentially-multi-objective
    // representation future rendering/UI reads. Distinct from SimWorld's
    // scalar objective_integrity_ (primary-objective HUD/win-condition value,
    // wired elsewhere) — nothing here touches that float.
    for (const ObjectivePoint& o : def.objectives) {
        const entt::entity e = world.ecs().registry().create();
        world.ecs().registry().emplace<sim::comp::Objective>(
            e, sim::comp::Objective{o.integrity, o.integrity, o.radius});
        world.ecs().registry().emplace<sim::comp::Transform>(
            e, sim::comp::Transform{o.position, 0.0f, 1.0f});
    }

    if (!def.objectives.empty()) {
        world.chaff_system().set_goal(def.objectives[0].position, def.objectives[0].radius);
    }
    world.chaff_system().set_world_bounds(def.world_bounds);

    // Spawn points: SimWorld is the only thing WaveDirector::tick() can
    // reach, and LevelDef doesn't survive past this function, so this is the
    // one place spawn point geometry can be captured for the run.
    std::vector<sim::SpawnPointRuntime> spawn_points;
    spawn_points.reserve(def.spawn_points.size());
    for (const SpawnPoint& p : def.spawn_points) {
        spawn_points.push_back(
            sim::SpawnPointRuntime{p.id, p.position, p.radius, resolve_spawn_point_lane_id(def, p)});
    }
    world.set_spawn_points(std::move(spawn_points));

    // Squad routes. Built here, after the mask exists (the derivation snaps
    // off-tissue points back inside it) and pushed into the world for the same
    // reason spawn points are: LevelDef does not survive this call, and the
    // squad registry is the only thing a running tick can reach.
    world.squads().set_paths(build_squad_paths(def, world.tissue()));

    return LevelLoadResult{true, "", 0};
}

namespace {

/// World-space spacing at which a lane centerline is resampled into a polyline.
/// Fine enough that the offset copies track a bend faithfully, coarse enough
/// that a long lane stays a few dozen points rather than a few thousand -- and
/// the anchor projection walks these points every tick, per squad.
constexpr f32 kPathResampleStep = 2.0f;

/// How much lumen one squad needs to itself, centre to centre, for two squads
/// side by side to read as two things rather than one.
///
/// A squad of SquadTuning::target_squad_size is about 11.6 world units across
/// at stock tuning (radius = 0.75 * sqrt(60)); this is that plus a gap of
/// similar size, because two blobs separated by less than their own width read
/// as one blob with a dent in it.
constexpr f32 kMinPathSpacing = 22.0f;

/// Clearance kept between the outermost path and the lane wall, so a squad
/// riding that path is not permanently pressed against tissue.
constexpr f32 kWallClearance = 8.0f;

/// Hard cap on derived paths per lane, whatever the width.
constexpr u32 kMaxAutoPaths = 4;

sim::VesselSpline to_spline(const Vessel& v) {
    sim::VesselSpline spline;
    spline.points.reserve(v.points.size());
    for (const VesselPoint& p : v.points)
        spline.points.push_back(sim::VesselPoint{p.position, p.width, 1.0f});
    return spline;
}

/// Walks a lane's vessels end to end, emitting a resampled centerline together
/// with the local lumen width at each sample. Width travels with the point
/// because the offset step below needs it per-sample: a lane that narrows into
/// a capillary must pull its outer paths in rather than push them into rock.
void sample_lane_centerline(const std::vector<const Vessel*>& vessels,
                            std::vector<Vec2>& out_points, std::vector<f32>& out_widths) {
    out_points.clear();
    out_widths.clear();
    for (const Vessel* v : vessels) {
        if (v->points.size() < 2) continue;
        const sim::VesselSpline spline = to_spline(*v);
        const f32 max_u = static_cast<f32>(v->points.size() - 1);

        // Estimate arclength off the control polygon to choose a sample count.
        // Catmull-Rom bows outside its polygon, so this under-estimates -- the
        // 1.5 factor is the same conservative allowance rasterize_vessel() uses.
        f32 polygon_len = 0.0f;
        for (usize i = 1; i < v->points.size(); ++i)
            polygon_len += math::length(v->points[i].position - v->points[i - 1].position);
        const u32 steps =
            static_cast<u32>(math::max(2.0f, (polygon_len * 1.5f) / kPathResampleStep));

        for (u32 k = 0; k <= steps; ++k) {
            const f32 u = max_u * (static_cast<f32>(k) / static_cast<f32>(steps));
            const sim::VesselPoint vp = sim::eval_spline(spline, u);
            // Drop a sample landing on top of the previous one, so a lane built
            // from touching vessels does not get zero-length segments (which
            // would make the arclength table non-strictly-increasing).
            if (!out_points.empty() && math::length_sq(vp.pos - out_points.back()) < 0.01f) {
                continue;
            }
            out_points.push_back(vp.pos);
            out_widths.push_back(vp.width);
        }
    }
}

/// Offsets a centerline sideways by `fraction` of the local width, then pulls
/// any point that landed off the tissue back toward the centerline until it is
/// walkable again. Returns false if the result is too short to be a path.
///
/// The snap is what makes automatic derivation safe on the existing content: a
/// generous offset through a wide chamber is exactly the offset that would sit
/// in rock where the same lane narrows, and an anchor parked inside a wall
/// would drag its squad into it. Rather than pick a timid global offset, the
/// offset is generous and clamped locally where the geometry demands.
bool offset_centerline(const std::vector<Vec2>& center, const std::vector<f32>& widths,
                       f32 fraction, const sim::TissueMask& mask, std::vector<Vec2>& out) {
    out.clear();
    if (center.size() < 2) return false;
    out.reserve(center.size());

    for (usize i = 0; i < center.size(); ++i) {
        const usize a = (i == 0) ? 0 : i - 1;
        const usize b = (i + 1 < center.size()) ? i + 1 : i;
        const Vec2 tangent = math::normalize_safe(center[b] - center[a]);
        const Vec2 normal{-tangent.y, tangent.x};

        f32 dist = widths[i] * fraction;
        Vec2 p = center[i] + normal * dist;
        // Walk back toward the centerline in a few halvings; the centerline is
        // walkable by construction, so this always lands somewhere legal.
        for (u32 attempt = 0; attempt < 4; ++attempt) {
            const IVec2 c = mask.world_to_cell(p);
            if (mask.walkable(c.x, c.y)) break;
            dist *= 0.5f;
            p = center[i] + normal * dist;
        }
        const IVec2 c = mask.world_to_cell(p);
        if (!mask.walkable(c.x, c.y)) p = center[i];
        if (!out.empty() && math::length_sq(p - out.back()) < 0.01f) continue;
        out.push_back(p);
    }
    return out.size() >= 2;
}

} // namespace

std::vector<sim::SquadPath> LevelLoader::build_squad_paths(const LevelDef& def,
                                                          const sim::TissueMask& mask,
                                                          u32 auto_paths_per_lane) const {
    std::vector<sim::SquadPath> out;

    // Lane order = first appearance across def.vessels, the same stable order
    // build_lane_ownership_map() uses, so a lane means the same thing in both.
    std::vector<std::string> lane_ids;
    std::vector<std::vector<const Vessel*>> lane_vessels;
    for (const Vessel& v : def.vessels) {
        const std::string lane = v.lane_id.empty() ? v.id : v.lane_id;
        usize li = 0;
        for (; li < lane_ids.size(); ++li)
            if (lane_ids[li] == lane) break;
        if (li == lane_ids.size()) {
            lane_ids.push_back(lane);
            lane_vessels.emplace_back();
        }
        lane_vessels[li].push_back(&v);
    }

    // Authored paths first. One that left `lane_id` blank resolves against
    // whichever lane centerline starts nearest its first point -- the same
    // "nearest first control point" rule resolve_spawn_point_lane_id() already
    // uses for spawn points, so the two cannot disagree about what a lane is.
    std::vector<bool> lane_authored(lane_ids.size(), false);
    for (const SquadPathDef& spd : def.squad_paths) {
        sim::SquadPath path;
        path.id = spd.id;
        path.half_width = spd.half_width;
        path.points = spd.points;
        path.rebuild_arc();

        path.lane_id = spd.lane_id;
        if (path.lane_id.empty() && !lane_ids.empty()) {
            f32 best = 3.4e38f;
            usize best_li = 0;
            for (usize li = 0; li < lane_ids.size(); ++li) {
                for (const Vessel* v : lane_vessels[li]) {
                    if (v->points.empty()) continue;
                    const f32 d = math::length_sq(v->points.front().position - spd.points.front());
                    if (d < best) {
                        best = d;
                        best_li = li;
                    }
                }
            }
            path.lane_id = lane_ids[best_li];
        }
        for (usize li = 0; li < lane_ids.size(); ++li)
            if (lane_ids[li] == path.lane_id) lane_authored[li] = true;
        out.push_back(std::move(path));
    }

    // Then derive a spread for every lane the author left alone. A lane that
    // authored even one path is left entirely to the author: mixing derived
    // paths into a hand-tuned lane would silently undo the tuning.
    const u32 cap = math::clamp(auto_paths_per_lane, 1u, kMaxAutoPaths);
    std::vector<Vec2> center;
    std::vector<f32> widths;
    std::vector<Vec2> offset;
    for (usize li = 0; li < lane_ids.size(); ++li) {
        if (lane_authored[li]) continue;
        sample_lane_centerline(lane_vessels[li], center, widths);
        if (center.size() < 2) continue;

        // The NARROWEST point decides how many paths fit, not the mean: a lane
        // that pinches has to carry its squads through the pinch, and paths
        // sized off the average would be shouldered into the wall there.
        f32 min_width = widths[0];
        f32 mean_width = 0.0f;
        for (f32 w : widths) {
            mean_width += w;
            min_width = math::min(min_width, w);
        }
        mean_width /= static_cast<f32>(widths.size());

        // Usable band = the lumen minus the wall clearance either side. How
        // many paths fit is then just how many kMinPathSpacing steps span it.
        //
        // Derived rather than fixed at three, because "three" is only right for
        // one particular lane width. On a 46-wide lane three paths sit 14 apart
        // -- narrower than a squad -- and the horde reads as one mass no matter
        // how well the steering works; on a 90-wide lane three paths waste half
        // the lane. The count follows the geometry instead.
        const f32 usable = math::max(0.0f, min_width - 2.0f * kWallClearance);
        u32 want = 1u + static_cast<u32>(usable / kMinPathSpacing);
        want = math::clamp(want, 1u, cap);

        for (u32 k = 0; k < want; ++k) {
            // Evenly spread across the usable band, centred on the centerline:
            // one path IS the centerline, two straddle it, three are
            // centre-plus-flanks, and so on.
            const f32 t = want == 1u ? 0.0f
                                     : (static_cast<f32>(k) / static_cast<f32>(want - 1u)) - 0.5f;
            const f32 frac = min_width > 0.0f ? (t * usable) / min_width : 0.0f;
            if (!offset_centerline(center, widths, frac, mask, offset)) continue;
            sim::SquadPath path;
            path.id = lane_ids[li] + "_auto_" + std::to_string(k);
            path.lane_id = lane_ids[li];
            path.points = offset;
            // Half the spacing between neighbouring paths: this caps a squad's
            // radius, so a squad can never grow wide enough to reach into the
            // next path's lane.
            path.half_width = want > 1u ? math::max(1.0f, (usable / static_cast<f32>(want - 1u)) * 0.5f)
                                        : math::max(1.0f, mean_width * 0.25f);
            path.rebuild_arc();
            out.push_back(std::move(path));
        }
    }
    return out;
}

namespace {

/// The built-in test level's wave table: the old region-agnostic "flat" curve
/// that used to live in waves.json, now the only place it survives. Headless
/// --bench/--screenshot/--sim-test runs supply no level file, and a level with
/// no waves is no longer loadable, so this has to carry real pressure itself.
SpawnEntry test_spawn(PathogenFamily family, u32 count, f32 start_time, f32 duration) {
    SpawnEntry e;
    e.family = family;
    e.count = count;
    e.start_time = start_time;
    e.duration = duration;
    return e;
}

std::vector<WaveDef> default_test_waves() {
    constexpr u32 kWaveCount = 8;
    std::vector<WaveDef> waves;
    waves.reserve(kWaveCount);
    for (u32 i = 0; i < kWaveCount; ++i) {
        WaveDef w;
        w.index = i;
        w.name = "flat_wave_" + std::to_string(i + 1);
        // A generous first window, then a fixed one -- not a curve. A bench
        // run wants a steady load, not an escalating sense of urgency.
        w.prep_time = (i == 0) ? 8.0f : 15.0f;
        w.atp_reward = 50u + i * 10u;

        const u32 base = 120u + i * 75u;
        w.spawns.push_back(test_spawn(PathogenFamily::Virus, base + 15u, 0.0f, 4.0f));
        if (i >= 1) w.spawns.push_back(test_spawn(PathogenFamily::Bacteria, base / 3u, 1.0f, 3.34f));
        if (i >= 3) {
            w.spawns.push_back(test_spawn(PathogenFamily::Bacteria, base / 4u, 0.67f, 5.34f));
        }
        waves.push_back(std::move(w));
    }
    return waves;
}

} // namespace

LevelDef LevelLoader::default_test_level() {
    LevelDef d;
    d.schema = 1;
    d.name = "default_test";
    d.region = "capillary";
    d.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    d.cell_size = 0.5f;

    Vessel v;
    v.id = "main";
    v.lane_id = "main";
    v.type = VesselType::Artery;
    v.points = {
        // Widths match the shipped levels' post-widening profile (~68 world
        // units of lumen). A squad is ~16 across once the crowd relaxes, so a
        // lane narrower than about 50 cannot hold two of them side by side --
        // and this level is what --bench/--screenshot/--sim-test run when no
        // level file is supplied, so it has to be representative of the ones
        // that ship or those modes measure a horde that behaves differently
        // from the game's.
        VesselPoint{Vec2{8.0f, 72.0f}, 68.0f},
        VesselPoint{Vec2{72.0f, 60.0f}, 62.0f},
        VesselPoint{Vec2{140.0f, 84.0f}, 56.0f},
        VesselPoint{Vec2{200.0f, 72.0f}, 60.0f},
        VesselPoint{Vec2{248.0f, 72.0f}, 68.0f},
    };
    d.vessels.push_back(v);
    d.spawn_points.push_back(SpawnPoint{"p0", Vec2{8.0f, 72.0f}, 4.0f, "main"});
    d.objectives.push_back(ObjectivePoint{"organ", Vec2{248.0f, 72.0f}, 5.0f, 100.0f});
    d.placement_zones.push_back(Rect{Vec2{16.0f, 40.0f}, Vec2{240.0f, 110.0f}});
    d.placement_zone_tags.push_back(PlacementZoneTag{});
    d.waves = default_test_waves();
    return d;
}

std::string LevelLoader::resolve_spawn_point_lane_id(const LevelDef& def,
                                                     const SpawnPoint& spawn_point) const {
    if (!spawn_point.lane_id.empty()) return spawn_point.lane_id;

    std::string best;
    f32 best_d2 = std::numeric_limits<f32>::max();
    for (const Vessel& v : def.vessels) {
        if (v.points.empty()) continue;
        const f32 d2 = math::length_sq(v.points.front().position - spawn_point.position);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = v.lane_id.empty() ? v.id : v.lane_id;
        }
    }
    return best;
}

LaneOwnershipMap LevelLoader::build_lane_ownership_map(const LevelDef& def) const {
    LaneOwnershipMap out;
    const f32 cell = def.cell_size > 0.0f ? def.cell_size : 0.5f;
    const Vec2 extent = def.world_bounds.size();
    out.width = static_cast<i32>(extent.x / cell);
    out.height = static_cast<i32>(extent.y / cell);
    out.cell_size = cell;
    out.world_origin = def.world_bounds.min;

    const usize cell_count = static_cast<usize>(out.width) * static_cast<usize>(out.height);
    if (out.width <= 0 || out.height <= 0) return out;
    out.owner.assign(cell_count, LaneOwnershipMap::kNoLane);

    // Stable lane index per distinct lane_id, in first-appearance order.
    // kNoLane (0xFF) is the sentinel, so cap at 254 real lanes -- DESIGN.md
    // §4.1 wants 2-4, so this is far from a practical limit.
    for (const Vessel& v : def.vessels) {
        const std::string& lid = v.lane_id.empty() ? v.id : v.lane_id;
        bool known = false;
        for (const std::string& seen : out.lane_ids) {
            if (seen == lid) { known = true; break; }
        }
        if (!known && out.lane_ids.size() < LaneOwnershipMap::kNoLane) {
            out.lane_ids.push_back(lid);
            out.lane_types.push_back(v.type);
        }
    }
    if (out.lane_ids.empty()) return out;

    // Rasterize each lane's own vessels into a private scratch TissueMask
    // (same disc-stamping rasterize_vessel() used inside rasterize_vessels(),
    // TissueRaster.h -- frozen, only called here, never modified), then union
    // its walkable cells into the shared owner grid.
    //
    // KNOWN APPROXIMATION: where two lanes' lumens geometrically overlap
    // (e.g. an authored convergence point where two splines' discs physically
    // intersect, not just sit close), the first lane processed (its index in
    // `out.lane_ids`, i.e. first appearance in def.vessels) claims the
    // overlapping cells and later lanes do not. This is a real, deliberate
    // simplification -- an exact partition of a shared, physically-merged
    // lumen isn't well-defined anyway (the cell IS both lanes at that point),
    // and first-claim-wins is deterministic and cheap. It only matters for
    // cells inside an actual geometric overlap; a normal side-by-side
    // convergence (separate lumens that just end near the same objective)
    // attributes perfectly.
    for (usize lane_idx = 0; lane_idx < out.lane_ids.size(); ++lane_idx) {
        const std::string& lane_id = out.lane_ids[lane_idx];
        std::vector<sim::VesselSpline> lane_splines;
        for (const Vessel& v : def.vessels) {
            const std::string& lid = v.lane_id.empty() ? v.id : v.lane_id;
            if (lid != lane_id) continue;
            sim::VesselSpline spline;
            spline.points.reserve(v.points.size());
            for (const VesselPoint& p : v.points) {
                spline.points.push_back(sim::VesselPoint{p.position, p.width, 1.0f});
            }
            lane_splines.push_back(std::move(spline));
        }
        if (lane_splines.empty()) continue;

        sim::TissueMask scratch;
        scratch.resize(out.width, out.height, cell, out.world_origin);
        sim::rasterize_vessels(scratch, lane_splines);

        for (i32 y = 0; y < out.height; ++y) {
            for (i32 x = 0; x < out.width; ++x) {
                if (!scratch.walkable(x, y)) continue;
                u8& cell_owner = out.owner[out.index(x, y)];
                if (cell_owner == LaneOwnershipMap::kNoLane) {
                    cell_owner = static_cast<u8>(lane_idx);
                }
            }
        }
    }
    return out;
}

} // namespace immune::game
