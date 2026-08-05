// Wave 2C: enemy roster tests — family table, chaff tuning derivation, elite
// spawning, parasite burrow/hide, biofilm clumping, tumour growth, and the
// allergen wave-modifier primitive.
#include "core/JobSystem.h"
#include "core/Rng.h"
#include "game/enemies/EnemyRoster.h"
#include "render/ChaffBatcher.h"
#include "render/Renderer.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/AiStateMachine.h"
#include "sim/ecs/NamedAgents.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::game;

// game::allergen_overreaction has deliberate external linkage but no header
// declaration (EnemyRoster.h is frozen and Allergen has no natural slot in
// its public surface — see the doc comment at its definition in
// EnemyRoster.cpp). Forward-declare it here to call it directly.
namespace immune::game {
void allergen_overreaction(sim::SimWorld& world, Vec2 origin, f32 radius, f32 kill_rate, f32 duration);
}

namespace {

sim::SimWorld make_world(u64 seed = 99) {
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.seed = seed;
    desc.max_chaff = 512;
    world.init(desc, nullptr);
    return world;
}

EnemyRoster make_roster() {
    EnemyRoster r;
    r.load_defaults();
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Family table
// ---------------------------------------------------------------------------

TEST_CASE("family table colours match the renderer's single source of truth", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const auto f = static_cast<PathogenFamily>(i);
        const FamilyDef& d = roster.family(f);
        REQUIRE(d.family == f);
        const Vec4 expected = render::family_color(f);
        REQUIRE(d.color.r == Catch::Approx(expected.r));
        REQUIRE(d.color.g == Catch::Approx(expected.g));
        REQUIRE(d.color.b == Catch::Approx(expected.b));
        REQUIRE(d.color.a == Catch::Approx(expected.a));
    }
}

TEST_CASE("family behaviour flags match DESIGN.md section 6's table", "[game][enemies]") {
    EnemyRoster roster = make_roster();

    const FamilyDef& virus = roster.family(PathogenFamily::Virus);
    REQUIRE(virus.replicates);
    REQUIRE_FALSE(virus.clumps);
    REQUIRE_FALSE(virus.drifts);
    REQUIRE_FALSE(virus.can_hide);
    REQUIRE_FALSE(virus.leaves_hazard);

    const FamilyDef& bacteria = roster.family(PathogenFamily::Bacteria);
    REQUIRE(bacteria.clumps);
    REQUIRE_FALSE(bacteria.replicates);
    REQUIRE_FALSE(bacteria.drifts);
    REQUIRE_FALSE(bacteria.can_hide);

    const FamilyDef& spore = roster.family(PathogenFamily::FungalSpore);
    REQUIRE(spore.drifts);
    REQUIRE(spore.leaves_hazard);
    REQUIRE_FALSE(spore.replicates);
    REQUIRE_FALSE(spore.clumps);
    REQUIRE_FALSE(spore.can_hide);

    const FamilyDef& parasite = roster.family(PathogenFamily::Parasite);
    REQUIRE(parasite.can_hide);
    REQUIRE_FALSE(parasite.replicates);
    REQUIRE_FALSE(parasite.clumps);
    REQUIRE_FALSE(parasite.drifts);

    // Neither is a chaff-swarm family: CancerCell is the boss/objective
    // enemy, Allergen is a wave modifier. No behaviour switches set.
    const FamilyDef& cancer = roster.family(PathogenFamily::CancerCell);
    REQUIRE_FALSE(cancer.replicates);
    REQUIRE_FALSE(cancer.clumps);
    REQUIRE_FALSE(cancer.drifts);
    REQUIRE_FALSE(cancer.can_hide);
    REQUIRE_FALSE(cancer.leaves_hazard);

    const FamilyDef& allergen = roster.family(PathogenFamily::Allergen);
    REQUIRE_FALSE(allergen.replicates);
    REQUIRE_FALSE(allergen.clumps);
    REQUIRE_FALSE(allergen.drifts);
    REQUIRE_FALSE(allergen.can_hide);
    REQUIRE_FALSE(allergen.leaves_hazard);
}

// ---------------------------------------------------------------------------
// apply_to_tuning()
// ---------------------------------------------------------------------------

TEST_CASE("apply_to_tuning produces sane, family-consistent ChaffFamilyParams", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    sim::ChaffTuning tuning{};
    roster.apply_to_tuning(tuning);

    for (u32 i = 0; i < kFamilyCount; ++i) {
        const sim::ChaffFamilyParams& p = tuning.family[i];
        INFO("family index " << i);
        REQUIRE(p.max_speed > 0.0f);
        REQUIRE(p.acceleration > 0.0f);
        REQUIRE(p.radius > 0.0f);
        REQUIRE(p.separation_radius > 0.0f);
        REQUIRE(p.separation_strength > 0.0f);
        REQUIRE(p.base_density > 0.0f);
    }

    // No disagreement possible by construction: replicates/drifts flags on
    // the family table are exactly what turns these two knobs on.
    REQUIRE(tuning.family[static_cast<u32>(PathogenFamily::Virus)].replication_rate > 0.0f);
    REQUIRE(tuning.family[static_cast<u32>(PathogenFamily::FungalSpore)].drift_bias > 0.0f);

    // Everyone else's replication_rate/drift_bias stays at zero.
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const auto f = static_cast<PathogenFamily>(i);
        if (f != PathogenFamily::Virus) {
            REQUIRE(tuning.family[i].replication_rate == 0.0f);
        }
        if (f != PathogenFamily::FungalSpore) {
            REQUIRE(tuning.family[i].drift_bias == 0.0f);
        }
    }

