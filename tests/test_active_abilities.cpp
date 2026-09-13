// Tests for the new game/abilities/ module (DESIGN.md §5.6).
#include "game/abilities/ActiveAbilities.h"

#include "game/towers/TowerSystem.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace immune;
using namespace immune::sim;
using namespace immune::game;

namespace {
SimWorld make_world() {
    SimWorld world;
    SimDesc desc;
    desc.seed = 1;
    desc.max_chaff = 4096;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}};
    world.init(desc, nullptr);
    return world;
}
} // namespace

TEST_CASE("all abilities start ready with their configured cooldowns", "[abilities]") {
    ActiveAbilitySystem abilities;
    abilities.load_defaults();
    for (u32 i = 0; i < kAbilityCount; ++i) {
        const auto id = static_cast<AbilityId>(i);
        REQUIRE(abilities.ready(id));
        REQUIRE(abilities.status(id).cooldown_remaining == 0.0f);
        REQUIRE(abilities.def(id).cooldown_seconds > 0.0f);
    }
}

TEST_CASE("casting an ability starts its cooldown and blocks recast until it elapses",
          "[abilities]") {
    SimWorld world = make_world();
    ActiveAbilitySystem abilities;
    abilities.load_defaults();

    REQUIRE(abilities.cast(world, AbilityId::HistamineFlare, Vec2{50.0f, 50.0f}));
    REQUIRE_FALSE(abilities.ready(AbilityId::HistamineFlare));
    REQUIRE_FALSE(abilities.cast(world, AbilityId::HistamineFlare, Vec2{50.0f, 50.0f}));

    const f32 total = abilities.def(AbilityId::HistamineFlare).cooldown_seconds;
    abilities.tick(total - 0.01f);
    REQUIRE_FALSE(abilities.ready(AbilityId::HistamineFlare));
    abilities.tick(0.02f);
    REQUIRE(abilities.ready(AbilityId::HistamineFlare));
    REQUIRE(abilities.cast(world, AbilityId::HistamineFlare, Vec2{50.0f, 50.0f}));
}

TEST_CASE("abilities have independent cooldowns", "[abilities]") {
    SimWorld world = make_world();
    ActiveAbilitySystem abilities;
    abilities.load_defaults();

    REQUIRE(abilities.cast(world, AbilityId::ComplementCascadeBurst, Vec2{10.0f, 10.0f}));
    REQUIRE_FALSE(abilities.ready(AbilityId::ComplementCascadeBurst));
    REQUIRE(abilities.ready(AbilityId::HistamineFlare));
    REQUIRE(abilities.ready(AbilityId::FeverResponse));
}

TEST_CASE("Complement Cascade Burst and Histamine Flare submit a real DamageField",
          "[abilities][damage]") {
    SimWorld world = make_world();
    ActiveAbilitySystem abilities;
    abilities.load_defaults();

    REQUIRE(world.damage().fields().empty());
    abilities.cast(world, AbilityId::ComplementCascadeBurst, Vec2{20.0f, 20.0f});
    REQUIRE(world.damage().fields().size() == 1);
    REQUIRE(world.damage().fields()[0].shape == FieldShape::Chain);
    REQUIRE(world.damage().fields()[0].origin.x == 20.0f);

    abilities.cast(world, AbilityId::HistamineFlare, Vec2{30.0f, 30.0f});
    REQUIRE(world.damage().fields().size() == 2);
    REQUIRE(world.damage().fields()[1].shape == FieldShape::Circle);
    REQUIRE(world.damage().fields()[1].lifetime > 0.0f);
}

