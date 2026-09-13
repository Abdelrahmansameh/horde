// Tests for WaveDirector's Prep->Spawning->Clearing->Complete state machine.
//
// The director no longer builds tables -- generate() and its region tuning are
// gone, and a table now comes from the level file (Level.h, AUTHORED WAVES) --
// so every case here hands it one and checks how it is scheduled and spawned.
// The shape of the shipped tables themselves is a level-loader concern; see
// tests/test_level_content.cpp.
#include "game/wave/WaveDirector.h"

#include "core/Rng.h"
#include "game/level/Level.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace immune;
using namespace immune::sim;
using namespace immune::game;

namespace {
SimWorld make_world_with_spawn_point() {
    SimWorld world;
    SimDesc desc;
    desc.seed = 999;
    desc.max_chaff = 8192;
    LevelDef level = LevelLoader::default_test_level();
    desc.world_bounds = level.world_bounds;
    world.init(desc, nullptr);
    LevelLoader loader;
    const auto res = loader.instantiate(level, world);
    REQUIRE(res.ok);
    REQUIRE_FALSE(world.spawn_points().empty());
    return world;
}
/// A two-wave table shaped like one a level would author: multiple families,
/// staggered start times, an 8s opening prep window.
std::vector<WaveDef> two_wave_table() {
    std::vector<WaveDef> waves;
    for (u32 i = 0; i < 2; ++i) {
        WaveDef w;
        w.index = i;
        w.name = "test_wave_" + std::to_string(i + 1);
        w.prep_time = (i == 0) ? 8.0f : 6.0f;
        w.atp_reward = 50u + i * 10u;
        SpawnEntry virus;
        virus.family = PathogenFamily::Virus;
        virus.count = 120u + i * 75u;
        virus.duration = 4.0f;
        w.spawns.push_back(virus);
        SpawnEntry bacteria;
        bacteria.family = PathogenFamily::Bacteria;
        bacteria.count = 40u + i * 25u;
        bacteria.start_time = 1.0f;
        bacteria.duration = 3.34f;
        w.spawns.push_back(bacteria);
        waves.push_back(std::move(w));
    }
    return waves;
}
} // namespace

TEST_CASE("a spawn point outside the world bounds still puts a horde on the board",
          "[wave][spawn][bounds]") {
    // Off-map spawning: the lane runs off the left edge and the horde walks in
    // from off-screen. Everything the agents touch -- the tissue mask, the
    // spatial hash, the out-of-bounds retirement test -- has to reach out
    // there, or the burst is retired on the tick it spawns and the wave stalls
    // forever waiting for a horde that never survives a frame.
    LevelDef level = LevelLoader::default_test_level();
    level.vessels[0].points[0].position = Vec2{-30.0f, 72.0f};
    level.spawn_points[0].position = Vec2{-20.0f, 72.0f};

    SimWorld world;
    SimDesc desc;
    desc.seed = 999;
    desc.max_chaff = 8192;
    desc.world_bounds = level.world_bounds;
    desc.sim_bounds = level_sim_bounds(level);
    world.init(desc, nullptr);
    LevelLoader loader;
    REQUIRE(loader.instantiate(level, world).ok);

    // The play rect is untouched -- the camera still frames the level, not the
    // approach corridor.
    REQUIRE(world.desc().world_bounds.min.x == level.world_bounds.min.x);
    REQUIRE(world.chaff_system().world_bounds().min.x < -20.0f);

    WaveDirector waves;
    waves.set_waves(two_wave_table());
    waves.start(world);

    Rng tick_rng; tick_rng.reseed(123);
    const f32 dt = 1.0f / 60.0f;
    bool reached_spawning = false;
    for (int i = 0; i < 600 && !reached_spawning; ++i) {
        waves.tick(world, tick_rng, dt);
        if (waves.status().phase == WavePhase::Spawning) reached_spawning = true;
    }
    REQUIRE(reached_spawning);

    for (int i = 0; i < 60; ++i) waves.tick(world, tick_rng, dt);
    REQUIRE(world.chaff().count() > 0);

    // Spawned off-map, and still alive after the ticks that would have retired
    // them under a play-rect despawn test.
    bool any_off_map = false;
    for (usize i = 0; i < world.chaff().count(); ++i) {
        if (world.chaff().pos_x[i] < level.world_bounds.min.x) any_off_map = true;
    }
    REQUIRE(any_off_map);

    const usize before = world.chaff().count();
    for (int i = 0; i < 60; ++i) world.tick();
    REQUIRE(world.chaff().count() >= before / 2);
}

