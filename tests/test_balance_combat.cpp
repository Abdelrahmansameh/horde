#include "BalanceTestIsolation.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "sim/CombatMetrics.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::sim;

namespace {

void init_world(SimWorld& world, bool metrics) {
    game::LevelDef level;
    const auto loaded = game::LevelLoader{}.load_string(R"JSON({
      "schema":2,"name":"combat_metrics","region":"capillary",
      "world":{"min":[0,0],"max":[120,80],"cell_size":1},
      "vessels":[{"id":"main","points":[{"p":[4,40],"w":40},{"p":[116,40],"w":40}]}],
      "objectives":[{"id":"organ","pos":[116,40],"half_extents":[2,15]}],
      "spawn_points":[{"id":"p0","pos":[4,40],"radius":2,"lane_id":"main"}],
      "waves":[{"name":"unused_fixture_wave","prep_time":1,"spawns":[
        {"family":"virus","count":1,"duration":1,"spawn_point_id":"p0"}
      ]}]
    })JSON", level);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    SimDesc desc;
    desc.seed = 12345;
    desc.max_chaff = 128;
    desc.max_swarmers = 32;
    desc.max_projectiles = 64;
    desc.max_fluid_particles = 64;
    desc.world_bounds = level.world_bounds;
    desc.sim_bounds = game::level_sim_bounds(level);
    desc.deterministic_rebakes = true;
    for (auto& family : desc.chaff_tuning.family) {
        family.max_speed = 0.0f;
        family.jitter = 0.0f;
        family.replication_rate = 0.0f;
    }
    world.init(desc, nullptr);
    const auto instantiated = game::LevelLoader{}.instantiate(level, world);
    INFO(instantiated.error);
    REQUIRE(instantiated.ok);
    if (metrics) world.enable_combat_metrics();
}

void spawn(SimWorld& world, Vec2 position, PathogenFamily family, f32 density) {
    ChaffSpawnParams enemy;
    enemy.position = position;
    enemy.family = family;
    enemy.density = density;
    REQUIRE(world.chaff().spawn(enemy).valid());
}

f64 source_damage(const CombatMetrics& metrics, CombatDamageSource source) {
    f64 result = 0.0;
    for (const auto& bucket : metrics.buckets())
        if (bucket.source == source) result += bucket.totals.density_removed;
    return result;
}

