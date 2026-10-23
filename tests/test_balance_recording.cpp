#include "BalanceTestIsolation.h"
#include "game/balance/RunRecording.h"
#include "app/BalanceMode.h"
#include "app/Cli.h"
#include "game/balance/BalanceRun.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/session/RunSetup.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace immune;
using namespace immune::game;
using namespace immune::game::balance;
using immune::config::Json;

namespace {

struct RecordingFixture {
    GameConfig base;
    MetaProgression meta;
    Json document;
    LevelDef level;
    sim::SimWorld world;
    TowerSystem towers;
    EnemyRoster enemies;
    Economy economy;
    WaveDirector waves;
    ActiveAbilitySystem abilities;
    LevelSystems systems;
    RunRecording recording;

    void initialize(bool sandbox = false, bool exact_document = true) {
        config::ConfigStore store;
        std::string error;
        REQUIRE(load_game_config(store, platform::asset_path("config"), base, error));
        // A compact, reliable combat encounter. These custom tuning values are
        // captured in the recording and restored by replay, never written out.
        auto& shooter = base.towers.mechanics[static_cast<u32>(TowerType::Neutrophil)];
        shooter.shooter.round_damage = 20.0f;
        shooter.shooter.round_speed = 200.0f;
        shooter.shooter.round_spread = 0.0f;
        shooter.shooter.volley_cone = 0.0f;
        shooter.shooter.gather_seconds = 0.0f;
        shooter.shooter.reload_seconds = 0.05f;
        shooter.swarm.max_health = 1000.0f;
        document = Json::parse(R"JSON({
          "schema":2,"name":"balance_recording_fixture","region":"capillary",
          "world":{"min":[0,0],"max":[120,80],"cell_size":1},
          "economy":{"starting_atp":300},
          "win":{"survive_seconds":1},
          "vessels":[{"id":"main","points":[{"p":[4.12345,40],"w":30},{"p":[116,40],"w":30}]}],
          "spawn_points":[{"id":"p0","pos":[40,40],"radius":1,"lane_id":"main"}],
          "objectives":[{"id":"organ","pos":[116,40],"half_extents":[4,15]}],
          "placement_zones":[{"min":[12,25],"max":[108,55]}],
          "waves":[{"name":"one","prep_time":0.5,"atp_reward":25,"spawns":[
            {"family":"virus","count":10,"duration":0.2,"spawn_point_id":"p0"}
          ]}]
        })JSON");
        const auto loaded = LevelLoader{}.load_string(document.dump(), level);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
        apply_enemy_config(enemies, base.enemies);
        auto desc = make_level_sim_desc(base, enemies, level, 789123);
        desc.deterministic_rebakes = true;
        world.init(desc, nullptr);
        REQUIRE(LevelLoader{}.instantiate(level, world).ok);
        towers.register_systems(world);
        enemies.register_systems(world);
        abilities.register_systems(world);
        const auto run = make_run_tuning(base, meta, sandbox);
        apply_run_tuning(base, run, level, world, towers, enemies, economy, abilities, sandbox);
        waves.set_waves(level.waves);
        waves.set_auto_start(false);
        waves.start(world);
        world.enable_combat_metrics(true, 32768);
        systems.world = &world;
        systems.towers = &towers;
        systems.enemies = &enemies;
        systems.economy = &economy;
        systems.waves = &waves;
        systems.abilities = &abilities;
        systems.survive_seconds = level.win.survive_seconds;
        recording.begin(level, base, meta, sandbox, exact_document ? document : Json{});
        recording.sample(world, economy, waves);
    }

    void deploy(u32 quantity = 2) {
        const u32 count = deploy_cells(towers, world, economy, TowerType::Neutrophil,
                                       Vec2{55.0f, 40.0f}, quantity);
        REQUIRE(count == quantity);
        Action action;
        action.tick = world.tick_index();
        action.tower = TowerType::Neutrophil;
        action.position = Vec2{55.0f, 40.0f};
        action.quantity = count;
        const auto hash = world.state_hash();
        const auto atp = economy.atp();
        recording.append(action);
        recording.sample(world, economy, waves);
        CHECK(world.state_hash() == hash);
        CHECK(economy.atp() == atp);
    }

    void start() {
        Action action;
        action.tick = world.tick_index();
        action.kind = ActionKind::StartWave;
        waves.request_early_start();
        recording.append(action);
    }

    SessionOutcome advance(u32 ticks) {
        SessionOutcome outcome = SessionOutcome::InProgress;
        for (u32 i = 0; i < ticks; ++i) {
            outcome = step_level(systems);
            world.combat_events().clear();
            if (world.tick_index() % 10 == 0) recording.sample(world, economy, waves);
            if (outcome != SessionOutcome::InProgress) break;
        }
        return outcome;
    }
};

Json replay_recording(const Json& recording) {
    GameConfig pinned;
    std::string error;
    REQUIRE(parse_game_config_documents(recording.at("base_config").get<std::vector<Json>>(), pinned, error));
    const auto replay = execute(recording.at("spec"), pinned);
    INFO(replay.at("diagnostics").dump());
    REQUIRE(replay.at("summary").at("valid") == true);
    return replay;
}

