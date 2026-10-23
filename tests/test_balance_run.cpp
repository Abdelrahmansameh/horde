#include "BalanceTestIsolation.h"
#include "game/balance/BalanceRun.h"
#include "game/config/GameConfig.h"
#include "game/meta/ImmunityTree.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <vector>

using namespace immune;
using namespace immune::game;
using immune::config::Json;

namespace {

GameConfig run_test_config() {
    config::ConfigStore store;
    GameConfig config;
    std::string error;
    const bool ok = load_game_config(store, platform::asset_path("config"), config, error);
    INFO(error);
    REQUIRE(ok);
    return config;
}

Json tiny_level(const char* name = "balance_run_tiny") {
    Json level = Json::parse(R"JSON({
      "schema":2,"name":"balance_run_tiny","region":"capillary",
      "world":{"min":[0,0],"max":[120,80],"cell_size":1},
      "economy":{"starting_atp":300,"income_multiplier":1},
      "allowed_towers":["neutrophil"],
      "win":{"survive_seconds":0.125},
      "vessels":[{"id":"main","points":[{"p":[4,40],"w":30},{"p":[116,40],"w":30}]}],
      "spawn_points":[{"id":"p0","pos":[4,40],"radius":1,"lane_id":"main"}],
      "objectives":[{"id":"organ","pos":[116,40],"half_extents":[4,15]}],
      "placement_zones":[{"min":[12,25],"max":[108,55]}],
      "waves":[{"name":"one","prep_time":0.1,"atp_reward":75,"spawns":[
        {"family":"virus","count":1,"duration":0.01,"spawn_point_id":"p0"}
      ]}]
    })JSON");
    level["name"] = name;
    return level;
}

Json tiny_spec() {
    return {{"schema", 1}, {"level_document", tiny_level()}, {"seed", 616},
            {"max_ticks", 120}, {"sample_ticks", 1},
            {"controller", {{"policy", "none"}, {"use_abilities", false}}},
            {"actions", Json::array({{{"tick", 0}, {"kind", "start_wave"}}})}};
}

void require_status(const Json& report, const char* expected) {
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("status") == expected);
}

Json replay_report(const Json& original) {
    GameConfig pinned;
    std::string error;
    const bool parsed = parse_game_config_documents(
        original.at("provenance").at("base_config").get<std::vector<Json>>(), pinned, error);
    INFO(error);
    REQUIRE(parsed);
    return balance::execute(original.at("replay_spec"), pinned);
}

struct LevelFile {
    std::filesystem::path path;

