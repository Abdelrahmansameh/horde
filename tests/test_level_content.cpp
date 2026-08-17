// Tests for Wave 2D's remaining scope: instantiate()'s comp::Objective entity
// spawning, and the two new content levels (chokepoint_pinch, floodplain_mucosal).
// Owner: Wave 2D.
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace immune;
using namespace immune::game;

namespace {

const char* kTwoObjectiveLevel = R"JSON({
  "schema": 1,
  "name": "two_objective_test",
  "region": "capillary",
  "world": { "min": [0, 0], "max": [64, 32], "cell_size": 1.0 },
  "vessels": [
    {
      "id": "main",
      "points": [
        { "p": [4, 16], "w": 6.0 },
        { "p": [32, 16], "w": 6.0 },
        { "p": [60, 16], "w": 6.0 }
      ],
      "children": []
    }
  ],
  "portals": [ { "id": "p0", "pos": [4, 16], "radius": 3.0 } ],
  "objectives": [
    { "id": "organ_a", "pos": [32, 16], "radius": 4.0, "integrity": 75 },
    { "id": "organ_b", "pos": [60, 16], "radius": 2.5, "integrity": 40 }
  ],
  "placement_zones": [ { "min": [10, 10], "max": [50, 22] } ]
})JSON";

sim::SimWorld make_world(const LevelDef& def) {
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = def.world_bounds;
    world.init(desc, nullptr);
    return world;
}

} // namespace

// ---- Deliverable 1: comp::Objective entity spawning -----------------------

TEST_CASE("instantiate spawns one comp::Objective entity per level objective",
          "[level][ecs][objective]") {
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(kTwoObjectiveLevel, def).ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.objectives.size() == 2);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);

    auto view = world.ecs().registry().view<const sim::comp::Objective, const sim::comp::Transform>();
    usize count = 0;
    for (auto e : view) { (void)e; ++count; }
    REQUIRE(count == def.objectives.size());

    // Match each ECS entity back to its authored ObjectivePoint by position, and
    // check integrity/max_integrity/radius/transform were carried through.
    for (const ObjectivePoint& o : def.objectives) {
        bool found = false;
        for (auto e : view) {
            const sim::comp::Transform& t = view.get<const sim::comp::Transform>(e);
            const f32 dx = t.position.x - o.position.x;
            const f32 dy = t.position.y - o.position.y;
            if (std::abs(dx) > 1e-4f || std::abs(dy) > 1e-4f) continue;

            const sim::comp::Objective& obj = view.get<const sim::comp::Objective>(e);
            REQUIRE(obj.integrity == o.integrity);
            REQUIRE(obj.max_integrity == o.integrity);
            REQUIRE(obj.radius == o.radius);
            REQUIRE(t.rotation == 0.0f);
            REQUIRE(t.scale == 1.0f);
            found = true;
            break;
        }
        REQUIRE(found);
    }
}

TEST_CASE("instantiate on capillary_test.json (a single-objective level) spawns exactly one "
          "comp::Objective entity",
          "[level][ecs][objective]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/capillary_test.json");
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.objectives.size() == 1);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);

    auto view = world.ecs().registry().view<const sim::comp::Objective>();
    usize count = 0;
    for (auto e : view) { (void)e; ++count; }
    REQUIRE(count == 1);
}

// ---- Deliverable 2: new content levels -------------------------------------

TEST_CASE("chokepoint_pinch.json loads, validates, and bakes a flow field reaching the objective",
          "[level][content][chokepoint]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/chokepoint_pinch.json");
    REQUIRE(platform::file_exists(path));
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.name == "chokepoint_pinch");
    REQUIRE_FALSE(def.portals.empty());
    REQUIRE_FALSE(def.objectives.empty());
    REQUIRE_FALSE(def.placement_zones.empty());

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);
    REQUIRE(world.flow().reachable(def.portals[0].position));

    // The pinch's narrowest point (x=67.56, along the lane's y=38 centerline)
    // must still be walkable (it's on the vessel), but a point ~2.1 world
    // units off the centerline there -- well inside the wide sections' radius
    // but outside the ~2.5-wide pinch's -- must not be, proving the width
    // profile actually narrows partway down the lane rather than staying
    // uniformly wide.
    const IVec2 pinch_center = world.tissue().world_to_cell(Vec2{67.56f, 38.0f});
    const IVec2 pinch_off = world.tissue().world_to_cell(Vec2{67.56f, 40.11f});
    const IVec2 wide_off = world.tissue().world_to_cell(Vec2{10.56f, 40.11f});
    REQUIRE(world.tissue().walkable(pinch_center.x, pinch_center.y));
    REQUIRE_FALSE(world.tissue().walkable(pinch_off.x, pinch_off.y));
    REQUIRE(world.tissue().walkable(wide_off.x, wide_off.y));
}

TEST_CASE("floodplain_mucosal.json loads, validates, and bakes a flow field from every portal",
          "[level][content][floodplain]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/floodplain_mucosal.json");
    REQUIRE(platform::file_exists(path));
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.name == "floodplain_mucosal");
    REQUIRE(def.portals.size() >= 2);
    REQUIRE_FALSE(def.objectives.empty());
    REQUIRE_FALSE(def.placement_zones.empty());

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);
    for (const SpawnPortal& p : def.portals) {
        REQUIRE(world.flow().reachable(p.position));
    }

    // Open floodplain: a point ~5.9 world units off the trunk's centerline
    // (well inside a ~1.3-1.6 wide chokepoint's radius, but inside this
    // level's ~30-wide trunk) must be walkable, proving the lane is wide
    // rather than a thin path.
    const IVec2 wide_off = world.tissue().world_to_cell(Vec2{110.97f, 52.88f});
    REQUIRE(world.tissue().walkable(wide_off.x, wide_off.y));
}
