// Tests for the pure, ImGui-free helpers in ui/HudFormat.h (Wave 4E). These
// back the wave-preview panel and the tower-selection click handler in
// Hud.cpp; neither ImGui rendering nor a live SimWorld is exercised here on
// purpose -- see ui/HudFormat.h's own header comment for why the logic was
// pulled out into a header Hud.cpp and this file both include directly.
#include "ui/HudFormat.h"

#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::ui::fmt;

TEST_CASE("pathogen_family_name covers every family with a non-placeholder string", "[hud]") {
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const std::string name = pathogen_family_name(static_cast<PathogenFamily>(i));
        REQUIRE_FALSE(name.empty());
        REQUIRE(name != "?");
    }
}

TEST_CASE("summarize_wave aggregates counts per family and skips absent families", "[hud]") {
    game::WaveDef wave;
    wave.spawns.push_back(game::SpawnEntry{PathogenFamily::Virus, 0, 40, 0.0f, 5.0f, ""});
    wave.spawns.push_back(game::SpawnEntry{PathogenFamily::Virus, 0, 10, 5.0f, 5.0f, ""});
    wave.spawns.push_back(game::SpawnEntry{PathogenFamily::Bacteria, 7, 1, 0.0f, 1.0f, ""});

    const auto summary = summarize_wave(wave);
    REQUIRE(summary.size() == 2);

    REQUIRE(summary[0].family == PathogenFamily::Virus);
    REQUIRE(summary[0].count == 50);
    REQUIRE_FALSE(summary[0].has_elite);

    REQUIRE(summary[1].family == PathogenFamily::Bacteria);
    REQUIRE(summary[1].count == 1);
    REQUIRE(summary[1].has_elite);
}

TEST_CASE("summarize_wave on an empty wave returns nothing", "[hud]") {
    game::WaveDef wave;
    REQUIRE(summarize_wave(wave).empty());
}

TEST_CASE("wave_has_elite is true iff some spawn carries a non-zero elite_id", "[hud]") {
    game::WaveDef wave;
    wave.spawns.push_back(game::SpawnEntry{PathogenFamily::Virus, 0, 40, 0.0f, 5.0f, ""});
    REQUIRE_FALSE(wave_has_elite(wave));

    wave.spawns.push_back(game::SpawnEntry{PathogenFamily::Parasite, 3, 1, 0.0f, 1.0f, ""});
    REQUIRE(wave_has_elite(wave));
}

TEST_CASE("format_countdown_seconds rounds and clamps negatives to 0s", "[hud]") {
    REQUIRE(format_countdown_seconds(12.4f) == "12s");
    REQUIRE(format_countdown_seconds(12.6f) == "13s");
    REQUIRE(format_countdown_seconds(0.0f) == "0s");
    REQUIRE(format_countdown_seconds(-0.001f) == "0s");
    REQUIRE(format_countdown_seconds(-5.0f) == "0s");
}

TEST_CASE("format_ability_cooldown reports Ready or a countdown", "[hud]") {
    game::AbilityStatus ready_status{true, 0.0f, 60.0f};
    REQUIRE(format_ability_cooldown(ready_status) == "Ready");

    game::AbilityStatus cooling{false, 23.6f, 60.0f};
    REQUIRE(format_ability_cooldown(cooling) == "24s");
}

TEST_CASE("pick_nearest returns an invalid EntityId when nothing is in range", "[hud]") {
    std::vector<std::pair<EntityId, Vec2>> towers = {
        {EntityId{1}, Vec2{10.0f, 10.0f}},
        {EntityId{2}, Vec2{50.0f, 50.0f}},
    };
    const EntityId picked = pick_nearest(towers, Vec2{0.0f, 0.0f}, 2.0f);
    REQUIRE_FALSE(picked.valid());
}

TEST_CASE("pick_nearest returns the closest entity within the pick radius", "[hud]") {
    std::vector<std::pair<EntityId, Vec2>> towers = {
        {EntityId{1}, Vec2{10.0f, 10.0f}},
        {EntityId{2}, Vec2{10.5f, 10.0f}},
        {EntityId{3}, Vec2{50.0f, 50.0f}},
    };
    const EntityId picked = pick_nearest(towers, Vec2{10.4f, 10.0f}, 2.0f);
    REQUIRE(picked.valid());
    REQUIRE(picked == EntityId{2});
}

TEST_CASE("pick_nearest is exact at the radius boundary", "[hud]") {
    std::vector<std::pair<EntityId, Vec2>> towers = {
        {EntityId{1}, Vec2{2.0f, 0.0f}},
    };
    REQUIRE(pick_nearest(towers, Vec2{0.0f, 0.0f}, 2.0f).valid());
    REQUIRE_FALSE(pick_nearest(towers, Vec2{0.0f, 0.0f}, 1.999f).valid());
}