    explicit LevelFile(const Json& document) {
        static u64 sequence = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("immune-balance-level-" + std::to_string(stamp) + "-" + std::to_string(++sequence) + ".json");
        REQUIRE(platform::write_text_file(path.string(), document.dump()));
    }
    ~LevelFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

} // namespace

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance run executes paid script actions and the real survival objective",
          "[balance][run][deployment]") {
    const GameConfig config = run_test_config();
    auto spec = tiny_spec();
    const u32 cost = config.towers.stats[static_cast<u32>(TowerType::Neutrophil)].build_cost;
    spec["opening_atp"] = cost + 3;
    spec["income_enabled"] = false;
    spec["spend_limit"] = cost;
    spec["actions"].insert(spec["actions"].begin(),
        Json{{"tick", 0}, {"kind", "deploy"}, {"tower", "neutrophil"}, {"position", {30, 40}}});
    const auto report = balance::execute(spec, config);
    require_status(report, "cleared");
    CHECK(report["summary"]["valid"] == true);
    CHECK(report["summary"]["duration_ticks"] == 8);
    CHECK(report["economy"]["opening_atp"] == cost + 3);
    CHECK(report["economy"]["spent"] == cost);
    CHECK(report["economy"]["atp"] == 3);
    CHECK(report["economy"]["damage_income"] == 0);
    CHECK(report["economy"]["wave_income"] == 0);
    CHECK(report["actions"][0]["accepted"] == true);
    CHECK(report["actions"][0]["deployed"] == 1);
    CHECK(report["units"]["neutrophil"]["deployed"] == 1);
    CHECK(report["summary"]["abilities_used"] == 0);
    CHECK(report["reward"]["first_clear"] == true);
    CHECK(report["reward"]["antibodies"] == config.meta.first_clear_antibodies);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance run fixed wallet disables a real completed wave bounty",
          "[balance][run][economy]") {
    const GameConfig config = run_test_config();
    auto spec = tiny_spec();
    spec["level_document"].erase("win");
    spec["level_document"]["spawn_points"][0]["pos"] = {116, 40};
    spec["opening_atp"] = 7;
    const auto paid = balance::execute(spec, config);
    require_status(paid, "cleared");
    REQUIRE(paid["waves"][0]["completed"] == true);
    CHECK(paid["economy"]["wave_income"] == 75);
    CHECK(paid["economy"]["atp"] == 82);
    spec["income_enabled"] = false;
    const auto fixed = balance::execute(spec, config);
    require_status(fixed, "cleared");
    CHECK(fixed["waves"][0]["completed"] == true);
    CHECK(fixed["economy"]["wave_income"] == 0);
    CHECK(fixed["economy"]["damage_income"] == 0);
    CHECK(fixed["economy"]["atp"] == 7);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance first-clear report exposes the same affordable discovery offers as progression and replay",
          "[balance][run][progression][discovery]") {
    const GameConfig base = run_test_config();
    const auto report = balance::execute(tiny_spec(), base);
    require_status(report, "cleared");
    CHECK(report.at("reward").at("antibodies") == 1);
    CHECK(report.at("discovery").at("available_choices") == 5);
    MetaProgression after;
    std::string error;
    REQUIRE(after.from_json(report.at("progression_after").dump(), error));
    CHECK(after.discovery_offers(base.meta).size() == report.at("discovery").at("offers").size());
    const auto starter_cost = after.next_cost(kTreeRoot, base.meta);
    for (const auto& offer : report.at("discovery").at("offers")) {
        CHECK(offer.at("cost").at("memory_cells") == 0);
        CHECK(offer.at("cost").at("antibodies") == 1);
        CHECK(offer.at("prerequisite").at("key") == tree_node(kTreeRoot).key);
        CHECK(offer.at("prerequisite").at("cost").at("memory_cells") == starter_cost.memory_cells);
    }
    const auto replay = replay_report(report);
    CHECK(replay.at("state_hash") == report.at("state_hash"));
    CHECK(replay.at("reward") == report.at("reward"));
    CHECK(replay.at("discovery") == report.at("discovery"));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance run reports invalid setup and required refused actions explicitly",
          "[balance][run][validation]") {
    const GameConfig config = run_test_config();
    auto spec = tiny_spec();
    SECTION("missing levels never fall back to a test lane") {
        spec.erase("level_document");
        spec["level"] = "assets/balance/levels/does-not-exist.json";
    }
    SECTION("unknown fields never silently change an experiment") {
        spec["controllre"] = Json::object();
    }
    SECTION("actions outside the fixed run interval are rejected") {
        spec["actions"][0]["tick"] = spec["max_ticks"];
    }
    SECTION("forbidden cells fail without spending") {
        spec["progression"] = {{"profile", "all_unlocked"}};
        spec["actions"][0] = {{"kind", "deploy"}, {"tower", "macrophage"}, {"position", {30, 40}}};
    }
    SECTION("illegal real progression purchases are refused") {
        spec["progression"] = {{"purchases", Json::array({{{"key", "neutrophil.round_damage"}}})}};
    }
    const auto report = balance::execute(spec, config);
    require_status(report, "invalid");
    CHECK(report["summary"]["valid"] == false);
    CHECK_FALSE(report["diagnostics"]["errors"].empty());
    if (report.contains("economy")) CHECK(report["economy"]["spent"] == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance run optional refused placement is recorded without a free defense",
          "[balance][run][validation]") {
    const GameConfig config = run_test_config();
    auto spec = tiny_spec();
    spec["actions"].insert(spec["actions"].begin(),
        Json{{"kind", "deploy"}, {"tower", "neutrophil"}, {"position", {-10, -10}}, {"required", false}});
    const auto report = balance::execute(spec, config);
    require_status(report, "cleared");
    CHECK(report["summary"]["actions_rejected"] == 1);
    CHECK(report["actions"][0]["accepted"] == false);
    CHECK(report["actions"][0]["deployed"] == 0);
    CHECK(report["economy"]["spent"] == 0);
    CHECK(report["units"]["neutrophil"]["deployed"] == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance run replay preserves overrides progression paid actions and state",
          "[balance][run][replay]") {
    const GameConfig config = run_test_config();
    auto spec = tiny_spec();
    spec["overrides"] = {{"economy.starting_atp", 419},
                          {"towers.neutrophil.payload.round_damage", 13.25}};
    spec["progression"] = {{"memory_cells", 1000},
        {"purchases", Json::array({{{"key", "neutrophil.round_damage"}, {"levels", 1}}})}};
    spec["actions"].insert(spec["actions"].begin(),
        Json{{"kind", "deploy"}, {"tower", "neutrophil"}, {"position", {30, 40}}});
    const auto original = balance::execute(spec, config);
    require_status(original, "cleared");
    const auto replay = replay_report(original);
    require_status(replay, "cleared");
    CHECK(replay["state_hash"] == original["state_hash"]);
    CHECK(replay["state"] == original["state"]);
    CHECK(replay["economy"] == original["economy"]);
    CHECK(replay["provenance"]["effective_config_hash"] == original["provenance"]["effective_config_hash"]);
    CHECK(replay["progression_after"] == original["progression_after"]);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance run replay retains partial optional deployment effects",
          "[balance][run][replay]") {
    const GameConfig config = run_test_config();
    auto spec = tiny_spec();
    const u32 cost = config.towers.stats[0].build_cost;
    spec["opening_atp"] = cost + 1;
    spec["income_enabled"] = false;
    spec["actions"].insert(spec["actions"].begin(),
        Json{{"kind", "deploy"}, {"tower", "neutrophil"}, {"quantity", 3},
             {"position", {30, 40}}, {"required", false}});
    const auto original = balance::execute(spec, config);
    require_status(original, "cleared");
    REQUIRE(original["actions"][0]["deployed"] == 1);
    const auto replay = replay_report(original);
    require_status(replay, "cleared");
    CHECK(replay["state_hash"] == original["state_hash"]);
    CHECK(replay["state"] == original["state"]);
    CHECK(replay["economy"] == original["economy"]);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance run capacity denial is invalid evidence rather than an easier victory",
          "[balance][run][capacity]") {
    const GameConfig config = run_test_config();
    auto spec = tiny_spec();
    spec["overrides"] = {{"sim.capacities.max_chaff", 1}};
    spec["level_document"]["waves"][0]["spawns"][0]["count"] = 10;
    spec["level_document"]["waves"][0]["spawns"][0]["duration"] = 0.001;
    const auto report = balance::execute(spec, config);
    require_status(report, "invalid");
    CHECK(report["summary"]["denied_spawns"].get<u64>() > 0);
    CHECK(report["reward"]["memory_cells"] == 0);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance campaign uses earned rewards for real between-level purchases",
          "[balance][run][campaign][meta]") {
    const GameConfig config = run_test_config();
    LevelFile first(tiny_level("balance_run_campaign_one"));
    LevelFile second(tiny_level("balance_run_campaign_two"));
    auto spec = tiny_spec();
    spec.erase("level_document");
    spec["campaign"] = {{"levels", {first.path.string(), second.path.string()}},
        {"max_attempts_per_level", 2}, {"max_total_attempts", 4},
        {"purchase_policy", {{"priority", {"neutrophil.round_damage", "hub.bone_marrow_reserve"}},
                              {"repeat", false}}}};
    const auto report = balance::execute(spec, config);
    require_status(report, "completed");
    REQUIRE(report["attempts"].size() == 2);
    CHECK(report["summary"]["levels_cleared"] == 2);
    CHECK(report["attempts"][0]["purchases"].empty());
    CHECK_FALSE(report["attempts"][1]["purchases"].empty());
    CHECK(report["attempts"][0]["reward"]["first_clear"] == true);
    CHECK(report["attempts"][1]["reward"]["first_clear"] == true);
    CHECK(report["attempts"][1]["progression_before"]["tree"].contains("neutrophil.round_damage"));
    u64 earned = 0, spent = 0;
    for (const auto& attempt : report["attempts"]) earned += attempt["reward"]["memory_cells"].get<u64>();
    for (const auto& purchase : report["purchases"]) spent += purchase["memory_cells"].get<u64>();
    CHECK(report["final_progression"]["memory_cells"].get<u64>() == earned - spent);
    CHECK(report["final_progression"]["antibodies"] == config.meta.first_clear_antibodies * 2);
    std::vector<Json> streamed;
    const auto observed = balance::execute(spec, config, {}, [&](const Json& attempt) {
        CHECK(attempt["attempt_index"] == streamed.size());
        CHECK(attempt["summary"]["status"] == "cleared");
        CHECK(attempt["progression_after"]["progress"]["levels_completed"] == streamed.size() + 1);
        streamed.push_back(attempt);
    });
    REQUIRE(streamed.size() == 2);
    CHECK(observed["summary"] == report["summary"]);
    CHECK(observed["final_progression"] == report["final_progression"]);
    for (usize i = 0; i < streamed.size(); ++i) {
        CHECK(streamed[i]["state_hash"] == report["attempts"][i]["state_hash"]);
        CHECK(streamed[i]["economy"] == report["attempts"][i]["economy"]);
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation, "balance campaign losses earn real retry currency and stop at the attempt bound",
          "[balance][run][campaign][meta]") {
    const GameConfig config = run_test_config();
    auto level = tiny_level("balance_run_campaign_loss");
    level.erase("win");
    level["spawn_points"][0]["pos"] = {60, 40};
    level["objectives"][0]["pos"] = {60, 40};
    level["objectives"][0]["half_extents"] = {40, 30};
    // SimWorld currently initializes integrity to 100 independently of an
    // authored ObjectivePoint integrity. Force a real lethal leak population.
    level["waves"][0]["spawns"][0]["count"] = 200;
    LevelFile file(level);
    auto spec = tiny_spec();
    spec.erase("level_document");
    spec["campaign"] = {{"levels", {file.path.string()}}, {"max_attempts_per_level", 2},
        {"max_total_attempts", 2}, {"purchase_policy", {{"priority", Json::array()}}}};
    const auto report = balance::execute(spec, config);
    require_status(report, "stalled");
    REQUIRE(report["attempts"].size() == 2);
    CHECK(report["summary"]["levels_cleared"] == 0);
    for (const auto& attempt : report["attempts"]) {
        CHECK(attempt["summary"]["status"] == "defeated");
        CHECK(attempt["reward"]["memory_cells"].get<u64>() >= config.meta.base_run_reward);
        CHECK(attempt["reward"]["antibodies"] == 0);
    }
    CHECK(report["final_progression"]["memory_cells"] == config.meta.base_run_reward * 2);
    CHECK(report["final_progression"]["antibodies"] == 0);
}
