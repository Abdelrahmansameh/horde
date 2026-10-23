#include "BalanceTestIsolation.h"
#include "game/balance/BalanceController.h"
#include "game/config/GameConfig.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/meta/MetaProgression.h"
#include "game/session/LevelSession.h"
#include "game/session/RunSetup.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/FileIO.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::game;

namespace {

GameConfig shipped_config() {
    config::ConfigStore store;
    GameConfig config;
    std::string error;
    const bool loaded = load_game_config(store, platform::asset_path("config"), config, error);
    INFO(error);
    REQUIRE(loaded);
    return config;
}

LevelDef setup_level() {
    LevelDef level;
    const auto parsed = LevelLoader{}.load_string(R"JSON({
      "schema":2,"name":"balance_setup_fixture","region":"capillary",
      "world":{"min":[0,0],"max":[120,80],"cell_size":1},
      "economy":{"starting_atp":777,"income_multiplier":2},
      "allowed_towers":["neutrophil"],
      "vessels":[{"id":"main","points":[{"p":[-30,40],"w":30},{"p":[116,40],"w":30}]}],
      "spawn_points":[{"id":"p0","pos":[-16,40],"radius":3,"lane_id":"main"}],
      "objectives":[{"id":"organ","pos":[116,40],"half_extents":[4,15]}],
      "placement_zones":[{"min":[12,25],"max":[108,55]}],
      "waves":[{"name":"w1","prep_time":1,"spawns":[
        {"family":"virus","count":10,"duration":1,"spawn_point_id":"p0"}
      ]}]
    })JSON", level);
    INFO(parsed.error);
    REQUIRE(parsed.ok);
    return level;
}

void buy_with_parents(MetaProgression& meta, TreeNode node, const MetaConfig& config) {
    const TreeNode parent = tree_node(node).parent;
    if (parent != TreeNode::Count && meta.level(parent) == 0) buy_with_parents(meta, parent, config);
    INFO(tree_node(node).key);
    REQUIRE(meta.purchase(node, config) == MetaProgression::PurchaseResult::Ok);
}

struct SetupFixture {
    LevelDef level;
    sim::SimWorld world;
    TowerSystem towers;
    EnemyRoster enemies;
    Economy economy;
    ActiveAbilitySystem abilities;
    WaveDirector waves;

