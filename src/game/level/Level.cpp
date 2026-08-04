// Wave 0 stub + the default test level. Loader is owned by Wave 2D.
#include "game/level/Level.h"

#include "sim/SimWorld.h"

namespace immune::game {

LevelLoadResult LevelLoader::load_file(const std::string&, LevelDef&) const {
    return LevelLoadResult{false, "LevelLoader::load_file not implemented (Wave 2D)", 0};
}

LevelLoadResult LevelLoader::load_string(const std::string&, LevelDef&) const {
    return LevelLoadResult{false, "LevelLoader::load_string not implemented (Wave 2D)", 0};
}

LevelLoadResult LevelLoader::validate(const LevelDef& def) const {
    if (def.schema != 1) return LevelLoadResult{false, "unsupported or missing level schema", 0};
    if (def.vessels.empty()) return LevelLoadResult{false, "level has no vessels", 0};
    if (def.portals.empty()) return LevelLoadResult{false, "level has no spawn portals", 0};
    if (def.objectives.empty()) return LevelLoadResult{false, "level has no objectives", 0};
    return LevelLoadResult{true, "", 0};
}

LevelLoadResult LevelLoader::instantiate(const LevelDef& def, sim::SimWorld& world) const {
    // Wave 2D: rasterize splines -> TissueMask, bake DistanceField + FlowField.
    // Wave 0 sets only the pieces the headless modes need to run.
    const f32 cell = def.cell_size > 0.0f ? def.cell_size : 0.5f;
    const Vec2 extent = def.world_bounds.size();
    world.tissue().resize(static_cast<i32>(extent.x / cell),
                          static_cast<i32>(extent.y / cell),
                          cell, def.world_bounds.min);
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