void add_latcher(SimWorld& world, Vec2 position, EntityId owner) {
    SwarmerProfile profile;
    profile.kind = SwarmerKind::Latch;
    profile.source = TowerType::CytotoxicT;
    profile.attach_seconds = 0.0f;
    profile.attach_radius = 4.0f;
    profile.search_radius = 15.0f;
    profile.dps = 6.0f;
    world.swarmers().set_profile(0, profile);
    SwarmerSpawnParams cell;
    cell.profile = 0;
    cell.position = position;
    cell.owner = owner;
    cell.persistent = true;
    REQUIRE(world.swarmers().spawn(cell));
}

} // namespace

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "combat metrics observe clamped damage modifiers and underground immunity",
          "[balance][combat-metrics]") {
    CombatMetrics metrics;
    metrics.enable();
    ChaffBuffers enemies;
    enemies.reserve(4);
    enemies.set_combat_metrics(&metrics);
    enemies.set_slowed_damage_multiplier(2.0f);
    ChaffSpawnParams enemy;
    enemy.density = 2.0f;
    enemy.family = PathogenFamily::Bacteria;
    enemy.flags = chaff_flags::kSlowed;
    REQUIRE(enemies.spawn(enemy).valid());
    const DamageAttribution tag{EntityId{17}, TowerType::Neutrophil, CombatDamageSource::Projectile};
    enemies.apply_density_loss(0, 0.25f, tag);
    enemies.apply_density_loss(0, 1.0f, tag);
    enemy.family = PathogenFamily::Parasite;
    enemy.flags = 0;
    REQUIRE(enemies.spawn(enemy).valid());
    enemies.burrow_state[1] = burrow_state::kUnderground;
    enemies.apply_density_loss(1, 7.0f, tag);

    const auto& total = metrics.totals().damage;
    REQUIRE(total.density_removed == Catch::Approx(2.0));
    REQUIRE(total.attempted_density == Catch::Approx(9.5));
    REQUIRE(total.overkill_density == Catch::Approx(0.5));
    REQUIRE(total.immune_density == Catch::Approx(7.0));
    REQUIRE(total.damage_kills == 1.0);
    REQUIRE(metrics.totals().by_family[static_cast<u32>(PathogenFamily::Bacteria)].density_removed == 2.0);
    REQUIRE(metrics.buckets().size() == 1);
    REQUIRE(metrics.buckets()[0].owner == EntityId{17});
    REQUIRE(metrics.buckets()[0].type == TowerType::Neutrophil);
    REQUIRE(enemies.density[1] == 2.0f);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "combat metrics reconcile every tick without changing a replay",
          "[balance][combat-metrics][determinism]") {
    SimWorld observed;
    SimWorld control;
    init_world(observed, true);
    init_world(control, false);
    for (SimWorld* world : {&observed, &control}) {
        spawn(*world, {35, 40}, PathogenFamily::Virus, 1.0f);
        spawn(*world, {55, 40}, PathogenFamily::Bacteria, 4.0f);
        spawn(*world, {75, 40}, PathogenFamily::Parasite, 5.0f);
        DamageField field;
        field.origin = {35, 40};
        field.radius = 3;
        field.kill_rate = 120;
        field.owner = EntityId{31};
        field.source_type = TowerType::GobletCell;
        world->damage().submit(field);
        ProjectileSpawnParams shot;
        shot.position = {55, 40};
        shot.damage = 2.5f;
        shot.hit_radius = 2;
        shot.owner = EntityId{32};
        REQUIRE(world->projectiles().spawn(shot));
        add_latcher(*world, {75, 40}, EntityId{33});
    }
    observed.combat_metrics().register_owner(EntityId{32}, TowerType::Neutrophil);
    f64 removed = 0;
    for (u32 tick = 0; tick < 90; ++tick) {
        observed.tick();
        control.tick();
        removed += observed.last_damage_stats().density_removed;
        REQUIRE(observed.state_hash() == control.state_hash());
        REQUIRE(observed.snapshot().objective_integrity == control.snapshot().objective_integrity);
        REQUIRE(observed.combat_metrics().totals().damage.density_removed == Catch::Approx(removed).margin(1e-5));
    }
    const auto& metrics = observed.combat_metrics();
    REQUIRE(source_damage(metrics, CombatDamageSource::Field) > 0);
    REQUIRE(source_damage(metrics, CombatDamageSource::Projectile) > 0);
    REQUIRE(source_damage(metrics, CombatDamageSource::Drain) > 0);
    u64 kills = 0;
    for (u32 family = 0; family < kFamilyCount; ++family)
        kills += observed.snapshot().chaff_killed_by_family[family];
    REQUIRE(metrics.totals().damage.damage_kills == static_cast<f64>(kills));
    REQUIRE(metrics.overflow_count() == 0);
    REQUIRE_FALSE(control.combat_metrics().enabled());
    REQUIRE(control.combat_metrics().buckets().empty());
    observed.reset_combat_metrics();
    REQUIRE(observed.combat_metrics().totals().damage.density_removed == 0);
    REQUIRE(observed.combat_metrics().buckets().empty());
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "mixed fluid metrics divide applied damage by deposited DPS",
          "[balance][combat-metrics][fluid]") {
    SimWorld world;
    SimWorld control;
    init_world(world, true);
    init_world(control, false);
    for (SimWorld* subject : {&world, &control}) {
        spawn(*subject, {50.5f, 40.5f}, PathogenFamily::Bacteria, 100.0f);
        FluidTuning tuning;
        tuning.stiffness = 0;
        tuning.near_stiffness = 0;
        tuning.drag = 0;
        subject->fluid_system().configure(subject->desc().sim_bounds, tuning);
        FluidJetParams jet;
        jet.owner = EntityId{41};
        jet.source = TowerType::GobletCell;
        jet.damage_per_second = 10;
        REQUIRE(subject->fluid().spawn({50.5f, 40.5f}, {}, jet));
        jet.owner = EntityId{42};
        jet.damage_per_second = 30;
        REQUIRE(subject->fluid().spawn({50.5f, 40.5f}, {}, jet));
    }
    world.tick();
    control.tick();
    REQUIRE(world.state_hash() == control.state_hash());
    REQUIRE(world.last_damage_stats().density_removed > 0);
    REQUIRE(world.combat_metrics().totals().damage.density_removed ==
            Catch::Approx(world.last_damage_stats().density_removed));
    f64 first = 0;
    f64 second = 0;
    for (const auto& bucket : world.combat_metrics().buckets()) {
        if (bucket.owner == EntityId{41}) first += bucket.totals.density_removed;
        if (bucket.owner == EntityId{42}) second += bucket.totals.density_removed;
    }
    REQUIRE(first > 0);
    REQUIRE(second == Catch::Approx(first * 3));
    REQUIRE(first + second == Catch::Approx(world.last_damage_stats().density_removed));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "attribution capacity loss is explicit and aggregate accounting remains complete",
          "[balance][combat-metrics]") {
    CombatMetrics metrics;
    metrics.enable(true, 1);
    metrics.record({EntityId{1}, TowerType::Neutrophil, CombatDamageSource::Projectile}, 0, 2, 1, 0, 1);
    metrics.record({EntityId{2}, TowerType::CytotoxicT, CombatDamageSource::Drain}, 0, 3, 2, 0, 0);
    REQUIRE(metrics.buckets().size() == 1);
    REQUIRE(metrics.overflow_count() == 1);
    REQUIRE(metrics.totals().damage.density_removed == 3);
    metrics.reset();
    REQUIRE(metrics.overflow_count() == 0);
    REQUIRE(metrics.totals().damage.density_removed == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "combat metrics keep swallowing bursts and scars with their owners",
          "[balance][combat-metrics][sources]") {
    SimWorld world;
    init_world(world, true);
    CombatDamageSource expected = CombatDamageSource::Unknown;
    TowerType expected_type = TowerType::Count;
    const EntityId owner{55};
    spawn(world, {50, 40}, PathogenFamily::Virus, 10.0f);

    SECTION("a swallowed captive credits its macrophage") {
        expected = CombatDamageSource::Swallow;
        expected_type = TowerType::Macrophage;
        SwarmerProfile profile;
        profile.kind = SwarmerKind::ArborGrabber;
        profile.source = expected_type;
        profile.search_radius = 20;
        profile.attach_radius = 10;
        profile.kite_fraction = 0;
        profile.arbor_arm_count = 1;
        profile.arbor_extend_seconds = 0.001f;
        profile.arbor_latch_seconds = 0.001f;
        profile.arbor_pull_seconds = 0.05f;
        world.swarmers().set_profile(0, profile);
        SwarmerSpawnParams cell;
        cell.position = {45, 40};
        cell.profile = 0;
        cell.owner = owner;
        cell.persistent = true;
        REQUIRE(world.swarmers().spawn(cell));
    }
    SECTION("a swarmer burst retains its producing cell") {
        expected = CombatDamageSource::Burst;
        expected_type = TowerType::CytotoxicT;
        SwarmerBurst burst;
        burst.origin = {50, 40};
        burst.owner = owner;
        burst.source = expected_type;
        burst.seconds = kFixedDt;
        burst.damage = 3;
        burst.falloff = 0;
        world.swarmer_system().effects().bursts.push_back(burst);
        world.apply_swarmer_effects();
    }
    SECTION("scar contact credits the builder owner") {
        expected = CombatDamageSource::ScarContact;
        expected_type = TowerType::Fibroblast;
        ImmunityTuning immunity;
        immunity.scar_contact_rate = 6;
        world.set_immunity(immunity);
        auto& registry = world.ecs().registry();
        const auto entity = registry.create();
        registry.emplace<comp::Transform>(entity, comp::Transform{{50, 40}, 0, 1});
        registry.emplace<comp::Health>(entity, comp::Health{100, 100, 0});
        comp::Scar scar;
        scar.owner = owner;
        scar.source = expected_type;
        registry.emplace<comp::Scar>(entity, scar);
    }

    f64 removed = 0;
    for (u32 tick = 0; tick < 60; ++tick) {
        world.tick();
        removed += world.last_damage_stats().density_removed;
    }
    REQUIRE(source_damage(world.combat_metrics(), expected) > 0);
    REQUIRE(world.combat_metrics().totals().damage.density_removed == Catch::Approx(removed).margin(1e-5));
    bool found = false;
    for (const auto& bucket : world.combat_metrics().buckets()) {
        if (bucket.source != expected) continue;
        REQUIRE(bucket.owner == owner);
        REQUIRE(bucket.type == expected_type);
        found = true;
    }
    REQUIRE(found);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "immune swallowed captives never produce removed density or ATP income",
          "[balance][combat-metrics][sources][regression]") {
    for (const bool mixed_chunk : {false, true}) {
        INFO("mixed surface and underground captives: " << mixed_chunk);
        SimWorld observed, unobserved;
        for (SimWorld* world : {&observed, &unobserved}) {
            init_world(*world, world == &observed);
            spawn(*world, {50, 40}, PathogenFamily::Parasite, 1.6f);
            spawn(*world, {51, 40}, mixed_chunk ? PathogenFamily::Virus : PathogenFamily::Parasite,
                  mixed_chunk ? 0.6f : 1.6f);
            world->chaff().burrow_state[0] = burrow_state::kUnderground;
            world->chaff().burrow_timer[0] = 10.0f;
            if (!mixed_chunk) {
                world->chaff().burrow_state[1] = burrow_state::kUnderground;
                world->chaff().burrow_timer[1] = 10.0f;
            }
            SwarmerProfile profile;
            profile.kind = SwarmerKind::ArborGrabber;
            profile.source = TowerType::Macrophage;
            profile.speed = 0.0f;
            profile.arbor_arm_count = 1;
            profile.arbor_pull_seconds = 0.001f;
            profile.max_health = 20.0f;
            profile.heal_per_kill = 4.0f;
            world->swarmers().set_profile(0, profile);
            SwarmerSpawnParams cell;
            cell.position = {45, 40};
            cell.profile = 0;
            cell.owner = EntityId{55};
            cell.persistent = true;
            REQUIRE(world->swarmers().spawn(cell));
            world->swarmers().health[0] = 5.0f;
            // Reproduce a chunk that has already been captured, whose
            // parasite dives before the arm's pull completes. Pulling keeps
            // the captive identity valid, while damage immunity rejects it.
            auto& arm = world->swarmers().arbor_grabber[0].arms[0];
            arm.phase = ArborArmPhase::Pulling;
            arm.captive.chaff_count = 2;
            for (u32 i = 0; i < 2; ++i) {
                arm.captive.chaff[i].handle = ChaffHandle{i, world->chaff().generation[i]};
                arm.captive.chaff[i].offset = Vec2{5.0f + static_cast<f32>(i), 0};
                world->chaff().flags[i] |= chaff_flags::kHidden;
            }
            world->tick();
            const f32 expected_removed = mixed_chunk ? 0.6f : 0.0f;
            REQUIRE(world->last_damage_stats().density_removed == Catch::Approx(expected_removed).margin(1e-6));
            REQUIRE(world->swarmer_system().last_stats().density_removed == Catch::Approx(expected_removed).margin(1e-6));
            REQUIRE(world->chaff().total_density() == Catch::Approx(mixed_chunk ? 1.6f : 3.2f));
            REQUIRE(world->chaff().count() == (mixed_chunk ? 1u : 2u));
            REQUIRE(world->snapshot().chaff_killed_by_family[static_cast<u32>(PathogenFamily::Parasite)] == 0);
            REQUIRE(world->swarmer_system().last_stats().hosts_finished == (mixed_chunk ? 1u : 0u));
            REQUIRE(world->swarmer_system().last_stats().healed == Catch::Approx(mixed_chunk ? 4.0f : 0.0f));
            REQUIRE(world->swarmers().health[0] == Catch::Approx(mixed_chunk ? 9.0f : 5.0f));

            game::Economy economy;
            game::EconomyConfig config;
            config.starting_atp = 0;
            config.atp_per_density = 10.0f;
            economy.configure(config);
            economy.reset();
            economy.credit_kills(world->last_damage_stats().density_removed);
            REQUIRE(economy.snapshot().damage_income == (mixed_chunk ? 6u : 0u));
        }
        REQUIRE(observed.state_hash() == unobserved.state_hash());
        const auto& metrics = observed.combat_metrics();
        REQUIRE(metrics.totals().damage.density_removed == Catch::Approx(mixed_chunk ? 0.6 : 0.0).margin(1e-6));
        REQUIRE(metrics.totals().damage.immune_density == Catch::Approx(mixed_chunk ? 1.6 : 3.2));
        REQUIRE(source_damage(metrics, CombatDamageSource::Swallow) == Catch::Approx(mixed_chunk ? 0.6 : 0.0).margin(1e-6));
        const auto& parasite = metrics.enemy_lifetimes()[static_cast<u32>(PathogenFamily::Parasite)];
        REQUIRE(parasite.first_hits == 0);
        REQUIRE(parasite.damage_deaths == 0);
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "sustain heals only for captives alive and actually killed by the completed pull",
          "[balance][combat-metrics][sources][regression][sustain]") {
    for (const bool live_meal : {false, true}) {
        INFO("one living surface captive: " << live_meal);
        SimWorld observed, unobserved;
        for (SimWorld* world : {&observed, &unobserved}) {
            init_world(*world, world == &observed);
            spawn(*world, {50, 40}, PathogenFamily::Virus, 0.6f);
            spawn(*world, {51, 40}, PathogenFamily::Virus, 0.6f);
            spawn(*world, {52, 40}, PathogenFamily::Parasite, 1.6f);
            // A different source finished one or both captives before this
            // arm completes. Their slots still exist until tick compaction.
            world->chaff().apply_density_loss(1, 0.6f);
            if (!live_meal) world->chaff().apply_density_loss(0, 0.6f);
            world->chaff().burrow_state[2] = burrow_state::kUnderground;
            world->chaff().burrow_timer[2] = 10.0f;
            SwarmerProfile profile;
            profile.kind = SwarmerKind::ArborGrabber;
            profile.source = TowerType::Macrophage;
            profile.speed = 0.0f;
            profile.arbor_arm_count = 1;
            profile.arbor_pull_seconds = 0.001f;
            profile.max_health = 20.0f;
            profile.heal_per_kill = 4.0f;
            world->swarmers().set_profile(0, profile);
            SwarmerSpawnParams cell;
            cell.position = {45, 40};
            cell.owner = EntityId{56};
            cell.persistent = true;
            REQUIRE(world->swarmers().spawn(cell));
            world->swarmers().health[0] = 5.0f;
            auto& arm = world->swarmers().arbor_grabber[0].arms[0];
            arm.phase = ArborArmPhase::Pulling;
            arm.captive.chaff_count = 3;
            for (u32 i = 0; i < 3; ++i) {
                arm.captive.chaff[i].handle = ChaffHandle{i, world->chaff().generation[i]};
                arm.captive.chaff[i].offset = Vec2{5.0f + static_cast<f32>(i), 0};
                world->chaff().flags[i] |= chaff_flags::kHidden;
            }
            world->tick();
            REQUIRE(world->swarmer_system().last_stats().hosts_finished == (live_meal ? 1u : 0u));
            REQUIRE(world->swarmer_system().last_stats().healed == Catch::Approx(live_meal ? 4.0f : 0.0f));
            REQUIRE(world->swarmers().health[0] == Catch::Approx(live_meal ? 9.0f : 5.0f));
            REQUIRE(world->last_damage_stats().density_removed == Catch::Approx(live_meal ? 0.6f : 0.0f));
            REQUIRE(world->chaff().total_density() == Catch::Approx(1.6f));
            REQUIRE(world->chaff().count() == 1);
        }
        REQUIRE(observed.state_hash() == unobserved.state_hash());
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "replication throttles and deterministic rebakes are observable",
          "[balance][combat-metrics][capacity]") {
    SimWorld world;
    init_world(world, true);
    world.chaff().reserve(1);
    auto tuning = world.chaff_system().tuning();
    tuning.family[static_cast<u32>(PathogenFamily::Virus)].replication_rate = 120.0f;
    world.chaff_system().set_tuning(tuning);
    spawn(world, {50, 40}, PathogenFamily::Virus, 1);
    world.flow().mark_dirty({{40, 30}, {45, 35}});
    world.flow().mark_dirty({{60, 30}, {65, 35}});
    world.tick();
    REQUIRE(world.combat_metrics().totals().replications_denied_capacity == 1);
    REQUIRE(world.snapshot().chaff_count == 1);
    REQUIRE_FALSE(world.flow().has_pending_rebake());
    REQUIRE(world.flow().stats().regions_processed == 2);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "enemy TTK follows identity through compaction and excludes non-damage exits",
          "[balance][combat-metrics][ttk]") {
    CombatMetrics metrics;
    metrics.enable();
    ChaffBuffers enemies;
    enemies.reserve(8);
    enemies.set_combat_metrics(&metrics);
    ChaffSpawnParams enemy;
    enemy.density = 2;
    REQUIRE(enemies.spawn(enemy).valid()); // Born at tick 0.
    metrics.set_tick(5);
    enemy.density = 3;
    REQUIRE(enemies.spawn(enemy).valid());
    metrics.set_tick(10);
    enemies.apply_density_loss(0, 1);
    metrics.set_tick(17);
    enemies.apply_density_loss(1, 1);
    metrics.set_tick(25);
    enemies.apply_density_loss(0, 1);
    REQUIRE(enemies.compact() == 1); // Second agent moves to slot zero.
    metrics.set_tick(30);
    enemies.apply_density_loss(0, 2);
    REQUIRE(enemies.compact() == 1);
    const auto family = static_cast<u32>(PathogenFamily::Virus);
    const auto& result = metrics.enemy_lifetimes()[family];
    REQUIRE(result.spawned == 2);
    REQUIRE(result.alive == 0);
    REQUIRE(result.damage_deaths == 2);
    REQUIRE(result.damage_ttk.observations == 2);
    REQUIRE(result.first_hits == 2);
    REQUIRE(result.damage_ttk.sum_ticks == 28);
    REQUIRE(result.damage_ttk.min_ticks == 13);
    REQUIRE(result.damage_ttk.max_ticks == 15);
    REQUIRE(result.damage_lifetime.sum_ticks == 50);

    metrics.set_tick(31);
    enemy.family = PathogenFamily::Parasite;
    REQUIRE(enemies.spawn(enemy).valid());
    metrics.set_tick(32);
    enemies.apply_density_loss(0, 0.5f);
    metrics.set_tick(40);
    enemies.kill(0, CombatRetirement::Leak);
    REQUIRE(enemies.compact() == 1);
    const auto& leaked = metrics.enemy_lifetimes()[static_cast<u32>(PathogenFamily::Parasite)];
    REQUIRE(leaked.leaked == 1);
    REQUIRE(leaked.first_hits == 1);
    REQUIRE(leaked.hit_alive == 0);
    REQUIRE(leaked.damage_ttk.observations == 0);
    REQUIRE(leaked.damage_lifetime.observations == 0);
    REQUIRE(leaked.retired_lifetime.sum_ticks == 9);
    enemy.family = PathogenFamily::Virus;
    REQUIRE(enemies.spawn(enemy).valid());
    metrics.set_tick(50);
    enemies.apply_density_loss(0, 0.5f);
    REQUIRE(result.alive == 1);
    REQUIRE(result.hit_alive == 1);
    REQUIRE(result.first_hits == 3);
    REQUIRE(result.damage_ttk.observations == 2);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "friendly TTK ends at hostile death and health damage is clamped",
          "[balance][combat-metrics][ttk][hostile]") {
    CombatMetrics metrics;
    metrics.enable();
    SwarmerBuffers cells;
    cells.reserve(8);
    cells.set_combat_metrics(&metrics);
    SwarmerProfile profile;
    profile.source = TowerType::Neutrophil;
    profile.kind = SwarmerKind::Shooter;
    profile.max_health = 10;
    cells.set_profile(0, profile);
    SwarmerSpawnParams cell;
    cell.position = {50, 40};
    cell.profile = 0;
    cell.owner = EntityId{77};
    REQUIRE(cells.spawn(cell));
    ChaffBuffers enemies;
    enemies.reserve(2);
    ChaffSpawnParams enemy;
    enemy.position = {50, 40};
    enemy.family = PathogenFamily::Bacteria;
    REQUIRE(enemies.spawn(enemy).valid());
    SpatialHash hash;
    SpatialHashDesc hash_desc;
    hash_desc.bounds = {{0, 0}, {100, 80}};
    hash.configure(hash_desc);
    hash.rebuild(enemies.pos_x.data(), enemies.pos_y.data(), enemies.count(), nullptr);
    HostileSystem hostile;
    HostileTuning tuning;
    tuning.enabled = true;
    auto& bacteria = tuning.family[static_cast<u32>(PathogenFamily::Bacteria)];
    bacteria.aura_radius = 5;
    bacteria.aura_dps = 120;
    hostile.set_tuning(tuning);
    FriendlyTowerList structures;
    TissueMask mask;
    metrics.set_tick(10);
    hostile.update(enemies, hash, cells, structures, mask, kFixedDt, nullptr);
    REQUIRE(cells.health[0] == Catch::Approx(8));
    metrics.set_tick(20);
    bacteria.aura_dps = 1200;
    hostile.set_tuning(tuning);
    hostile.update(enemies, hash, cells, structures, mask, kFixedDt, nullptr);
    REQUIRE(metrics.totals().hostile_actual_cell_damage == Catch::Approx(10));
    REQUIRE(metrics.friendly_lifetimes().size() == 1);
    const auto& dead = metrics.friendly_lifetimes()[0].stats;
    REQUIRE(dead.damage_deaths == 1); // Before the next swarmer compaction.
    REQUIRE(dead.alive == 0);
    REQUIRE(dead.damage_ttk.observations == 1);
    REQUIRE(dead.damage_ttk.sum_ticks == 10);
    REQUIRE(dead.damage_lifetime.sum_ticks == 20);
    metrics.set_tick(21);
    cells.compact();
    REQUIRE(dead.damage_deaths == 1);

    profile.source = TowerType::GobletCell;
    cells.set_profile(1, profile);
    cell.profile = 1;
    cell.owner = EntityId{78};
    REQUIRE(cells.spawn(cell));
    metrics.set_tick(23);
    cells.observe_hostile_damage(0, cells.health[0], 1);
    cells.health[0] -= 1;
    metrics.set_tick(27);
    cells.kill(0, CombatRetirement::Consumed);
    cells.compact();
    const auto& consumed = metrics.friendly_lifetimes()[1].stats;
    REQUIRE(consumed.consumed == 1);
    REQUIRE(consumed.first_hits == 1);
    REQUIRE(consumed.hit_alive == 0);
    REQUIRE(consumed.damage_deaths == 0);
    REQUIRE(consumed.damage_ttk.observations == 0);
    REQUIRE(consumed.retired_lifetime.sum_ticks == 6);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "enabling telemetry on existing agents does not invent their birth time",
          "[balance][combat-metrics][ttk]") {
    SimWorld world;
    init_world(world, false);
    spawn(world, {50, 40}, PathogenFamily::Virus, 1);
    world.run_ticks(12);
    world.enable_combat_metrics();
    world.chaff().apply_density_loss(0, 1);
    world.chaff().compact();
    const auto& result = world.combat_metrics().enemy_lifetimes()[static_cast<u32>(PathogenFamily::Virus)];
    REQUIRE(result.entered_alive == 1);
    REQUIRE(result.spawned == 0);
    REQUIRE(result.damage_deaths == 1);
    REQUIRE(result.damage_ttk.observations == 1);
    REQUIRE(result.damage_ttk.sum_ticks == 0); // Same-tick effective hit and death.
    REQUIRE(result.damage_lifetime.observations == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "real navigation exits and friendly bounds retirement never enter damage TTK",
          "[balance][combat-metrics][ttk][retirement]") {
    SimWorld world;
    init_world(world, true);
    SECTION("an enemy reaching the objective is a leak") {
        spawn(world, {116, 40}, PathogenFamily::Virus, 1);
        world.tick();
        const auto& stats = world.combat_metrics().enemy_lifetimes()[static_cast<u32>(PathogenFamily::Virus)];
        REQUIRE(stats.leaked == 1);
        REQUIRE(stats.damage_deaths == 0);
        REQUIRE(stats.damage_ttk.observations == 0);
        REQUIRE(stats.alive == 0);
    }
    SECTION("an enemy leaving world bounds is a despawn") {
        spawn(world, {-10000, 40}, PathogenFamily::Bacteria, 1);
        world.tick();
        const auto& stats = world.combat_metrics().enemy_lifetimes()[static_cast<u32>(PathogenFamily::Bacteria)];
        REQUIRE(stats.out_of_bounds == 1);
        REQUIRE(stats.damage_deaths == 0);
        REQUIRE(stats.damage_ttk.observations == 0);
    }
    SECTION("a cell leaving world bounds has no hostile killer") {
        SwarmerProfile profile;
        profile.source = TowerType::Neutrophil;
        profile.kind = SwarmerKind::Shooter;
        world.swarmers().set_profile(0, profile);
        SwarmerSpawnParams cell;
        cell.position = {-10000, 40};
        cell.owner = EntityId{88};
        cell.persistent = true;
        REQUIRE(world.swarmers().spawn(cell));
        world.tick();
        REQUIRE(world.combat_metrics().friendly_lifetimes().size() == 1);
        const auto& stats = world.combat_metrics().friendly_lifetimes()[0].stats;
        REQUIRE(stats.out_of_bounds == 1);
        REQUIRE(stats.damage_deaths == 0);
        REQUIRE(stats.damage_ttk.observations == 0);
        REQUIRE(stats.alive == 0);
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "finite friendly pools count complete denied batches without changing a replay",
          "[balance][combat-metrics][capacity][determinism]") {
    SimWorld observed;
    SimWorld control;
    init_world(observed, true);
    init_world(control, false);
    for (auto* world : {&observed, &control}) {
        world->projectiles().reserve(1);
        world->fluid().reserve(2);
        world->swarmers().reserve(1);
        ProjectileSpawnParams shot;
        shot.position = {60, 40};
        shot.owner = EntityId{91};
        shot.source = TowerType::Neutrophil;
        REQUIRE(world->projectiles().spawn(shot));
        REQUIRE_FALSE(world->projectiles().spawn(shot));
        FluidJetParams jet;
        jet.owner = EntityId{92};
        jet.source = TowerType::GobletCell;
        jet.damage_per_second = 0;
        REQUIRE(world->fluid_system().splash(world->fluid(), jet, {50, 40}, 3, 0, 7) == 2);
        SwarmerProfile profile;
        profile.kind = SwarmerKind::Shooter;
        profile.source = TowerType::Neutrophil;
        world->swarmers().set_profile(0, profile);
        SwarmerSpawnParams cell;
        cell.position = {40, 40};
        cell.owner = EntityId{93};
        cell.persistent = true;
        REQUIRE(world->swarmers().spawn(cell));
        REQUIRE_FALSE(world->swarmers().spawn(cell));
    }
    const auto& pools = observed.combat_metrics().totals().pools;
    REQUIRE(pools.projectiles.requested == 2);
    REQUIRE(pools.projectiles.spawned == 1);
    REQUIRE(pools.projectiles.denied == 1);
    REQUIRE(pools.projectiles.highwater == 1);
    REQUIRE(pools.fluid.requested == 7);
    REQUIRE(pools.fluid.spawned == 2);
    REQUIRE(pools.fluid.denied == 5);
    REQUIRE(pools.fluid.highwater == 2);
    REQUIRE(pools.cells.requested == 2);
    REQUIRE(pools.cells.spawned == 1);
    REQUIRE(pools.cells.denied == 1);
    REQUIRE(pools.cells.highwater == 1);
    for (u32 tick = 0; tick < 10; ++tick) {
        observed.tick();
        control.tick();
        REQUIRE(observed.state_hash() == control.state_hash());
    }
    REQUIRE(control.combat_metrics().totals().pools.projectiles.requested == 0);
    const auto& activity = observed.combat_metrics().activity()[static_cast<u32>(TowerType::Neutrophil)];
    REQUIRE(activity.observed_ticks == 10);
    REQUIRE(activity.targetless_ticks == 10);
    REQUIRE(activity.loaded_ticks == 10);
    REQUIRE(activity.attacking_ticks == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "projectile telemetry separates applied hits walls expiry and repeated piercing hits",
          "[balance][combat-metrics][projectile]") {
    SimWorld world;
    init_world(world, true);
    spawn(world, {50, 40}, PathogenFamily::Virus, 10);
    ProjectileSpawnParams shot;
    shot.position = {50, 40};
    shot.source = TowerType::Neutrophil;
    shot.owner = EntityId{101};
    shot.hit_radius = 1;
    shot.flags = projectile_flags::kPiercing;
    REQUIRE(world.projectiles().spawn(shot));
    shot.flags = 0;
    shot.owner = EntityId{102};
    shot.position = {70, 40};
    shot.lifetime = 0;
    REQUIRE(world.projectiles().spawn(shot));
    shot.owner = EntityId{103};
    shot.position = {60, 5};
    shot.lifetime = 1;
    REQUIRE(world.projectiles().spawn(shot));
    world.tick();
    world.tick();
    const auto& outcomes = world.combat_metrics().totals().projectile_outcomes;
    REQUIRE(outcomes.agent_hits == 2);
    REQUIRE(outcomes.projectiles_hit == 1);
    REQUIRE(outcomes.retired_on_hit == 0);
    REQUIRE(outcomes.wall_impacts == 1);
    REQUIRE(outcomes.expired == 1);
    REQUIRE(world.projectiles().count() == 1);
    bool found = false;
    for (const auto& bucket : world.combat_metrics().buckets()) {
        if (bucket.owner != EntityId{101}) continue;
        REQUIRE(bucket.type == TowerType::Neutrophil);
        REQUIRE(bucket.projectile_outcomes.agent_hits == 2);
        REQUIRE(bucket.projectile_outcomes.projectiles_hit == 1);
        found = true;
    }
    REQUIRE(found);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "sampled activity distinguishes target downtime reload and builder pause",
          "[balance][combat-metrics][activity]") {
    SimWorld world;
    init_world(world, true);
    SwarmerProfile shooter;
    shooter.source = TowerType::Neutrophil;
    shooter.kind = SwarmerKind::Shooter;
    shooter.search_radius = 20;
    shooter.attach_radius = 10;
    shooter.magazine_size = 1;
    shooter.gather_seconds = 0;
    shooter.reload_seconds = 1;
    world.swarmers().set_profile(0, shooter);
    SwarmerSpawnParams cell;
    cell.owner = EntityId{111};
    cell.position = {45, 40};
    cell.persistent = true;
    REQUIRE(world.swarmers().spawn(cell));
    world.tick();
    spawn(world, {50, 40}, PathogenFamily::Virus, 100);
    for (u32 tick = 0; tick < 20; ++tick) world.tick();
    const auto& fire = world.combat_metrics().activity()[static_cast<u32>(TowerType::Neutrophil)];
    REQUIRE(fire.observed_ticks == 21);
    REQUIRE(fire.targetless_ticks >= 1);
    REQUIRE(fire.reloading_ticks > 0);
    REQUIRE(world.combat_metrics().totals().pools.projectiles.spawned > 0);
    REQUIRE(fire.loaded_ticks + fire.reloading_ticks + fire.gathering_ticks + fire.firing_ticks == fire.observed_ticks);

    SwarmerProfile builder;
    builder.kind = SwarmerKind::Builder;
    builder.source = TowerType::Fibroblast;
    world.swarmers().set_profile(1, builder);
    cell.profile = 1;
    cell.owner = EntityId{112};
    cell.position = {70, 40};
    REQUIRE(world.swarmers().spawn(cell));
    world.swarmer_system().set_builders_active(false);
    for (u32 tick = 0; tick < 5; ++tick) world.tick();
    const auto& build = world.combat_metrics().activity()[static_cast<u32>(TowerType::Fibroblast)];
    REQUIRE(build.observed_ticks == 5);
    REQUIRE(build.builder_paused_ticks == 5);
    REQUIRE(build.builder_travel_ticks == 0);
    REQUIRE(build.builder_rest_ticks == 0);
}
