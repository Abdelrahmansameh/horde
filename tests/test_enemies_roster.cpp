// Wave 2C: enemy roster tests — family table and chaff tuning derivation.
// The elite table is empty pending a redesign, so what is left to assert about
// it is that it stays empty and that its framework still installs cleanly.
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

    // Virus replicates; Bacteria is a plain chaff family with no behaviour of
    // its own. Those are the only two families, and `replicates` is the only
    // behaviour switch left on FamilyDef.
    REQUIRE(roster.family(PathogenFamily::Virus).replicates);
    REQUIRE_FALSE(roster.family(PathogenFamily::Bacteria).replicates);
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

    // No disagreement possible by construction: the `replicates` flag on the
    // family table is exactly what turns this knob on.
    REQUIRE(tuning.family[static_cast<u32>(PathogenFamily::Virus)].replication_rate > 0.0f);

    // Everyone else's replication_rate stays at zero, and no family drifts.
    for (u32 i = 0; i < kFamilyCount; ++i) {
        if (static_cast<PathogenFamily>(i) != PathogenFamily::Virus) {
            REQUIRE(tuning.family[i].replication_rate == 0.0f);
        }
        REQUIRE(tuning.family[i].drift_bias == 0.0f);
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
// Elite table
// ---------------------------------------------------------------------------

TEST_CASE("the elite table is empty and spawn_elite fails cleanly", "[game][enemies]") {
    // Every elite the roster used to ship has been removed pending a redesign.
    // The framework around them is still wired: register_systems() installs the
    // named-agent tier, and spawn_elite() reports failure rather than crashing.
    EnemyRoster roster = make_roster();
    REQUIRE(roster.elites().empty());
    REQUIRE(roster.find_elite("no_such_elite") == nullptr);

    sim::SimWorld world = make_world();
    roster.register_systems(world);
    REQUIRE_FALSE(roster.spawn_elite(world, 1, Vec2{5.0f, 5.0f}).valid());
    REQUIRE_FALSE(roster.spawn_elite(world, 999, Vec2{5.0f, 5.0f}).valid());
}

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
    world.chaff_system().set_goal(Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f});
    const Rect b = desc.world_bounds;
    // This test measures replication, not despawning. Family silhouettes are
    // now substantially larger, so contact relaxation can legitimately move
    // edge-seeded bodies outside the small synthetic benchmark rectangle.
    // Keep them alive with a generous despawn envelope while leaving the
    // spatial density and every movement/replication rule unchanged.
    world.chaff_system().set_world_bounds(
        Rect{Vec2{b.min.x - 1000.0f, b.min.y - 1000.0f},
             Vec2{b.max.x + 1000.0f, b.max.y + 1000.0f}});

    Rng& rng = world.rng();
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

    // If named::install() had been re-run on every call, the named-agent
    // systems would be registered three times over and each would run three
    // times per tick. Ticking a world with nothing in it must stay clean.
    world.tick();
    world.tick();
    REQUIRE(world.ecs().registry().storage<sim::comp::NamedAgent>().size() == 0u);
}
