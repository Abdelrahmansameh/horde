// Tests for game/level/Level.cpp: JSON parsing, validation, and
// instantiate()'s TissueMask/DistanceField/FlowField wiring. Owner: Wave 2D.
#include "core/Math.h"
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_approx.hpp>
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
  "spawn_points": [ { "id": "p0", "pos": [4, 16], "radius": 3.0 } ],
  "objectives": [ { "id": "organ", "pos": [60, 16], "radius": 4.0, "integrity": 100 } ],
  "placement_zones": [ { "min": [10, 10], "max": [50, 22] } ],
  "ambient_drift": [1.0, -2.0],
  "waves": [
    { "name": "test_wave_1", "prep_time": 5.0, "atp_reward": 40,
      "spawns": [ { "family": "virus", "count": 20, "start_time": 0.0, "duration": 2.0 } ] }
  ]
})JSON";

/// kValidLevel with its one objective's field list swapped for `fields`, so a
/// footprint test only has to spell the part it is about.
std::string level_with_objective(const std::string& fields) {
    std::string s = kValidLevel;
    const std::string old = R"({ "id": "organ", "pos": [60, 16], "radius": 4.0, "integrity": 100 })";
    const std::string::size_type at = s.find(old);
    if (at == std::string::npos) return s;
    return s.replace(at, old.size(), fields);
}

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

    REQUIRE(def.spawn_points.size() == 1);
    REQUIRE(def.spawn_points[0].id == "p0");
    REQUIRE(def.spawn_points[0].position.x == 4.0f);
    REQUIRE(def.spawn_points[0].position.y == 16.0f);
    REQUIRE(def.spawn_points[0].radius == 3.0f);

    REQUIRE(def.objectives.size() == 1);
    REQUIRE(def.objectives[0].id == "organ");
    REQUIRE(def.objectives[0].position.x == 60.0f);
    REQUIRE(def.objectives[0].integrity == 100.0f);
    // Legacy "radius": the square that value used to describe.
    REQUIRE(def.objectives[0].half_extents.x == 4.0f);
    REQUIRE(def.objectives[0].half_extents.y == 4.0f);
    REQUIRE(def.objectives[0].rotation == 0.0f);

    REQUIRE(def.placement_zones.size() == 1);
    REQUIRE(def.placement_zones[0].min.x == 10.0f);
    REQUIRE(def.placement_zones[0].max.x == 50.0f);

    REQUIRE(def.ambient_drift.x == 1.0f);
    REQUIRE(def.ambient_drift.y == -2.0f);

    // A round trip through validate() should also accept it.
    const LevelLoadResult vres = loader.validate(def);
    REQUIRE(vres.ok);
}

TEST_CASE("an objective authors an oriented rectangle", "[level][loader][objective]") {
    LevelLoader loader;
    LevelDef def;

    SECTION("half_extents + rotation, degrees in the file and radians in the struct") {
        const LevelLoadResult res = loader.load_string(
            level_with_objective(
                R"({ "id": "organ", "pos": [60, 16], "half_extents": [6, 2], "rotation": 90 })"),
            def);
        REQUIRE(res.ok);
        REQUIRE(def.objectives[0].half_extents.x == 6.0f);
        REQUIRE(def.objectives[0].half_extents.y == 2.0f);
        REQUIRE(def.objectives[0].rotation == Catch::Approx(math::kPi * 0.5f));
        // A quarter turn puts the long axis on +y: the point-in-footprint test
        // is what every other system asks, so it is what is pinned here.
        REQUIRE(def.objectives[0].contains(Vec2{60.0f, 21.0f}));
        REQUIRE_FALSE(def.objectives[0].contains(Vec2{65.0f, 16.0f}));
    }
    SECTION("half_extents wins over a radius authored alongside it") {
        const LevelLoadResult res = loader.load_string(
            level_with_objective(
                R"({ "id": "organ", "pos": [60, 16], "radius": 4.0, "half_extents": [6, 2] })"),
            def);
        REQUIRE(res.ok);
        REQUIRE(def.objectives[0].half_extents.x == 6.0f);
        REQUIRE(def.objectives[0].half_extents.y == 2.0f);
    }
    SECTION("a non-positive extent is rejected, not silently clamped") {
        const LevelLoadResult res = loader.load_string(
            level_with_objective(R"({ "id": "organ", "pos": [60, 16], "half_extents": [6, 0] })"),
            def);
        REQUIRE_FALSE(res.ok);
        REQUIRE(res.error.find("extents") != std::string::npos);
    }
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
    REQUIRE(def.spawn_points.size() == 1);
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
      "vessels": [], "spawn_points": [], "objectives": []
    })JSON";

    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string(noSchema, def);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("schema") != std::string::npos);
}