    // Radius mirrors the renderer's silhouette table, so collision size and
    // drawn size can never disagree.
    const f32 virus_radius = tuning.family[static_cast<u32>(PathogenFamily::Virus)].radius;
    const f32 expected_virus_radius = render::family_visual(PathogenFamily::Virus).silhouette * 0.5f;
    REQUIRE(virus_radius == Catch::Approx(expected_virus_radius));
}

TEST_CASE("apply_to_tuning does not disagree with the roster's own family table", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    sim::ChaffTuning tuning{};
    roster.apply_to_tuning(tuning);
    for (u32 i = 0; i < kFamilyCount; ++i) {
        REQUIRE(tuning.family[i].base_density == roster.family(static_cast<PathogenFamily>(i)).base_density);
    }
}

// ---------------------------------------------------------------------------
// Elite table & spawning
// ---------------------------------------------------------------------------

TEST_CASE("roster ships at least the three required elites", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    REQUIRE(roster.elites().size() >= 3);
    REQUIRE(roster.find_elite("parasite_burrower") != nullptr);
    REQUIRE(roster.find_elite("biofilm_colony") != nullptr);
    REQUIRE(roster.find_elite("tumor_mass") != nullptr);
    REQUIRE(roster.find_elite("no_such_elite") == nullptr);
}

TEST_CASE("spawn_elite creates an entity whose stats match the EliteDef", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();

    const EliteDef* def = roster.find_elite("parasite_burrower");
    REQUIRE(def != nullptr);

    const EntityId id = roster.spawn_elite(world, def->id, Vec2{5.0f, 5.0f});
    REQUIRE(id.valid());

    entt::registry& registry = world.ecs().registry();
    const entt::entity raw = world.ecs().from_id(id);
    REQUIRE(registry.valid(raw));

    const auto& health = registry.get<sim::comp::Health>(raw);
    REQUIRE(health.max == Catch::Approx(def->max_health));
    REQUIRE(health.current == Catch::Approx(def->max_health));
    REQUIRE(health.armor == Catch::Approx(def->armor));

    const auto& named = registry.get<sim::comp::NamedAgent>(raw);
    REQUIRE(named.family == def->family);
    REQUIRE(named.tier == static_cast<u8>(def->tier));

    const auto& transform = registry.get<sim::comp::Transform>(raw);
    REQUIRE(transform.position.x == Catch::Approx(5.0f));
    REQUIRE(transform.position.y == Catch::Approx(5.0f));
}

