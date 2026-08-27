// System ordering is a determinism guarantee, not a convenience.
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace immune;
using namespace immune::sim;

TEST_CASE("systems run in phase then sort-key order", "[sim][ecs][determinism]") {
    SimWorld world;
    SimDesc desc;
    desc.max_chaff = 64;
    world.init(desc, nullptr);

    std::vector<std::string> order;
    EcsWorld& ecs = world.ecs();

    // Registered deliberately out of order.
    ecs.add_system(SystemPhase::Combat,     "combat",  0, [&](SystemContext&) { order.push_back("combat"); });
    ecs.add_system(SystemPhase::PreUpdate,  "pre_b",  10, [&](SystemContext&) { order.push_back("pre_b"); });
    ecs.add_system(SystemPhase::PostUpdate, "post",    0, [&](SystemContext&) { order.push_back("post"); });
    ecs.add_system(SystemPhase::PreUpdate,  "pre_a",  -5, [&](SystemContext&) { order.push_back("pre_a"); });
    ecs.add_system(SystemPhase::Movement,   "move",    0, [&](SystemContext&) { order.push_back("move"); });
    ecs.add_system(SystemPhase::AI,         "ai",      0, [&](SystemContext&) { order.push_back("ai"); });

    world.tick(nullptr);

    const std::vector<std::string> expected = {"pre_a", "pre_b", "ai", "move", "combat", "post"};
    REQUIRE(order == expected);

    // And identical on every subsequent tick.
    order.clear();
    world.tick(nullptr);
    REQUIRE(order == expected);
}

TEST_CASE("named_agent_count counts only NamedAgent entities", "[sim][ecs]") {
    EcsWorld ecs;
    auto& reg = ecs.registry();
    for (int i = 0; i < 5; ++i) {
        const auto e = reg.create();
        reg.emplace<comp::Transform>(e);
        reg.emplace<comp::NamedAgent>(e);
    }
    for (int i = 0; i < 3; ++i) {
        const auto e = reg.create();
        reg.emplace<comp::Tower>(e);
    }
    REQUIRE(ecs.named_agent_count() == 5);
}

TEST_CASE("EntityId round-trips through the registry handle", "[sim][ecs]") {
    EcsWorld ecs;
    const auto e = ecs.registry().create();
    const EntityId id = ecs.to_id(e);
    REQUIRE(id.valid());
    REQUIRE(ecs.from_id(id) == e);
    REQUIRE(!ecs.to_id(entt::null).valid());
}

TEST_CASE("sim tick advances deterministically and hashes stably",
          "[sim][determinism]") {
    auto run = [](u64 ticks) {
        SimWorld w;
        SimDesc d;
        d.seed = 4242;
        d.max_chaff = 512;
        w.init(d, nullptr);
        for (u32 i = 0; i < 100; ++i) {
            ChaffSpawnParams p;
            p.position = Vec2{w.rng().range_f(0.0f, 100.0f), w.rng().range_f(0.0f, 100.0f)};
            p.family = static_cast<PathogenFamily>(i % kFamilyCount);
            w.chaff().spawn(p);
        }
        w.run_ticks(ticks);
        return w.state_hash();
    };
    REQUIRE(run(120) == run(120));
    REQUIRE(run(120) != run(0));
}

TEST_CASE("sim snapshot reflects the chaff store", "[sim]") {
    SimWorld world;
    SimDesc desc;
    desc.max_chaff = 128;
    world.init(desc, nullptr);
    for (int i = 0; i < 10; ++i) {
        ChaffSpawnParams p;
        p.family = PathogenFamily::Bacteria;
        p.density = 2.0f;
        world.chaff().spawn(p);
    }
    const auto s = world.snapshot();
    REQUIRE(s.chaff_count == 10);
    REQUIRE(s.total_density == 20.0f);
    REQUIRE(s.chaff_by_family[static_cast<u32>(PathogenFamily::Bacteria)] == 10);
    REQUIRE(s.tick == 0);
}

// ---------------------------------------------------------------------------
// Starting a level is a full reset (App re-uses one SimWorld for every level)
// ---------------------------------------------------------------------------

TEST_CASE("init() drops the previous level's systems", "[sim][ecs]") {
    SimWorld world;
    SimDesc desc;
    desc.max_chaff = 64;
    world.init(desc, nullptr);

    int runs = 0;
    world.ecs().add_system(SystemPhase::Combat, "counter", 0,
                           [&](SystemContext&) { ++runs; });
    world.tick(nullptr);
    REQUIRE(runs == 1);

    // Starting another level. Whoever registered that system re-registers it
    // after init(); until they do, it must not run at all -- and when they do,
    // it must run once per tick, not twice.
    world.init(desc, nullptr);
    world.tick(nullptr);
    REQUIRE(runs == 1);

    world.ecs().add_system(SystemPhase::Combat, "counter", 0,
                           [&](SystemContext&) { ++runs; });
    world.tick(nullptr);
    REQUIRE(runs == 2);
}

TEST_CASE("init() drops the previous level's registry context", "[sim][ecs]") {
    struct InstallGuard { bool installed = false; };

    SimWorld world;
    SimDesc desc;
    desc.max_chaff = 64;
    world.init(desc, nullptr);

    // The pattern EnemyRoster/NamedAgents use to install once per world. A
    // guard that survived init() would leave the new level with no named-agent
    // systems at all, since init() has just dropped the ones it installed.
    world.ecs().registry().ctx().emplace<InstallGuard>().installed = true;
    REQUIRE(world.ecs().registry().ctx().emplace<InstallGuard>().installed);

    world.init(desc, nullptr);
    REQUIRE(!world.ecs().registry().ctx().emplace<InstallGuard>().installed);
}
