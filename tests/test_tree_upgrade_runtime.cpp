#include "BalanceTestIsolation.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/config/GameConfig.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/enemies/EnemyRoster.h"
#include "game/level/Level.h"
#include "game/meta/ImmunityTree.h"
#include "game/session/RunSetup.h"
#include "game/towers/TowerSystem.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using namespace immune;
using namespace immune::game;

namespace {
GameConfig shipped() {
    config::ConfigStore store;
    GameConfig result;
    std::string error;
    REQUIRE(load_game_config(store, platform::asset_path("config"), result, error));
    return result;
}

struct RuntimeFixture {
    sim::SimWorld world;
    LevelDef level;
    TowerSystem towers;
    EnemyRoster enemies;
    Economy economy;
    ActiveAbilitySystem abilities;

    RuntimeFixture(GameConfig base, const TreeLevels& levels, bool hostile = false) {
        const auto parsed = LevelLoader{}.load_string(R"JSON({
          "schema":2,"name":"tree_runtime","region":"capillary",
          "world":{"min":[0,0],"max":[120,80],"cell_size":1},
          "economy":{"starting_atp":1000},
          "vessels":[{"id":"main","points":[{"p":[4,40],"w":40},{"p":[116,40],"w":40}]}],
          "objectives":[{"id":"organ","pos":[116,40],"half_extents":[2,15]}],
          "spawn_points":[{"id":"p0","pos":[4,40],"radius":2,"lane_id":"main"}],
          "waves":[{"name":"fixture_only","spawns":[{"family":"virus","count":1,"duration":1,"spawn_point_id":"p0"}]}]
        })JSON", level);
        REQUIRE(parsed.ok);
        base.sim.hostile.enabled = hostile;
        apply_enemy_config(enemies, base.enemies);
        auto desc = make_level_sim_desc(base, enemies, level, 901);
        desc.max_chaff = 256;
        desc.max_swarmers = 16;
        desc.max_projectiles = 512;
        desc.max_fluid_particles = 64;
        desc.deterministic_rebakes = true;
        for (auto& family : desc.chaff_tuning.family) {
            family.max_speed = 0;
            family.jitter = 0;
            family.replication_rate = 0;
        }
        world.init(desc, nullptr);
        REQUIRE(LevelLoader{}.instantiate(level, world).ok);
        towers.register_systems(world);
        enemies.register_systems(world);
        abilities.register_systems(world);
        RunTuning run;
        run.config = base;
        run.tree = apply_immunity_tree(levels, run.config);
        apply_run_tuning(base, run, level, world, towers, enemies, economy, abilities);
        world.enable_combat_metrics();
    }

    void cell(TowerType type, Vec2 position = {45,40}) {
        REQUIRE(deploy_cells(towers, world, economy, type, position, 1) == 1);
    }
    void virus(Vec2 position, f32 density = 0.6f) {
        sim::ChaffSpawnParams spawn;
        spawn.position = position;
        spawn.family = PathogenFamily::Virus;
        spawn.density = density;
        REQUIRE(world.chaff().spawn(spawn).valid());
    }
};

struct Throughput { u64 shots; f64 applied; u64 kills; };
Throughput magazine_throughput(u8 rank, u8 coordinated = 0) {
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = rank;
    levels[TreeNode::SystemicPotency] = coordinated;
    RuntimeFixture fixture(shipped(), levels);
    fixture.cell(TowerType::Neutrophil);
    for (u32 i = 0; i < 80; ++i)
        fixture.virus({58.0f + static_cast<f32>(i % 8) * 0.9f,
                       35.5f + static_cast<f32>(i / 8) * 0.9f});
    for (u32 tick = 0; tick < 720; ++tick) fixture.world.tick();
    const auto& total = fixture.world.combat_metrics().totals();
    REQUIRE(total.pools.projectiles.denied == 0);
    REQUIRE(fixture.economy.snapshot().total_spent == fixture.towers.stats(TowerType::Neutrophil).build_cost);
    return {total.pools.projectiles.spawned, total.damage.density_removed,
            fixture.world.snapshot().chaff_killed_by_family[0]};
}

