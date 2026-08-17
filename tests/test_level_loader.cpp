// Tests for game/level/Level.cpp: JSON parsing, validation, and
// instantiate()'s TissueMask/DistanceField/FlowField wiring. Owner: Wave 2D.
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace immune;
using namespace immune::game;

namespace {

const char* kValidLevel = R"JSON({
  "schema": 1,
  "name": "test_level",
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
  "objectives": [ { "id": "organ", "pos": [60, 16], "radius": 4.0, "integrity": 100 } ],
  "placement_zones": [ { "min": [10, 10], "max": [50, 22] } ],
  "ambient_drift": [1.0, -2.0]
})JSON";

sim::SimWorld make_world(const LevelDef& def) {
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = def.world_bounds;
    world.init(desc, nullptr);
    return world;
}

} // namespace

// ---- load_string: valid schema round trip ---------------------------------

TEST_CASE("load_string parses a valid level into a matching LevelDef", "[level][loader]") {
    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string(kValidLevel, def);

    REQUIRE(res.ok);
    REQUIRE(res.error.empty());

    REQUIRE(def.schema == 1);
    REQUIRE(def.name == "test_level");
    REQUIRE(def.region == "capillary");
    REQUIRE(def.world_bounds.min.x == 0.0f);
    REQUIRE(def.world_bounds.min.y == 0.0f);
    REQUIRE(def.world_bounds.max.x == 64.0f);
    REQUIRE(def.world_bounds.max.y == 32.0f);
    REQUIRE(def.cell_size == 1.0f);

    REQUIRE(def.vessels.size() == 1);
    REQUIRE(def.vessels[0].id == "main");
    REQUIRE(def.vessels[0].points.size() == 3);
    REQUIRE(def.vessels[0].points[0].position.x == 4.0f);
    REQUIRE(def.vessels[0].points[0].position.y == 16.0f);
    REQUIRE(def.vessels[0].points[0].width == 6.0f);
    REQUIRE(def.vessels[0].children.empty());

    REQUIRE(def.portals.size() == 1);
    REQUIRE(def.portals[0].id == "p0");
    REQUIRE(def.portals[0].position.x == 4.0f);
    REQUIRE(def.portals[0].position.y == 16.0f);
    REQUIRE(def.portals[0].radius == 3.0f);

    REQUIRE(def.objectives.size() == 1);
    REQUIRE(def.objectives[0].id == "organ");
    REQUIRE(def.objectives[0].position.x == 60.0f);
    REQUIRE(def.objectives[0].integrity == 100.0f);

    REQUIRE(def.placement_zones.size() == 1);
    REQUIRE(def.placement_zones[0].min.x == 10.0f);
    REQUIRE(def.placement_zones[0].max.x == 50.0f);

    REQUIRE(def.ambient_drift.x == 1.0f);
    REQUIRE(def.ambient_drift.y == -2.0f);

    // A round trip through validate() should also accept it.
    const LevelLoadResult vres = loader.validate(def);
    REQUIRE(vres.ok);
}

TEST_CASE("load_file reads and parses assets/levels/capillary_test.json", "[level][loader]") {
    const std::string path = platform::asset_path("levels/capillary_test.json");
    REQUIRE(platform::file_exists(path));

    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_file(path, def);
    REQUIRE(res.ok);
    REQUIRE(def.name == "capillary_test");
    REQUIRE(def.vessels.size() == 1);
    REQUIRE(def.portals.size() == 1);
    REQUIRE(def.objectives.size() == 1);

    const LevelLoadResult vres = loader.validate(def);
    REQUIRE(vres.ok);
}

TEST_CASE("load_file reports an error for a missing file", "[level][loader]") {
    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_file("assets/levels/does_not_exist.json", def);
    REQUIRE_FALSE(res.ok);
    REQUIRE_FALSE(res.error.empty());
}

// ---- Schema rejection -------------------------------------------------

TEST_CASE("load_string rejects a level with no \"schema\" field", "[level][loader]") {
    const char* noSchema = R"JSON({
      "name": "no_schema",
      "vessels": [], "portals": [], "objectives": []
    })JSON";

    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string(noSchema, def);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("schema") != std::string::npos);
}