struct RecordingFiles {
    std::filesystem::path input;
    std::filesystem::path output;
    RecordingFiles() {
        static u64 sequence = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string name = "immune-player-recording-" + std::to_string(stamp) + "-" + std::to_string(++sequence);
        input = std::filesystem::temp_directory_path() / (name + ".json");
        output = std::filesystem::temp_directory_path() / (name + "-report.json");
    }
    ~RecordingFiles() {
        std::error_code error;
        std::filesystem::remove(input, error);
        std::filesystem::remove(output, error);
    }
};

// The CLI deliberately emits its full machine report to C stdout. Keep that
// report in a temporary stream during this serial contract test; the --out
// artifact is inspected below. The original descriptor is always restored.
class QuietCliOutput {
public:
    QuietCliOutput() {
        std::fflush(stdout);
#ifdef _WIN32
        original_ = _dup(_fileno(stdout));
#else
        original_ = dup(fileno(stdout));
#endif
        sink_ = std::tmpfile();
        if (original_ < 0 || sink_ == nullptr) {
            cleanup();
            throw std::runtime_error("cannot create temporary CLI output stream");
        }
#ifdef _WIN32
        const int result = _dup2(_fileno(sink_), _fileno(stdout));
#else
        const int result = dup2(fileno(sink_), fileno(stdout));
#endif
        if (result < 0) { cleanup(); throw std::runtime_error("cannot capture CLI output stream"); }
    }
    ~QuietCliOutput() { cleanup(); }
    QuietCliOutput(const QuietCliOutput&) = delete;
    QuietCliOutput& operator=(const QuietCliOutput&) = delete;
private:
    void cleanup() {
        std::fflush(stdout);
        if (original_ >= 0) {
#ifdef _WIN32
            _dup2(original_, _fileno(stdout));
            _close(original_);
#else
            dup2(original_, fileno(stdout));
            close(original_);
#endif
            original_ = -1;
        }
        if (sink_ != nullptr) { std::fclose(sink_); sink_ = nullptr; }
    }
    int original_ = -1;
    std::FILE* sink_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "player balance recording observes real paid actions and replays exact combat",
          "[balance][recording][replay]") {
    RecordingFixture fixture;
    fixture.initialize();
    const std::string progression = fixture.meta.to_json();
    fixture.deploy(2);
    fixture.start();
    const auto outcome = fixture.advance(120);
    REQUIRE(outcome == SessionOutcome::Cleared);
    const auto hash = fixture.world.state_hash();
    const auto recording = fixture.recording.finish(fixture.world, fixture.economy, fixture.waves, outcome);
    CHECK_FALSE(fixture.recording.active());
    CHECK(fixture.world.state_hash() == hash);
    CHECK(fixture.meta.to_json() == progression);
    CHECK(recording["kind"] == "recording");
    CHECK(recording["replayable"] == true);
    CHECK(recording["full_precision"] == true);
    CHECK(recording["spec"]["level_document"] == fixture.document);
    CHECK(recording["spec"]["actions"][0]["quantity"] == 2);
    CHECK(recording["spec"]["max_ticks"] == fixture.world.tick_index());
    REQUIRE(recording["expected"]["combat"]["enemy_lifetimes"]["virus"]["damage_ttk"]["observations"].get<u64>() > 0);
    const auto replay = replay_recording(recording);
    std::vector<std::string> errors;
    const bool matches = recording_matches(recording, replay, errors);
    INFO(Json(errors).dump());
    CHECK(matches);
    CHECK(replay["economy"] == recording["expected"]["economy"]);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "aborted player recordings stop at the observed tick without rewards",
          "[balance][recording][replay]") {
    RecordingFixture fixture;
    fixture.initialize();
    fixture.start();
    REQUIRE(fixture.advance(20) == SessionOutcome::InProgress);
    const auto recording = fixture.recording.finish(fixture.world, fixture.economy, fixture.waves);
    CHECK(recording["replayable"] == true);
    CHECK(recording["expected"]["status"] == "aborted");
    CHECK(recording["expected"]["reward"]["memory_cells"] == 0);
    CHECK(recording["expected"]["reward"]["antibodies"] == 0);
    const auto replay = replay_recording(recording);
    CHECK(replay["summary"]["status"] == "timeout");
    CHECK(replay["summary"]["duration_ticks"] == 20);
    std::vector<std::string> errors;
    CHECK(recording_matches(recording, replay, errors));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "player recording preserves accepted ability casts and sandbox rules",
          "[balance][recording][abilities]") {
    RecordingFixture fixture;
    fixture.initialize(true);
    fixture.deploy(1);
    Action cast;
    cast.kind = ActionKind::CastAbility;
    cast.tick = fixture.world.tick_index();
    cast.ability = AbilityId::HistamineFlare;
    cast.position = Vec2{40.0f, 40.0f};
    REQUIRE(fixture.abilities.cast(fixture.world, cast.ability, cast.position));
    fixture.recording.append(cast);
    fixture.start();
    const auto outcome = fixture.advance(120);
    REQUIRE(outcome == SessionOutcome::Cleared);
    const auto recording = fixture.recording.finish(fixture.world, fixture.economy, fixture.waves, outcome);
    CHECK(recording["spec"]["progression"]["profile"] == "all_unlocked");
    CHECK(recording["expected"]["reward"]["memory_cells"] == 0);
    CHECK(recording["spec"]["actions"][1]["kind"] == "cast");
    const auto replay = replay_recording(recording);
    std::vector<std::string> errors;
    const bool matches = recording_matches(recording, replay, errors);
    INFO(Json(errors).dump());
    CHECK(matches);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "player recording comparison diagnoses economy wave and lifecycle drift",
          "[balance][recording][verification]") {
    RecordingFixture fixture;
    fixture.initialize();
    fixture.deploy(1);
    fixture.start();
    const auto outcome = fixture.advance(120);
    const auto recording = fixture.recording.finish(fixture.world, fixture.economy, fixture.waves, outcome);
    auto replay = replay_recording(recording);
    SECTION("ATP drift") { replay["economy"]["atp"] = replay["economy"]["atp"].get<u64>() + 1; }
    SECTION("objective drift") { replay["state"]["integrity"] = -1; }
    SECTION("wave clock drift") { replay["wave_status"]["phase_time_remaining"] = 0.123; }
    SECTION("TTK observation drift") {
        replay["combat"]["enemy_lifetimes"]["virus"]["damage_ttk"]["observations"] = 999;
    }
    std::vector<std::string> errors;
    CHECK_FALSE(recording_matches(recording, replay, errors));
    CHECK_FALSE(errors.empty());
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "player recording marks unsupported endpoint states without dropping actions",
          "[balance][recording][validation]") {
    RecordingFixture fixture;
    fixture.initialize();
    SECTION("zero ticks") {}
    SECTION("accepted action after final simulation tick") {
        REQUIRE(fixture.advance(1) == SessionOutcome::InProgress);
        fixture.deploy(1);
    }
    SECTION("non-deterministic rebake session") {
        fixture.world.init(make_level_sim_desc(fixture.base, fixture.enemies, fixture.level, 123), nullptr);
    }
    const auto recording = fixture.recording.finish(fixture.world, fixture.economy, fixture.waves);
    CHECK(recording["replayable"] == false);
    CHECK_FALSE(recording["diagnostics"]["errors"].empty());
    std::vector<std::string> errors;
    CHECK_FALSE(recording_matches(recording, Json::object(), errors));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "player recording same-tick samples replace stale observations and mark rounded fallback",
          "[balance][recording][serialization]") {
    RecordingFixture fixture;
    fixture.initialize(false, false);
    fixture.deploy(1);
    fixture.recording.sample(fixture.world, fixture.economy, fixture.waves);
    const auto recording = fixture.recording.finish(fixture.world, fixture.economy, fixture.waves);
    CHECK(recording["timeline"].size() == 1);
    CHECK(recording["timeline"][0]["economy"]["spent"].get<u64>() > 0);
    CHECK(recording["timeline"][0]["friendly_alive"] == 1);
    CHECK(recording["full_precision"] == false);
    CHECK_FALSE(recording["diagnostics"]["warnings"].empty());
    CHECK(recording["spec"]["actions"].size() == 1);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance CLI imports pinned player recordings and rejects observed drift",
          "[balance][recording][cli]") {
    RecordingFixture fixture;
    fixture.initialize();
    fixture.deploy(1);
    fixture.start();
    const auto outcome = fixture.advance(120);
    auto recording = fixture.recording.finish(fixture.world, fixture.economy, fixture.waves, outcome);
    bool mismatch = false;
    SECTION("matching observation") {}
    SECTION("tampered observation") {
        recording["expected"]["state"]["integrity"] = -1;
        mismatch = true;
    }
    RecordingFiles files;
    REQUIRE(platform::write_text_file(files.input.string(), recording.dump()));
    std::vector<std::string> args{"immune", "--balance-run", files.input.string(), "--out", files.output.string()};
    std::vector<char*> argv;
    for (auto& argument : args) argv.push_back(argument.data());
    const auto options = immune::app::parse_args(static_cast<int>(argv.size()), argv.data());
    REQUIRE(options.mode == immune::app::Mode::BalanceRun);
    int exit_code = -1;
    {
        QuietCliOutput quiet;
        exit_code = immune::app::run_balance(options);
    }
    CHECK(exit_code == (mismatch ? 1 : 0));
    const auto text = platform::read_text_file(files.output.string());
    REQUIRE(text.has_value());
    const auto report = Json::parse(*text);
    CHECK(report["recording_validation"]["matches"] == !mismatch);
    CHECK(report["summary"]["status"] == (mismatch ? "invalid" : "cleared"));
    if (mismatch) CHECK_FALSE(report["diagnostics"]["errors"].empty());
}