TEST_CASE("start() with an empty table reports all_waves_complete immediately",
          "[wave][lifecycle]") {
    SimWorld world = make_world_with_spawn_point();
    WaveDirector waves;
    waves.start(world);
    REQUIRE(waves.status().all_waves_complete);
}

TEST_CASE("a wave spawns real chaff during Spawning, then advances through Clearing to the next wave",
          "[wave][lifecycle][spawn]") {
    SimWorld world = make_world_with_spawn_point();
    WaveDirector waves;
    waves.set_waves(two_wave_table());
    waves.start(world);

    REQUIRE(waves.status().phase == WavePhase::Prep);
    REQUIRE(world.chaff().count() == 0);

    Rng tick_rng; tick_rng.reseed(123);
    const f32 dt = 1.0f / 60.0f;

    // Drive through Prep -> Spawning; the first wave's prep_time is 8s.
    bool reached_spawning = false;
    for (int i = 0; i < 600 && !reached_spawning; ++i) { // up to 10s
        waves.tick(world, tick_rng, dt);
        if (waves.status().phase == WavePhase::Spawning) reached_spawning = true;
    }
    REQUIRE(reached_spawning);

    // Drive further; chaff should actually appear in the sim (the real
    // production path: spawn_burst from the resolved spawn point).
    for (int i = 0; i < 120; ++i) waves.tick(world, tick_rng, dt); // 2s of spawning
    REQUIRE(world.chaff().count() > 0);

    // Force the horde gone so Clearing resolves quickly rather than waiting
    // out the 60s stall-guard timeout.
    for (usize i = 0; i < world.chaff().count(); ++i) world.chaff().kill(i);
    world.chaff().compact();
    REQUIRE(world.chaff().count() == 0);

    // Finish draining any remaining spawn ramp, then cross into Clearing and
    // out the other side into wave 2's Prep.
    bool reached_wave_two = false;
    for (int i = 0; i < 3600 && !reached_wave_two; ++i) { // generous: up to 60s
        waves.tick(world, tick_rng, dt);
        // Chaff keeps arriving from the still-active Spawning ramp; keep
        // clearing it so total_density can actually reach zero.
        for (usize a = 0; a < world.chaff().count(); ++a) world.chaff().kill(a);
        world.chaff().compact();
        if (waves.status().wave_index == 1) reached_wave_two = true;
    }
    REQUIRE(reached_wave_two);
    REQUIRE(waves.status().phase == WavePhase::Prep);
}

TEST_CASE("the last wave completing sets all_waves_complete", "[wave][lifecycle]") {
    SimWorld world = make_world_with_spawn_point();
    WaveDirector waves;
    // A single, tiny wave so the whole lifecycle finishes fast.
    WaveDef w;
    w.index = 0;
    w.prep_time = 0.1f;
    SpawnEntry e;
    e.family = PathogenFamily::Virus;
    e.count = 5;
    e.start_time = 0.0f;
    e.duration = 0.1f;
    w.spawns.push_back(e);
    waves.set_waves({w});
    waves.start(world);

    Rng tick_rng; tick_rng.reseed(1);
    const f32 dt = 1.0f / 60.0f;
    bool completed = false;
    for (int i = 0; i < 3600 && !completed; ++i) {
        waves.tick(world, tick_rng, dt);
        for (usize a = 0; a < world.chaff().count(); ++a) world.chaff().kill(a);
        world.chaff().compact();
        if (waves.status().all_waves_complete) completed = true;
    }
    REQUIRE(completed);
    REQUIRE(waves.status().phase == WavePhase::Complete);
}

