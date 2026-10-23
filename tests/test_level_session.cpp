#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/session/LevelSession.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::game;

namespace {

struct SessionFixture {
    LevelDef level;
    sim::SimWorld world;
    TowerSystem towers;
    Economy economy;
    WaveDirector waves;

    void init(u64 seed = 12345) {
        const auto loaded = LevelLoader{}.load_string(R"JSON({
          "schema":2,"name":"session_fixture","region":"capillary",
          "world":{"min":[0,0],"max":[120,80],"cell_size":1},
          "economy":{"starting_atp":777,"income_multiplier":2},
          "allowed_towers":["neutrophil"],
          "vessels":[{"id":"main","points":[{"p":[4,40],"w":30},{"p":[116,40],"w":30}]}],
          "spawn_points":[{"id":"p0","pos":[4,40],"radius":3,"lane_id":"main"}],
          "objectives":[{"id":"organ","pos":[116,40],"half_extents":[4,15]}],
          "placement_zones":[{"min":[12,25],"max":[108,55]}],
          "waves":[{"name":"w1","prep_time":0.2,"spawns":[
            {"family":"virus","count":10,"duration":1,"spawn_point_id":"p0"},
            {"family":"bacteria","count":10,"duration":1,"spawn_point_id":"p0"},
            {"family":"parasite","count":10,"duration":1,"spawn_point_id":"p0"}
          ]}]
        })JSON", level);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
        sim::SimDesc desc;
        desc.seed = seed;
        desc.max_chaff = 128;
        desc.world_bounds = level.world_bounds;
        desc.sim_bounds = level_sim_bounds(level);
        world.init(desc, nullptr);
        const auto instantiated = LevelLoader{}.instantiate(level, world);
        INFO(instantiated.error);
        REQUIRE(instantiated.ok);
        towers.register_systems(world);
        apply_level_rules(level, EconomyConfig{}, economy, towers);
    }

    LevelSystems systems(bool with_waves = false) {
        LevelSystems result;
        result.world = &world;
        result.towers = &towers;
        result.economy = &economy;
        if (with_waves) result.waves = &waves;
        return result;
    }
};

} // namespace

TEST_CASE("authored level economy scales kill income without compounding",
          "[level][session][economy]") {
    SessionFixture fixture;
    fixture.init();
    EconomyConfig base;
    base.starting_atp = 340; // Includes a 40 ATP progression bonus.
    base.atp_per_density = 1.0f;

    apply_level_rules(fixture.level, base, fixture.economy, fixture.towers, 40);
    REQUIRE(fixture.economy.atp() == 817);
    fixture.economy.credit_kills(4.0f);
    REQUIRE(fixture.economy.atp() == 825);

    // Reapplying a level uses the supplied base, rather than multiplying the
    // previously applied income or adding the progression bonus again.
    apply_level_rules(fixture.level, base, fixture.economy, fixture.towers, 40);
    REQUIRE(fixture.economy.atp() == 817);
    fixture.economy.credit_kills(4.0f);
    REQUIRE(fixture.economy.atp() == 825);

    fixture.level.economy = LevelEconomy{};
    fixture.level.allowed_towers.clear();
    apply_level_rules(fixture.level, base, fixture.economy, fixture.towers, 40);
    REQUIRE(fixture.economy.atp() == 340);
    fixture.economy.credit_kills(4.0f);
    REQUIRE(fixture.economy.atp() == 344);
    REQUIRE(fixture.towers.allowed_towers() == 0);
    REQUIRE(fixture.towers.tower_allowed(TowerType::Macrophage));
}

TEST_CASE("time passing without kills never earns ATP", "[level][session][economy]") {
    SessionFixture fixture;
    fixture.init();
    bool with_waves = false;
    SECTION("without a wave director") {}
    SECTION("during Prep") {
        with_waves = true;
        fixture.waves.set_waves(fixture.level.waves);
        fixture.waves.set_auto_start(false);
        fixture.waves.start(fixture.world);
    }
    SECTION("during an active wave") {
        with_waves = true;
        fixture.waves.set_waves(fixture.level.waves);
        fixture.waves.start(fixture.world);
        fixture.waves.request_early_start();
    }
    const auto before = fixture.economy.snapshot();
    for (u32 tick = 0; tick < 180; ++tick) step_level(fixture.systems(with_waves));
    REQUIRE(fixture.world.tick_index() == 180);
    REQUIRE(fixture.world.last_damage_stats().density_removed == 0.0f);
    REQUIRE(fixture.economy.atp() == before.atp);
    REQUIRE(fixture.economy.snapshot().total_earned == before.total_earned);
}

TEST_CASE("kill ATP retains fractional rewards and resets their carry", "[economy]") {
    EconomyConfig config;
    config.starting_atp = 0;
    Economy economy;
    economy.configure(config);
    for (u32 i = 0; i < 3; ++i) economy.credit_kills(0.25f);
    REQUIRE(economy.atp() == 0);
    economy.credit_kills(0.25f);
    REQUIRE(economy.atp() == 1);
    REQUIRE(economy.snapshot().total_earned == 1);
    economy.credit_kills(0.0f);
    economy.credit_kills(-1.0f);
    REQUIRE(economy.atp() == 1);
    economy.credit_kills(0.75f);
    economy.reset();
    economy.credit_kills(0.25f);
    REQUIRE(economy.atp() == 0);
    REQUIRE(economy.snapshot().total_earned == 0);
}