TEST_CASE("spawn_elite works for all three registered elites", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();

    for (const EliteDef& def : roster.elites()) {
        const EntityId id = roster.spawn_elite(world, def.id, Vec2{0.0f, 0.0f});
        INFO("elite " << def.name);
        REQUIRE(id.valid());
    }
    REQUIRE(world.ecs().named_agent_count() == roster.elites().size());
}

TEST_CASE("spawn_elite rejects an unknown elite id", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();
    const EntityId id = roster.spawn_elite(world, /*elite_id*/ 9999, Vec2{0.0f, 0.0f});
    REQUIRE_FALSE(id.valid());
}

// ---------------------------------------------------------------------------
// Parasite burrow/hide
// ---------------------------------------------------------------------------

TEST_CASE("parasite_burrower periodically transitions to Burrowed and resurfaces",
          "[game][enemies][ai]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();

    const EliteDef* def = roster.find_elite("parasite_burrower");
    REQUIRE(def != nullptr);
    const EntityId id = roster.spawn_elite(world, def->id, Vec2{0.0f, 0.0f});
    REQUIRE(id.valid());

    entt::registry& registry = world.ecs().registry();
    const entt::entity raw = world.ecs().from_id(id);

    bool saw_burrowed = false;
    bool resurfaced_after_burrow = false;
    // Spawning clears in 0.25s, periodic burrow fires 3.5s into Advancing:
    // give it generous headroom (well past 3.75s = 225 ticks) plus the 1.8s
    // hide duration to resurface.
    for (int i = 0; i < 400; ++i) {
        world.tick();
        const auto state = registry.get<sim::comp::AiBrain>(raw).state;
        if (state == sim::comp::AiState::Burrowed) saw_burrowed = true;
        if (saw_burrowed && state == sim::comp::AiState::Advancing) {
            resurfaced_after_burrow = true;
            break;
        }
    }
    REQUIRE(saw_burrowed);
    REQUIRE(resurfaced_after_burrow);
}

// ---------------------------------------------------------------------------
// Biofilm colony clumping
// ---------------------------------------------------------------------------

TEST_CASE("biofilm_colony's pulse flags nearby chaff Bacteria as clumped", "[game][enemies][ai]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();

    const EliteDef* def = roster.find_elite("biofilm_colony");
    REQUIRE(def != nullptr);
    const Vec2 colony_pos{20.0f, 20.0f};
    const EntityId id = roster.spawn_elite(world, def->id, colony_pos);
    REQUIRE(id.valid());

    // Force the ability off cooldown so the first pulse fires as soon as the
    // colony leaves Spawning, instead of waiting out the full cooldown.
    entt::registry& registry = world.ecs().registry();
    const entt::entity raw = world.ecs().from_id(id);
    registry.get<sim::comp::AiBrain>(raw).next_ability_cd = 0.0f;

    // A small cluster of Bacteria chaff right next to the colony, plus one
    // far away that must NOT get clumped (radius control).
    sim::ChaffBuffers& chaff = world.chaff();
    for (int i = 0; i < 6; ++i) {
        sim::ChaffSpawnParams p;
        p.position = colony_pos + Vec2{0.3f * static_cast<f32>(i), 0.0f};
        p.family = PathogenFamily::Bacteria;
        p.density = 1.0f;
        chaff.spawn(p);
    }
    sim::ChaffSpawnParams far;
    far.position = colony_pos + Vec2{80.0f, 0.0f};
    far.family = PathogenFamily::Bacteria;
    far.density = 1.0f;
    const sim::ChaffHandle far_handle = chaff.spawn(far);

    bool any_clumped = false;
    // Windup (0.5s) + Spawning clear (0.4s) + margin.
    for (int i = 0; i < 120 && !any_clumped; ++i) {
        world.tick();
        for (usize idx = 0; idx < chaff.count(); ++idx) {
            if (chaff.flags[idx] & sim::chaff_flags::kClumped) { any_clumped = true; break; }
        }
    }
    REQUIRE(any_clumped);

    const usize far_idx = chaff.resolve(far_handle);
    if (far_idx != sim::ChaffBuffers::npos) {
        REQUIRE_FALSE(chaff.flags[far_idx] & sim::chaff_flags::kClumped);
    }
}

