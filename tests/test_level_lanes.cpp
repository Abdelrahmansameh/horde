// Tests for Wave 4D's multi-lane level schema: Vessel::lane_id/type,
// SpawnPortal::lane_id + resolve_portal_lane_id(), PlacementZoneTag, and
// LaneOwnershipMap/build_lane_ownership_map(). Owner: Wave 4D.
//
// Two concerns, deliberately kept separate:
//   1. Regression: the three pre-existing single-lane/single-vessel-type
//      levels must still load, validate, and instantiate exactly as before,
//      picking up sensible schema defaults with zero JSON changes.
//   2. New: assets/levels/lane_schema_test.json exercises the actual
//      multi-lane schema end to end -- three differently-typed lanes with
//      distinct portals, converging on one shared objective.
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace immune;
using namespace immune::game;

namespace {

sim::SimWorld make_world(const LevelDef& def) {
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = def.world_bounds;
    world.init(desc, nullptr);
    return world;
}

} // namespace

// ---- VesselType string mapping ---------------------------------------------

TEST_CASE("vessel_type_from_string round-trips every named archetype", "[level][lanes]") {
    REQUIRE(vessel_type_from_string("artery") == VesselType::Artery);
    REQUIRE(vessel_type_from_string("vein") == VesselType::Vein);
    REQUIRE(vessel_type_from_string("lymphatic") == VesselType::Lymphatic);
    REQUIRE(vessel_type_from_string("nerve_adjacent") == VesselType::NerveAdjacent);
    REQUIRE(vessel_type_from_string("mucosal_fold") == VesselType::MucosalFold);

    for (VesselType t : {VesselType::Artery, VesselType::Vein, VesselType::Lymphatic,
                          VesselType::NerveAdjacent, VesselType::MucosalFold}) {
        REQUIRE(vessel_type_from_string(vessel_type_to_string(t)) == t);
    }
}

TEST_CASE("vessel_type_from_string defaults unknown or empty strings to Artery", "[level][lanes]") {
    REQUIRE(vessel_type_from_string("") == VesselType::Artery);
    REQUIRE(vessel_type_from_string("plasma_river") == VesselType::Artery);
}

// ---- Regression: existing single-lane levels get sensible defaults --------

TEST_CASE("a vessel with no lane_id/vessel_type in JSON defaults lane_id to its own id "
          "and vessel_type to Artery",
          "[level][lanes][regression]") {
    const char* json = R"JSON({
      "schema": 1,
      "vessels": [ { "id": "main", "points": [ {"p":[0,0],"w":4}, {"p":[10,0],"w":4} ] } ],
      "portals": [ { "id": "p0", "pos": [0,0] } ],
      "objectives": [ { "id": "o", "pos": [10,0] } ]
    })JSON";

    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(json, def).ok);
    REQUIRE(def.vessels.size() == 1);
    REQUIRE(def.vessels[0].lane_id == "main");
    REQUIRE(def.vessels[0].type == VesselType::Artery);
    REQUIRE(def.portals[0].lane_id.empty()); // unset, not inferred at parse time
}

TEST_CASE("the three pre-existing content levels still load, validate, and instantiate",
          "[level][lanes][regression]") {
    for (const char* name : {"capillary_test.json", "chokepoint_pinch.json", "floodplain_mucosal.json"}) {
        INFO("level = " << name);
        const std::string path = platform::asset_path(std::string("levels/") + name);
        REQUIRE(platform::file_exists(path));

        LevelLoader loader;
        LevelDef def;
        REQUIRE(loader.load_file(path, def).ok);
        REQUIRE(loader.validate(def).ok);

        // Every vessel picked up the "lane_id defaults to id, type defaults to
        // Artery" fallback -- none of these three files author either field.
        for (const Vessel& v : def.vessels) {
            REQUIRE(v.lane_id == v.id);
            REQUIRE(v.type == VesselType::Artery);
        }

        // placement_zone_tags stays index-aligned with placement_zones and
        // picks up PlacementZoneTag's defaults (none of these files author
        // "concentrated"/"priority" either).
        REQUIRE(def.placement_zone_tags.size() == def.placement_zones.size());
        for (const PlacementZoneTag& tag : def.placement_zone_tags) {
            REQUIRE_FALSE(tag.concentrated);
            REQUIRE(tag.priority == 1.0f);
        }

        sim::SimWorld world = make_world(def);
        const LevelLoadResult res = loader.instantiate(def, world);
        REQUIRE(res.ok);
        for (const SpawnPortal& p : def.portals) {
            REQUIRE(world.flow().reachable(p.position));
        }

        // A single-vessel-type level is still exactly one lane once resolved.
        const LaneOwnershipMap map = loader.build_lane_ownership_map(def);
        REQUIRE(map.lane_ids.size() == def.vessels.size());
    }
}

