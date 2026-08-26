// Tests for in-lane obstacles: the five carve primitives
// (sim/flowfield/ObstacleRaster.h), their JSON surface and validation
// (game/level/Level.cpp), and the four downstream behaviours the feature is
// supposed to inherit for free rather than special-case -- SDF/visual identity
// with the lane wall, flow-field rerouting, lane attribution, and tower
// placement refusal.
//
// The through-line of the whole file: an obstacle must be indistinguishable
// from ground the vessel spline never covered. Every assertion below is
// phrased as "the island behaves exactly like the outer wall", because the
// moment one of them needs a special case, the design has drifted.
#include "game/level/Level.h"
#include "game/towers/TowerSystem.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "sim/flowfield/ObstacleRaster.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace immune;
using namespace immune::game;

namespace {

sim::SimWorld make_world(const LevelDef& def) {
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.seed = 4242;
    desc.world_bounds = def.world_bounds;
    world.init(desc, nullptr);
    return world;
}

/// A wide straight lane from (10,50) to (190,50), 40 units across (y 30..70),
/// with `obstacles` spliced in. Wide enough that every shape below leaves a
/// generous way round, so any unreachability an assertion finds is the
/// obstacle's doing and not a too-tight lane.
std::string lane_with(const std::string& obstacles) {
    return R"JSON({
  "schema": 1,
  "name": "obstacle_fixture",
  "region": "capillary",
  "world": { "min": [0, 0], "max": [200, 100], "cell_size": 0.5 },
  "vessels": [
    { "id": "main", "points": [
        { "p": [10, 50], "w": 40.0 },
        { "p": [100, 50], "w": 40.0 },
        { "p": [190, 50], "w": 40.0 } ] }
  ],
  "obstacles": [)JSON" + obstacles + R"JSON(],
  "spawn_points": [ { "id": "p0", "pos": [12, 50], "radius": 4.0 } ],
  "objectives": [ { "id": "organ", "pos": [188, 50], "radius": 5.0, "integrity": 100 } ],
  "waves": [
    { "name": "w1", "prep_time": 5.0, "atp_reward": 40,
      "spawns": [ { "family": "virus", "count": 10, "start_time": 0.0, "duration": 1.0 } ] }
  ]
})JSON";
}

bool walkable_at(const sim::TissueMask& mask, Vec2 p) {
    const IVec2 c = mask.world_to_cell(p);
    return mask.walkable(c.x, c.y);
}

/// Loads a one-obstacle lane and bakes it. Fails the test on any loader error,
/// so a caller can go straight to asserting geometry.
sim::SimWorld bake(const std::string& obstacle_json, LevelDef& def) {
    LevelLoader loader;
    const LevelLoadResult parsed = loader.load_string(lane_with(obstacle_json), def);
    INFO("load error: " << parsed.error);
    REQUIRE(parsed.ok);
    sim::SimWorld world = make_world(def);
    const LevelLoadResult inst = loader.instantiate(def, world);
    INFO("instantiate error: " << inst.error);
    REQUIRE(inst.ok);
    return world;
}

} // namespace

// ---- The five primitives ---------------------------------------------------

TEST_CASE("a disc obstacle carves a solid island inside the lumen", "[level][obstacles]") {
    LevelDef def;
    sim::SimWorld world = bake(R"({ "id": "plaque", "shape": "disc", "pos": [100, 50], "radius": 8 })", def);
    const sim::TissueMask& mask = world.tissue();

    REQUIRE(def.obstacles.size() == 1);
    REQUIRE(def.obstacles[0].shape == ObstacleShape::Disc);

    REQUIRE_FALSE(walkable_at(mask, Vec2{100.0f, 50.0f}));  // centre
    REQUIRE_FALSE(walkable_at(mask, Vec2{100.0f, 56.0f}));  // inside, off-centre
    REQUIRE(walkable_at(mask, Vec2{100.0f, 62.0f}));        // past the rim, still lumen
    REQUIRE(walkable_at(mask, Vec2{86.0f, 50.0f}));         // upstream of it
}

