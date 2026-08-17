// Tests for the optional authored-wave block in the level schema (Level.h) and
// for assets/levels/floodplain_max_horde.json, the peak-agent-count level that
// is the reason the block exists.
//
// The concurrency check below drives the real WaveDirector but deliberately
// does NOT call SimWorld::tick(): the question is "does this table put N agents
// alive at once", which is spawn accounting, and ticking 10k agents for 20
// simulated seconds would turn a unit test into a benchmark. Chaff only leaves
// the buffer via a tick (goal despawn / kills), so with no ticks the live count
// is exactly the number spawned so far -- which is what makes the peak readable
// here at all.
#include "game/level/Level.h"
#include "game/wave/WaveDirector.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace immune;
using namespace immune::game;

namespace {

/// Matches App::load_level's world sizing: default SimDesc apart from bounds,
/// so max_chaff here is the same cap the shipping game runs with.
sim::SimWorld make_world(const LevelDef& def) {
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = def.world_bounds;
    world.init(desc, nullptr);
    return world;
}

u32 wave_total(const WaveDef& w) {
    u32 total = 0;
    for (const SpawnEntry& e : w.spawns) total += e.count;
    return total;
}

const char* kAuthoredWaveLevel = R"JSON({
  "schema": 1,
  "name": "authored_wave_test",
  "region": "capillary",
  "world": { "min": [0, 0], "max": [64, 32], "cell_size": 1.0 },
  "vessels": [
    { "id": "main", "points": [ { "p": [4, 16], "w": 6.0 }, { "p": [60, 16], "w": 6.0 } ] }
  ],
  "portals": [ { "id": "p0", "pos": [4, 16], "radius": 3.0 } ],
  "objectives": [ { "id": "organ", "pos": [60, 16], "radius": 2.5, "integrity": 40 } ],
  "waves": [
    { "name": "w1", "prep_time": 3.0, "atp_reward": 25, "spawns": [
        { "family": "virus", "count": 40, "start_time": 0.0, "duration": 2.0, "portal_id": "p0" } ] },
    { "name": "w2", "prep_time": 1.5, "modifier": "swarm", "spawns": [
        { "family": "fungal_spore", "count": 12, "start_time": 0.5, "duration": 1.0 } ] }
  ]
})JSON";

} // namespace

TEST_CASE("a level may author its own wave table", "[level][waves]") {
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_string(kAuthoredWaveLevel, def).ok);
    REQUIRE(loader.validate(def).ok);

    REQUIRE(def.waves.size() == 2);
    // index is assigned from array position, never read from the JSON.
    REQUIRE(def.waves[0].index == 0);
    REQUIRE(def.waves[1].index == 1);
    REQUIRE(def.waves[0].name == "w1");
    REQUIRE(def.waves[0].prep_time == 3.0f);
    REQUIRE(def.waves[0].atp_reward == 25);
    REQUIRE(def.waves[0].modifier == WaveModifier::None);
    REQUIRE(def.waves[0].spawns.size() == 1);
    REQUIRE(def.waves[0].spawns[0].family == PathogenFamily::Virus);
    REQUIRE(def.waves[0].spawns[0].count == 40);
    REQUIRE(def.waves[0].spawns[0].portal_id == "p0");
    REQUIRE(def.waves[1].modifier == WaveModifier::Swarm);
    REQUIRE(def.waves[1].spawns[0].family == PathogenFamily::FungalSpore);
    // Omitted optional fields fall back to WaveDef/SpawnEntry's own defaults.
    REQUIRE(def.waves[1].atp_reward == 0);
    REQUIRE(def.waves[1].spawns[0].portal_id.empty());
}

TEST_CASE("a level with no 'waves' block parses to an empty table (generate() fallback)",
          "[level][waves]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/capillary_test.json");
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(def.waves.empty());
}

