// Tests for the real WaveDirector::generate()/tick() implementation (the
// Prep->Spawning->Clearing->Complete state machine written directly, not by a
// sub-agent, as the minimum-to-playable pass).
#include "game/wave/WaveDirector.h"

#include "core/Rng.h"
#include "game/level/Level.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::sim;
using namespace immune::game;

namespace {
SimWorld make_world_with_portal() {
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
    REQUIRE_FALSE(world.portals().empty());
    return world;
}
} // namespace

TEST_CASE("generate() produces the requested wave count with escalating pressure",
          "[wave][generate]") {
    Rng rng;
    rng.reseed(1);
    const auto waves = WaveDirector::generate("capillary", 5, rng);
    REQUIRE(waves.size() == 5);
    for (u32 i = 0; i < waves.size(); ++i) {
        REQUIRE(waves[i].index == i);
        REQUIRE_FALSE(waves[i].spawns.empty());
        u32 total = 0;
        for (const auto& s : waves[i].spawns) total += s.count;
        REQUIRE(total > 0);
        if (i > 0) {
            u32 prev_total = 0;
            for (const auto& s : waves[i - 1].spawns) prev_total += s.count;
            REQUIRE(total >= prev_total); // later waves are never lighter
        }
    }
}

TEST_CASE("generate() is deterministic for a fixed seed", "[wave][generate][determinism]") {
    Rng rng_a; rng_a.reseed(42);
    Rng rng_b; rng_b.reseed(42);
    const auto a = WaveDirector::generate("capillary", 4, rng_a);
    const auto b = WaveDirector::generate("capillary", 4, rng_b);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].spawns.size() == b[i].spawns.size());
        for (usize j = 0; j < a[i].spawns.size(); ++j) {
            REQUIRE(a[i].spawns[j].count == b[i].spawns[j].count);
            REQUIRE(a[i].spawns[j].family == b[i].spawns[j].family);
        }
    }
}

TEST_CASE("start() with an empty table reports all_waves_complete immediately",
          "[wave][lifecycle]") {
    SimWorld world = make_world_with_portal();
    WaveDirector waves;
    waves.start(world);
    REQUIRE(waves.status().all_waves_complete);
}

TEST_CASE("a wave spawns real chaff during Spawning, then advances through Clearing to the next wave",
          "[wave][lifecycle][spawn]") {
    SimWorld world = make_world_with_portal();
    Rng gen_rng; gen_rng.reseed(7);
    WaveDirector waves;
    waves.set_waves(WaveDirector::generate("capillary", 2, gen_rng));
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
    // production path: spawn_burst from the resolved portal).
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
    SimWorld world = make_world_with_portal();
    Rng gen_rng; gen_rng.reseed(3);
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
    SimWorld world = make_world_with_portal();
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

TEST_CASE("take_pending_atp_reward() accrues WaveDef::atp_reward when a wave finishes clearing, "
          "and drains to zero",
          "[wave][economy]") {
    SimWorld world = make_world_with_portal();
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
