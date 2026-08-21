// Tests for the gym command layer (game/gym/GymCommands.h) and for the gym
// level itself (assets/levels/gym.json).
//
// The command layer is the thing a human uses to poke every system by hand, so
// the properties worth pinning are: it never crashes on garbage, it reports
// failure instead of silently doing nothing, and each command actually moves
// the state it claims to move. The ImGui console on top of it is not tested
// here (it owns no logic) -- everything below runs headless.
#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/gym/GymCommands.h"
#include "game/level/Level.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::game;

namespace {

/// A gym-shaped harness: the real gym level if it is on disk (so the level and
/// the commands are tested against each other), otherwise the built-in test
/// level so this file still runs from an odd working directory.
struct Harness {
    sim::SimWorld world;
    TowerSystem towers;
    EnemyRoster enemies;
    WaveDirector waves;
    Economy economy;
    ActiveAbilitySystem abilities;
    GymSpawnQueue spawns;
    GymToggles toggles;
    LevelDef level;
    GymContext ctx;

    explicit Harness(const std::string& level_path = "assets/levels/gym.json") {
        LevelLoader loader;
        if (!platform::file_exists(level_path) || !loader.load_file(level_path, level).ok) {
            level = LevelLoader::default_test_level();
        }
        sim::SimDesc desc;
        desc.world_bounds = level.world_bounds;
        desc.max_chaff = 20000;
        enemies.load_defaults();
        enemies.apply_to_tuning(desc.chaff_tuning);
        world.init(desc, nullptr);
        REQUIRE(loader.instantiate(level, world).ok);
        towers.register_systems(world);
        enemies.register_systems(world);
        waves.set_waves(level.waves);
        waves.start(world);
        economy.configure(EconomyConfig{});
        abilities.load_defaults();

        ctx.world = &world;
        ctx.towers = &towers;
        ctx.enemies = &enemies;
        ctx.waves = &waves;
        ctx.economy = &economy;
        ctx.abilities = &abilities;
        ctx.spawns = &spawns;
        ctx.toggles = &toggles;
    }

    GymResult run(const std::string& line) { return gym_execute(ctx, line); }

    /// One sim step, driven the way app/ drives it: queued spawns first, then
    /// the world.
    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            spawns.tick(world);
            world.tick(nullptr);
            toggles.apply(world);
        }
    }
};

} // namespace

// ---- Parsing and error reporting -------------------------------------------

TEST_CASE("every table entry is a real command", "[gym][commands]") {
    Harness h;
    for (const GymCommandInfo& c : gym_commands()) {
        const GymResult r = h.run(std::string("help ") + c.name);
        INFO("command: " << c.name);
        CHECK(r.ok);
        CHECK_FALSE(r.message.empty());
    }
}

TEST_CASE("garbage input fails loudly instead of doing nothing", "[gym][commands]") {
    Harness h;
    const char* kJunk[] = {
        "notacommand", "spawn zebra 10", "spawn virus 10 at nowhere", "spawn virus -5",
        "tower toaster", "cast telekinesis", "wave 999", "elite nope", "field 0 0",
        "vfx sparkles", "time -1", "step 0", "atp banana", "overlay nothing",
    };
    for (const char* line : kJunk) {
        const GymResult r = h.run(line);
        INFO("line: " << line);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.message.empty());
    }
}

TEST_CASE("blank lines and comments are no-ops", "[gym][commands]") {
    Harness h;
    CHECK(h.run("").ok);
    CHECK(h.run("   ").ok);
    CHECK(h.run("# spawn virus 500").ok);
    CHECK(h.world.chaff().count() == 0);
}

TEST_CASE("commands with no subsystem report it rather than crashing", "[gym][commands]") {
    GymContext empty;   // every pointer null, every hook unset
    const char* kNeedsSomething[] = {"spawn virus 10", "tower all", "cast fever", "atp 100",
                                     "wave status",    "stats",    "cam fit",    "restart",
                                     "level gym",      "time 2",   "step 1",     "overlay debug"};
    for (const char* line : kNeedsSomething) {
        const GymResult r = gym_execute(empty, line);
        INFO("line: " << line);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.message.empty());
    }
    // Pure-text commands still work with nothing bound at all.
    CHECK(gym_execute(empty, "help").ok);
}

// ---- Horde ------------------------------------------------------------------