TEST_CASE("Fever Response relieves every placed tower's current cooldown, and only that",
          "[abilities]") {
    SimWorld world = make_world();
    ActiveAbilitySystem abilities;
    abilities.load_defaults();

    auto& registry = world.ecs().registry();
    const auto e1 = registry.create();
    registry.emplace<comp::Tower>(e1, comp::Tower{TowerType::Macrophage, 1, 8.0f, 5.0f, 1.0f});
    const auto e2 = registry.create();
    registry.emplace<comp::Tower>(e2, comp::Tower{TowerType::NKCell, 1, 8.0f, 1.0f, 1.0f});

    REQUIRE(world.damage().fields().empty());
    REQUIRE(abilities.cast(world, AbilityId::FeverResponse, Vec2{0.0f, 0.0f}));

    const f32 relief = abilities.def(AbilityId::FeverResponse).fever_cooldown_relief;
    REQUIRE(registry.get<comp::Tower>(e1).cooldown == 5.0f - relief);
    REQUIRE(registry.get<comp::Tower>(e2).cooldown == 0.0f); // clamped, was already below relief
    REQUIRE(world.damage().fields().empty()); // no field submitted for this ability
}

TEST_CASE("ability_name returns a real string for every id", "[abilities]") {
    for (u32 i = 0; i < kAbilityCount; ++i) {
        const std::string name = ability_name(static_cast<AbilityId>(i));
        REQUIRE_FALSE(name.empty());
        REQUIRE(name != "?");
    }
}

// ---------------------------------------------------------------------------
// Fibrin Clot: the one ability with a footprint.
//
// Scene: one straight lane, `lane_height` cells tall, running the full 60-cell
// width at cell_size 1, flowing left to right. The default clot is a 14 x 3
// bar laid ACROSS the flow, so on a 30-tall lane it leaves 8 cells open on
// either side, and on a 12-tall lane it would seal the lane outright -- which
// is the case the cast must refuse.
// ---------------------------------------------------------------------------
namespace {

constexpr i32 kLaneW = 60;
constexpr Vec2 kLaneGoal{57.0f, 15.0f};

SimWorld make_lane(i32 lane_height) {
    SimWorld world;
    SimDesc desc;
    desc.seed = 7;
    desc.max_chaff = 1024;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{static_cast<f32>(kLaneW), 30.0f}};
    world.init(desc, nullptr);

    TissueMask& mask = world.tissue();
    mask.resize(kLaneW, 30, 1.0f, Vec2{0.0f, 0.0f});
    const i32 y0 = 15 - lane_height / 2;
    for (i32 y = y0; y < y0 + lane_height; ++y)
        for (i32 x = 0; x < kLaneW; ++x) mask.set_walkable(x, y, true);

    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goals = {sim::FlowGoal{mask.world_to_cell(kLaneGoal)}};
    world.flow().bake(mask, fdesc);
    return world;
}

u32 walkable_count(const TissueMask& mask) {
    u32 n = 0;
    for (i32 y = 0; y < mask.height(); ++y)
        for (i32 x = 0; x < mask.width(); ++x) n += mask.walkable(x, y) ? 1u : 0u;
    return n;
}

u32 barrier_count(SimWorld& world) {
    u32 n = 0;
    for ([[maybe_unused]] auto e : world.ecs().registry().view<comp::Barrier>()) ++n;
    return n;
}

/// Runs only the ECS half of the tick, which is where the clot's clock lives.
void step_ecs(SimWorld& world, u32 ticks) {
    for (u32 i = 0; i < ticks; ++i) {
        SystemContext ctx{world, world.ecs().registry(), world.rng(), kFixedDt, world.tick_index()};
        world.ecs().tick(ctx);
    }
}

} // namespace