    void initialize(const GameConfig& base, const MetaProgression& meta, bool sandbox = false) {
        if (level.vessels.empty()) level = setup_level();
        apply_enemy_config(enemies, base.enemies);
        const auto desc = make_level_sim_desc(base, enemies, level, 4242);
        world.init(desc, nullptr);
        REQUIRE(LevelLoader{}.instantiate(level, world).ok);
        towers.register_systems(world);
        enemies.register_systems(world);
        abilities.register_systems(world);
        const auto run = make_run_tuning(base, meta, sandbox);
        apply_run_tuning(base, run, level, world, towers, enemies, economy, abilities, sandbox);
        waves.set_waves(level.waves);
        waves.set_auto_start(false);
        waves.start(world);
    }
};

} // namespace

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "shared balance setup preserves loaded tuning and applies purchases once",
          "[balance][setup][meta]") {
    const GameConfig base = shipped_config();
    MetaProgression meta;
    const auto fresh = make_run_tuning(base, meta);
    const u32 neutrophil = static_cast<u32>(TowerType::Neutrophil);
    CHECK(fresh.config.towers.stats[neutrophil].build_cost == base.towers.stats[neutrophil].build_cost);
    CHECK(fresh.config.towers.mechanics[neutrophil].shooter.round_damage == base.towers.mechanics[neutrophil].shooter.round_damage);
    CHECK(fresh.config.economy.starting_atp == base.economy.starting_atp);
    CHECK(fresh.tree.tower_mask == (1u << neutrophil));
    CHECK(fresh.tree.ability_mask == 0);
    CHECK(fresh.tree.immunity.leak_damage == base.sim.globals.objective_damage_per_leak);

    meta.credit(1000000, 1000);
    buy_with_parents(meta, TreeNode::NeutrophilRoundDamage, base.meta);
    buy_with_parents(meta, TreeNode::BoneMarrowReserve, base.meta);
    buy_with_parents(meta, TreeNode::BoneMarrowReserve, base.meta);
    buy_with_parents(meta, TreeNode::EfficientClearance, base.meta);
    buy_with_parents(meta, TreeNode::MacrophageRoot, base.meta);
    buy_with_parents(meta, TreeNode::HistamineUnlock, base.meta);
    const auto upgraded = make_run_tuning(base, meta);
    const auto repeated = make_run_tuning(base, meta);
    CHECK(upgraded.config.towers.mechanics[neutrophil].shooter.reload_seconds < base.towers.mechanics[neutrophil].shooter.reload_seconds);
    CHECK(upgraded.config.economy.starting_atp > base.economy.starting_atp);
    CHECK(upgraded.config.economy.atp_per_density > base.economy.atp_per_density);
    CHECK((upgraded.tree.tower_mask & (1u << static_cast<u32>(TowerType::Macrophage))) != 0);
    CHECK((upgraded.tree.ability_mask & (1u << static_cast<u32>(AbilityId::HistamineFlare))) != 0);
    CHECK(repeated.config.economy.starting_atp == upgraded.config.economy.starting_atp);
    CHECK(repeated.config.towers.mechanics[neutrophil].shooter.round_damage == upgraded.config.towers.mechanics[neutrophil].shooter.round_damage);
    CHECK(repeated.config.towers.mechanics[neutrophil].shooter.reload_seconds == upgraded.config.towers.mechanics[neutrophil].shooter.reload_seconds);
    // The source config remains usable for the next fresh campaign/run.
    const auto fresh_again = make_run_tuning(base, MetaProgression{});
    CHECK(fresh_again.config.economy.starting_atp == base.economy.starting_atp);
    CHECK(fresh_again.config.towers.mechanics[neutrophil].shooter.round_damage == base.towers.mechanics[neutrophil].shooter.round_damage);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "shared balance setup combines authored opening economy with progression and restrictions",
          "[balance][setup][economy][deployment]") {
    const GameConfig base = shipped_config();
    MetaProgression meta;
    meta.credit(1000000, 1000);
    buy_with_parents(meta, TreeNode::BoneMarrowReserve, base.meta);
    buy_with_parents(meta, TreeNode::EfficientClearance, base.meta);
    buy_with_parents(meta, TreeNode::MacrophageRoot, base.meta);
    const auto run = make_run_tuning(base, meta);
    SetupFixture fixture;
    fixture.initialize(base, meta);
    const u32 starting_bonus = run.config.economy.starting_atp - base.economy.starting_atp;
    const u32 opening = fixture.level.economy.starting_atp + starting_bonus;
    REQUIRE(fixture.economy.atp() == opening);
    CHECK(fixture.towers.tower_unlocked(TowerType::Macrophage));
    CHECK_FALSE(fixture.towers.tower_allowed(TowerType::Macrophage));
    REQUIRE(deploy_cells(fixture.towers, fixture.world, fixture.economy,
                         TowerType::Macrophage, Vec2{30.0f, 40.0f}, 1) == 0);
    CHECK(fixture.economy.atp() == opening);
    const u32 cost = run.config.towers.stats[static_cast<u32>(TowerType::Neutrophil)].build_cost;
    REQUIRE(deploy_cells(fixture.towers, fixture.world, fixture.economy,
                         TowerType::Neutrophil, Vec2{30.0f, 40.0f}, 1) == 1);
    CHECK(fixture.economy.atp() == opening - cost);
    CHECK(fixture.economy.snapshot().total_spent == cost);
    const auto& cell_profile = fixture.world.swarmers().profile_of(0);
    CHECK(cell_profile.round_damage == Catch::Approx(run.config.towers.mechanics[static_cast<u32>(TowerType::Neutrophil)].shooter.round_damage));
    CHECK(cell_profile.max_health == Catch::Approx(run.config.towers.mechanics[static_cast<u32>(TowerType::Neutrophil)].swarm.max_health));
    fixture.economy.credit_kills(10.0f);
    const u32 expected_credit = static_cast<u32>(10.0f * run.config.economy.atp_per_density * fixture.level.economy.income_multiplier);
    CHECK(fixture.economy.atp() == opening - cost + expected_credit);

    apply_run_tuning(base, run, fixture.level, fixture.world, fixture.towers,
                     fixture.enemies, fixture.economy, fixture.abilities);
    CHECK(fixture.economy.atp() == opening);
    CHECK(fixture.economy.snapshot().total_spent == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "shared balance sandbox grants unlocks while preserving baseline numbers and level rules",
          "[balance][setup][sandbox]") {
    const GameConfig base = shipped_config();
    MetaProgression meta;
    meta.debug_set_all_skills(true);
    const auto sandbox = make_run_tuning(base, meta, true);
    CHECK(sandbox.config.economy.starting_atp == base.economy.starting_atp);
    CHECK(sandbox.config.towers.mechanics[0].shooter.round_damage == base.towers.mechanics[0].shooter.round_damage);
    SetupFixture fixture;
    fixture.initialize(base, meta, true);
    CHECK(fixture.towers.unlocked_towers() == TowerSystem::kAllTowersMask);
    CHECK(fixture.abilities.unlocked_mask() == ActiveAbilitySystem::kAllAbilitiesMask);
    CHECK(fixture.economy.atp() == fixture.level.economy.starting_atp);
    CHECK_FALSE(fixture.towers.tower_allowed(TowerType::Macrophage));
    CHECK(fixture.world.immunity().leak_damage == base.sim.globals.objective_damage_per_leak);
    CHECK(fixture.world.hostile().tuning().damage_taken_mult == 1.0f);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "shared balance world description carries capacities loaded enemies and authored geometry",
          "[balance][setup][config]") {
    GameConfig base = shipped_config();
    // Distinct sentinel values catch accidental fallback to SimDesc defaults.
    base.sim.capacities.max_chaff = 257;
    base.sim.capacities.max_projectiles = 513;
    base.sim.capacities.max_swarmers = 769;
    base.sim.capacities.max_fluid_particles = 1025;
    base.sim.globals.spatial_cell_size = 3.25f;
    base.sim.globals.flow_wall_cost = 0.75f;
    EnemyRoster enemies;
    apply_enemy_config(enemies, base.enemies);
    LevelDef level = setup_level();
    level.ambient_drift = Vec2{1.25f, -0.5f};
    const auto desc = make_level_sim_desc(base, enemies, level, 654321);
    CHECK(desc.seed == 654321);
    CHECK(desc.max_chaff == 257);
    CHECK(desc.max_projectiles == 513);
    CHECK(desc.max_swarmers == 769);
    CHECK(desc.max_fluid_particles == 1025);
    CHECK(desc.max_damage_fields == base.sim.capacities.max_damage_fields);
    CHECK(desc.max_combat_events == base.sim.capacities.max_combat_events);
    CHECK(desc.max_chaff_death_events == base.sim.capacities.max_chaff_death_events);
    CHECK(desc.world_bounds.min == level.world_bounds.min);
    CHECK(desc.world_bounds.max == level.world_bounds.max);
    CHECK(desc.sim_bounds.min.x < desc.world_bounds.min.x);
    CHECK(desc.sim_bounds.contains(level.spawn_points.front().position));
    CHECK(desc.spatial_cell_size == base.sim.globals.spatial_cell_size);
    CHECK(desc.flow_wall_cost == base.sim.globals.flow_wall_cost);
    CHECK(desc.flow_rebake_budget_ms == base.sim.globals.flow_rebake_budget_ms);
    CHECK(desc.chaff_tuning.ambient_drift == level.ambient_drift);
    CHECK(desc.fluid_tuning.substeps == base.sim.fluid.substeps);
    sim::ChaffTuning expected;
    enemies.apply_to_tuning(expected);
    for (u32 f = 0; f < kFamilyCount; ++f) {
        CHECK(desc.chaff_tuning.family[f].max_speed == expected.family[f].max_speed);
        CHECK(desc.chaff_tuning.family[f].base_density == expected.family[f].base_density);
        CHECK(desc.chaff_tuning.family[f].replication_rate == expected.family[f].replication_rate);
    }
    level.ambient_drift = Vec2{0.0f, 0.0f};
    CHECK(make_level_sim_desc(base, enemies, level, 1).chaff_tuning.ambient_drift == base.sim.globals.ambient_drift);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "shared balance run result rewards damage kills rather than leaks or out of bounds removals",
          "[balance][setup][accounting]") {
    GameConfig base = shipped_config();
    base.sim.globals.max_replications_per_tick = 0;
    SetupFixture fixture;
    fixture.initialize(base, MetaProgression{});
    for (u32 f = 0; f < kFamilyCount; ++f) {
        for (Vec2 position : {Vec2{30.0f, 34.0f + static_cast<f32>(f) * 6.0f},
                              Vec2{116.0f, 40.0f}, Vec2{-200.0f, 40.0f}}) {
            sim::ChaffSpawnParams spawn;
            spawn.position = position;
            spawn.family = static_cast<PathogenFamily>(f);
            REQUIRE(fixture.world.chaff().spawn(spawn).valid());
        }
    }
    sim::DamageField field;
    field.origin = Vec2{30.0f, 40.0f};
    field.radius = 12.0f;
    field.kill_rate = 6000.0f;
    fixture.world.damage().submit(field);
    fixture.world.tick();
    const auto snapshot = fixture.world.snapshot();
    const auto result = collect_run_result(fixture.world, fixture.waves, false);
    CHECK(snapshot.chaff_killed_total == 9);
    CHECK(result.chaff_killed_total == 3);
    CHECK_FALSE(result.won);
    CHECK(result.waves_cleared == 0);
    for (u32 f = 0; f < kFamilyCount; ++f) {
        CHECK(snapshot.chaff_killed_by_family[f] == 1);
        CHECK(snapshot.chaff_leaked_by_family[f] == 1);
        CHECK(snapshot.chaff_despawned_by_family[f] == 1);
    }
    CHECK(collect_run_result(fixture.world, fixture.waves, true).won);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "shipped campaign setup yields a legal paid initial controller intent",
          "[balance][setup][campaign]") {
    const GameConfig base = shipped_config();
    for (const char* filename : {"levels/campaign_01_first_bend.json",
                                 "levels/campaign_04_twin_channels.json",
                                 "levels/campaign_09_funnel_walls.json"}) {
        CAPTURE(filename);
        SetupFixture fixture;
        const auto loaded = LevelLoader{}.load_file(platform::asset_path(filename), fixture.level);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
        fixture.initialize(base, MetaProgression{});
        balance::BalanceController controller;
        const auto actions = controller.decide({fixture.world, fixture.level, fixture.towers,
                                                fixture.economy, fixture.waves, fixture.abilities});
        bool purchased = false;
        for (const auto& action : actions) {
            if (action.kind != balance::ActionKind::Deploy) continue;
            CHECK(action.tower == TowerType::Neutrophil);
            CHECK(action.quantity == 1);
            const u32 initial = fixture.economy.atp();
            REQUIRE(fixture.towers.validate_deploy(fixture.world, action.tower,
                                                   action.position, initial).valid());
            REQUIRE(deploy_cells(fixture.towers, fixture.world, fixture.economy,
                                 action.tower, action.position, action.quantity) == 1);
            CHECK(fixture.economy.atp() == initial - fixture.towers.stats(action.tower).build_cost);
            purchased = true;
        }
        CHECK(purchased);
    }
}