// ---- New: lane_schema_test.json multi-lane fixture -------------------------

TEST_CASE("lane_schema_test.json loads with three distinctly-typed lanes and matching portals",
          "[level][lanes][fixture]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/lane_schema_test.json");
    REQUIRE(platform::file_exists(path));
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);

    REQUIRE(def.vessels.size() == 3);
    REQUIRE(def.vessels[0].lane_id == "artery_main");
    REQUIRE(def.vessels[0].type == VesselType::Artery);
    REQUIRE(def.vessels[1].lane_id == "lymph_main");
    REQUIRE(def.vessels[1].type == VesselType::Lymphatic);
    REQUIRE(def.vessels[2].lane_id == "nerve_main");
    REQUIRE(def.vessels[2].type == VesselType::NerveAdjacent);

    REQUIRE(def.portals.size() == 3);
    REQUIRE(loader.resolve_portal_lane_id(def, def.portals[0]) == "artery_main");
    REQUIRE(loader.resolve_portal_lane_id(def, def.portals[1]) == "lymph_main");
    REQUIRE(loader.resolve_portal_lane_id(def, def.portals[2]) == "nerve_main");

    // One shared objective every lane converges on.
    REQUIRE(def.objectives.size() == 1);

    // The authored placement zones round-trip their concentrated/priority tags.
    REQUIRE(def.placement_zones.size() == 2);
    REQUIRE(def.placement_zone_tags.size() == 2);
    REQUIRE(def.placement_zone_tags[0].concentrated);
    REQUIRE(def.placement_zone_tags[0].priority == 2.0f);
    REQUIRE_FALSE(def.placement_zone_tags[1].concentrated);
    REQUIRE(def.placement_zone_tags[1].priority == 1.0f);
}

TEST_CASE("resolve_portal_lane_id falls back to nearest-vessel inference when a portal "
          "omits lane_id",
          "[level][lanes]") {
    LevelDef def;
    def.schema = 1;

    Vessel a;
    a.id = "a";
    a.lane_id = "lane_a";
    a.points = {VesselPoint{Vec2{0.0f, 0.0f}, 4.0f}, VesselPoint{Vec2{10.0f, 0.0f}, 4.0f}};
    Vessel b;
    b.id = "b";
    b.lane_id = "lane_b";
    b.points = {VesselPoint{Vec2{0.0f, 100.0f}, 4.0f}, VesselPoint{Vec2{10.0f, 100.0f}, 4.0f}};
    def.vessels = {a, b};

    SpawnPortal near_a;
    near_a.id = "p_near_a";
    near_a.position = Vec2{1.0f, 1.0f}; // close to a's start, far from b's
    // lane_id left empty on purpose.

    SpawnPortal near_b;
    near_b.id = "p_near_b";
    near_b.position = Vec2{1.0f, 99.0f}; // close to b's start

    LevelLoader loader;
    REQUIRE(loader.resolve_portal_lane_id(def, near_a) == "lane_a");
    REQUIRE(loader.resolve_portal_lane_id(def, near_b) == "lane_b");
}