TEST_CASE("Fibrin Clot carves a bar across the local flow and leaves a way round it",
          "[abilities][clot][flowfield]") {
    SimWorld world = make_lane(30);
    ActiveAbilitySystem abilities;
    abilities.load_defaults();
    const AbilityDef& d = abilities.def(AbilityId::FibrinClot);
    REQUIRE(d.barrier_half_length > d.barrier_half_width);

    const Vec2 at{30.0f, 15.0f};
    const u32 before = walkable_count(world.tissue());
    REQUIRE(abilities.cast(world, AbilityId::FibrinClot, at));
    REQUIRE_FALSE(abilities.ready(AbilityId::FibrinClot));

    const TissueMask& mask = world.tissue();
    const i32 hl = static_cast<i32>(d.barrier_half_length);
    const i32 hw = static_cast<i32>(d.barrier_half_width);
    // The flow runs +x here, so the bar stands vertically: thin in x, long in y.
    REQUIRE_FALSE(mask.walkable(30, 15));
    REQUIRE_FALSE(mask.walkable(30, 15 + hl - 1));
    REQUIRE_FALSE(mask.walkable(30, 15 - hl + 1));
    REQUIRE(mask.walkable(30, 15 + hl + 2));
    REQUIRE(mask.walkable(30, 15 - hl - 2));
    REQUIRE(mask.walkable(30 + hw + 2, 15));
    REQUIRE(mask.walkable(30 - hw - 2, 15));
    REQUIRE(walkable_count(mask) < before);

    // Roughly the bar's area, in cells, was carved -- not a disc, not the box's
    // whole bounding rect.
    const u32 carved = before - walkable_count(mask);
    const f32 expected = (2.0f * d.barrier_half_length) * (2.0f * d.barrier_half_width);
    REQUIRE(static_cast<f32>(carved) > expected * 0.7f);
    REQUIRE(static_cast<f32>(carved) < expected * 1.4f);

    // The flow field was told, and once re-solved still reaches the goal from
    // upstream: the clot squeezes the lane, it does not seal it.
    REQUIRE(world.flow().has_pending_rebake());
    world.flow().rebake_pending(mask);
    REQUIRE(world.flow().reachable(Vec2{10.0f, 15.0f}));

    // One barrier entity, oriented across the flow, carrying its clock.
    REQUIRE(barrier_count(world) == 1);
    auto view = world.ecs().registry().view<comp::Barrier, comp::Transform>();
    for (auto e : view) {
        const comp::Barrier& b = view.get<comp::Barrier>(e);
        const comp::Transform& tf = view.get<comp::Transform>(e);
        REQUIRE(b.half_extents.x == d.barrier_half_length);
        REQUIRE(b.half_extents.y == d.barrier_half_width);
        REQUIRE(b.remaining == d.field_duration);
        REQUIRE(b.duration == d.field_duration);
        REQUIRE(tf.position.x == at.x);
        // Perpendicular to +x flow: |rotation| ~ pi/2.
        REQUIRE(std::fabs(std::fabs(tf.rotation) - 1.5707963f) < 0.05f);
    }
}

TEST_CASE("Fibrin Clot dissolves on schedule and hands the tissue back",
          "[abilities][clot][flowfield]") {
    SimWorld world = make_lane(30);
    ActiveAbilitySystem abilities;
    abilities.load_defaults();
    abilities.register_systems(world);
    const f32 duration = abilities.def(AbilityId::FibrinClot).field_duration;

    const u32 before = walkable_count(world.tissue());
    REQUIRE(abilities.cast(world, AbilityId::FibrinClot, Vec2{30.0f, 15.0f}));
    world.flow().rebake_pending(world.tissue());
    REQUIRE_FALSE(world.flow().has_pending_rebake());
    REQUIRE(walkable_count(world.tissue()) < before);

    // Just short of the clock: still standing.
    const u32 total_ticks = static_cast<u32>(duration * static_cast<f32>(kTicksPerSecond));
    step_ecs(world, total_ticks - 2);
    REQUIRE(barrier_count(world) == 1);
    REQUIRE_FALSE(world.tissue().walkable(30, 15));

    // Over it: entity gone, every carved cell walkable again, and the flow
    // field told to re-solve over the bar.
    step_ecs(world, 4);
    REQUIRE(barrier_count(world) == 0);
    REQUIRE(walkable_count(world.tissue()) == before);
    REQUIRE(world.tissue().walkable(30, 15));
    REQUIRE(world.flow().has_pending_rebake());

    // The cooldown is independent of the clot's lifetime: it is still ticking.
    REQUIRE_FALSE(abilities.ready(AbilityId::FibrinClot));
}