// ---------------------------------------------------------------------------
// Tumour growth
// ---------------------------------------------------------------------------

TEST_CASE("register_systems installs a tumour growth system that grows radius toward max and stops",
          "[game][enemies][ai]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();
    roster.register_systems(world);

    entt::registry& registry = world.ecs().registry();
    const entt::entity e = registry.create();
    registry.emplace<sim::comp::TumorMass>(e, sim::comp::TumorMass{/*growth_rate*/ 3.0f,
                                                                    /*radius*/ 0.0f,
                                                                    /*max_radius*/ 3.0f});

    for (int i = 0; i < 30; ++i) world.tick();
    const f32 mid_radius = registry.get<sim::comp::TumorMass>(e).radius;
    REQUIRE(mid_radius > 0.0f);
    REQUIRE(mid_radius < 3.0f);

    for (int i = 0; i < 200; ++i) world.tick();
    const sim::comp::TumorMass& grown = registry.get<sim::comp::TumorMass>(e);
    REQUIRE(grown.radius == Catch::Approx(3.0f));

    // Keeps ticking without overshooting max_radius.
    for (int i = 0; i < 60; ++i) world.tick();
    REQUIRE(registry.get<sim::comp::TumorMass>(e).radius == Catch::Approx(3.0f));
}

TEST_CASE("spawn_elite attaches a growing TumorMass to the tumor_mass boss", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();

    const EliteDef* def = roster.find_elite("tumor_mass");
    REQUIRE(def != nullptr);
    REQUIRE(def->family == PathogenFamily::CancerCell);

    const EntityId id = roster.spawn_elite(world, def->id, Vec2{10.0f, 10.0f});
    REQUIRE(id.valid());

    entt::registry& registry = world.ecs().registry();
    const entt::entity raw = world.ecs().from_id(id);
    REQUIRE(registry.all_of<sim::comp::TumorMass>(raw));
    const sim::comp::TumorMass& tumor = registry.get<sim::comp::TumorMass>(raw);
    REQUIRE(tumor.growth_rate > 0.0f);
    REQUIRE(tumor.radius < tumor.max_radius);
}

// ---------------------------------------------------------------------------
// Allergen wave-modifier hook
// ---------------------------------------------------------------------------

TEST_CASE("allergen_overreaction submits a friendly-fire-flagged damage field", "[game][enemies]") {
    sim::SimWorld world = make_world();
    REQUIRE(world.damage().fields().empty());

    allergen_overreaction(world, Vec2{12.0f, 8.0f}, /*radius*/ 6.0f, /*kill_rate*/ 4.0f, /*duration*/ 2.0f);

    REQUIRE(world.damage().fields().size() == 1);
    const sim::DamageField& field = world.damage().fields().front();
    REQUIRE(field.friendly_fire);
    REQUIRE(field.shape == sim::FieldShape::Circle);
    REQUIRE(field.radius == Catch::Approx(6.0f));
    REQUIRE(field.kill_rate == Catch::Approx(4.0f));
    REQUIRE(field.lifetime == Catch::Approx(2.0f));
    REQUIRE(field.origin.x == Catch::Approx(12.0f));
    REQUIRE(field.origin.y == Catch::Approx(8.0f));
}

