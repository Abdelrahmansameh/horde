// Tests for the new game/abilities/ module (DESIGN.md §5.6).
#include "game/abilities/ActiveAbilities.h"

#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"

#include <catch2/catch_test_macros.hpp>

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