TEST_CASE("Fibrin Clot refuses a point off the tissue and keeps its cooldown",
          "[abilities][clot]") {
    // A 12-tall lane centred on y = 15 leaves y = 2 as rock.
    SimWorld world = make_lane(12);
    ActiveAbilitySystem abilities;
    abilities.load_defaults();

    const u32 before = walkable_count(world.tissue());
    REQUIRE_FALSE(abilities.cast(world, AbilityId::FibrinClot, Vec2{30.0f, 2.0f}));
    REQUIRE(abilities.ready(AbilityId::FibrinClot));
    REQUIRE(walkable_count(world.tissue()) == before);
    REQUIRE(barrier_count(world) == 0);
}

TEST_CASE("Fibrin Clot refuses a bar that would seal the lane outright",
          "[abilities][clot][reachability]") {
    // 12 tall: the 14-long bar spans the whole cross-section.
    SimWorld world = make_lane(12);
    ActiveAbilitySystem abilities;
    abilities.load_defaults();
    REQUIRE(world.flow().reachable(Vec2{10.0f, 15.0f}));

    const u32 before = walkable_count(world.tissue());
    REQUIRE_FALSE(abilities.cast(world, AbilityId::FibrinClot, Vec2{30.0f, 15.0f}));
    REQUIRE(abilities.ready(AbilityId::FibrinClot));
    REQUIRE(walkable_count(world.tissue()) == before);
    REQUIRE(barrier_count(world) == 0);
    REQUIRE_FALSE(world.flow().has_pending_rebake());
}

TEST_CASE("Fibrin Clot leaves cells that were already blocked alone, so a footprint under it "
          "survives the clot dissolving",
          "[abilities][clot][towers]") {
    SimWorld world = make_lane(30);
    ActiveAbilitySystem abilities;
    abilities.load_defaults();
    abilities.register_systems(world);

    // Block a patch of the lane by hand where the bar will land, standing in
    // for a tower footprint the clot is dropped across.
    TissueMask& mask = world.tissue();
    for (i32 y = 13; y <= 17; ++y)
        for (i32 x = 29; x <= 31; ++x) mask.set_walkable(x, y, false);
    const u32 before = walkable_count(mask);

    REQUIRE(abilities.cast(world, AbilityId::FibrinClot, Vec2{30.0f, 15.0f}));
    REQUIRE(walkable_count(mask) < before);

    const f32 duration = abilities.def(AbilityId::FibrinClot).field_duration;
    step_ecs(world, static_cast<u32>(duration * static_cast<f32>(kTicksPerSecond)) + 2);
    REQUIRE(barrier_count(world) == 0);
    REQUIRE(walkable_count(mask) == before);
    REQUIRE_FALSE(mask.walkable(30, 15));   // the pre-existing block survived
    REQUIRE(mask.walkable(30, 20));         // the clot's own cells came back
}

TEST_CASE("a tower cannot be built on a live Fibrin Clot, and can again once it dissolves",
          "[abilities][clot][towers][placement]") {
    SimWorld world = make_lane(30);
    ActiveAbilitySystem abilities;
    abilities.load_defaults();
    abilities.register_systems(world);
    TowerSystem ts;

    const Vec2 at{30.0f, 15.0f};
    REQUIRE(ts.validate(world, TowerType::Macrophage, at, 100000).result == PlacementResult::Ok);
    REQUIRE(abilities.cast(world, AbilityId::FibrinClot, at));

    // On it, and brushing its end from a footprint radius away: refused.
    REQUIRE(ts.validate(world, TowerType::Macrophage, at, 100000).result == PlacementResult::Overlapping);
    const f32 hl = abilities.def(AbilityId::FibrinClot).barrier_half_length;
    REQUIRE(ts.validate(world, TowerType::Macrophage, Vec2{30.0f, 15.0f + hl + 1.0f}, 100000).result ==
            PlacementResult::Overlapping);
    // Well clear of it along the lane: fine.
    REQUIRE(ts.validate(world, TowerType::Macrophage, Vec2{38.0f, 15.0f}, 100000).result == PlacementResult::Ok);

    const f32 duration = abilities.def(AbilityId::FibrinClot).field_duration;
    step_ecs(world, static_cast<u32>(duration * static_cast<f32>(kTicksPerSecond)) + 2);
    REQUIRE(ts.validate(world, TowerType::Macrophage, at, 100000).result == PlacementResult::Ok);
}
