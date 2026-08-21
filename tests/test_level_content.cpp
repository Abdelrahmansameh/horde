// Tests for Wave 2D's remaining scope: instantiate()'s comp::Objective entity
// spawning, and the two new content levels (capillary_switchback, floodplain_mucosal).
// Owner: Wave 2D.
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

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
  "placement_zones": [ { "min": [10, 10], "max": [50, 22] } ],
  "waves": [
    { "name": "w1", "prep_time": 5.0, "atp_reward": 40,
      "spawns": [ { "family": "virus", "count": 20, "start_time": 0.0, "duration": 2.0 } ] }
  ]
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

TEST_CASE("capillary_switchback.json loads, validates, and bakes a flow field reaching the objective",
          "[level][content][switchback]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/capillary_switchback.json");
    REQUIRE(platform::file_exists(path));
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.name == "capillary_switchback");
    REQUIRE_FALSE(def.portals.empty());
    REQUIRE_FALSE(def.objectives.empty());
    REQUIRE_FALSE(def.placement_zones.empty());

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);
    REQUIRE(world.flow().reachable(def.portals[0].position));

    // This level replaced a pinch with a switchback, so pin both halves of
    // that. (a) The lumen holds its ~11-wide profile end to end: x=67.56 sits
    // mid-lane on both the top run (y=12.5) and the doubled-back middle run
    // (y=38), and ~4.4 units off either centerline is still walkable -- the
    // old level's narrowest point was ~2.5 wide there.
    const IVec2 top_off = world.tissue().world_to_cell(Vec2{67.56f, 16.9f});
    const IVec2 mid_off = world.tissue().world_to_cell(Vec2{67.56f, 42.4f});
    REQUIRE(world.tissue().walkable(top_off.x, top_off.y));
    REQUIRE(world.tissue().walkable(mid_off.x, mid_off.y));

    // (b) The two runs are separate passes of the same lane, not one fat
    // corridor: the tissue between them (y=25, squarely in the gap) is wall.
    // That gap is what makes a tower cluster on it cover both passes, which is
    // the concentration the pinch used to fake (DESIGN.md 4.3).
    const IVec2 between = world.tissue().world_to_cell(Vec2{67.56f, 25.0f});
    REQUIRE_FALSE(world.tissue().walkable(between.x, between.y));
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
    // (outside the radius of any capillary lane, but well inside this level's
    // ~30-wide trunk) must be walkable, proving the lane is wide rather than a
    // thin path.
    const IVec2 wide_off = world.tissue().world_to_cell(Vec2{110.97f, 52.88f});
    REQUIRE(world.tissue().walkable(wide_off.x, wide_off.y));
}

// ---- Every shipped level carries its own wave table ----------------------

TEST_CASE("every level under assets/levels loads, validates, and authors waves",
          "[level][content][waves]") {
    // Waves are per-level only now (Level.h, AUTHORED WAVES): there is no
    // generator to fall back to, so a level shipped without a table is broken
    // content. Sweeping the directory is what makes that a build failure
    // rather than something the player finds.
    const std::string dir = platform::asset_path("levels");
    REQUIRE(std::filesystem::is_directory(dir));

    u32 checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const std::string path = entry.path().string();
        INFO("level = " << path);

        LevelLoader loader;
        LevelDef def;
        REQUIRE(loader.load_file(path, def).ok);
        REQUIRE(loader.validate(def).ok);
        REQUIRE_FALSE(def.waves.empty());
        for (const WaveDef& w : def.waves) {
            INFO("wave = " << w.name);
            REQUIRE_FALSE(w.spawns.empty());
            u32 total = 0;
            for (const SpawnEntry& s : w.spawns) total += s.count;
            REQUIRE(total > 0);
        }
        ++checked;
    }
    REQUIRE(checked >= 13);
}

TEST_CASE("the built-in fallback level authors waves too", "[level][content][waves]") {
    // --bench/--screenshot/--sim-test with no --level land here, and an empty
    // table would mean a headless run with no pressure at all.
    const LevelDef def = LevelLoader::default_test_level();
    LevelLoader loader;
    REQUIRE(loader.validate(def).ok);
    REQUIRE_FALSE(def.waves.empty());
}