TEST_CASE("a capsule obstacle carves the whole stadium, not just its endpoints",
          "[level][obstacles]") {
    LevelDef def;
    sim::SimWorld world =
        bake(R"({ "shape": "capsule", "points": [[90, 40], [110, 60]], "radius": 3 })", def);
    const sim::TissueMask& mask = world.tissue();

    REQUIRE_FALSE(walkable_at(mask, Vec2{90.0f, 40.0f}));    // end a
    REQUIRE_FALSE(walkable_at(mask, Vec2{110.0f, 60.0f}));   // end b
    REQUIRE_FALSE(walkable_at(mask, Vec2{100.0f, 50.0f}));   // midpoint of the bar
    REQUIRE(walkable_at(mask, Vec2{100.0f, 44.0f}));         // beside it (offset > radius)
}

TEST_CASE("a box obstacle honours its rotation", "[level][obstacles]") {
    // A 16x4 bar at 90 degrees is tall, not wide: the point 6 units ABOVE the
    // centre is inside it and the point 6 units to the SIDE is not, which is
    // the exact opposite of the same bar unrotated.
    LevelDef def;
    sim::SimWorld world = bake(
        R"({ "shape": "box", "pos": [100, 50], "half_extents": [8, 2], "rotation": 90 })", def);
    const sim::TissueMask& mask = world.tissue();

    REQUIRE_FALSE(walkable_at(mask, Vec2{100.0f, 50.0f}));
    REQUIRE_FALSE(walkable_at(mask, Vec2{100.0f, 56.0f}));   // along the rotated long axis
    REQUIRE(walkable_at(mask, Vec2{106.0f, 50.0f}));         // along the rotated short axis
}

TEST_CASE("a polygon obstacle fills a concave outline without filling its notch",
          "[level][obstacles]") {
    // A chevron opening to +x: the notch between its arms must stay lumen,
    // which is the property an even-odd fill has and a convex hull does not.
    LevelDef def;
    sim::SimWorld world = bake(
        R"({ "shape": "polygon", "points": [[88, 38], [112, 50], [88, 62], [96, 50]] })", def);
    const sim::TissueMask& mask = world.tissue();

    REQUIRE_FALSE(walkable_at(mask, Vec2{95.0f, 44.0f}));   // inside an arm
    REQUIRE_FALSE(walkable_at(mask, Vec2{95.0f, 56.0f}));   // inside the other arm
    REQUIRE(walkable_at(mask, Vec2{90.0f, 50.0f}));         // the notch, still open
    REQUIRE(walkable_at(mask, Vec2{115.0f, 50.0f}));        // past the tip
}

TEST_CASE("a ridge obstacle follows its spline and its per-point widths",
          "[level][obstacles]") {
    LevelDef def;
    sim::SimWorld world = bake(R"({ "shape": "ridge", "points": [
        { "p": [90, 44], "w": 8 },
        { "p": [100, 50], "w": 8 },
        { "p": [110, 56], "w": 8 } ] })", def);
    const sim::TissueMask& mask = world.tissue();

    REQUIRE_FALSE(walkable_at(mask, Vec2{90.0f, 44.0f}));
    REQUIRE_FALSE(walkable_at(mask, Vec2{100.0f, 50.0f}));
    REQUIRE_FALSE(walkable_at(mask, Vec2{110.0f, 56.0f}));
    // Between two control points: the curve is stamped continuously, so a
    // sample halfway along is solid too rather than falling through a gap.
    REQUIRE_FALSE(walkable_at(mask, Vec2{95.0f, 47.0f}));
    REQUIRE(walkable_at(mask, Vec2{100.0f, 62.0f}));        // clear of the septum
}

// ---- The downstream behaviours that must come for free ---------------------

TEST_CASE("an obstacle is the same material as the outer wall in the SDF",
          "[level][obstacles][sdf]") {
    // tissue.frag draws interstitium, wall and lumen from the SDF alone, so
    // "looks like the lane's edge" IS "has the lane edge's distance profile":
    // negative inside, zero-crossing at the surface, rising into the lumen.
    LevelDef def;
    sim::SimWorld world = bake(R"({ "shape": "disc", "pos": [100, 50], "radius": 8 })", def);
    const sim::DistanceField& sdf = world.sdf();

    REQUIRE(sdf.sample(Vec2{100.0f, 50.0f}) < 0.0f);        // deep inside the island
    REQUIRE(sdf.sample(Vec2{100.0f, 60.0f}) > 0.0f);        // lumen just past its rim
    // Clearance grows with distance from the island, exactly as it does when
    // walking inward from the vessel's own boundary.
    REQUIRE(sdf.sample(Vec2{100.0f, 64.0f}) > sdf.sample(Vec2{100.0f, 60.0f}));
    // And the gradient points out of the island, which is what steers agents
    // off it (ChaffSystem's resolve_wall_contact reads exactly this).
    REQUIRE(sdf.gradient(Vec2{100.0f, 56.0f}).y > 0.0f);
}

