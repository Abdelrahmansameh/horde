// game/level/Level.cpp — spline JSON loader + sim wiring. Owner: Wave 2D,
// extended by Wave 4D for the multi-lane schema (see Level.h's header comment
// for the rationale and the JSON shape).
//
// Parses the JSON schema documented in Level.h, validates it, and rasterizes
// it into a SimWorld's TissueMask/DistanceField/FlowField. See TissueRaster.h
// and FlowField.h (Wave 1A) for the geometry -> grid pipeline this drives.
#include "game/level/Level.h"

#include "core/Clock.h"
#include "core/Math.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/ObstacleRaster.h"
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

const char* obstacle_shape_to_string(ObstacleShape s) {
    switch (s) {
        case ObstacleShape::Disc: return "disc";
        case ObstacleShape::Capsule: return "capsule";
        case ObstacleShape::Box: return "box";
        case ObstacleShape::Polygon: return "polygon";
        case ObstacleShape::Ridge: return "ridge";
    }
    return "disc";
}

bool obstacle_shape_from_string(const std::string& s, ObstacleShape& out) {
    if (s == "disc") { out = ObstacleShape::Disc; return true; }
    if (s == "capsule") { out = ObstacleShape::Capsule; return true; }
    if (s == "box") { out = ObstacleShape::Box; return true; }
    if (s == "polygon") { out = ObstacleShape::Polygon; return true; }
    if (s == "ridge") { out = ObstacleShape::Ridge; return true; }
    // No fallback on purpose (see ObstacleShape): the caller reports the typo.
    return false;
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

/// One point of an obstacle's `points` array. Two spellings are accepted on
/// purpose: a bare `[x,y]` pair, which is all a polygon vertex or a capsule end
/// ever needs, and the vessel-style `{"p":[x,y],"w":w}` object, which a ridge
/// needs for its per-point thickness. Bare pairs get width 0 rather than
/// VesselPoint's 4.0 default, so a ridge that forgets its widths fails the
/// positive-width check below instead of silently carving a 4-unit bar.
VesselPoint parse_obstacle_point(const json& j, const std::string& ctx) {
    VesselPoint vp;
    if (j.is_array()) {
        vp.position = parse_vec2(j, "obstacles[].points[]");
        vp.width = 0.0f;
        return vp;
    }
    if (!j.is_object() || !j.contains("p")) {
        throw std::runtime_error(ctx + ": point must be [x,y] or {\"p\":[x,y],\"w\":w}");
    }
    vp.position = parse_vec2(j.at("p"), "obstacles[].points[].p");
    vp.width = j.value("w", 0.0f);
    return vp;
}

/// Obstacles are validated here rather than in validate() for the same reason
/// the wave table is: validate() is not on the app's load path, so a check that
/// only lives there does not protect a level the game actually loads.
ObstacleDef parse_obstacle(const json& j, usize index) {
    std::string ctx = "obstacles[" + std::to_string(index) + "]";
    ObstacleDef o;
    if (!j.is_object()) throw std::runtime_error(ctx + " must be an object");
    o.id = j.value("id", std::string{});
    if (!o.id.empty()) ctx += " ('" + o.id + "')";

    if (!j.contains("shape")) throw std::runtime_error(ctx + ": missing 'shape'");
    const std::string shape = j.at("shape").get<std::string>();
    if (!obstacle_shape_from_string(shape, o.shape)) {
        throw std::runtime_error(ctx + ": unknown shape '" + shape +
                                 "' (expected disc|capsule|box|polygon|ridge)");
    }

    if (j.contains("points")) {
        const json& pts = j.at("points");
        if (!pts.is_array()) throw std::runtime_error(ctx + ": 'points' must be an array");
        o.points.reserve(pts.size());
        for (const auto& p : pts) o.points.push_back(parse_obstacle_point(p, ctx));
    }
    if (j.contains("pos")) o.position = parse_vec2(j.at("pos"), "obstacles[].pos");
    if (j.contains("half_extents")) {
        o.half_extents = parse_vec2(j.at("half_extents"), "obstacles[].half_extents");
    }
    // Degrees in the file, radians in the struct: nobody hand-authors radians,
    // and the conversion has exactly one right place to live.
    o.rotation = j.value("rotation", 0.0f) * (math::kPi / 180.0f);

    switch (o.shape) {
        case ObstacleShape::Disc:
            o.radius = j.value("radius", 0.0f);
            if (o.radius <= 0.0f) throw std::runtime_error(ctx + ": disc needs a positive 'radius'");
            break;
        case ObstacleShape::Capsule:
            o.radius = j.value("radius", 0.0f);
            if (o.points.size() != 2) {
                throw std::runtime_error(ctx + ": capsule needs exactly 2 'points'");
            }
            if (o.radius <= 0.0f) {
                throw std::runtime_error(ctx + ": capsule needs a positive 'radius'");
            }
            break;
        case ObstacleShape::Box:
            if (o.half_extents.x <= 0.0f || o.half_extents.y <= 0.0f) {
                throw std::runtime_error(ctx + ": box needs positive 'half_extents'");
            }
            break;
        case ObstacleShape::Polygon:
            // `inflate` grows the polygon outward; it is the same field the
            // round shapes call `radius`, since both are "how far past the
            // authored skeleton does solid ground reach".
            o.radius = j.value("inflate", 0.0f);
            if (o.points.size() < 3 && !(o.points.size() == 2 && o.radius > 0.0f)) {
                throw std::runtime_error(
                    ctx + ": polygon needs at least 3 'points' (or 2 with a positive 'inflate')");
            }
            if (o.radius < 0.0f) throw std::runtime_error(ctx + ": polygon 'inflate' must be >= 0");
            break;
        case ObstacleShape::Ridge:
            if (o.points.size() < 2) throw std::runtime_error(ctx + ": ridge needs 2+ 'points'");
            for (const VesselPoint& p : o.points) {
                if (p.width <= 0.0f) {
                    throw std::runtime_error(ctx + ": every ridge point needs a positive 'w'");
                }
            }
            break;
    }
    return o;
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
    // Two spellings of the same footprint. "half_extents" is what the editor
    // writes and what a rectangle actually needs; "radius" is what every level
    // authored before objectives had a shape carries, and it means the square
    // that value used to describe. Reading both is one line and keeps those
    // files loading unedited.
    if (j.contains("half_extents")) {
        o.half_extents = parse_vec2(j.at("half_extents"), "objectives[].half_extents");
    } else {
        const f32 r = j.value("radius", 5.0f);
        o.half_extents = Vec2{r, r};
    }
    if (o.half_extents.x <= 0.0f || o.half_extents.y <= 0.0f) {
        throw std::runtime_error(ctx + " ('" + o.id + "'): needs positive extents");
    }
    // Degrees in the file, radians in the struct -- same contract as an
    // obstacle box's 'rotation'.
    o.rotation = j.value("rotation", 0.0f) * (math::kPi / 180.0f);
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
    static const char* kNames[kFamilyCount] = {"virus", "bacteria", "parasite"};
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
    // schema 2. Both default to today's behaviour when absent, so a v1 file
    // parses to exactly the SpawnEntry it always did.
    e.squad_size = j.value("squad_size", u32{0});
    if (j.contains("squad_paths")) {
        for (const auto& p : j.at("squad_paths")) e.squad_paths.push_back(p.get<std::string>());
    }
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

// ---------------------------------------------------------------------------
// Obstacles: carving, and the two checks that need the whole LevelDef.
// ---------------------------------------------------------------------------

sim::VesselSpline to_ridge_spline(const ObstacleDef& o) {
    sim::VesselSpline spline;
    spline.points.reserve(o.points.size());
    for (const VesselPoint& p : o.points) {
        spline.points.push_back(sim::VesselPoint{p.position, p.width, 1.0f});
    }
    return spline;
}

/// game::ObstacleDef -> the matching sim/flowfield/ObstacleRaster.h primitive.
/// Called on the real mask by instantiate() and on the per-lane scratch masks
/// by build_lane_ownership_map(), so the two can never disagree about which
/// cells an obstacle covers.
void carve_obstacles(sim::TissueMask& mask, const std::vector<ObstacleDef>& obstacles) {
    for (const ObstacleDef& o : obstacles) {
        switch (o.shape) {
            case ObstacleShape::Disc:
                sim::carve_disc(mask, o.position, o.radius);
                break;
            case ObstacleShape::Capsule:
                sim::carve_capsule(mask, o.points[0].position, o.points[1].position, o.radius);
                break;
            case ObstacleShape::Box:
                sim::carve_box(mask, o.position, o.half_extents, o.rotation);
                break;
            case ObstacleShape::Polygon: {
                std::vector<Vec2> verts;
                verts.reserve(o.points.size());
                for (const VesselPoint& p : o.points) verts.push_back(p.position);
                sim::carve_polygon(mask, verts, o.radius);
                break;
            }
            case ObstacleShape::Ridge:
                sim::carve_ridge(mask, to_ridge_spline(o));
                break;
        }
    }
}

/// Is `p` inside the solid? Analytic rather than a mask lookup so the checks
/// below can run on a LevelDef alone, before any grid exists. It mirrors the
/// carve functions' geometry (and borrows their two distance helpers outright)
/// but not their cell-center quantization, so a point within half a cell of a
/// boundary may disagree with the rasterized mask -- fine for the "did the
/// author bury a spawn point" question, not a substitute for asking the mask.
bool obstacle_contains(const ObstacleDef& o, Vec2 p) {
    switch (o.shape) {
        case ObstacleShape::Disc:
            return math::length_sq(p - o.position) <= o.radius * o.radius;
        case ObstacleShape::Capsule:
            return sim::detail::dist_sq_to_segment(p, o.points[0].position,
                                                   o.points[1].position) <= o.radius * o.radius;
        case ObstacleShape::Box: {
            const Vec2 d = p - o.position;
            const f32 cs = std::cos(o.rotation);
            const f32 sn = std::sin(o.rotation);
            return std::fabs(d.x * cs + d.y * sn) <= o.half_extents.x &&
                   std::fabs(-d.x * sn + d.y * cs) <= o.half_extents.y;
        }
        case ObstacleShape::Polygon: {
            std::vector<Vec2> verts;
            verts.reserve(o.points.size());
            for (const VesselPoint& v : o.points) verts.push_back(v.position);
            if (verts.size() >= 3 && sim::detail::point_in_polygon(p, verts)) return true;
            if (o.radius <= 0.0f) return false;
            for (usize i = 0, j = verts.size() - 1; i < verts.size(); j = i++) {
                if (sim::detail::dist_sq_to_segment(p, verts[j], verts[i]) <= o.radius * o.radius) {
                    return true;
                }
            }
            return false;
        }
        case ObstacleShape::Ridge: {
            const sim::VesselSpline spline = to_ridge_spline(o);
            const f32 span = static_cast<f32>(o.points.size() - 1);
            // 32 samples per segment: finer than the widest plausible gap
            // between a ridge's own stamped discs, and this runs once per
            // spawn point at load.
            const i32 steps = 32 * static_cast<i32>(span);
            for (i32 k = 0; k <= steps; ++k) {
                const sim::VesselPoint vp =
                    sim::eval_spline(spline, span * static_cast<f32>(k) / static_cast<f32>(steps));
                const f32 r = vp.width * 0.5f;
                if (math::length_sq(p - vp.pos) <= r * r) return true;
            }
            return false;
        }
    }
    return false;
}

/// Cross-field obstacle checks, shared by load_string() (so the game's own
/// load path enforces them) and validate() (so a LevelDef built in code does
/// too). Returns an empty string when the level is fine.
///
/// Burying a spawn point or an objective is the failure mode worth naming
/// precisely: the level still loads, the horde still spawns, and it spawns
/// inside rock, which reads as a mysteriously dead lane rather than as the
/// authoring mistake it is. The wider failure -- an obstacle that seals a lane
/// without burying either endpoint -- cannot be seen from the LevelDef and is
/// caught after the flow bake in instantiate().
std::string check_obstacles(const LevelDef& def) {
    for (const ObstacleDef& o : def.obstacles) {
        const std::string name = o.id.empty() ? std::string(obstacle_shape_to_string(o.shape))
                                              : ("'" + o.id + "'");
        for (const SpawnPoint& s : def.spawn_points) {
            if (obstacle_contains(o, s.position)) {
                return "obstacle " + name + " buries spawn point '" + s.id + "'";
            }
        }
        for (const ObjectivePoint& ob : def.objectives) {
            if (obstacle_contains(o, ob.position)) {
                return "obstacle " + name + " buries objective '" + ob.id + "'";
            }
        }
    }
    return {};
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
        // Both versions load. Every schema-2 field is optional with today's
        // behaviour as its default, so a v1 file and the v2 file the writer
        // emits for it parse to the same LevelDef.
        if (def.schema != 1 && def.schema != 2) {
            return LevelLoadResult{false,
                                   "level JSON has unsupported schema version " +
                                       std::to_string(def.schema) + " (expected 1 or 2)",
                                   0};
        }

        def.name = j.value("name", std::string{});
        def.region = j.value("region", std::string{});

        // ---- schema 2 header ------------------------------------------------
        def.display_name = j.value("display_name", std::string{});
        def.description = j.value("description", std::string{});
        def.author = j.value("author", std::string{});
        def.difficulty = j.value("difficulty", 0);
        if (j.contains("tags")) {
            for (const auto& t : j.at("tags")) def.tags.push_back(t.get<std::string>());
        }
        if (j.contains("allowed_towers")) {
            for (const auto& t : j.at("allowed_towers")) {
                def.allowed_towers.push_back(t.get<std::string>());
            }
        }
        if (j.contains("economy")) {
            const json& e = j.at("economy");
            def.economy.starting_atp = e.value("starting_atp", u32{0});
            def.economy.income_multiplier = e.value("income_multiplier", 1.0f);
        }
        if (j.contains("camera")) {
            const json& c = j.at("camera");
            if (c.contains("center")) {
                def.camera.center = parse_vec2(c.at("center"), "camera.center");
                def.camera.has_center = true;
            }
            def.camera.view_height = c.value("view_height", 0.0f);
            def.camera.min_view_height = c.value("min_view_height", 0.0f);
            def.camera.max_view_height = c.value("max_view_height", 0.0f);
        }
        if (j.contains("win")) {
            def.win.survive_seconds = j.at("win").value("survive_seconds", 0.0f);
        }
        if (j.contains("editor")) {
            const json& e = j.at("editor");
            def.editor.present = true;
            def.editor.grid_size = e.value("grid_size", 0.0f);
            if (e.contains("camera_center")) {
                def.editor.camera_center = parse_vec2(e.at("camera_center"), "editor.camera_center");
            }
            def.editor.camera_view_height = e.value("camera_view_height", 0.0f);
            def.editor.notes = e.value("notes", std::string{});
        }

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
        if (j.contains("obstacles")) {
            const json& arr = j.at("obstacles");
            if (!arr.is_array()) throw std::runtime_error("'obstacles' must be an array");
            def.obstacles.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i) def.obstacles.push_back(parse_obstacle(arr[i], i));
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
        // Needs spawn points, objectives and obstacles all parsed, so it can
        // only run once the whole object is read.
        if (const std::string err = check_obstacles(def); !err.empty()) {
            throw std::runtime_error(err);
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
    if (def.schema != 1 && def.schema != 2) {
        return LevelLoadResult{false, "unsupported or missing level schema", 0};
    }
    if (def.vessels.empty()) return LevelLoadResult{false, "level has no vessels", 0};
    if (def.spawn_points.empty()) return LevelLoadResult{false, "level has no spawn points", 0};
    if (def.objectives.empty()) return LevelLoadResult{false, "level has no objectives", 0};
    // Same rule load_string() enforces, repeated here so a LevelDef built in
    // code (a test fixture, default_test_level()) cannot skip it either.
    if (def.waves.empty()) return LevelLoadResult{false, "level declares no waves", 0};
    // Ditto for obstacles: load_string() already ran this, but a LevelDef
    // assembled in code never went through it.
    if (const std::string err = check_obstacles(def); !err.empty()) {
        return LevelLoadResult{false, err, 0};
    }
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

Rect level_sim_bounds(const LevelDef& def) {
    const Vec2 extent = def.world_bounds.size();
    // A degenerate world rect has no grid to grow; validate() rejects it and
    // bake_geometry() below produces an empty mask either way.
    if (extent.x <= 0.0f || extent.y <= 0.0f) return def.world_bounds;
    const f32 cell = def.cell_size > 0.0f ? def.cell_size : 0.5f;

    Vec2 lo = def.world_bounds.min;
    Vec2 hi = def.world_bounds.max;
    const auto include = [&lo, &hi](Vec2 p, f32 pad) {
        lo = Vec2{math::min(lo.x, p.x - pad), math::min(lo.y, p.y - pad)};
        hi = Vec2{math::max(hi.x, p.x + pad), math::max(hi.y, p.y + pad)};
    };
    for (const SpawnPoint& p : def.spawn_points) {
        // The DISC, not the point: spawn_burst() scatters the wave across the
        // whole radius, so a disc that straddles the edge would lose its outer
        // half to the despawn test. The extra cells past it are walking room --
        // an agent on the very last cell of the grid has no tissue to steer
        // into and no flow field cell to read.
        const f32 pad = math::max(p.radius, 0.0f) + cell * 8.0f;
        include(p.position, pad);
    }
    for (const Vessel& v : def.vessels) {
        for (const VesselPoint& p : v.points) {
            if (p.position.x >= def.world_bounds.min.x && p.position.x <= def.world_bounds.max.x &&
                p.position.y >= def.world_bounds.min.y && p.position.y <= def.world_bounds.max.y) {
                continue;
            }
            // A vessel is rendered and rasterized as a strip around its
            // control points. Include its full end-cap, plus a few cells for
            // the SDF/flow stencil, so an end may deliberately leave frame.
            include(p.position, math::max(p.width * 0.5f, 0.0f) + cell * 4.0f);
        }
    }
    for (const ObjectivePoint& o : def.objectives) {
        if (o.position.x >= def.world_bounds.min.x && o.position.x <= def.world_bounds.max.x &&
            o.position.y >= def.world_bounds.min.y && o.position.y <= def.world_bounds.max.y) {
            continue;
        }
        // FlowField seeds the objective's full footprint, not only its centre.
        // Keeping that footprint on-grid makes an off-frame organ behave just
        // like an in-frame one while the camera remains clamped to the play
        // rectangle.
        const f32 hx = math::max(o.half_extents.x, 0.0f);
        const f32 hy = math::max(o.half_extents.y, 0.0f);
        // The radius covers the footprint even when the objective is rotated.
        include(o.position, std::sqrt(hx * hx + hy * hy) + cell * 4.0f);
    }
    if (lo.x == def.world_bounds.min.x && lo.y == def.world_bounds.min.y &&
        hi.x == def.world_bounds.max.x && hi.y == def.world_bounds.max.y) {
        return def.world_bounds;
    }

    // BACKSTOP. Every cell of this rect is allocated four times over (mask,
    // SDF, flow, lane ownership) and swept by the flow bake, so a mistyped
    // coordinate must not be able to ask for a grid the size of the address
    // space. One world extent of growth per side is far more than any
    // off-screen lane, spawn, or objective needs; content past it stays off
    // the grid, and validate_level() reports it rather than quietly moving
    // something the author did not put there.
    lo = Vec2{math::max(lo.x, def.world_bounds.min.x - extent.x),
              math::max(lo.y, def.world_bounds.min.y - extent.y)};
    hi = Vec2{math::min(hi.x, def.world_bounds.max.x + extent.x),
              math::min(hi.y, def.world_bounds.max.y + extent.y)};

    // Whole cells, outward. Growing by an exact multiple of cell_size keeps
    // every cell the un-grown level already had at the same world position, so
    // adding an off-map spawn point does not re-quantize the geometry that was
    // already there (and does not shift a bake that a test pinned).
    const auto cells_out = [cell](f32 d) { return math::max(std::ceil(d / cell), 0.0f) * cell; };
    Rect out;
    out.min = Vec2{def.world_bounds.min.x - cells_out(def.world_bounds.min.x - lo.x),
                   def.world_bounds.min.y - cells_out(def.world_bounds.min.y - lo.y)};
    out.max = Vec2{def.world_bounds.max.x + cells_out(hi.x - def.world_bounds.max.x),
                   def.world_bounds.max.y + cells_out(hi.y - def.world_bounds.max.y)};
    return out;
}

LevelLoadResult LevelLoader::bake_geometry(const LevelDef& def, const GeometryBakeDesc& desc,
                                           sim::TissueMask& mask, sim::DistanceField& sdf,
                                           sim::FlowField& flow, GeometryBakeStats* stats,
                                           RenderSdf* out_render_sdf) const {
    GeometryBakeStats scratch;
    GeometryBakeStats& st = stats ? *stats : scratch;
    st = GeometryBakeStats{};
    const WallClock bake_timer;
    WallClock stage;
    const f32 cell = def.cell_size > 0.0f ? def.cell_size : 0.5f;
    // The SIM rect, not the play rect: a spawn point outside world_bounds still
    // needs mask, SDF and flow under it. Identical to world_bounds unless the
    // level actually authored one out there (level_sim_bounds()).
    const Rect grid = level_sim_bounds(def);
    const Vec2 extent = grid.size();
    mask.resize(static_cast<i32>(extent.x / cell), static_cast<i32>(extent.y / cell), cell,
                grid.min);

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
    sim::rasterize_vessels(mask, splines);
    st.rasterize_ms = stage.elapsed_ms();
    stage = WallClock{};

    // Solid islands back out of the lumen. This is the ONE ordering rule the
    // feature has (ObstacleRaster.h): after the lumen exists, before the SDF
    // is baked. Everything an obstacle does downstream -- shading as vessel
    // wall, deflecting steering, rerouting the horde, refusing towers -- is a
    // consequence of the distance field below having seen it, and none of it
    // happens if these two calls are the other way round.
    carve_obstacles(mask, def.obstacles);
    st.carve_ms = stage.elapsed_ms();
    stage = WallClock{};

    // The drawn field, written back into walkability (see the header). Baked
    // here rather than by the renderer's caller so that a mask and the picture
    // of it can never come from two different bakes.
    {
        RenderSdf scratch;
        RenderSdf& rs = out_render_sdf != nullptr ? *out_render_sdf : scratch;
        rs = bake_render_sdf(def);
        if (rs.valid()) {
            for (i32 y = 0; y < mask.height(); ++y) {
                for (i32 x = 0; x < mask.width(); ++x) {
                    const bool open = rs.sample(mask.cell_to_world(x, y)) > 0.0f;
                    if (open == mask.walkable(x, y)) continue;
                    // Almost always a wall cell becoming lumen (a fillet, a
                    // run-off). The other direction is a sub-texel sliver
                    // the stamp happened to catch a cell centre in, which no
                    // one drew and nothing should squeeze through.
                    mask.set_walkable(x, y, open);
                    if (open) mask.set_cost(x, y, 1.0f);
                }
            }
        }
    }
    st.render_sdf_ms = stage.elapsed_ms();
    stage = WallClock{};

    // TissueMask -> DistanceField -> FlowField, per the pipeline in FlowField.h.
    sdf.bake(mask);
    st.sdf_ms = stage.elapsed_ms();
    stage = WallClock{};

    // Wall-proximity traversal cost, written into the mask's cost channel (the
    // one TissueMask has always carried for sludge and NETs) now that the SDF
    // that feeds it exists.
    //
    // Without it, the cheapest route round the end of a septum is the one that
    // grazes the tip, so every route in the chamber converges on that one point
    // and the horde beelines to it instead of sweeping round. Charging for wall
    // proximity buys back a standoff -- on capillary_switchback the streamline
    // goes from grazing the tip (0 clearance) to holding ~8 units off it.
    //
    // It does NOT fully open a hairpin into the wide arc a fluid would take.
    // That is not a tuning shortfall: a shortest-path field converging at a
    // convex corner is a property of the metric, and this term can only bias
    // it, because clearance alone cannot distinguish the tip of a septum from
    // the side of a straight lane. Pushed hard enough to round the hairpin, the
    // same term tilts every lane's field toward its own centreline and the
    // overlay grows a chevron. The defaults sit at the measured knee: an
    // exponent this high keeps the penalty in a ~2-unit layer at the lining, so
    // the open middle of a vessel stays flat.
    //
    // Baked once, from the load-time SDF. A tower placed later blocks cells and
    // reroutes the field, but does not re-stamp this: the SDF is deliberately
    // not rebaked in-frame (see ChaffSystem's wall-contact notes), and a tower
    // is small next to a lumen, so the cost it would have added is not worth a
    // full distance transform per placement. Both a full bake and an
    // incremental rebake read the same stamped values, so they still agree
    // exactly.
    if (desc.flow_wall_cost > 0.0f && desc.flow_wall_falloff > 0.0f) {
        const f32 falloff = desc.flow_wall_falloff;
        const f32 gain = desc.flow_wall_cost;
        const f32 exponent = math::max(desc.flow_wall_exponent, 1.0f);
        for (i32 y = 0; y < mask.height(); ++y) {
            for (i32 x = 0; x < mask.width(); ++x) {
                if (!mask.walkable(x, y)) continue;
                const f32 t = math::saturate(sdf.sample(mask.cell_to_world(x, y)) / falloff);
                mask.set_cost(x, y,
                              mask.cost(x, y) * (1.0f + gain * std::pow(1.0f - t, exponent)));
            }
        }
    }

    st.wall_cost_ms = stage.elapsed_ms();
    stage = WallClock{};

    sim::FlowFieldBakeDesc flow_desc;
    flow_desc.smoothing_radius = desc.flow_smoothing_radius;
    flow_desc.goals.reserve(def.objectives.size());
    for (const ObjectivePoint& o : def.objectives) {
        // The whole objective rectangle is a sink, because ChaffSystem despawns
        // an agent the moment it enters the footprint -- the rim IS the goal. A
        // single-cell sink instead aims every agent at the exact centre from
        // across the level, which is what made a wide vessel read as a funnel.
        flow_desc.goals.push_back(
            sim::FlowGoal{mask.world_to_cell(o.position), o.half_extents, o.rotation});
    }
    flow.bake(mask, flow_desc);
    st.flow_ms = stage.elapsed_ms();
    st.total_ms = bake_timer.elapsed_ms();

    return LevelLoadResult{true, "", 0};
}

LevelLoadResult LevelLoader::instantiate(const LevelDef& def, sim::SimWorld& world,
                                         RenderSdf* out_render_sdf) const {
    // The geometry half. Every SimDesc value the bake reads is forwarded here
    // and nowhere else, so the editor's standalone bake and this one cannot
    // silently disagree about wall cost or smoothing.
    GeometryBakeDesc bake_desc;
    bake_desc.flow_smoothing_radius = world.desc().flow_smoothing_radius;
    bake_desc.flow_wall_cost = world.desc().flow_wall_cost;
    bake_desc.flow_wall_falloff = world.desc().flow_wall_falloff;
    bake_desc.flow_wall_exponent = world.desc().flow_wall_exponent;
    if (const LevelLoadResult r = bake_geometry(def, bake_desc, world.tissue(), world.sdf(),
                                                world.flow(), nullptr, out_render_sdf);
        !r.ok) {
        return r;
    }

    // Did the carving seal a lane? A spawn point with no route to any
    // objective is a lane that quietly does nothing all match, and an obstacle
    // is by far the easiest way to author one by accident -- a bar two units
    // wider than intended closes a channel that still looks open in the JSON.
    // The baked field is the only place the question can be answered
    // (connectivity is a property of the whole grid, not of any one shape), and
    // failing the load is the only response an author cannot miss.
    //
    // Gated on the level actually having obstacles: an existing level with an
    // unreachable spawn point has whatever behaviour it has always had, and
    // this is not the change that should start rejecting it.
    if (!def.obstacles.empty()) {
        for (const SpawnPoint& sp : def.spawn_points) {
            if (world.flow().reachable(sp.position)) continue;
            return LevelLoadResult{false,
                                   "spawn point '" + sp.id +
                                       "' cannot reach any objective -- an obstacle seals its lane",
                                   0};
        }
    }

    // ECS objective entities: the structural, potentially-multi-objective
    // representation future rendering/UI reads. Distinct from SimWorld's
    // scalar objective_integrity_ (primary-objective HUD/win-condition value,
    // wired elsewhere) — nothing here touches that float.
    for (const ObjectivePoint& o : def.objectives) {
        const entt::entity e = world.ecs().registry().create();
        world.ecs().registry().emplace<sim::comp::Objective>(
            e, sim::comp::Objective{o.integrity, o.integrity, o.half_extents, o.rotation});
        world.ecs().registry().emplace<sim::comp::Transform>(
            e, sim::comp::Transform{o.position, 0.0f, 1.0f});
    }

    if (!def.objectives.empty()) {
        const ObjectivePoint& primary = def.objectives[0];
        world.chaff_system().set_goal(primary.position, primary.half_extents, primary.rotation);
    }
    // The despawn rect is the SIM rect. Bounding it by the play area instead
    // would retire an off-map spawn's whole burst on the tick it appeared.
    world.chaff_system().set_world_bounds(level_sim_bounds(def));

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

    // Buildable area. Empty means "anywhere" -- the behaviour every level
    // without zones has always had -- so threading this changes nothing for
    // content that authors none.
    world.set_placement_zones(def.placement_zones);

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

        auto walkable_at = [&mask](Vec2 q) {
            const IVec2 c = mask.world_to_cell(q);
            return mask.walkable(c.x, c.y);
        };

        f32 dist = widths[i] * fraction;
        Vec2 p = center[i] + normal * dist;
        // Walk back toward the centerline in a few halvings. This handles the
        // common case: the lane narrowed and a generous offset overshot the
        // wall.
        bool placed = false;
        for (u32 attempt = 0; attempt < 4; ++attempt) {
            if (walkable_at(p)) { placed = true; break; }
            dist *= 0.5f;
            p = center[i] + normal * dist;
        }
        if (!placed && walkable_at(center[i])) {
            p = center[i];
            placed = true;
        }
        // The centerline used to be walkable by construction, so the two steps
        // above were the whole story. An authored obstacle can now sit ON the
        // centerline, which leaves the halvings converging onto solid rock --
        // so sweep sideways instead, both ways, out to the local half width,
        // and take the first legal spot. That is what makes a derived path go
        // AROUND an island rather than through it.
        if (!placed) {
            const f32 step = math::max(mask.cell_size(), 0.25f);
            const f32 limit = math::max(widths[i] * 0.5f, step * 4.0f);
            for (f32 d = step; d <= limit && !placed; d += step) {
                for (const f32 sign : {1.0f, -1.0f}) {
                    const Vec2 candidate = center[i] + normal * (d * sign);
                    if (!walkable_at(candidate)) continue;
                    p = candidate;
                    placed = true;
                    break;
                }
            }
        }
        // Still nothing: the lane is fully blocked at this station. Drop the
        // sample rather than park an anchor inside rock -- the path simply
        // steps across the gap, and the squad's own steering handles the
        // detour it implies.
        if (!placed) continue;
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
    d.objectives.push_back(
        ObjectivePoint{"organ", Vec2{248.0f, 72.0f}, Vec2{5.0f, 5.0f}, 0.0f, 100.0f});
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
    // Same rect as bake_geometry()'s mask, or the two grids stop being cell-for-
    // cell parallel the moment a level puts a spawn point off the play area.
    const Rect grid = level_sim_bounds(def);
    const Vec2 extent = grid.size();
    out.width = static_cast<i32>(extent.x / cell);
    out.height = static_cast<i32>(extent.y / cell);
    out.cell_size = cell;
    out.world_origin = grid.min;

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
        // Same carve instantiate() applies to the real mask, so a cell inside
        // an island is unowned rather than attributed to the lane it sits in.
        // A lane's ownership footprint is meant to be the ground that lane's
        // traffic can actually occupy; solid rock in the middle of it is not
        // that, and leaving it claimed would let lane_at() answer "artery" for
        // a point no agent can ever stand on.
        carve_obstacles(scratch, def.obstacles);

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