struct Survival { Tick death; f64 applied; f64 ttk; };
Survival under_pressure(TreeNode node, u8 rank, TowerType type) {
    auto base = shipped();
    auto& mechanics = base.towers.mechanics[static_cast<u32>(type)];
    mechanics.swarm.speed = 0;
    base.towers.stats[static_cast<u32>(type)].family_mask = 0;
    auto& attack = base.enemies.families[0].attack;
    attack.latch_dps = 20;
    attack.latch_cap_swarmer = 1;
    attack.latch_reach = 10;
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = 1;
    levels[TreeNode::BoneMarrowReserve] = 1;
    levels[TreeNode::FieldRequisition] = 1;
    levels[TreeNode::MacrophageRoot] = 1;
    levels[TreeNode::MacrophageHealth] = 1;
    levels[node] = rank;
    RuntimeFixture fixture(base, levels, true);
    fixture.cell(type);
    fixture.virus({46,40});
    while (fixture.world.tick_index() < 1500 && fixture.world.snapshot().swarmers_killed_total == 0)
        fixture.world.tick();
    const auto& metrics = fixture.world.combat_metrics();
    REQUIRE(fixture.world.snapshot().swarmers_killed_total == 1);
    REQUIRE(metrics.friendly_lifetimes().size() == 1);
    const auto& life = metrics.friendly_lifetimes()[0].stats;
    REQUIRE(life.damage_deaths == 1);
    REQUIRE(life.damage_ttk.observations == 1);
    return {fixture.world.tick_index(), metrics.totals().hostile_actual_cell_damage,
            life.damage_ttk.sum_ticks};
}

u64 first_cluster(u8 rank, u8 coordinated = 0, Tick* first_death = nullptr) {
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = 1;
    levels[TreeNode::MacrophageRoot] = 1;
    levels[TreeNode::MacrophageGrabSpeed] = 1;
    levels[TreeNode::MacrophageArms] = rank;
    levels[TreeNode::SystemicPotency] = coordinated;
    RuntimeFixture fixture(shipped(), levels);
    fixture.cell(TowerType::Macrophage);
    for (u32 i = 0; i < 12; ++i)
        fixture.virus({54.0f + static_cast<f32>(i % 4) * 0.2f,
                       39.7f + static_cast<f32>(i / 4) * 0.2f});
    while (fixture.world.tick_index() < 120 && fixture.world.snapshot().chaff_killed_by_family[0] == 0)
        fixture.world.tick();
    const auto kills = fixture.world.snapshot().chaff_killed_by_family[0];
    if (first_death) *first_death = fixture.world.tick_index();
    REQUIRE(kills > 0);
    REQUIRE(fixture.world.combat_metrics().totals().damage.density_removed == Catch::Approx(kills * 0.6));
    REQUIRE(fixture.world.combat_metrics().totals().pools.cells.denied == 0);
    return kills;
}

struct BodyPush { f32 cell_distance; f32 enemy_distance; };
BodyPush membrane_push(u8 rank) {
    auto base = shipped();
    base.towers.mechanics[static_cast<u32>(TowerType::Macrophage)].swarm.speed = 0;
    base.towers.stats[static_cast<u32>(TowerType::Macrophage)].family_mask = 0;
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = 1;
    levels[TreeNode::MacrophageRoot] = 1;
    levels[TreeNode::MacrophageHealth] = 1;
    levels[TreeNode::MacrophageWall] = rank;
    RuntimeFixture fixture(base, levels);
    fixture.cell(TowerType::Macrophage);
    fixture.virus({45.2f,40});
    fixture.world.tick();
    REQUIRE(fixture.world.swarmer_system().last_stats().body_blocks > 0);
    return {std::abs(fixture.world.swarmers().pos_x[0] - 45.0f),
            std::abs(fixture.world.chaff().pos_x[0] - 45.2f)};
}

f64 drain_ttk(u8 coordinated) {
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = 1;
    levels[TreeNode::CytotoxicRoot] = 1;
    levels[TreeNode::SystemicPotency] = coordinated;
    RuntimeFixture fixture(shipped(), levels);
    fixture.cell(TowerType::CytotoxicT);
    sim::ChaffSpawnParams enemy;
    enemy.position = {45.4f,40};
    enemy.family = PathogenFamily::Parasite;
    enemy.density = 1.6f;
    REQUIRE(fixture.world.chaff().spawn(enemy).valid());
    fixture.world.chaff().burrow_timer[0] = 10;
    while (fixture.world.tick_index() < 120 && fixture.world.snapshot().chaff_killed_by_family[2] == 0)
        fixture.world.tick();
    const auto& life = fixture.world.combat_metrics().enemy_lifetimes()[2];
    REQUIRE(life.damage_deaths == 1);
    REQUIRE(life.damage_ttk.observations == 1);
    return life.damage_ttk.sum_ticks;
}

