// Tests for the balance harness: game/autoplay (the bot), game/telemetry (the
// report), sim/Attribution.h (per-owner damage), and the per-family counters
// SimSnapshot grew for them.
//
// WHAT THESE TESTS ARE ACTUALLY DEFENDING
// A balance report is a measurement instrument, and an instrument that is
// wrong is worse than no instrument -- a wrong number gets acted on. So the
// assertions here are about the instrument's honesty rather than about the
// bot playing well:
//   - the bot cannot spend ATP it does not have (or every price is a lie);
//   - attribution sums to what the sim itself says was destroyed (or a tower
//     ranking is a ranking of bookkeeping bugs);
//   - per-family kills + leaks + out-of-bounds reconcile with spawns;
//   - attaching the collector does not change the sim (or the run being
//     measured is not the run that ships).
#include "game/autoplay/AutoPlayer.h"
#include "game/config/GameConfig.h"
#include "game/economy/Economy.h"
#include "game/gym/GymCommands.h"
#include "game/enemies/EnemyRoster.h"
#include "game/level/Level.h"
#include "game/session/LevelSession.h"
#include "game/telemetry/RunTelemetry.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "core/Math.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::game;

namespace {

/// One assembled level, ready to tick. Mirrors app/AutoplayMode.cpp's setup;
/// kept local rather than shared because a test that reached into the mode's
/// private setup would stop being a test of the pieces.
struct Harness {
    sim::SimWorld world;
    LevelDef level;
    LaneOwnershipMap lanes;
    TowerSystem towers;
    EnemyRoster enemies;
    WaveDirector waves;
    Economy economy;
    GymSpawnQueue spawns;
    GymToggles toggles;
    AutoPlayer bot;

    LevelSystems systems() {
        LevelSystems s;
        s.world = &world;
        s.towers = &towers;
        s.enemies = &enemies;
        s.waves = &waves;
        s.economy = &economy;
        s.spawns = &spawns;
        s.toggles = &toggles;
        return s;
    }
};

bool build(Harness& h, const std::string& path, u64 seed = 12345) {
    LevelLoader loader;
    if (!loader.load_file(path, h.level).ok) return false;

    h.enemies.load_defaults();
    sim::SimDesc desc;
    desc.seed = seed;
    desc.max_chaff = 16384;
    desc.world_bounds = h.level.world_bounds;
    h.enemies.apply_to_tuning(desc.chaff_tuning);
    h.world.init(desc, nullptr);
    if (!loader.instantiate(h.level, h.world).ok) return false;
    h.lanes = loader.build_lane_ownership_map(h.level);
    h.towers.register_systems(h.world);
    h.enemies.register_systems(h.world);
    h.waves.set_waves(h.level.waves);
    h.waves.start(h.world);
    h.economy.configure(EconomyConfig{});
    h.economy.reset();
    h.toggles.hold_integrity = h.world.snapshot().objective_integrity;
    return true;
}

std::vector<std::string> shipped_levels() {
    std::vector<std::string> out;
    const std::string dir = platform::asset_path("levels");
    if (!std::filesystem::is_directory(dir)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            out.push_back(entry.path().string());
        }
    }
    return out;
}

/// Runs the bot for `ticks`, returning the harness so callers can assert on it.
void play(Harness& h, u64 ticks, RunTelemetry* telemetry = nullptr) {
    LevelSystems systems = h.systems();
    for (u64 t = 1; t <= ticks; ++t) {
        const AutoPlayAction action = h.bot.tick(h.world, h.towers, h.economy, t);
        if (telemetry != nullptr) {
            if (action.kind == AutoPlayAction::Kind::Placed) {
                telemetry->on_tower_placed(action.tower, action.type, action.position, action.cost, t);
            } else if (action.kind == AutoPlayAction::Kind::Upgraded) {
                telemetry->on_tower_upgraded(action.tower, action.tier, action.cost, t);
            }
        }
        const SessionOutcome outcome = step_level(systems, nullptr);
        if (telemetry != nullptr) telemetry->sample(h.world, h.waves, h.economy);
        if (outcome != SessionOutcome::InProgress) break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The planner
// ---------------------------------------------------------------------------

TEST_CASE("the planner finds buildable sites on every shipped level", "[autoplay]") {
    const std::vector<std::string> levels = shipped_levels();
    REQUIRE_FALSE(levels.empty());

    for (const std::string& path : levels) {
        Harness h;
        INFO("level: " << path);
        REQUIRE(build(h, path));
        h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);
        // A level the bot cannot build on is a level the harness cannot
        // measure, which is a content bug worth failing on rather than a
        // quiet zero in a report.
        CHECK_FALSE(h.bot.sites().empty());

        // Every planned site must be somewhere a player could also build.
        for (const PlannedSite& s : h.bot.sites()) {
            const PlacementQuery q = h.towers.validate(h.world, s.type, s.position, 1'000'000u);
            INFO("site (" << s.position.x << ", " << s.position.y << ") -> result "
                          << static_cast<int>(q.result));
            CHECK(q.valid());
        }
    }
}

TEST_CASE("planned sites are spread, not stacked", "[autoplay]") {
    Harness h;
    REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json")));
    h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);
    REQUIRE(h.bot.sites().size() >= 2);

    // Two towers on the same cell fight over the same agents and would make
    // the per-tower damage figures meaningless.
    for (usize i = 0; i < h.bot.sites().size(); ++i) {
        for (usize j = i + 1; j < h.bot.sites().size(); ++j) {
            const Vec2 d = h.bot.sites()[i].position - h.bot.sites()[j].position;
            CHECK(math::length_sq(d) > 1.0f);
        }
    }
}

