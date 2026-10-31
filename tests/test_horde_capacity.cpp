#include "core/CapacityDefaults.h"
#include "config/ConfigStore.h"
#include "game/config/GameConfig.h"
#include "game/enemies/EnemyConfigApply.h"
#include "render/ChaffBatcher.h"
#include "sim/SimWorld.h"
#include "sim/hostile/HostileAttacks.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace immune;

TEST_CASE("shipped horde capacity matches simulation render and hostile defaults", "[config][capacity]") {
    config::ConfigStore store;
    game::GameConfig shipped;
    std::string error;
    REQUIRE(game::load_game_config(store, "assets/config", shipped, error));
    CHECK(shipped.sim.capacities.max_chaff == kDefaultChaffCapacity);
    CHECK(game::default_game_config().sim.capacities.max_chaff == kDefaultChaffCapacity);
    CHECK(sim::SimDesc{}.max_chaff == kDefaultChaffCapacity);
    CHECK(render::RendererDesc{}.max_chaff_instances == kDefaultChaffCapacity);
    CHECK(render::ChaffBatchParams{}.per_family_capacity == kDefaultChaffCapacity);
    CHECK(shipped.sim.squads.max_squads == kDefaultSquadCapacity);
    const auto hostile = game::hostile_tuning(shipped.sim.hostile);
    CHECK(hostile.max_harpoons == kDefaultHarpoonCapacity);
    CHECK(hostile.max_toxin_shots == kDefaultToxinShotCapacity);
    CHECK(render::RendererDesc{}.max_harpoon_instances == hostile.max_harpoons);
    config::Registry registry;
    game::bind_game_config(registry, shipped);
    REQUIRE(registry.set("sim.hostile.max_harpoons", "1200", error));
    REQUIRE(registry.set("sim.hostile.max_toxin_shots", "2400", error));
    CHECK(game::hostile_tuning(shipped.sim.hostile).max_harpoons == 1200);
    CHECK(game::hostile_tuning(shipped.sim.hostile).max_toxin_shots == 2400);
}

TEST_CASE("a full 64k spore population retains high indices in storage spatial queries and draw batches",
          "[sim][render][capacity]") {
    sim::ChaffBuffers chaff;
    chaff.reserve(kDefaultChaffCapacity);
    sim::ChaffHandle last;
    for (u32 i = 0; i < kDefaultChaffCapacity; ++i) {
        sim::ChaffSpawnParams p;
        p.family = PathogenFamily::Spore;
        p.position = Vec2{static_cast<f32>(i % 256), static_cast<f32>(i / 256)};
        p.density = 3.0f;
        p.size_scale = 1.0f;
        last = chaff.spawn(p);
        REQUIRE(last.valid());
    }
    REQUIRE(last.index == 65535);
    REQUIRE(chaff.resolve(last) == 65535);
    CHECK_FALSE(chaff.spawn({}).valid());
    CHECK(chaff.family_count(PathogenFamily::Spore) == 65536);

    sim::SpatialHash hash;
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 256.0f}};
    hash.configure({bounds, 4.0f});
    hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count());
    std::vector<u32> found;
    hash.query_rect(bounds, found);
    REQUIRE(found.size() == 65536);
    std::sort(found.begin(), found.end());
    CHECK(found.back() == 65535);
    CHECK(std::adjacent_find(found.begin(), found.end()) == found.end());

    render::OccupancyGrid occupancy;
    occupancy.bounds = bounds;
    occupancy.cell_size = hash.cell_size();
    occupancy.dims = hash.grid_dims();
    occupancy.occupancy = hash.occupancy();
    render::ChaffBatchParams params;
    params.lod_blob_enabled = false;
    std::vector<render::ChaffInstance> instances(kFamilyCount * params.per_family_capacity);
    const auto result = render::build_chaff_batches(chaff, occupancy, params, instances.data(), nullptr);
    CHECK(result.instances_total == 65536);
    CHECK(result.family_counts[static_cast<u32>(PathogenFamily::Spore)] == 65536);
    CHECK(result.instances_dropped == 0);
    chaff.kill(0);
    CHECK(chaff.compact() == 1);
    CHECK(chaff.resolve(last) == 0);
    CHECK(chaff.family_count(PathogenFamily::Spore) == 65535);
}