TEST_CASE("the flow field routes around an obstacle instead of through it",
          "[level][obstacles][flow]") {
    LevelDef def;
    sim::SimWorld world = bake(R"({ "shape": "disc", "pos": [100, 50], "radius": 10 })", def);

    // The island itself is not walkable ground, so it is not reachable...
    REQUIRE_FALSE(world.flow().reachable(Vec2{100.0f, 50.0f}));
    // ...but everything the horde actually uses still is, from the spawn point
    // all the way past the island to the objective.
    REQUIRE(world.flow().reachable(def.spawn_points[0].position));
    REQUIRE(world.flow().reachable(Vec2{100.0f, 64.0f}));   // the way round
    REQUIRE(world.flow().reachable(Vec2{140.0f, 50.0f}));   // downstream of it

    // Approaching the island's upper-left shoulder, the field carries the
    // horde up and over it rather than into its face. (Probed off the
    // centerline on purpose: dead ahead of a centred island the geometry is
    // symmetric and either way round is equally short, so the sign of a probe
    // there is a coin flip, not a behaviour.)
    const Vec2 dir = world.flow().sample(Vec2{88.0f, 56.0f});
    REQUIRE(dir.y > 0.0f);
}

TEST_CASE("instantiate fails when an obstacle seals the lane", "[level][obstacles][validation]") {
    // A bar spanning the full 40-unit lumen and then some. Nothing about the
    // JSON looks wrong; only the baked field can tell, which is why the check
    // lives after the bake.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(
                    lane_with(R"({ "id": "wall", "shape": "box", "pos": [100, 50],
                                   "half_extents": [3, 30] })"), def)
                .ok);
    sim::SimWorld world = make_world(def);
    const LevelLoadResult res = loader.instantiate(def, world);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("p0") != std::string::npos);
}

TEST_CASE("an obstacle that buries a spawn point or objective is rejected at load",
          "[level][obstacles][validation]") {
    LevelLoader loader;
    LevelDef def;

    const LevelLoadResult on_spawn = loader.load_string(
        lane_with(R"({ "id": "oops", "shape": "disc", "pos": [12, 50], "radius": 6 })"), def);
    REQUIRE_FALSE(on_spawn.ok);
    REQUIRE(on_spawn.error.find("spawn point") != std::string::npos);

    const LevelLoadResult on_objective = loader.load_string(
        lane_with(R"({ "shape": "capsule", "points": [[180, 50], [188, 50]], "radius": 4 })"), def);
    REQUIRE_FALSE(on_objective.ok);
    REQUIRE(on_objective.error.find("objective") != std::string::npos);
}

TEST_CASE("malformed obstacles fail the load rather than carving something plausible",
          "[level][obstacles][validation]") {
    LevelLoader loader;
    LevelDef def;

    // A typo'd shape name is the dangerous one: a silent fallback would carve
    // a real, wrong obstacle and the level would ship with it.
    REQUIRE_FALSE(loader.load_string(lane_with(R"({ "shape": "sphere", "pos": [100,50], "radius": 4 })"), def).ok);
    REQUIRE_FALSE(loader.load_string(lane_with(R"({ "shape": "disc", "pos": [100,50] })"), def).ok);
    REQUIRE_FALSE(loader.load_string(lane_with(R"({ "shape": "capsule", "points": [[100,50]], "radius": 3 })"), def).ok);
    REQUIRE_FALSE(loader.load_string(lane_with(R"({ "shape": "box", "pos": [100,50] })"), def).ok);
    REQUIRE_FALSE(loader.load_string(lane_with(R"({ "shape": "polygon", "points": [[100,50],[104,52]] })"), def).ok);
    // A ridge whose points use the bare [x,y] spelling has no widths, so it
    // would carve nothing; that is an authoring error, not a silent no-op.
    REQUIRE_FALSE(loader.load_string(lane_with(R"({ "shape": "ridge", "points": [[90,50],[110,50]] })"), def).ok);
}