TEST_CASE("spawn puts agents in the world", "[gym][spawn]") {
    Harness h;
    REQUIRE(h.run("spawn virus 250").ok);
    CHECK(h.world.chaff().count() == 250);
    CHECK(h.world.chaff().family_count(PathogenFamily::Virus) == 250);

    SECTION("targeted at an explicit point") {
        REQUIRE(h.run("spawn bacteria 100 at 120,75").ok);
        // Whatever did not fit in the first burst streams in over the next few
        // ticks; the requested count still all arrives.
        for (int i = 0; i < 120 && h.spawns.pending() > 0; ++i) h.step();
        CHECK(h.world.chaff().family_count(PathogenFamily::Bacteria) >= 100);
    }
    SECTION("targeted at a portal by id") {
        if (!h.world.portals().empty()) {
            const std::string id = h.world.portals().front().id;
            REQUIRE(h.run("spawn parasite 40 at " + id).ok);
            CHECK(h.world.chaff().family_count(PathogenFamily::Parasite) == 40);
        }
    }
    SECTION("all families at once") {
        REQUIRE(h.run("spawn all 30").ok);
        for (u32 f = 0; f < kFamilyCount; ++f) {
            CHECK(h.world.chaff().family_count(static_cast<PathogenFamily>(f)) >= 30);
        }
    }
}

TEST_CASE("kill flags chaff and the next tick removes it", "[gym][spawn]") {
    Harness h;
    // Deliberately not virus: viral replication can add an agent *during* the
    // same tick that removes the flagged ones, so "kill virus" leaving one
    // newborn behind is correct sim behaviour and would make this test lie
    // about what `kill` does.
    REQUIRE(h.run("spawn parasite 100").ok);
    REQUIRE(h.run("spawn bacteria 100").ok);
    const u32 bacteria_before = h.world.chaff().family_count(PathogenFamily::Bacteria);

    REQUIRE(h.run("kill parasite").ok);
    h.world.tick(nullptr);
    CHECK(h.world.chaff().family_count(PathogenFamily::Parasite) == 0);
    CHECK(h.world.chaff().family_count(PathogenFamily::Bacteria) == bacteria_before);

    REQUIRE(h.run("kill all").ok);
    h.world.tick(nullptr);
    CHECK(h.world.chaff().count() == 0);
}

TEST_CASE("an oversized spawn streams in instead of spilling off the lane",
          "[gym][spawn][queue]") {
    Harness h;
    // Far more than any one burst can place on tissue at a portal.
    REQUIRE(h.run("spawn bacteria 3000 at p_lymph").ok);
    const usize immediate = h.world.chaff().count();
    CHECK(immediate > 0);
    CHECK(immediate < 3000);
    CHECK(h.spawns.pending() == 3000 - immediate);

    // Drain the queue the way the game does, then let the horde settle.
    for (int i = 0; i < 600 && h.spawns.pending() > 0; ++i) h.step();
    CHECK(h.spawns.pending() == 0);

    const sim::SimSnapshot snap = h.world.snapshot();
    // Everything asked for arrived, and none of it arrived by teleporting into
    // the objective: a spawn command must never damage the thing under test.
    CHECK(snap.chaff_count + snap.chaff_killed_total >= 3000);
    CHECK(snap.objective_integrity == 100.0f);
}

TEST_CASE("flood spawns from every portal", "[gym][spawn]") {
    Harness h;
    if (h.world.portals().empty()) return;
    REQUIRE(h.run("flood 60").ok);
    CHECK(h.world.chaff().count() >= 10u * h.world.portals().size());
}

TEST_CASE("spawning never damages the objective", "[gym][spawn]") {
    Harness h;
    REQUIRE(h.run("flood 1200").ok);
    for (int i = 0; i < 120; ++i) h.step();
    CHECK(h.world.snapshot().objective_integrity == 100.0f);
    CHECK(h.world.snapshot().chaff_leaked_total == 0);
}

TEST_CASE("elite spawns a named ECS agent", "[gym][elite]") {
    Harness h;
    REQUIRE(h.run("elite list").ok);
    const u64 before = h.world.snapshot().named_count;
    REQUIRE(h.run("elite tumor_mass at 104,66").ok);
    CHECK(h.world.snapshot().named_count > before);

    REQUIRE(h.run("elite all").ok);
    CHECK(h.world.snapshot().named_count >= before + h.enemies.elites().size());
}

// ---- Towers, abilities, economy ---------------------------------------------