Tick mucus_delivery(u8 coordinated) {
    auto base = shipped();
    // Keep this approach-time observation below the particle pool limit;
    // changing particle count is identical in both diagnostic arms.
    base.towers.mechanics[static_cast<u32>(TowerType::GobletCell)].mucus_bomber.droplets = 20;
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = 1;
    levels[TreeNode::GobletRoot] = 1;
    levels[TreeNode::SystemicPotency] = coordinated;
    RuntimeFixture fixture(base, levels);
    fixture.cell(TowerType::GobletCell);
    fixture.virus({58,40}); // inside the shipped 14-unit acquisition radius
    for (u32 tick = 0; tick < 120; ++tick) {
        fixture.world.tick();
        const auto& owners = fixture.world.combat_metrics().friendly_lifetimes();
        if (!owners.empty() && owners[0].stats.consumed == 1) break;
    }
    const auto& metrics = fixture.world.combat_metrics();
    REQUIRE(metrics.friendly_lifetimes()[0].stats.consumed == 1);
    REQUIRE(metrics.totals().pools.fluid.spawned == 20);
    REQUIRE(metrics.totals().pools.fluid.denied == 0);
    return fixture.world.tick_index();
}

Tick next_builder_work(u8 coordinated) {
    auto base = shipped();
    base.towers.mechanics[static_cast<u32>(TowerType::Fibroblast)].builder.roam_speed_mult = 0;
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = 1;
    levels[TreeNode::FibroblastRoot] = 1;
    levels[TreeNode::SystemicPotency] = coordinated;
    RuntimeFixture fixture(base, levels);
    fixture.cell(TowerType::Fibroblast);
    fixture.world.swarmers().set_goal(0, {45,40});
    fixture.world.tick();
    REQUIRE(fixture.world.snapshot().scars_built_total == 1);
    for (u32 tick = 0; tick < 600; ++tick) {
        fixture.world.tick();
        const bool travelling = (fixture.world.swarmers().flags[0] & sim::swarmer_flags::kHasGoal) != 0;
        if (travelling || fixture.world.swarmer_system().last_stats().built > 0)
            return fixture.world.tick_index();
    }
    FAIL("persistent builder never resumed work after its real scar construction");
    return 0;
}

