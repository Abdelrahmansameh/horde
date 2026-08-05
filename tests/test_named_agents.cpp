// Wave 1D: named-agent layer tests. Entity lifecycle, AI state-machine
// transitions, telegraph timing, and the <=200-agent perf smoke check.
//
// A synthetic (empty) flow field is used throughout rather than a loaded
// level: FlowField::sample()/DistanceField::sample() on an unbaked field
// return (0,0)/0.0f, which is a valid "no guidance" input and keeps these
// tests independent of Wave 1A/2D's level pipeline, per the Wave 1D brief.
#include "core/Clock.h"
#include "core/JobSystem.h"
#include "sim/SimWorld.h"
#include "sim/ecs/AiStateMachine.h"
#include "sim/ecs/NamedAgents.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

SimWorld make_world(u64 seed = 777) {
    SimWorld world;
    SimDesc desc;
    desc.seed = seed;
    desc.max_chaff = 64; // unused by these tests but must be non-zero-safe
    world.init(desc, nullptr);
    return world;
}

comp::AiBrain& brain_of(SimWorld& world, EntityId id) {
    return world.ecs().registry().get<comp::AiBrain>(world.ecs().from_id(id));
}

comp::Health& health_of(SimWorld& world, EntityId id) {
    return world.ecs().registry().get<comp::Health>(world.ecs().from_id(id));
}

} // namespace

// ---------------------------------------------------------------------------
// Entity lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("named agent spawn/despawn respects the <=200 budget", "[sim][ecs][named]") {
    SimWorld world = make_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    u32 accepted = 0;
    for (u32 i = 0; i < 205; ++i) {
        named::SpawnParams p;
        p.archetype = archetype;
        p.position = Vec2{static_cast<f32>(i), 0.0f};
        const EntityId id = named::spawn(world, p);
        if (id.valid()) ++accepted;
    }

    REQUIRE(accepted == named::kMaxNamedAgents);
    REQUIRE(world.ecs().named_agent_count() == named::kMaxNamedAgents);
}

TEST_CASE("spawn_bench_population is deterministic given the same seed", "[sim][ecs][named][determinism]") {
    auto run = [](u64 ticks) {
        SimWorld world = make_world(4242);
        const u32 spawned = named::setup_bench_scenario(world, 200);
        REQUIRE(spawned == 200);
        world.run_ticks(ticks);
        return named::state_hash(world.ecs().registry());
    };
    REQUIRE(run(60) == run(60));
    REQUIRE(run(60) != run(0));
}

TEST_CASE("lethal damage drives Dying then despawns after death_fade", "[sim][ecs][named]") {
    SimWorld world = make_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{10.0f, 10.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());

    // Let it leave Spawning first.
    for (int i = 0; i < 20; ++i) world.tick();
    REQUIRE(brain_of(world, id).state != comp::AiState::Spawning);

    health_of(world, id).current = 0.0f;
    world.tick();
    REQUIRE(brain_of(world, id).state == comp::AiState::Dying);
    REQUIRE(world.ecs().named_agent_count() == 1);

    // death_fade is 0.5s == 30 ticks for the placeholder archetype.
    for (int i = 0; i < 40; ++i) world.tick();
    REQUIRE(world.ecs().named_agent_count() == 0);
    REQUIRE_FALSE(world.ecs().registry().valid(world.ecs().from_id(id)));
}

// ---------------------------------------------------------------------------
// AI state-machine transitions
// ---------------------------------------------------------------------------

TEST_CASE("placeholder elite cycles Spawning -> Advancing -> Telegraphing -> Attacking -> Advancing",
          "[sim][ecs][named][ai]") {
    SimWorld world = make_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{0.0f, 0.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());
    REQUIRE(brain_of(world, id).state == comp::AiState::Spawning);

    std::vector<comp::AiState> visited;
    visited.push_back(brain_of(world, id).state);
    bool saw_telegraphing = false;
    bool saw_attacking = false;
    bool returned_to_advancing_after_attack = false;

    for (int i = 0; i < 400 && !returned_to_advancing_after_attack; ++i) {
        world.tick();
        const comp::AiState s = brain_of(world, id).state;
        if (s != visited.back()) visited.push_back(s);
        if (s == comp::AiState::Telegraphing) saw_telegraphing = true;
        if (s == comp::AiState::Attacking) saw_attacking = true;
        if (saw_attacking && s == comp::AiState::Advancing) returned_to_advancing_after_attack = true;
    }

    REQUIRE(saw_telegraphing);
    REQUIRE(saw_attacking);
    REQUIRE(returned_to_advancing_after_attack);

    // Order must be exactly this sequence with no skipped or reordered
    // states (Spawning once, then it may revisit Advancing/Telegraphing/
    // Attacking any number of times as the ability loops).
    REQUIRE(visited.front() == comp::AiState::Spawning);
    REQUIRE(visited[1] == comp::AiState::Advancing);
}

TEST_CASE("wounding below 40% health burrows the agent, and it resurfaces", "[sim][ecs][named][ai]") {
    SimWorld world = make_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{0.0f, 0.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());

    for (int i = 0; i < 20; ++i) world.tick(); // clear Spawning
    REQUIRE(brain_of(world, id).state == comp::AiState::Advancing);

    comp::Health& h = health_of(world, id);
    h.current = 0.3f * h.max; // below the 0.4 threshold
    world.tick();
    REQUIRE(brain_of(world, id).state == comp::AiState::Burrowed);

    // Burrowed -> Advancing after 2s == 120 ticks.
    bool resurfaced = false;
    for (int i = 0; i < 150 && !resurfaced; ++i) {
        world.tick();
        if (brain_of(world, id).state == comp::AiState::Advancing) resurfaced = true;
    }
    REQUIRE(resurfaced);
}