TEST_CASE("single-type profiles plan only their own tower", "[autoplay]") {
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const TowerType type = static_cast<TowerType>(t);
        Harness h;
        REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json")));
        AutoPlayConfig cfg;
        cfg.profile = AutoPlayProfile::SingleType;
        cfg.single_type = type;
        h.bot.configure(cfg);
        h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);
        INFO("type: " << tower_type_name(type));
        for (const PlannedSite& s : h.bot.sites()) CHECK(s.type == type);
    }
}

TEST_CASE("profile names round-trip", "[autoplay]") {
    const char* names[] = {"greedy-cheapest", "spread-coverage", "save-for-tier3",
                           "single-type:neutrophil", "single-type:nk_cell"};
    for (const char* name : names) {
        AutoPlayProfile profile{};
        TowerType type = TowerType::Neutrophil;
        INFO("profile: " << name);
        REQUIRE(parse_autoplay_profile(name, profile, type));
        CHECK(autoplay_profile_name(profile, type) == std::string(name));
    }
    AutoPlayProfile profile{};
    TowerType type{};
    CHECK_FALSE(parse_autoplay_profile("single-type:not_a_tower", profile, type));
    CHECK_FALSE(parse_autoplay_profile("wishful-thinking", profile, type));
}

// ---------------------------------------------------------------------------
// The bot's spending
// ---------------------------------------------------------------------------

TEST_CASE("the bot never spends ATP it does not have", "[autoplay]") {
    Harness h;
    REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json")));
    h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);

    LevelSystems systems = h.systems();
    u32 previous = h.economy.atp();
    u32 purchases = 0;
    for (u64 t = 1; t <= 5'000; ++t) {
        const AutoPlayAction action = h.bot.tick(h.world, h.towers, h.economy, t);
        if (action.kind != AutoPlayAction::Kind::None) {
            ++purchases;
            // Whatever it bought, it had the money: the balance shrank by
            // exactly the price, and never below zero (u32 would wrap).
            CHECK(action.cost <= previous);
            CHECK(h.economy.atp() == previous - action.cost);
        }
        if (step_level(systems, nullptr) != SessionOutcome::InProgress) break;
        previous = h.economy.atp();
    }
    // A bot that bought nothing at all would pass every check above vacuously.
    CHECK(purchases > 0);
}

TEST_CASE("upgrade_cost prices the current tier and stops at tier 3", "[autoplay][towers]") {
    Harness h;
    REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json")));
    h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);
    REQUIRE_FALSE(h.bot.sites().empty());

    const PlannedSite& site = h.bot.sites()[0];
    const EntityId tower = h.towers.place(h.world, site.type, site.position);
    REQUIRE(tower.valid());

    for (u8 tier = 1; tier <= 2; ++tier) {
        const u32 quoted = h.towers.upgrade_cost(h.world, tower);
        CHECK(quoted == h.towers.stats(site.type, tier).upgrade_cost);
        CHECK(h.towers.upgrade(h.world, tower) == tier + 1);
    }
    // Tier 3 is the end of the line, and a price of 0 is how a caller knows.
    CHECK(h.towers.upgrade_cost(h.world, tower) == 0);
    CHECK(h.towers.upgrade(h.world, tower) == 0);
    CHECK(h.towers.upgrade_cost(h.world, EntityId{}) == 0);
}

// ---------------------------------------------------------------------------
// Attribution and telemetry honesty
// ---------------------------------------------------------------------------