TEST_CASE("load_string rejects an unsupported schema version", "[level][loader]") {
    // 1 and 2 both load (schema 2 is additive; see Level.h). 3 is the first
    // version this build does not know, and a level from the future must fail
    // loudly rather than silently drop the fields it does not understand.
    const char* badSchema = R"JSON({
      "schema": 3,
      "name": "future_schema",
      "vessels": [], "spawn_points": [], "objectives": []
    })JSON";

    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string(badSchema, def);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("schema") != std::string::npos);
}

TEST_CASE("schema 2 loads, and every new field is optional", "[level][loader]") {
    // The compatibility promise: a v2 file that uses none of the new fields is
    // the same LevelDef as the v1 file it came from.
    const char* v2 = R"JSON({
      "schema": 2,
      "name": "v2_minimal",
      "world": { "min": [0,0], "max": [64,32], "cell_size": 1.0 },
      "vessels": [ { "id": "main", "points": [ {"p":[6,16],"w":10}, {"p":[58,16],"w":10} ] } ],
      "spawn_points": [ { "id": "p0", "pos": [8,16], "radius": 3 } ],
      "objectives": [ { "id": "o", "pos": [56,16], "radius": 4 } ],
      "waves": [ { "name": "w", "spawns": [ { "family": "virus", "count": 10 } ] } ]
    })JSON";
    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string(v2, def);
    INFO(res.error);
    REQUIRE(res.ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.display_name.empty());
    REQUIRE(def.allowed_towers.empty());
    REQUIRE(def.economy.starting_atp == 0);
    REQUIRE_FALSE(def.camera.has_center);
    REQUIRE_FALSE(def.editor.present);
    REQUIRE(def.waves[0].spawns[0].squad_size == 0);
}

TEST_CASE("schema 2 parses every new field", "[level][loader]") {
    const char* v2 = R"JSON({
      "schema": 2,
      "name": "v2_full",
      "display_name": "The Full House",
      "description": "everything at once",
      "author": "tester",
      "difficulty": 3,
      "tags": ["tutorial", "skin"],
      "region": "skin",
      "world": { "min": [0,0], "max": [64,32], "cell_size": 1.0 },
      "camera": { "center": [30,16], "view_height": 40, "min_view_height": 10,
                  "max_view_height": 90 },
      "economy": { "starting_atp": 450, "income_multiplier": 1.5 },
      "allowed_towers": ["macrophage", "neutrophil"],
      "win": { "survive_seconds": 120 },
      "vessels": [ { "id": "main", "points": [ {"p":[6,16],"w":10}, {"p":[58,16],"w":10} ] } ],
      "spawn_points": [ { "id": "p0", "pos": [8,16], "radius": 3 } ],
      "objectives": [ { "id": "o", "pos": [56,16], "radius": 4 } ],
      "waves": [ { "name": "w", "spawns": [
        { "family": "virus", "count": 900, "squad_size": 150,
          "squad_paths": ["a_0", "a_2"] } ] } ],
      "editor": { "grid_size": 2.5, "notes": "mind the bend" }
    })JSON";
    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string(v2, def);
    INFO(res.error);
    REQUIRE(res.ok);

    REQUIRE(def.display_name == "The Full House");
    REQUIRE(def.description == "everything at once");
    REQUIRE(def.author == "tester");
    REQUIRE(def.difficulty == 3);
    REQUIRE(def.tags == std::vector<std::string>{"tutorial", "skin"});
    REQUIRE(def.allowed_towers == std::vector<std::string>{"macrophage", "neutrophil"});
    REQUIRE(def.economy.starting_atp == 450);
    REQUIRE(def.economy.income_multiplier == Catch::Approx(1.5f));
    REQUIRE(def.camera.has_center);
    REQUIRE(def.camera.center.x == Catch::Approx(30.0f));
    REQUIRE(def.camera.view_height == Catch::Approx(40.0f));
    REQUIRE(def.win.survive_seconds == Catch::Approx(120.0f));
    REQUIRE(def.editor.present);
    REQUIRE(def.editor.grid_size == Catch::Approx(2.5f));
    REQUIRE(def.editor.notes == "mind the bend");
    REQUIRE(def.waves[0].spawns[0].squad_size == 150);
    REQUIRE(def.waves[0].spawns[0].squad_paths ==
            std::vector<std::string>{"a_0", "a_2"});
}