TEST_CASE("instantiate on lane_schema_test.json bakes a flow field reachable from all three portals",
          "[level][lanes][fixture][instantiate]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/lane_schema_test.json");
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);

    for (const SpawnPortal& p : def.portals) {
        INFO("portal = " << p.id);
        const IVec2 pc = world.tissue().world_to_cell(p.position);
        REQUIRE(world.tissue().walkable(pc.x, pc.y));
        REQUIRE(world.flow().reachable(p.position));
    }

    const IVec2 goal_cell = world.tissue().world_to_cell(def.objectives[0].position);
    REQUIRE(world.tissue().walkable(goal_cell.x, goal_cell.y));
}

// ---- LaneOwnershipMap: lane identity surviving rasterization ---------------

TEST_CASE("build_lane_ownership_map attributes known points on lane_schema_test.json "
          "to the correct lane, well away from the shared convergence point",
          "[level][lanes][ownership]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/lane_schema_test.json");
    REQUIRE(loader.load_file(path, def).ok);

    const LaneOwnershipMap map = loader.build_lane_ownership_map(def);

    REQUIRE(map.lane_ids.size() == 3);
    REQUIRE(map.lane_ids[0] == "artery_main");
    REQUIRE(map.lane_types[0] == VesselType::Artery);
    REQUIRE(map.lane_ids[1] == "lymph_main");
    REQUIRE(map.lane_types[1] == VesselType::Lymphatic);
    REQUIRE(map.lane_ids[2] == "nerve_main");
    REQUIRE(map.lane_types[2] == VesselType::NerveAdjacent);

    // Grid geometry matches the level's world/cell_size, independent of any
    // live TissueMask. Derived from the level rather than hardcoded: these were
    // literal cell counts (320x280) until a pass widened every lane and grew
    // the world to fit, at which point the numbers were wrong and said nothing
    // about whether the mapping worked.
    const Vec2 world_size = def.world_bounds.size();
    REQUIRE(map.cell_size == def.cell_size);
    REQUIRE(map.width == static_cast<i32>(world_size.x / def.cell_size));
    REQUIRE(map.height == static_cast<i32>(world_size.y / def.cell_size));

    // Each portal must be attributed to the lane it is authored on. Probing at
    // the portal's real position (not a copied literal) keeps this meaningful
    // when the level geometry moves.
    for (const SpawnPortal& portal : def.portals) {
        const i32 owner = map.lane_at(portal.position);
        INFO("portal " << portal.id << " lane_id=" << portal.lane_id);
        REQUIRE(owner >= 0);
        REQUIRE(map.lane_ids[static_cast<usize>(owner)] == portal.lane_id);
    }

    // A mid-lane point on the lymph lane, taken from its own control points and
    // nudged back along the lane so it stays clear of the shared convergence
    // point where all three lumens meet.
    const Vessel* lymph = nullptr;
    for (const Vessel& v : def.vessels) {
        if (v.lane_id == "lymph_main") lymph = &v;
    }
    REQUIRE(lymph != nullptr);
    REQUIRE(lymph->points.size() >= 2);
    const Vec2 mid = lymph->points[lymph->points.size() - 2].position;
    REQUIRE(map.lane_at(mid) == 1);

    // A point nowhere near any lane is unowned.
    REQUIRE(map.lane_at(def.world_bounds.min + Vec2{1.0f, 1.0f}) == -1);

    // The shared convergence point: all three lanes' final control point
    // coincides with the same width, so this is a genuine lumen overlap.
    // build_lane_ownership_map()'s documented first-claim-wins rule means the
    // first lane in authoring order (artery_main, index 0) owns it -- this is
    // the known approximation, asserted explicitly rather than left as an
    // accident.
    REQUIRE(map.lane_at(lymph->points.back().position) == 0);
}

TEST_CASE("build_lane_ownership_map on a single-lane level yields exactly one lane "
          "covering the whole vessel",
          "[level][lanes][ownership][regression]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/capillary_test.json");
    REQUIRE(loader.load_file(path, def).ok);

    const LaneOwnershipMap map = loader.build_lane_ownership_map(def);
    REQUIRE(map.lane_ids.size() == 1);
    REQUIRE(map.lane_ids[0] == "main");

    // The portal and objective positions both fall on that one lane.
    REQUIRE(map.lane_at(def.portals[0].position) == 0);
    REQUIRE(map.lane_at(def.objectives[0].position) == 0);
}