TEST_CASE("attribution sums to what the sim says was destroyed", "[autoplay][telemetry]") {
    Harness h;
    REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json")));
    h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);

    RunTelemetry telemetry;
    telemetry.begin(h.world, h.level, h.lanes);
    play(h, 6'000, &telemetry);
    telemetry.end(h.world);

    f64 attributed = 0.0;
    for (const TowerTelemetry& t : telemetry.towers()) attributed += t.total_density_removed();
    REQUIRE(attributed > 0.0);

    // Attributed kills can never EXCEED what the sim retired as killed. They
    // can fall short: environmental hazards (fungal death clouds, allergen
    // self-damage) carry no owner by design, and nothing should book their
    // work against a tower.
    const sim::SimSnapshot snap = h.world.snapshot();
    u64 killed = 0;
    for (u32 f = 0; f < kFamilyCount; ++f) killed += snap.chaff_killed_by_family[f];
    u64 attributed_kills = 0;
    for (const TowerTelemetry& t : telemetry.towers()) {
        for (u32 f = 0; f < kFamilyCount; ++f) attributed_kills += t.chaff_killed[f];
    }
    CHECK(attributed_kills <= killed);
    CHECK(attributed_kills > 0);
}

TEST_CASE("per-family counters reconcile with spawns", "[autoplay][sim]") {
    Harness h;
    REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json")));
    h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);
    play(h, 6'000);

    const sim::SimSnapshot snap = h.world.snapshot();
    u64 spawned = 0;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        // Everything spawned is either dead, leaked, out of bounds, or still
        // standing. If this does not hold, every rate in the report is wrong.
        const u64 accounted = snap.chaff_killed_by_family[f] + snap.chaff_leaked_by_family[f] +
                              snap.chaff_despawned_by_family[f] + snap.chaff_by_family[f];
        INFO("family " << f);
        CHECK(accounted == snap.chaff_spawned_by_family[f]);
        spawned += snap.chaff_spawned_by_family[f];
    }
    CHECK(spawned > 0);
    // The historical aggregate keeps its old meaning: every retirement.
    u64 retired = 0;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        retired += snap.chaff_killed_by_family[f] + snap.chaff_leaked_by_family[f] +
                   snap.chaff_despawned_by_family[f];
    }
    CHECK(retired == snap.chaff_killed_total);
}

TEST_CASE("attaching the collector does not change the sim", "[autoplay][telemetry]") {
    // The whole harness rests on this: if measuring the run changes the run,
    // the numbers describe a game nobody plays.
    Harness plain;
    REQUIRE(build(plain, platform::asset_path("levels/skin_1_breach.json"), 777));
    plain.bot.plan(plain.level, plain.lanes, plain.world, plain.towers, plain.waves);
    play(plain, 3'000);

    Harness measured;
    REQUIRE(build(measured, platform::asset_path("levels/skin_1_breach.json"), 777));
    measured.bot.plan(measured.level, measured.lanes, measured.world, measured.towers,
                      measured.waves);
    RunTelemetry telemetry;
    telemetry.begin(measured.world, measured.level, measured.lanes);
    play(measured, 3'000, &telemetry);
    telemetry.end(measured.world);

    CHECK(plain.world.state_hash() == measured.world.state_hash());
    CHECK(plain.world.snapshot().tick == measured.world.snapshot().tick);
    CHECK(plain.economy.atp() == measured.economy.atp());
}

TEST_CASE("a run is reproducible from level, seed, and profile", "[autoplay]") {
    auto run_once = [](u64 seed) {
        Harness h;
        REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json"), seed));
        h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);
        play(h, 2'000);
        return h.world.state_hash();
    };
    CHECK(run_once(4242) == run_once(4242));
}

TEST_CASE("the report is well-formed JSON carrying its own provenance", "[autoplay][telemetry]") {
    Harness h;
    REQUIRE(build(h, platform::asset_path("levels/skin_1_breach.json")));
    h.bot.plan(h.level, h.lanes, h.world, h.towers, h.waves);
    RunTelemetry telemetry;
    telemetry.begin(h.world, h.level, h.lanes);
    play(h, 2'000, &telemetry);
    telemetry.finish(RunResult::TickLimit);
    telemetry.end(h.world);

    RunTelemetry::ReportHeader header;
    header.level_path = "assets/levels/skin_1_breach.json";
    header.level_name = h.level.name;
    header.profile = "greedy-cheapest";
    header.seed = 12345;
    header.config_hash = "deadbeef";
    const std::string text = telemetry.to_json(header);

    // A report that cannot say which level, seed, and tuning produced it is a
    // number without a claim attached.
    CHECK(text.find("\"level\"") != std::string::npos);
    CHECK(text.find("\"seed\"") != std::string::npos);
    CHECK(text.find("\"config_hash\"") != std::string::npos);
    CHECK(text.find("\"towers_by_type\"") != std::string::npos);
    CHECK(text.find("\"waves\"") != std::string::npos);
    CHECK(text.find("\"families\"") != std::string::npos);
    CHECK(text.find("\"timeline\"") != std::string::npos);
    CHECK(text.find("\"atp_per_density\"") != std::string::npos);
    CHECK(text.find("tick_limit") != std::string::npos);
}