TEST_CASE("load_string rejects malformed JSON", "[level][loader]") {
    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult res = loader.load_string("{ not valid json", def);
    REQUIRE_FALSE(res.ok);
    REQUIRE_FALSE(res.error.empty());
}

// ---- load_string: waves are required ------------------------------------

TEST_CASE("load_string rejects a level that authors no waves", "[level][loader][waves]") {
    // Waves are per-level only (Level.h, AUTHORED WAVES) -- there is no
    // region-shaped generator left to fall back to, so a level without a table
    // has to fail at load rather than run empty.
    LevelLoader loader;

    {
        LevelDef def;
        const LevelLoadResult res = loader.load_string(R"JSON({"schema":1,
            "vessels":[{"id":"v","points":[{"p":[0,0],"w":2},{"p":[1,1],"w":2}]}],
            "spawn_points":[{"id":"p0","pos":[0,0]}],
            "objectives":[{"id":"o","pos":[1,1]}]})JSON", def);
        REQUIRE_FALSE(res.ok);
        REQUIRE(res.error.find("waves") != std::string::npos);
    }
    {
        LevelDef def;
        const LevelLoadResult res = loader.load_string(R"JSON({"schema":1,
            "vessels":[{"id":"v","points":[{"p":[0,0],"w":2},{"p":[1,1],"w":2}]}],
            "spawn_points":[{"id":"p0","pos":[0,0]}],
            "objectives":[{"id":"o","pos":[1,1]}],
            "waves":[]})JSON", def);
        REQUIRE_FALSE(res.ok);
        REQUIRE(res.error.find("waves") != std::string::npos);
    }
}

// ---- validate(): missing required sections -----------------------------

TEST_CASE("validate rejects a schema-valid level missing vessels/spawn_points/objectives",
          "[level][loader][validate]") {
    LevelLoader loader;

    {
        LevelDef def;
        const LevelLoadResult res =
            loader.load_string(R"JSON({"schema":1,"spawn_points":[{"id":"p0","pos":[0,0]}],
                                        "objectives":[{"id":"o","pos":[1,1]}],
                                        "waves":[{"name":"w1","prep_time":1.0,"spawns":[{"family":"virus","count":1,"duration":1.0}]}]})JSON", def);
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
                                        "objectives":[{"id":"o","pos":[1,1]}],
                                        "waves":[{"name":"w1","prep_time":1.0,"spawns":[{"family":"virus","count":1,"duration":1.0}]}]})JSON", def);
        REQUIRE(res.ok);
        const LevelLoadResult vres = loader.validate(def);
        REQUIRE_FALSE(vres.ok);
        REQUIRE(vres.error.find("spawn point") != std::string::npos);
    }
    {
        LevelDef def;
        const LevelLoadResult res =
            loader.load_string(R"JSON({"schema":1,
                                        "vessels":[{"id":"v","points":[{"p":[0,0],"w":2},{"p":[1,1],"w":2}]}],
                                        "spawn_points":[{"id":"p0","pos":[0,0]}],
                                        "waves":[{"name":"w1","prep_time":1.0,"spawns":[{"family":"virus","count":1,"duration":1.0}]}]})JSON", def);
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

    // Spawn point and objective positions must land on rasterized (walkable) tissue.
    const IVec2 spawn_point_cell = world.tissue().world_to_cell(def.spawn_points[0].position);
    const IVec2 objective_cell = world.tissue().world_to_cell(def.objectives[0].position);
    REQUIRE(world.tissue().walkable(spawn_point_cell.x, spawn_point_cell.y));
    REQUIRE(world.tissue().walkable(objective_cell.x, objective_cell.y));

    // The objective must be reachable from the spawn point via the baked flow field.
    REQUIRE(world.flow().reachable(def.spawn_points[0].position));

    // A point clearly off the vessel (well below the corridor) stays unwalkable.
    REQUIRE_FALSE(world.tissue().walkable(0, 0));
}