TEST_CASE("authored cell restrictions govern paid direct deployment",
          "[level][session][deployment][economy]") {
    SessionFixture fixture;
    fixture.init();
    const Vec2 position{30.0f, 40.0f};
    const u32 initial = fixture.economy.atp();
    REQUIRE(initial == 777);
    REQUIRE(fixture.towers.validate_deploy(fixture.world, TowerType::Macrophage,
                                         position, initial).result ==
            PlacementResult::TowerNotAllowed);
    REQUIRE(deploy_cells(fixture.towers, fixture.world, fixture.economy,
                         TowerType::Macrophage, position, 1) == 0);
    REQUIRE(fixture.world.swarmers().count() == 0);
    REQUIRE(fixture.economy.atp() == initial);

    const u32 cost = fixture.towers.stats(TowerType::Neutrophil).build_cost;
    REQUIRE(cost > 0);
    REQUIRE(cost <= initial);
    REQUIRE(deploy_cells(fixture.towers, fixture.world, fixture.economy,
                         TowerType::Neutrophil, position, 1) == 1);
    REQUIRE(fixture.world.swarmers().count() == 1);
    REQUIRE(fixture.economy.atp() == initial - cost);
    REQUIRE(fixture.economy.snapshot().total_spent == cost);
}

TEST_CASE("per-family counters reconcile damage deaths leaks bounds and survivors",
          "[sim][session][accounting]") {
    SessionFixture fixture;
    fixture.init();
    const u32 initial_atp = fixture.economy.atp();
    for (u32 family = 0; family < kFamilyCount; ++family) {
        const f32 y = 34.0f + 6.0f * static_cast<f32>(family);
        for (const Vec2 position : {Vec2{30.0f, y}, Vec2{116.0f, y},
                                    Vec2{-100.0f, y}, Vec2{70.0f, y}}) {
            sim::ChaffSpawnParams spawn;
            spawn.family = static_cast<PathogenFamily>(family);
            spawn.position = position;
            REQUIRE(fixture.world.chaff().spawn(spawn).valid());
        }
    }
    sim::DamageField field;
    field.origin = Vec2{30.0f, 40.0f};
    field.radius = 10.0f;
    field.kill_rate = 600.0f;
    fixture.world.damage().submit(field);
    REQUIRE(step_level(fixture.systems()) == SessionOutcome::InProgress);
    REQUIRE(fixture.world.last_damage_stats().density_removed == Catch::Approx(3.0f));
    // Only the three damage kills pay: 1 ATP per density, doubled by this
    // level's income multiplier. Leaks and out-of-bounds despawns pay nothing.
    REQUIRE(fixture.economy.atp() == initial_atp + 6);

    const auto snapshot = fixture.world.snapshot();
    u64 retired = 0;
    for (u32 family = 0; family < kFamilyCount; ++family) {
        INFO("family " << family);
        REQUIRE(snapshot.chaff_spawned_by_family[family] == 4);
        CHECK(snapshot.chaff_killed_by_family[family] == 1);
        CHECK(snapshot.chaff_leaked_by_family[family] == 1);
        CHECK(snapshot.chaff_despawned_by_family[family] == 1);
        CHECK(snapshot.chaff_by_family[family] == 1);
        const u64 removals = snapshot.chaff_killed_by_family[family] +
                             snapshot.chaff_leaked_by_family[family] +
                             snapshot.chaff_despawned_by_family[family];
        CHECK(removals + snapshot.chaff_by_family[family] ==
              snapshot.chaff_spawned_by_family[family]);
        retired += removals;
    }
    CHECK(retired == snapshot.chaff_killed_total);
}

TEST_CASE("a serial session repeats waves deployment and economy from the same seed",
          "[level][session][determinism]") {
    SessionFixture first, second;
    first.init(4242);
    second.init(4242);
    first.waves.set_waves(first.level.waves);
    second.waves.set_waves(second.level.waves);
    first.waves.start(first.world);
    second.waves.start(second.world);

    for (u32 tick = 0; tick < 360; ++tick) {
        INFO("tick " << tick);
        if (tick == 10 || tick == 70) {
            REQUIRE(deploy_cells(first.towers, first.world, first.economy,
                                 TowerType::Neutrophil, Vec2{30.0f, 40.0f}, 1) == 1);
            REQUIRE(deploy_cells(second.towers, second.world, second.economy,
                                 TowerType::Neutrophil, Vec2{30.0f, 40.0f}, 1) == 1);
        }
        REQUIRE(step_level(first.systems(true)) == step_level(second.systems(true)));
        REQUIRE(first.world.state_hash() == second.world.state_hash());
        REQUIRE(first.world.snapshot().objective_integrity ==
                second.world.snapshot().objective_integrity);
        REQUIRE(first.economy.atp() == second.economy.atp());
        REQUIRE(first.economy.snapshot().total_earned == second.economy.snapshot().total_earned);
        REQUIRE(first.economy.snapshot().total_spent == second.economy.snapshot().total_spent);
        REQUIRE(first.waves.status().wave_index == second.waves.status().wave_index);
        REQUIRE(first.waves.status().phase == second.waves.status().phase);
        REQUIRE(first.waves.status().remaining_to_spawn == second.waves.status().remaining_to_spawn);
    }
    REQUIRE(first.economy.snapshot().total_spent > 0);
    REQUIRE(first.economy.snapshot().total_earned > 777);
    const auto snapshot = first.world.snapshot();
    for (u32 family = 0; family < kFamilyCount; ++family)
        REQUIRE(snapshot.chaff_spawned_by_family[family] == 10);
}