TEST_CASE("the squad registry covers a full 64k horde in 48-agent cohorts", "[sim][capacity][squads]") {
    sim::SquadRegistry squads;
    sim::SquadPath path;
    path.id = "capacity"; path.lane_id = "main";
    path.points = {Vec2{0.0f, 0.0f}, Vec2{100.0f, 0.0f}};
    path.arc = {0.0f, 100.0f};
    squads.set_paths({path});
    constexpr u32 needed = (kDefaultChaffCapacity + 47) / 48;
    static_assert(needed > 1024);
    for (u32 i = 0; i < needed; ++i) REQUIRE(squads.create_squad(0, Vec2{0.0f, 0.0f}) != sim::kNoSquad);
}

namespace {
struct HostileCapacityFixture {
    sim::ChaffBuffers chaff;
    sim::SwarmerBuffers swarmers;
    sim::SpatialHash hash;
    sim::FriendlyTowerList towers;
    sim::HostileSystem hostile;
    sim::HostileTuning tuning;
    HostileCapacityFixture(u32 count, PathogenFamily family) {
        chaff.reserve(count);
        swarmers.reserve(1);
        hash.configure({{Vec2{0.0f, 0.0f}, Vec2{160.0f, 160.0f}}, 4.0f});
        for (u32 i = 0; i < count; ++i) {
            sim::ChaffSpawnParams p;
            p.family = family;
            p.position = Vec2{40.0f, 80.0f};
            REQUIRE(chaff.spawn(p).valid());
            chaff.harpoon_scan[i] = 0.0f;
            chaff.toxin_rounds[i] = 1;
            chaff.toxin_reload[i] = -1.0f;
            chaff.toxin_cooldown[i] = 0.0f;
        }
        sim::FriendlyTower target;
        target.id = EntityId{1}; target.position = Vec2{70.0f, 80.0f};
        target.radius = 3.0f; target.health = 100000.0f;
        towers.add(target); towers.sort();
        tuning.enabled = true;
    }
    sim::HostileStats step() {
        hostile.set_tuning(tuning);
        hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count());
        return hostile.update(chaff, hash, swarmers, towers, sim::TissueMask{}, kFixedDt, nullptr);
    }
};
}

TEST_CASE("spore hose storage supports more than 1024 simultaneous attackers and reports deferral", "[sim][hostile][capacity]") {
    HostileCapacityFixture f(1100, PathogenFamily::Spore);
    auto& hp = f.tuning.harpoon[static_cast<u32>(PathogenFamily::Spore)];
    hp.enabled = true; hp.range = 100.0f;
    SECTION("new default stores the whole group") {
        const auto stats = f.step();
        CHECK(f.hostile.harpoons().size() == 1100);
        CHECK(stats.harpoons_denied_capacity == 0);
    }
    SECTION("a deliberately small pool leaves blocked shooters ready and observable") {
        f.tuning.max_harpoons = 1024;
        const auto stats = f.step();
        CHECK(f.hostile.harpoons().size() == 1024);
        CHECK(stats.harpoons_denied_capacity == 76);
        CHECK(f.chaff.harpoon_state[1099] == sim::harpoon_state::kReady);
    }
}

TEST_CASE("toxin storage deferral keeps ammunition and is included in combat metrics", "[sim][hostile][capacity]") {
    HostileCapacityFixture f(2, PathogenFamily::Bacteria);
    auto& bacteria = f.tuning.family[static_cast<u32>(PathogenFamily::Bacteria)];
    bacteria.toxin_damage = 1.0f; bacteria.toxin_range = 100.0f;
    f.tuning.max_toxin_shots = 0;
    const auto blocked = f.step();
    CHECK(blocked.toxin_shots_denied_capacity == 2);
    CHECK(blocked.toxin_fired == 0);
    CHECK(f.chaff.toxin_rounds[0] == 1);
    CHECK(f.chaff.toxin_rounds[1] == 1);
    f.tuning.max_toxin_shots = kDefaultToxinShotCapacity;
    const auto allowed = f.step();
    CHECK(allowed.toxin_fired == 2);
    CHECK(allowed.toxin_shots_denied_capacity == 0);
    sim::CombatMetrics metrics;
    metrics.enable(true, 1);
    metrics.record_hostile(0, 0, 0, 0, 0, 0, blocked.toxin_shots_denied_capacity, 76);
    CHECK(metrics.totals().toxin_shots_denied_capacity == 2);
    CHECK(metrics.totals().harpoons_denied_capacity == 76);
}