TEST_CASE("towers cannot be placed on an obstacle", "[level][obstacles][towers]") {
    LevelDef def;
    sim::SimWorld world = bake(R"({ "shape": "disc", "pos": [100, 50], "radius": 10 })", def);

    TowerSystem towers;
    constexpr u32 kPlentyOfAtp = 100000u;

    // Dead centre of the island is not tissue at all.
    const PlacementQuery on_island =
        towers.validate(world, TowerType::Neutrophil, Vec2{100.0f, 50.0f}, kPlentyOfAtp);
    REQUIRE_FALSE(on_island.valid());
    REQUIRE(on_island.result == PlacementResult::NotOnTissue);

    // Hard against its rim there is tissue but no room, the same refusal a
    // tower gets when it is shoved against the lane's outer wall.
    const PlacementQuery on_rim =
        towers.validate(world, TowerType::Neutrophil, Vec2{100.0f, 60.2f}, kPlentyOfAtp);
    REQUIRE_FALSE(on_rim.valid());
    REQUIRE(on_rim.result == PlacementResult::InsufficientClearance);

    // But the open lumen beside the island is still buildable -- an obstacle
    // must not poison the ground around it.
    const PlacementQuery beside =
        towers.validate(world, TowerType::Neutrophil, Vec2{100.0f, 66.0f}, kPlentyOfAtp);
    INFO("result = " << static_cast<int>(beside.result) << " clearance = " << beside.clearance);
    REQUIRE(beside.valid());
}

TEST_CASE("obstacle cells belong to no lane", "[level][obstacles][lanes]") {
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(lane_with(R"({ "shape": "disc", "pos": [100, 50], "radius": 8 })"), def).ok);

    const LaneOwnershipMap map = loader.build_lane_ownership_map(def);
    REQUIRE(map.lane_ids.size() == 1);
    // Solid rock is not ground the lane's traffic can occupy, so lane_at()
    // must not answer "main" for a point no agent can ever stand on.
    REQUIRE(map.lane_at(Vec2{100.0f, 50.0f}) == -1);
    REQUIRE(map.lane_at(Vec2{100.0f, 64.0f}) == 0);
}

TEST_CASE("derived squad paths route around an obstacle rather than into it",
          "[level][obstacles][squads]") {
    // The derivation offsets a lane's centerline sideways, and the centerline
    // is exactly what an island parked mid-lane sits on -- so this is the one
    // place the feature needed real work rather than inheritance.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(lane_with(R"({ "shape": "disc", "pos": [100, 50], "radius": 9 })"), def).ok);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);

    const std::vector<sim::SquadPath> paths = loader.build_squad_paths(def, world.tissue());
    REQUIRE_FALSE(paths.empty());
    for (const sim::SquadPath& path : paths) {
        REQUIRE(path.points.size() >= 2);
        for (const Vec2& p : path.points) {
            INFO("path point " << p.x << "," << p.y);
            REQUIRE(walkable_at(world.tissue(), p));
        }
    }
}

// ---- Shipped content -------------------------------------------------------

TEST_CASE("plaque_field.json loads, bakes, and keeps its lane open past all five shapes",
          "[level][obstacles][content]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/plaque_field.json");
    REQUIRE(platform::file_exists(path));
    const LevelLoadResult parsed = loader.load_file(path, def);
    INFO("load error: " << parsed.error);
    REQUIRE(parsed.ok);
    REQUIRE(loader.validate(def).ok);

    // One of each primitive, so the shipped level is also the coverage that
    // catches a schema change nobody re-ran the fixtures against.
    REQUIRE(def.obstacles.size() == 5);
    bool seen[5] = {false, false, false, false, false};
    for (const ObstacleDef& o : def.obstacles) seen[static_cast<usize>(o.shape)] = true;
    for (bool s : seen) REQUIRE(s);

    sim::SimWorld world = make_world(def);
    const LevelLoadResult inst = loader.instantiate(def, world);
    INFO("instantiate error: " << inst.error);
    REQUIRE(inst.ok);   // fails if any obstacle sealed the lane
    for (const SpawnPoint& p : def.spawn_points) {
        REQUIRE(world.flow().reachable(p.position));
    }
    // Every authored obstacle actually carved something -- an obstacle that
    // landed outside the lumen is a no-op the author would never see.
    for (const ObstacleDef& o : def.obstacles) {
        INFO("obstacle " << o.id);
        const Vec2 probe = (o.shape == ObstacleShape::Disc || o.shape == ObstacleShape::Box)
                               ? o.position
                               : o.points[o.points.size() / 2].position;
        REQUIRE_FALSE(walkable_at(world.tissue(), probe));
    }
}