TEST_CASE("load_string rejects an unsupported schema version", "[level][loader]") {
    const char* badSchema = R"JSON({
      "schema": 2,
      "name": "future_schema",
      "vessels": [], "portals": [], "objectives": []
    })JSON";

    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string(badSchema, def);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("schema") != std::string::npos);
}

TEST_CASE("load_string rejects malformed JSON", "[level][loader]") {
    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string("{ not valid json", def);
    REQUIRE_FALSE(res.ok);
    REQUIRE_FALSE(res.error.empty());
}

// ---- validate(): missing required sections -----------------------------

TEST_CASE("validate rejects a schema-valid level missing vessels/portals/objectives",
          "[level][loader][validate]") {
    LevelLoader loader;

    {
        LevelDef def;
        const LevelLoadResult res =
            loader.load_string(R"JSON({"schema":1,"portals":[{"id":"p0","pos":[0,0]}],
                                        "objectives":[{"id":"o","pos":[1,1]}]})JSON", def);
        REQUIRE(res.ok); // parses fine, missing sections aren't a parse error
        const LevelLoadResult vres = loader.validate(def);
        REQUIRE_FALSE(vres.ok);
        REQUIRE(vres.error.find("vessel") != std::string::npos);
    }
    {
        LevelDef def;
        const LevelLoadResult res =
            loader.load_string(R"JSON({"schema":1,
                                        "vessels":[{"id":"v","points":[{"p":[0,0],"w":2},{"p":[1,1],"w":2}]}],
                                        "objectives":[{"id":"o","pos":[1,1]}]})JSON", def);
        REQUIRE(res.ok);
        const LevelLoadResult vres = loader.validate(def);
        REQUIRE_FALSE(vres.ok);
        REQUIRE(vres.error.find("portal") != std::string::npos);
    }
    {
        LevelDef def;
        const LevelLoadResult res =
            loader.load_string(R"JSON({"schema":1,
                                        "vessels":[{"id":"v","points":[{"p":[0,0],"w":2},{"p":[1,1],"w":2}]}],
                                        "portals":[{"id":"p0","pos":[0,0]}]})JSON", def);
        REQUIRE(res.ok);
        const LevelLoadResult vres = loader.validate(def);
        REQUIRE_FALSE(vres.ok);
        REQUIRE(vres.error.find("objective") != std::string::npos);
    }
}

// ---- instantiate(): rasterization + bake wiring -----------------------

TEST_CASE("instantiate rasterizes vessels and bakes a flow field the objective is reachable through",
          "[level][loader][instantiate]") {
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(kValidLevel, def).ok);
    REQUIRE(loader.validate(def).ok);

    sim::SimWorld world = make_world(def);
    const LevelLoadResult res = loader.instantiate(def, world);
    REQUIRE(res.ok);

    // Portal and objective positions must land on rasterized (walkable) tissue.
    const IVec2 portal_cell = world.tissue().world_to_cell(def.portals[0].position);
    const IVec2 objective_cell = world.tissue().world_to_cell(def.objectives[0].position);
    REQUIRE(world.tissue().walkable(portal_cell.x, portal_cell.y));
    REQUIRE(world.tissue().walkable(objective_cell.x, objective_cell.y));

    // The objective must be reachable from the portal via the baked flow field.
    REQUIRE(world.flow().reachable(def.portals[0].position));

    // A point clearly off the vessel (well below the corridor) stays unwalkable.
    REQUIRE_FALSE(world.tissue().walkable(0, 0));
}

TEST_CASE("instantiate on capillary_test.json points the flow field from the portal toward the objective",
          "[level][loader][instantiate]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/capillary_test.json");
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);

    REQUIRE(world.flow().reachable(def.portals[0].position));

    // Portal sits at (6.48,44), objective at (153.14,44): flow at the portal
    // should point strongly toward +x (toward the objective), not zero and
    // not backward.
    const Vec2 dir = world.flow().sample(def.portals[0].position);
    REQUIRE(dir.x > 0.5f);
    REQUIRE(std::abs(dir.y) < 0.7f);
}
