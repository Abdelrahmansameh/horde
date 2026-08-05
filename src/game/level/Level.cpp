// game/level/Level.cpp — spline JSON loader + sim wiring. Owner: Wave 2D.
//
// Parses the JSON schema documented in Level.h, validates it, and rasterizes
// it into a SimWorld's TissueMask/DistanceField/FlowField. See TissueRaster.h
// and FlowField.h (Wave 1A) for the geometry -> grid pipeline this drives.
#include "game/level/Level.h"

#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/TissueRaster.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace immune::game {

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
            for (usize i = 0; i < arr.size(); ++i) def.placement_zones.push_back(parse_rect(arr[i], i));
        }
        if (j.contains("ambient_drift")) {
            def.ambient_drift = parse_vec2(j.at("ambient_drift"), "ambient_drift");
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
    v.points = {
        VesselPoint{Vec2{8.0f, 72.0f}, 8.0f},
        VesselPoint{Vec2{72.0f, 60.0f}, 7.0f},
        VesselPoint{Vec2{140.0f, 84.0f}, 5.0f},
        VesselPoint{Vec2{200.0f, 72.0f}, 6.0f},
        VesselPoint{Vec2{248.0f, 72.0f}, 8.0f},
    };
    d.vessels.push_back(v);
    d.portals.push_back(SpawnPortal{"p0", Vec2{8.0f, 72.0f}, 4.0f});
    d.objectives.push_back(ObjectivePoint{"organ", Vec2{248.0f, 72.0f}, 5.0f, 100.0f});
    d.placement_zones.push_back(Rect{Vec2{16.0f, 40.0f}, Vec2{240.0f, 110.0f}});
    return d;
}

} // namespace immune::game