// ---------------------------------------------------------------------------
// Perf-adjacent regression: replication must not run away at bench scale.
// Mirrors app/Modes.cpp's chaff10k scenario (10,000 agents cycling through
// every family, max_chaff 16384) but with apply_to_tuning()'s real values
// plugged in via desc.chaff_tuning, since the headless --bench harness
// (src/app/Modes.cpp::build_world) does not currently call
// EnemyRoster::apply_to_tuning() at all — see this wave's report.
// ---------------------------------------------------------------------------

TEST_CASE("apply_to_tuning's replication_rate stays bounded at chaff10k bench scale",
          "[game][enemies][perf]") {
    EnemyRoster roster = make_roster();

    sim::SimWorld world;
    sim::SimDesc desc;
    desc.seed = 1234;
    desc.max_chaff = 16384; // matches bench_scenarios()'s chaff10k entry
    roster.apply_to_tuning(desc.chaff_tuning);
    world.init(desc, nullptr);
    // ChaffSystem defaults to a live goal at the origin with a 2-unit radius
    // (sim/chaff/ChaffSystem.h), which would otherwise quietly consume any
    // agent that happens to spawn near (0,0) as "reached the objective" and
    // confound a pure replication-vs-capacity measurement. A real level sets
    // this from its objective (game/level/Level.cpp); this synthetic scenario
    // has none, so disable it explicitly (radius <= 0 turns goal consumption
    // off per ChaffSystem::update).
    world.chaff_system().set_goal(Vec2{0.0f, 0.0f}, 0.0f);

    Rng& rng = world.rng();
    const Rect b = desc.world_bounds;
    for (u32 i = 0; i < 10000; ++i) {
        sim::ChaffSpawnParams p;
        p.position = Vec2{rng.range_f(b.min.x, b.max.x), rng.range_f(b.min.y, b.max.y)};
        p.velocity = rng.unit_disc() * 2.0f;
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        p.density = 1.0f;
        world.chaff().spawn(p);
    }
    REQUIRE(world.chaff().count() == 10000);

    for (int i = 0; i < 600; ++i) world.tick();

    const usize final_count = world.chaff().count();
    INFO("chaff count after 600 ticks (started at 10000, cap " << desc.max_chaff << "): " << final_count);
    // Never silently clips at capacity -- that would mean replication is
    // pushing harder than the population can absorb.
    REQUIRE(final_count < desc.max_chaff);
    // Not a collapse either. Uniform-random dense packing at t=0 (10,000
    // agents in a 256x144 box, ~1.9 units apart on average) makes the
    // separation pass push overlapping agents hard in the first few ticks,
    // and some of that gets flung past world_bounds and despawned -- a
    // pre-existing ChaffSystem dynamic (Wave 1B), not something
    // apply_to_tuning() introduces. A generous floor here just guards
    // against a genuine regression (e.g. replication accidentally negative,
    // or every family's max_speed zeroed).
    REQUIRE(final_count > desc.max_chaff / 4);
}

// ---------------------------------------------------------------------------
// register_systems() idempotency
// ---------------------------------------------------------------------------

TEST_CASE("register_systems is idempotent across repeated calls on the same world", "[game][enemies]") {
    EnemyRoster roster = make_roster();
    sim::SimWorld world = make_world();

    roster.register_systems(world);
    roster.register_systems(world);
    roster.register_systems(world);

    // If named::install() or the archetype table had been re-registered on
    // every call, tumour growth would run 3x as fast; spawning an elite would
    // also have failed noisily if the archetype table had grown unbounded.
    entt::registry& registry = world.ecs().registry();
    const entt::entity e = registry.create();
    registry.emplace<sim::comp::TumorMass>(e, sim::comp::TumorMass{1.0f, 0.0f, 1.0f});
    world.tick();
    REQUIRE(registry.get<sim::comp::TumorMass>(e).radius == Catch::Approx(1.0f / 60.0f));

    const EliteDef* def = roster.find_elite("parasite_burrower");
    REQUIRE(def != nullptr);
    REQUIRE(roster.spawn_elite(world, def->id, Vec2{0.0f, 0.0f}).valid());
}