struct ToxinSurvival { u64 hits; f64 ttk; f64 applied; };
ToxinSurvival specialist_toxin_survival(TowerType type, TreeNode health, u8 rank) {
    auto base = shipped();
    base.towers.mechanics[static_cast<u32>(type)].swarm.speed = 0;
    // Keep the cell in the firing lane rather than killing or consuming its
    // attacker. Both arms use the actual shipped nine-damage toxin payload.
    base.towers.stats[static_cast<u32>(type)].family_mask = 0;
    REQUIRE(base.enemies.families[static_cast<u32>(PathogenFamily::Bacteria)].attack.toxin_damage == 9.0f);
    TreeLevels levels;
    levels[TreeNode::NeutrophilRoundDamage] = 1;
    const auto root = tower_root(type);
    if (root != TreeNode::Count) levels[root] = 1;
    levels[health] = rank;
    RuntimeFixture fixture(base, levels, true);
    fixture.cell(type);
    sim::ChaffSpawnParams enemy;
    enemy.position = {53,40};
    enemy.family = PathogenFamily::Bacteria;
    enemy.density = 0.9f;
    REQUIRE(fixture.world.chaff().spawn(enemy).valid());
    while (fixture.world.tick_index() < 600 && fixture.world.snapshot().swarmers_killed_total == 0)
        fixture.world.tick();
    REQUIRE(fixture.world.snapshot().swarmers_killed_total == 1);
    const auto& metrics = fixture.world.combat_metrics();
    REQUIRE(metrics.friendly_lifetimes().size() == 1);
    const auto& life = metrics.friendly_lifetimes()[0].stats;
    REQUIRE(life.damage_deaths == 1);
    REQUIRE(life.damage_ttk.observations == 1);
    return {metrics.totals().toxin_hits, life.damage_ttk.sum_ticks,
            metrics.totals().hostile_actual_cell_damage};
}
} // namespace

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "each requisition rank strictly lowers every shipped cell's paid placement cost",
                 "[balance][meta][tree][upgrade-runtime][requisition]") {
    const auto base = shipped();
    REQUIRE(tree_node(TreeNode::FieldRequisition).max_level == 3);
    std::array<u32, kTowerTypeCount> previous{};
    for (u8 rank = 0; rank <= 3; ++rank) {
        TreeLevels levels;
        levels[TreeNode::NeutrophilRoundDamage] = 1;
        levels[TreeNode::BoneMarrowReserve] = 1;
        levels[TreeNode::FieldRequisition] = rank;
        levels[TreeNode::CytotoxicRoot] = 1;
        levels[TreeNode::MacrophageRoot] = 1;
        levels[TreeNode::GobletRoot] = 1;
        levels[TreeNode::FibroblastRoot] = 1;
        RuntimeFixture fixture(base, levels);
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            const auto type = static_cast<TowerType>(t);
            const auto cost = fixture.towers.stats(type).build_cost;
            CAPTURE(rank, t, cost, previous[t]);
            if (rank == 0) CHECK(cost == base.towers.stats[t].build_cost);
            else CHECK(cost < previous[t]);
            const auto before = fixture.economy.snapshot().total_spent;
            fixture.cell(type);
            CHECK(fixture.economy.snapshot().total_spent - before == cost);
            previous[t] = cost;
        }
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "each specialist resilience rank survives more shipped toxin hits",
                 "[balance][meta][tree][upgrade-runtime][toxin-health]") {
    for (const auto type : {TowerType::CytotoxicT, TowerType::GobletCell}) {
        const auto health = type == TowerType::CytotoxicT ? TreeNode::CytotoxicHealth : TreeNode::GobletHealth;
        auto previous = specialist_toxin_survival(type, health, 0);
        REQUIRE(previous.hits == 2);
        for (u8 rank = 1; rank <= 4; ++rank) {
            const auto current = specialist_toxin_survival(type, health, rank);
            CAPTURE(static_cast<u32>(type), rank, previous.hits, current.hits, previous.ttk, current.ttk);
            CHECK(current.hits > previous.hits);
            CHECK(current.ttk > previous.ttk);
            CHECK(current.applied == Catch::Approx(10.0 + 10.0 * rank));
            previous = current;
        }
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "each vitality and stamina rank survives another shipped toxin hit with longer actual TTK",
                 "[balance][meta][tree][upgrade-runtime][toxin-health]") {
    for (const auto type : {TowerType::Neutrophil, TowerType::CytotoxicT}) {
        const auto health = type == TowerType::Neutrophil ? TreeNode::NeutrophilVitality : TreeNode::CytotoxicStamina;
        auto previous = specialist_toxin_survival(type, health, 0);
        REQUIRE(previous.hits == 2);
        REQUIRE(previous.applied == Catch::Approx(10.0));
        for (u8 rank = 1; rank <= tree_node(health).max_level; ++rank) {
            const auto current = specialist_toxin_survival(type, health, rank);
            CAPTURE(static_cast<u32>(type), rank, previous.hits, current.hits, previous.ttk, current.ttk);
            CHECK(current.hits == previous.hits + 1);
            CHECK(current.ttk > previous.ttk);
            CHECK(current.applied == Catch::Approx(10.0 + 9.0 * rank));
            previous = current;
        }
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "apoptosis collateral reaches a nearby live enemy after its host dies",
                 "[balance][meta][tree][upgrade-runtime][apoptosis]") {
    for (const bool shipped_contact : {false, true}) {
        for (const bool capstone : {false, true}) {
            TreeLevels levels;
            levels[TreeNode::NeutrophilRoundDamage] = 1;
            levels[TreeNode::CytotoxicRoot] = 1;
            levels[TreeNode::CytotoxicCapstone] = capstone ? 1u : 0u;
            auto base = shipped();
            // Test both a fixed prescribed victim (deferred scheduling) and real
            // shipped body contact, which settles packed viruses farther apart.
            if (!shipped_contact)
                base.enemies.families[static_cast<u32>(PathogenFamily::Virus)].chaff.contact_stiffness = 0;
            RuntimeFixture fixture(base, levels);
            fixture.cell(TowerType::CytotoxicT);
            fixture.virus({45.4f,40});
            fixture.virus({47.0f,40});
            fixture.virus({70.0f,40}); // outside the pulse and current acquisition
            u32 raised = 0;
            while (fixture.world.tick_index() < 120 && fixture.world.snapshot().chaff_killed_by_family[0] == 0) {
                fixture.world.tick();
                raised += fixture.world.swarmer_system().last_stats().kill_pulses;
            }
            REQUIRE(fixture.world.snapshot().chaff_killed_by_family[0] == 1);
            REQUIRE(raised == (capstone ? 1u : 0u));
            // The field is submitted after field damage this tick. It must
            // survive upkeep and actually hit the collateral on the next tick.
            fixture.world.tick();
            CAPTURE(shipped_contact, capstone);
            CHECK(fixture.world.snapshot().chaff_killed_by_family[0] == (capstone ? 2u : 1u));
            f64 burst_applied = 0;
            for (const auto& bucket : fixture.world.combat_metrics().buckets())
                if (bucket.source == sim::CombatDamageSource::Burst) burst_applied += bucket.totals.density_removed;
            CHECK(burst_applied == Catch::Approx(capstone ? 0.6 : 0.0));
            CHECK(fixture.world.combat_metrics().totals().damage.density_removed ==
                  Catch::Approx(fixture.world.snapshot().chaff_killed_by_family[0] * 0.6).margin(0.08));
        }
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "starter replenishment increases real shots and ordinary chaff kills without inflating damage",
                 "[balance][meta][tree][upgrade-runtime]") {
    const auto baseline = magazine_throughput(0);
    const auto first = magazine_throughput(1);
    const auto maximum = magazine_throughput(5);
    CAPTURE(baseline.shots, first.shots, maximum.shots, baseline.applied, first.applied, maximum.applied);
    CHECK(first.shots > baseline.shots);
    // Additional rounds can miss a moving/retargeted firing line. This
    // observation proves rank-one throughput without inventing a guarantee
    // that every finite encounter converts each extra round into a kill.
    CHECK(first.applied >= baseline.applied);
    CHECK(first.kills >= baseline.kills);
    CHECK(maximum.shots > first.shots);
    CHECK(maximum.kills > first.kills);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "reserve and membrane upgrades improve actual friendly damage-death TTK",
                 "[balance][meta][tree][upgrade-runtime]") {
    const auto reserve0 = under_pressure(TreeNode::EliteResponse, 0, TowerType::Neutrophil);
    const auto reserve1 = under_pressure(TreeNode::EliteResponse, 1, TowerType::Neutrophil);
    const auto wall0 = under_pressure(TreeNode::MacrophageWall, 0, TowerType::Macrophage);
    const auto wall3 = under_pressure(TreeNode::MacrophageWall, 3, TowerType::Macrophage);
    CAPTURE(reserve0.death, reserve1.death, wall0.death, wall3.death);
    CHECK(reserve1.death > reserve0.death);
    CHECK(reserve1.ttk > reserve0.ttk);
    CHECK(reserve1.applied == Catch::Approx(reserve0.applied * 1.15).margin(1e-3));
    CHECK(wall3.death > wall0.death);
    CHECK(wall3.ttk > wall0.ttk);
    CHECK(wall3.applied == Catch::Approx(wall0.applied * 1.6).margin(1e-3));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "cluster intake increases actual enemies swallowed by the same fixed arms",
                 "[balance][meta][tree][upgrade-runtime]") {
    const auto baseline = first_cluster(0);
    const auto upgraded = first_cluster(1);
    CAPTURE(baseline, upgraded);
    CHECK(upgraded > baseline);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "defensive membrane transfers more real crowd displacement to the pathogen",
                 "[balance][meta][tree][upgrade-runtime]") {
    const auto baseline = membrane_push(0);
    const auto upgraded = membrane_push(3);
    CAPTURE(baseline.cell_distance, upgraded.cell_distance, baseline.enemy_distance, upgraded.enemy_distance);
    CHECK(upgraded.cell_distance < baseline.cell_distance);
    CHECK(upgraded.enemy_distance > baseline.enemy_distance);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "coordinated response improves actual role throughput and delivery",
                 "[balance][meta][tree][upgrade-runtime]") {
    const auto shooter0 = magazine_throughput(1, 0);
    const auto shooter3 = magazine_throughput(1, 3);
    Tick capture0 = 0, capture3 = 0;
    const auto swallowed0 = first_cluster(0, 0, &capture0);
    const auto swallowed3 = first_cluster(0, 3, &capture3);
    const auto drain0 = drain_ttk(0), drain3 = drain_ttk(3);
    const auto delivery0 = mucus_delivery(0), delivery3 = mucus_delivery(3);
    const auto builder0 = next_builder_work(0), builder3 = next_builder_work(3);
    CAPTURE(shooter0.shots, shooter3.shots, shooter0.kills, shooter3.kills,
            capture0, capture3, drain0, drain3, delivery0, delivery3, builder0, builder3);
    CHECK(shooter3.shots > shooter0.shots);
    CHECK(shooter3.kills > shooter0.kills);
    CHECK(swallowed3 == swallowed0);
    CHECK(capture3 < capture0);
    CHECK(drain3 < drain0);
    CHECK(delivery3 < delivery0);
    CHECK(builder3 < builder0);
}