TEST_CASE("instantiate on capillary_test.json points the flow field from the spawn point toward the objective",
          "[level][loader][instantiate]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/capillary_test.json");
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);

    REQUIRE(world.flow().reachable(def.spawn_points[0].position));

    // Spawn point sits at (6.48,44), objective at (153.14,44): flow at the spawn point
    // should point strongly toward +x (toward the objective), not zero and
    // not backward.
    const Vec2 dir = world.flow().sample(def.spawn_points[0].position);
    REQUIRE(dir.x > 0.5f);
    REQUIRE(std::abs(dir.y) < 0.7f);
}

// ---------------------------------------------------------------------------
// squad_paths (sim/squad/Squads.h)
// ---------------------------------------------------------------------------

namespace {

/// kValidLevel with a `squad_paths` block spliced in before "waves".
std::string level_with_squad_paths(const std::string& paths_json) {
    std::string base = kValidLevel;
    const std::string marker = "\"waves\":";
    const auto at = base.find(marker);
    REQUIRE(at != std::string::npos);
    return base.substr(0, at) + "\"squad_paths\": " + paths_json + ",\n  " + base.substr(at);
}

} // namespace

TEST_CASE("load_string parses an authored squad_paths block", "[level][loader][squad]") {
    LevelLoader loader;
    LevelDef def;
    const std::string json = level_with_squad_paths(R"([
    { "id": "high_road", "lane_id": "main",
      "points": [ [4, 12], [32, 10], [60, 12] ], "half_width": 5.5 },
    { "id": "low_road", "points": [ [4, 20], [32, 22], [60, 20] ] }
  ])");
    const LevelLoadResult r = loader.load_string(json, def);
    INFO(r.error);
    REQUIRE(r.ok);
    REQUIRE(def.squad_paths.size() == 2);

    REQUIRE(def.squad_paths[0].id == "high_road");
    REQUIRE(def.squad_paths[0].lane_id == "main");
    REQUIRE(def.squad_paths[0].points.size() == 3);
    REQUIRE(def.squad_paths[0].points[1].x == 32.0f);
    REQUIRE(def.squad_paths[0].half_width == 5.5f);

    // Omitted lane_id stays empty here; build_squad_paths() resolves it against
    // the geometry, which this function cannot see.
    REQUIRE(def.squad_paths[1].lane_id.empty());
    REQUIRE(def.squad_paths[1].half_width == 6.0f);   // documented default

    REQUIRE(loader.validate(def).ok);
}

TEST_CASE("a level with no squad_paths still loads and validates",
          "[level][loader][squad]") {
    // The back-compat guarantee that let this ship without editing any of the
    // existing level files.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(kValidLevel, def).ok);
    REQUIRE(def.squad_paths.empty());
    REQUIRE(loader.validate(def).ok);
}

TEST_CASE("load_string rejects malformed squad paths", "[level][loader][squad]") {
    LevelLoader loader;
    LevelDef def;

    SECTION("a path with one point has no direction to carry an anchor along") {
        const LevelLoadResult r = loader.load_string(
            level_with_squad_paths(R"([{ "id": "stub", "points": [ [4, 12] ] }])"), def);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.error.find("at least 2 points") != std::string::npos);
    }
    SECTION("a path with no id") {
        const LevelLoadResult r = loader.load_string(
            level_with_squad_paths(R"([{ "points": [ [4, 12], [60, 12] ] }])"), def);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.error.find("missing 'id'") != std::string::npos);
    }
    SECTION("a path with no points") {
        const LevelLoadResult r =
            loader.load_string(level_with_squad_paths(R"([{ "id": "nopoints" }])"), def);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.error.find("missing 'points'") != std::string::npos);
    }
    SECTION("a non-positive half_width") {
        const LevelLoadResult r = loader.load_string(
            level_with_squad_paths(
                R"([{ "id": "flat", "points": [ [4, 12], [60, 12] ], "half_width": 0.0 }])"),
            def);
        REQUIRE_FALSE(r.ok);
        REQUIRE(r.error.find("half_width") != std::string::npos);
    }
}