TEST_CASE("tower places for free and upgrades", "[gym][towers]") {
    Harness h;
    const u32 atp_before = h.economy.atp();
    REQUIRE(h.run("tower all").ok);
    CHECK_FALSE(h.towers.placed_towers().empty());
    CHECK(h.economy.atp() == atp_before);   // the gym never charges

    REQUIRE(h.run("upgrade all").ok);
    REQUIRE(h.run("fire").ok);
    REQUIRE(h.run("sell all").ok);
    CHECK(h.towers.placed_towers().empty());
    CHECK(h.economy.atp() > atp_before);    // refunds are credited
}

TEST_CASE("tower reports why a placement was refused", "[gym][towers]") {
    Harness h;
    // Far outside the world bounds: no nudge can rescue this one.
    const GymResult r = h.run("tower neutrophil at -500,-500");
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("cannot place") != std::string::npos);
}

TEST_CASE("cast consumes an ability and ready refills it", "[gym][abilities]") {
    Harness h;
    REQUIRE(h.run("cast histamine at 104,66").ok);
    CHECK_FALSE(h.abilities.ready(AbilityId::HistamineFlare));

    const GymResult again = h.run("cast histamine at 104,66");
    CHECK_FALSE(again.ok);                       // on cooldown, reported not ignored

    REQUIRE(h.run("ready").ok);
    CHECK(h.abilities.ready(AbilityId::HistamineFlare));
    CHECK(h.run("cast histamine at 104,66").ok);
}

TEST_CASE("atp sets and adds", "[gym][economy]") {
    Harness h;
    REQUIRE(h.run("atp 1000").ok);
    CHECK(h.economy.atp() == 1000);
    REQUIRE(h.run("atp +250").ok);
    CHECK(h.economy.atp() == 1250);
    REQUIRE(h.run("atp 0").ok);
    CHECK(h.economy.atp() == 0);
}

// ---- Damage, waves, time ----------------------------------------------------

TEST_CASE("field submits a damage field that actually thins the horde", "[gym][damage]") {
    Harness h;
    REQUIRE(h.run("spawn virus 400 at 104,66 radius 4").ok);
    const f32 density_before = h.world.chaff().total_density();

    REQUIRE(h.run("field 20 400 1 at 104,66").ok);
    for (int i = 0; i < 10; ++i) h.world.tick(nullptr);

    CHECK(h.world.chaff().total_density() < density_before);
}

TEST_CASE("invuln holds the objective while leaks still count", "[gym][objective]") {
    Harness h;
    if (h.world.portals().empty()) return;

    // Park a horde right on the objective so it leaks immediately and hard.
    REQUIRE(h.run("spawn bacteria 400 at objective").ok);

    SECTION("off by default, so the real loss condition still works") {
        CHECK_FALSE(h.toggles.objective_invulnerable);
        h.step(120);
        CHECK(h.world.snapshot().chaff_leaked_total > 0);
        CHECK(h.world.snapshot().objective_integrity < 100.0f);
    }

    SECTION("on: integrity holds, and the leak counter still moves") {
        REQUIRE(h.run("invuln on").ok);
        CHECK(h.toggles.objective_invulnerable);
        h.step(120);
        const sim::SimSnapshot snap = h.world.snapshot();
        CHECK(snap.objective_integrity == 100.0f);
        CHECK(snap.chaff_leaked_total > 0);   // the leak itself is not suppressed
    }

    SECTION("toggling back off lets it take damage again") {
        REQUIRE(h.run("invuln on").ok);
        h.step(60);
        REQUIRE(h.run("invuln off").ok);
        CHECK_FALSE(h.toggles.objective_invulnerable);
        h.step(120);
        CHECK(h.world.snapshot().objective_integrity < 100.0f);
    }

    SECTION("bare `invuln` flips it, and `godmode` is the same command") {
        REQUIRE(h.run("invuln").ok);
        CHECK(h.toggles.objective_invulnerable);
        REQUIRE(h.run("godmode off").ok);
        CHECK_FALSE(h.toggles.objective_invulnerable);
    }
}

TEST_CASE("invuln restores integrity the moment it is switched on", "[gym][objective]") {
    Harness h;
    if (h.world.portals().empty()) return;
    REQUIRE(h.run("spawn bacteria 400 at objective").ok);
    h.step(120);
    REQUIRE(h.world.snapshot().objective_integrity < 100.0f);

    REQUIRE(h.run("invuln on").ok);
    // Not "next tick": switching it on mid-breach has to stop the bleeding now.
    CHECK(h.world.snapshot().objective_integrity == 100.0f);
}

