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

SpawnPortal parse_portal(const json& j, usize index) {
    const std::string ctx = "portals[" + std::to_string(index) + "]";
    SpawnPortal p;
    if (!j.contains("id")) throw std::runtime_error(ctx + ": missing 'id'");
    p.id = j.at("id").get<std::string>();
    if (!j.contains("pos")) throw std::runtime_error(ctx + " ('" + p.id + "'): missing 'pos'");
    p.position = parse_vec2(j.at("pos"), "portals[].pos");
    p.radius = j.value("radius", 3.0f);
    // Empty is a valid, expected value here (legacy/single-lane levels never
    // set it); callers resolve the effective lane via
    // LevelLoader::resolve_portal_lane_id() rather than reading this raw.
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
/// sim-test scripts ("virus", "bacteria", "fungal_spore", ...). Unlike the
/// vessel_type case, an unrecognized family is an error rather than a silent
/// default: a typo'd family name changes which horde a wave sends, and that is
/// exactly the kind of mistake a level author needs told about.
PathogenFamily parse_family(const std::string& s, const std::string& ctx) {
    static const char* kNames[kFamilyCount] = {
        "virus", "bacteria", "fungal_spore", "parasite", "cancer_cell", "allergen"};
    for (u32 f = 0; f < kFamilyCount; ++f) {
        if (s == kNames[f]) return static_cast<PathogenFamily>(f);
    }
    throw std::runtime_error(ctx + ": unknown family '" + s + "'");
}

WaveModifier parse_wave_modifier(const std::string& s, const std::string& ctx) {
    if (s.empty() || s == "none") return WaveModifier::None;
    if (s == "allergen" || s == "allergen_overreaction") return WaveModifier::AllergenOverreaction;
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
    e.portal_id = j.value("portal_id", std::string{});
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
        if (j.contains("portals")) {
            const json& arr = j.at("portals");
            if (!arr.is_array()) throw std::runtime_error("'portals' must be an array");
            def.portals.reserve(arr.size());
            for (usize i = 0; i < arr.size(); ++i) def.portals.push_back(parse_portal(arr[i], i));
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
        if (j.contains("ambient_drift")) {
            def.ambient_drift = parse_vec2(j.at("ambient_drift"), "ambient_drift");
        }
        if (j.contains("waves")) {
            const json& arr = j.at("waves");
            if (!arr.is_array()) throw std::runtime_error("'waves' must be an array");
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
    if (def.portals.empty()) return LevelLoadResult{false, "level has no spawn portals", 0};
    if (def.objectives.empty()) return LevelLoadResult{false, "level has no objectives", 0};
    // Authored waves only: a named portal that doesn't exist would make
    // WaveDirector fall back to the first portal at runtime, so the wave would
    // silently come out of the wrong lane instead of failing loudly here.
    for (const WaveDef& w : def.waves) {
        for (const SpawnEntry& e : w.spawns) {
            if (e.portal_id.empty()) continue;
            bool found = false;
            for (const SpawnPortal& p : def.portals) {
                if (p.id == e.portal_id) { found = true; break; }
            }
            if (!found) {
                return LevelLoadResult{false,
                                       "wave '" + w.name + "' spawns from unknown portal '" +
                                           e.portal_id + "'",
                                       0};
            }
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

    // Spawn portals: SimWorld is the only thing WaveDirector::tick() can
    // reach, and LevelDef doesn't survive past this function, so this is the
    // one place portal geometry can be captured for the run.
    std::vector<sim::SpawnPortalRuntime> portals;
    portals.reserve(def.portals.size());
    for (const SpawnPortal& p : def.portals) {
        portals.push_back(sim::SpawnPortalRuntime{p.id, p.position, p.radius});
    }
    world.set_portals(std::move(portals));

    return LevelLoadResult{true, "", 0};
}

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
        VesselPoint{Vec2{8.0f, 72.0f}, 8.0f},
        VesselPoint{Vec2{72.0f, 60.0f}, 7.0f},
        VesselPoint{Vec2{140.0f, 84.0f}, 5.0f},
        VesselPoint{Vec2{200.0f, 72.0f}, 6.0f},
        VesselPoint{Vec2{248.0f, 72.0f}, 8.0f},
    };
    d.vessels.push_back(v);
    d.portals.push_back(SpawnPortal{"p0", Vec2{8.0f, 72.0f}, 4.0f, "main"});
    d.objectives.push_back(ObjectivePoint{"organ", Vec2{248.0f, 72.0f}, 5.0f, 100.0f});
    d.placement_zones.push_back(Rect{Vec2{16.0f, 40.0f}, Vec2{240.0f, 110.0f}});
    d.placement_zone_tags.push_back(PlacementZoneTag{});
    return d;
}

std::string LevelLoader::resolve_portal_lane_id(const LevelDef& def, const SpawnPortal& portal) const {
    if (!portal.lane_id.empty()) return portal.lane_id;

    std::string best;
    f32 best_d2 = std::numeric_limits<f32>::max();
    for (const Vessel& v : def.vessels) {
        if (v.points.empty()) continue;
        const f32 d2 = math::length_sq(v.points.front().position - portal.position);
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