TEST_CASE("validate rejects a squad path naming a lane that does not exist",
          "[level][loader][squad]") {
    // An omitted lane_id is resolved to the nearest lane by design; a MISPELLED
    // one must not be, or the author silently gets squads on a lane they never
    // meant to touch.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(
                    level_with_squad_paths(
                        R"([{ "id": "typo", "lane_id": "mian", "points": [ [4,12], [60,12] ] }])"),
                    def)
                .ok);
    const LevelLoadResult v = loader.validate(def);
    REQUIRE_FALSE(v.ok);
    REQUIRE(v.error.find("unknown lane") != std::string::npos);
}

TEST_CASE("build_squad_paths derives a spread for a lane that authored none",
          "[level][loader][squad]") {
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(kValidLevel, def).ok);

    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = def.world_bounds;
    world.init(desc, nullptr);
    REQUIRE(loader.instantiate(def, world).ok);

    // How many paths a lane gets follows its WIDTH -- see kMinPathSpacing in
    // Level.cpp. kValidLevel's vessel is 6 units wide, narrower than a single
    // squad, so exactly one path (the centerline) is the right answer: putting
    // two side by side in a lane that cannot hold them would be worse than
    // putting none.
    const auto& paths = world.squads().paths();
    REQUIRE(paths.size() == 1);
    for (const sim::SquadPath& p : paths) {
        REQUIRE(p.lane_id == "main");
        REQUIRE(p.valid());
        REQUIRE(p.length() > 0.0f);
        REQUIRE(p.half_width > 0.0f);
        // Every derived point must be on tissue -- an anchor inside a wall
        // would drag its squad into it.
        for (Vec2 pt : p.points) {
            const IVec2 c = world.tissue().world_to_cell(pt);
            REQUIRE(world.tissue().walkable(c.x, c.y));
        }
    }
}

TEST_CASE("a wide lane derives several paths, a narrow one derives a single lane",
          "[level][loader][squad]") {
    // The rule the squad layer actually depends on: path count follows lumen
    // width, so a level does not have to be authored knowing anything about
    // squads for its lanes to carry the right number of them.
    LevelLoader loader;

    auto paths_for_width = [&loader](f32 width) {
        LevelDef def;
        REQUIRE(loader.load_string(kValidLevel, def).ok);
        def.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{200.0f, 200.0f}};
        for (VesselPoint& vp : def.vessels[0].points) {
            vp.width = width;
            vp.position.y = 100.0f;
        }
        def.spawn_points[0].position = Vec2{def.vessels[0].points.front().position.x, 100.0f};
        def.objectives[0].position = Vec2{def.vessels[0].points.back().position.x, 100.0f};

        sim::SimWorld world;
        sim::SimDesc desc;
        desc.world_bounds = def.world_bounds;
        world.init(desc, nullptr);
        REQUIRE(loader.instantiate(def, world).ok);
        return world.squads().paths().size();
    };

    const usize narrow = paths_for_width(10.0f);
    const usize wide = paths_for_width(80.0f);
    INFO("narrow=" << narrow << " wide=" << wide);
    REQUIRE(narrow == 1);
    REQUIRE(wide >= 3);
}

TEST_CASE("an authored lane is left entirely to the author", "[level][loader][squad]") {
    // Mixing derived paths into a hand-tuned lane would silently undo the tuning.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(
                    level_with_squad_paths(
                        R"([{ "id": "only", "lane_id": "main", "points": [ [4,16], [60,16] ] }])"),
                    def)
                .ok);

    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = def.world_bounds;
    world.init(desc, nullptr);
    REQUIRE(loader.instantiate(def, world).ok);

    REQUIRE(world.squads().paths().size() == 1);
    REQUIRE(world.squads().paths()[0].id == "only");
}