TEST_CASE("wave start skips prep and wave <n> jumps", "[gym][waves]") {
    Harness h;
    if (h.waves.waves().size() < 2) return;

    REQUIRE(h.run("wave status").ok);
    REQUIRE(h.run("wave start").ok);
    // Prep is measured in seconds and a wave's first spawn lands shortly after
    // the skip; a second of ticks is enough for the phase to have moved on.
    for (int i = 0; i < 90; ++i) h.waves.tick(h.world, h.world.rng(), kFixedDt);
    CHECK(h.waves.status().phase != WavePhase::Prep);

    const std::string second = h.waves.waves().size() > 1 ? h.waves.waves()[1].name : std::string{};
    Harness h2;
    REQUIRE(h2.run("wave 2").ok);
    CHECK(h2.waves.waves().front().name == second);
    CHECK(h2.waves.waves().front().index == 0u);
}

TEST_CASE("step advances the sim by exactly the requested ticks", "[gym][time]") {
    Harness h;
    const Tick before = h.world.tick_index();
    REQUIRE(h.run("step 45").ok);
    CHECK(h.world.tick_index() == before + 45);
}

TEST_CASE("vfx raises combat events for the particle layer", "[gym][vfx]") {
    Harness h;
    REQUIRE(h.run("vfx list").ok);
    REQUIRE(h.run("vfx explosion at 104,66").ok);
    CHECK(h.world.combat_events().size() == 1);
    REQUIRE(h.run("vfx all").ok);
    CHECK(h.world.combat_events().size() > 1);
}

TEST_CASE("a script runs several commands and stops at the first failure", "[gym][commands]") {
    Harness h;
    const GymResult ok = gym_execute_script(h.ctx, "spawn virus 50; spawn bacteria 50");
    CHECK(ok.ok);
    CHECK(h.world.chaff().count() == 100);

    const GymResult bad = gym_execute_script(h.ctx, "spawn virus 10\nnotacommand\nspawn virus 10");
    CHECK_FALSE(bad.ok);
    CHECK(h.world.chaff().count() == 110);   // the third line never ran
}

// ---- The gym level itself ---------------------------------------------------

TEST_CASE("the gym level loads, validates, and is reachable from every portal",
          "[gym][level]") {
    const std::string path = "assets/levels/gym.json";
    if (!platform::file_exists(path)) return;   // running from an unexpected cwd

    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult load = loader.load_file(path, def);
    INFO(load.error);
    REQUIRE(load.ok);
    REQUIRE(loader.validate(def).ok);

    // One lane per vessel type is the whole point: a gym that cannot show you
    // an artery next to a lymph channel is not a gym. Each lane is authored as
    // a spawn chamber plus a trunk sharing one lane_id, so count lanes, not
    // vessels.
    std::vector<std::string> lanes;
    bool seen[5] = {};
    for (const Vessel& v : def.vessels) {
        seen[static_cast<u32>(v.type)] = true;
        const std::string& id = v.lane_id.empty() ? v.id : v.lane_id;
        if (std::find(lanes.begin(), lanes.end(), id) == lanes.end()) lanes.push_back(id);
    }
    CHECK(lanes.size() == 5);
    for (bool s : seen) CHECK(s);

    CHECK(def.portals.size() == 5);
    CHECK_FALSE(def.waves.empty());
    // Every portal must feed the objective, or a lane is decoration.
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = def.world_bounds;
    desc.max_chaff = 20000;
    world.init(desc, nullptr);
    REQUIRE(loader.instantiate(def, world).ok);
    for (const SpawnPortal& p : def.portals) {
        INFO("portal: " << p.id);
        CHECK(world.flow().reachable(p.position));
    }
}

TEST_CASE("the gym level's wave table exercises every family and modifier",
          "[gym][level][waves]") {
    const std::string path = "assets/levels/gym.json";
    if (!platform::file_exists(path)) return;

    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_file(path, def).ok);

    bool family_seen[kFamilyCount] = {};
    bool elite_seen = false;
    bool modifier_seen[4] = {};
    for (const WaveDef& w : def.waves) {
        modifier_seen[static_cast<u32>(w.modifier)] = true;
        for (const SpawnEntry& e : w.spawns) {
            family_seen[static_cast<u32>(e.family)] = true;
            if (e.elite_id != 0) elite_seen = true;
        }
    }
    for (bool f : family_seen) CHECK(f);
    CHECK(elite_seen);
    for (bool m : modifier_seen) CHECK(m);
}