TEST_CASE("request_early_start() skips the remaining prep countdown", "[wave][lifecycle]") {
    SimWorld world = make_world_with_spawn_point();
    WaveDirector waves;
    WaveDef w;
    w.prep_time = 20.0f;
    SpawnEntry e;
    e.count = 1;
    e.duration = 0.1f;
    w.spawns.push_back(e);
    waves.set_waves({w});
    waves.start(world);
    REQUIRE(waves.status().phase == WavePhase::Prep);

    Rng rng; rng.reseed(1);
    waves.request_early_start();
    waves.tick(world, rng, 1.0f / 60.0f);
    REQUIRE(waves.status().phase == WavePhase::Spawning);
}

TEST_CASE("an unpinned wave entry cycles squads through every authored spawn point",
          "[wave][spawn][squads]") {
    SimWorld world = make_world_with_spawn_point();
    world.set_spawn_points({
        SpawnPointRuntime{"left", Vec2{20.0f, 50.0f}, 0.0f, "main"},
        SpawnPointRuntime{"right", Vec2{100.0f, 50.0f}, 0.0f, "main"},
    });
    SquadPath path;
    path.id = "main_path";
    path.lane_id = "main";
    path.points = {Vec2{0.0f, 50.0f}, Vec2{160.0f, 50.0f}};
    path.rebuild_arc();
    world.squads().set_paths({path});
    SquadTuning tuning = world.squads().tuning();
    tuning.spawn_spacing = 0.0f;
    world.squads().set_tuning(tuning);

    WaveDef w;
    w.prep_time = 0.0f;
    SpawnEntry e;
    e.count = 4;
    e.duration = 0.1f;
    e.squad_size = 1;
    // Empty is intentional: this is the authored "cycle all" mode.
    w.spawns.push_back(e);

    WaveDirector waves;
    waves.set_waves({w});
    waves.start(world);
    Rng rng;
    rng.reseed(3);
    waves.tick(world, rng, 0.01f); // Prep -> Spawning.
    waves.tick(world, rng, 0.1f);  // Release all four one-agent squads.

    REQUIRE(world.chaff().count() == 4);
    REQUIRE(world.chaff().pos_x[0] == 20.0f);
    REQUIRE(world.chaff().pos_x[1] == 100.0f);
    REQUIRE(world.chaff().pos_x[2] == 20.0f);
    REQUIRE(world.chaff().pos_x[3] == 100.0f);
}

TEST_CASE("take_pending_atp_reward() accrues WaveDef::atp_reward when a wave finishes clearing, "
          "and drains to zero",
          "[wave][economy]") {
    SimWorld world = make_world_with_spawn_point();
    WaveDirector waves;
    WaveDef w;
    w.prep_time = 0.1f;
    w.atp_reward = 77;
    SpawnEntry e;
    e.family = PathogenFamily::Virus;
    e.count = 3;
    e.start_time = 0.0f;
    e.duration = 0.1f;
    w.spawns.push_back(e);
    waves.set_waves({w});
    waves.start(world);

    // Nothing accrued before the wave has even started clearing.
    REQUIRE(waves.take_pending_atp_reward() == 0);

    Rng rng; rng.reseed(5);
    const f32 dt = 1.0f / 60.0f;
    bool completed = false;
    for (int i = 0; i < 3600 && !completed; ++i) {
        waves.tick(world, rng, dt);
        for (usize a = 0; a < world.chaff().count(); ++a) world.chaff().kill(a);
        world.chaff().compact();
        if (waves.status().all_waves_complete) completed = true;
    }
    REQUIRE(completed);

    // The reward from the single wave clearing should have accrued exactly
    // once, and draining it resets to zero (not re-earned on the next poll).
    REQUIRE(waves.take_pending_atp_reward() == 77);
    REQUIRE(waves.take_pending_atp_reward() == 0);
}