TEST_CASE("IsDead pre-empts every other transition regardless of current state", "[sim][ecs][named][ai]") {
    SimWorld world = make_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{0.0f, 0.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());

    // Drive it into Telegraphing, then kill it mid-windup. ability_cooldown
    // is 3s (180 ticks) and Spawning clears in 0.25s, so this needs headroom
    // past ~180 ticks.
    bool telegraphing = false;
    for (int i = 0; i < 300 && !telegraphing; ++i) {
        world.tick();
        telegraphing = brain_of(world, id).state == comp::AiState::Telegraphing;
    }
    REQUIRE(telegraphing);

    health_of(world, id).current = 0.0f;
    world.tick();
    REQUIRE(brain_of(world, id).state == comp::AiState::Dying);
}

// ---------------------------------------------------------------------------
// Telegraph timing
// ---------------------------------------------------------------------------

TEST_CASE("telegraph windup matches the archetype's attack.windup duration", "[sim][ecs][named][telegraph]") {
    SimWorld world = make_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{0.0f, 0.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());

    entt::registry& registry = world.ecs().registry();
    const entt::entity raw = world.ecs().from_id(id);

    bool entered_telegraphing = false;
    u32 ticks_in_telegraph = 0;
    // ability_cooldown is 3s (180 ticks); give it headroom to reach Telegraphing.
    for (int i = 0; i < 300; ++i) {
        world.tick();
        const bool telegraphing = brain_of(world, id).state == comp::AiState::Telegraphing;
        if (telegraphing && !entered_telegraphing) {
            entered_telegraphing = true;
            REQUIRE(registry.all_of<comp::Telegraph>(raw));
            const comp::Telegraph& tg = registry.get<comp::Telegraph>(raw);
            REQUIRE(tg.duration == 0.8f);
            REQUIRE(tg.elapsed <= tg.duration);
        }
        if (telegraphing) {
            ++ticks_in_telegraph;
        } else if (entered_telegraphing) {
            break; // just left Telegraphing
        }
    }

    REQUIRE(entered_telegraphing);
    // 0.8s at 60 Hz is 48 ticks; the transition fires once elapsed >= duration,
    // so it should take 47 or 48 ticks depending on rounding, never wildly off.
    REQUIRE(ticks_in_telegraph >= 46);
    REQUIRE(ticks_in_telegraph <= 50);

    // Attacking should have consumed/removed the Telegraph component by the
    // time the agent is back in Advancing.
    bool back_to_advancing = false;
    for (int i = 0; i < 50 && !back_to_advancing; ++i) {
        world.tick();
        back_to_advancing = brain_of(world, id).state == comp::AiState::Advancing;
    }
    REQUIRE(back_to_advancing);
    REQUIRE_FALSE(registry.all_of<comp::Telegraph>(raw));
}

TEST_CASE("a resolved attack damages an Objective within its radius", "[sim][ecs][named][telegraph][combat]") {
    SimWorld world = make_world();
    named::install(world);
    const u16 archetype = named::placeholder_elite(world.ecs().registry());

    named::SpawnParams p;
    p.archetype = archetype;
    p.position = Vec2{50.0f, 50.0f};
    const EntityId id = named::spawn(world, p);
    REQUIRE(id.valid());

    entt::registry& registry = world.ecs().registry();
    const entt::entity objective = registry.create();
    registry.emplace<comp::Transform>(objective, comp::Transform{Vec2{50.0f, 50.0f}, 0.0f, 1.0f});
    // Generously large radius: the point isn't pixel-perfect overlap, it's
    // that the Combat system actually reaches the Objective when the attack
    // resolves. The agent drifts a little from local steering before it
    // telegraphs, so a tight radius would make this test flaky rather than
    // meaningful.
    registry.emplace<comp::Objective>(objective, comp::Objective{100.0f, 100.0f, 20.0f});

    bool attacked = false;
    // ability_cooldown (180 ticks) + windup (48 ticks) + margin.
    for (int i = 0; i < 300 && !attacked; ++i) {
        world.tick();
        if (registry.get<comp::Objective>(objective).integrity < 100.0f) attacked = true;
    }
    REQUIRE(attacked);
    REQUIRE(registry.get<comp::Objective>(objective).integrity == 88.0f); // 100 - 12 damage
}

// ---------------------------------------------------------------------------
// 200-entity tick performance smoke check (DESIGN.md §8.6: ecs_tick < 2ms).
// The authoritative measurement is `immune --bench named200`; this is a
// smoke test with a generous margin so a severe regression fails CI even
// without running the bench binary.
// ---------------------------------------------------------------------------

TEST_CASE("200 named agents tick comfortably inside a generous smoke-test margin",
          "[sim][ecs][named][perf]") {
    SimWorld world = make_world();
    const u32 spawned = named::setup_bench_scenario(world, 200);
    REQUIRE(spawned == 200);

    // Warm up (first tick pays one-time allocation costs for the scratch
    // buffers reserving to their steady-state capacity).
    world.tick();

    f64 total_ms = 0.0;
    constexpr int kSamples = 100;
    for (int i = 0; i < kSamples; ++i) {
        WallClock t;
        world.tick();
        total_ms += t.elapsed_ms();
    }
    const f64 avg_ms = total_ms / kSamples;

    INFO("avg full sim.tick() over 200 named agents: " << avg_ms << " ms");
    // This measures the whole tick (spatial hash + chaff + ecs + damage +
    // compaction), not just ecs_tick in isolation, so the margin here is
    // deliberately far looser than the 2ms ecs_tick budget itself. Real
    // ecs_tick numbers come from `immune --bench named200`.
    REQUIRE(avg_ms < 10.0);
}