TEST_CASE("authored waves reject a bad family, a zero duration, and an unknown portal",
          "[level][waves]") {
    LevelLoader loader;
    LevelDef def;

    std::string bad_family = kAuthoredWaveLevel;
    bad_family.replace(bad_family.find("\"virus\""), 7, "\"virsu\"");
    REQUIRE_FALSE(loader.load_string(bad_family, def).ok);

    std::string bad_duration = kAuthoredWaveLevel;
    bad_duration.replace(bad_duration.find("\"duration\": 2.0"), 15, "\"duration\": 0.0");
    REQUIRE_FALSE(loader.load_string(bad_duration, def).ok);

    // Parses fine (the portal name is a valid string); validate() is what
    // catches that no such portal exists.
    std::string bad_portal = kAuthoredWaveLevel;
    bad_portal.replace(bad_portal.find("\"portal_id\": \"p0\""), 17, "\"portal_id\": \"p9\"");
    REQUIRE(loader.load_string(bad_portal, def).ok);
    REQUIRE_FALSE(loader.validate(def).ok);
}

TEST_CASE("floodplain_max_horde.json loads, validates, and is one very wide lane",
          "[level][content][max_horde]") {
    LevelLoader loader;
    LevelDef def;
    const std::string path = platform::asset_path("levels/floodplain_max_horde.json");
    REQUIRE(platform::file_exists(path));
    REQUIRE(loader.load_file(path, def).ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.name == "floodplain_max_horde");
    REQUIRE(def.region == "mucosal");
    REQUIRE(def.vessels.size() == 1);
    REQUIRE(def.portals.size() == 1);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);
    REQUIRE(world.flow().reachable(def.portals[0].position));

    // "Wide" is the whole point of the level, so pin it: ~21.8 world units off
    // the centerline is walkable (this trunk peaks at ~55 wide), ~30.6 is not.
    // For scale, floodplain_mucosal -- already the widest shipping level --
    // peaks at ~30 wide itself.
    const IVec2 wide = world.tissue().world_to_cell(Vec2{76.8f, 50.62f + 21.82f});
    const IVec2 outside = world.tissue().world_to_cell(Vec2{76.8f, 50.62f + 30.55f});
    REQUIRE(world.tissue().walkable(wide.x, wide.y));
    REQUIRE_FALSE(world.tissue().walkable(outside.x, outside.y));
}

TEST_CASE("floodplain_max_horde's final wave puts ~10,000 agents on the field at once",
          "[level][content][max_horde][waves]") {
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_file(platform::asset_path("levels/floodplain_max_horde.json"), def).ok);
    REQUIRE(loader.validate(def).ok);
    REQUIRE(def.waves.size() == 8);

    sim::SimWorld world = make_world(def);
    REQUIRE(loader.instantiate(def, world).ok);

    const WaveDef& final_wave = def.waves.back();
    const u32 authored_peak = wave_total(final_wave);
    REQUIRE(authored_peak == 10000);
    // Every wave must fit the chaff buffer with room to spare -- a table that
    // asks for more than max_chaff doesn't spawn more agents, it just silently
    // drops the overflow (WaveDirector::tick counts the shortfall as done).
    REQUIRE(authored_peak < world.desc().max_chaff);
    for (const WaveDef& w : def.waves) REQUIRE(wave_total(w) <= authored_peak);

    // Drive the real director through the final wave alone.
    WaveDirector director;
    director.set_waves({final_wave});
    director.start(world);
    director.request_early_start();

    u64 peak_live = 0;
    // Prep + the longest spawn window, with slack; see the file comment for why
    // no SimWorld::tick() runs here.
    for (u32 i = 0; i < 60 * 20; ++i) {
        director.tick(world, world.rng(), 1.0f / 60.0f);
        peak_live = std::max<u64>(peak_live, world.chaff().count());
        if (director.status().phase != WavePhase::Spawning &&
            director.status().phase != WavePhase::Prep) {
            break;
        }
    }

    // spawn_burst can place slightly fewer than asked, and the per-entry ramp
    // truncates, so this is a floor, not equality.
    REQUIRE(peak_live >= static_cast<u64>(authored_peak) * 99 / 100);
    REQUIRE(peak_live <= world.desc().max_chaff);
}
