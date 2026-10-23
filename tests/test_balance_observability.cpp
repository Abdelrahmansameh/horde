#include "BalanceTestIsolation.h"
#include "game/balance/BalanceRun.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

using namespace immune;
using namespace immune::game;
using immune::config::Json;

namespace {

GameConfig observable_config() {
    config::ConfigStore store;
    GameConfig config;
    std::string error;
    REQUIRE(load_game_config(store,platform::asset_path("config"),config,error));
    config.sim.capacities.max_chaff = 128;
    config.sim.capacities.max_projectiles = 64;
    config.sim.capacities.max_fluid_particles = 64;
    config.sim.capacities.max_swarmers = 8;
    config.sim.hostile.enabled = false;
    for (auto& speed : config.enemies.speed_tiers) { speed.max_speed = 0; speed.jitter = 0; }
    for (auto& family : config.enemies.families) family.chaff.replication_rate = 0;
    auto& neutrophil = config.towers.mechanics[static_cast<u32>(TowerType::Neutrophil)];
    neutrophil.swarm.speed = 0;
    neutrophil.swarm.search_radius = 100;
    neutrophil.swarm.attach_radius = 20;
    neutrophil.shooter.magazine_size = 4;
    neutrophil.shooter.gather_seconds = 0;
    neutrophil.shooter.fire_interval = 0;
    neutrophil.shooter.reload_seconds = 2;
    neutrophil.shooter.round_speed = 5;
    neutrophil.shooter.round_hit_radius = 0.01f;
    neutrophil.shooter.kite_fraction = 0;
    return config;
}

Json observable_spec(const char* type = "neutrophil") {
    const auto level = Json::parse(R"JSON({
      "schema":2,"name":"observability_fixture","region":"capillary",
      "world":{"min":[0,0],"max":[120,80],"cell_size":1},
      "economy":{"starting_atp":1000,"income_multiplier":1},
      "allowed_towers":["neutrophil","goblet_cell"],
      "win":{"survive_seconds":0.25},
      "vessels":[{"id":"main","points":[{"p":[4,40],"w":30},{"p":[116,40],"w":30}]}],
      "spawn_points":[{"id":"p0","pos":[50,40],"radius":1,"lane_id":"main"}],
      "objectives":[{"id":"organ","pos":[116,40],"half_extents":[3,15]}],
      "placement_zones":[{"min":[12,25],"max":[108,55]}],
      "waves":[{"name":"probe","prep_time":0,"spawns":[
        {"family":"virus","count":20,"duration":0.01,"spawn_point_id":"p0"}
      ]}]
    })JSON");
    return {{"schema",1},{"level_document",level},{"seed",1234},{"max_ticks",120},{"sample_ticks",1},
        {"controller",{{"policy","none"},{"use_abilities",false}}},
        {"progression",{{"profile","all_unlocked"}}},
        {"actions",Json::array({
            {{"tick",0},{"kind","deploy"},{"tower",type},{"position",{45,40}}},
            {{"tick",0},{"kind","start_wave"}}
        })}};
}

} // namespace

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation,
                 "balance reports reconcile emitted shots live rounds activity and the ATP ledger",
                 "[balance][observability]") {
    const auto report = balance::execute(observable_spec(),observable_config());
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    REQUIRE(report.at("combat").at("enabled") == true);
    const auto& shots = report.at("units").at("neutrophil").at("projectiles");
    REQUIRE(shots.at("spawned").get<u64>() > 0);
    REQUIRE(shots.at("spawned") == report.at("combat").at("pools").at("projectiles").at("spawned"));
    REQUIRE(shots.at("projectiles_hit").get<u64>() <= shots.at("spawned").get<u64>());
    REQUIRE(report.at("combat").at("projectile_outcomes").at("live_at_end") == report.at("state").at("projectiles_live"));
    REQUIRE(report.at("combat_conservation").at("projectile_population") == true);
    REQUIRE(report.at("combat_conservation").at("attributed_emissions") == true);
    REQUIRE(report.at("combat_conservation").at("attributed_damage") == true);
    REQUIRE(report.at("conservation").at("economy").at("reconciled") == true);
    for (const auto* family : {"virus","bacteria","parasite"})
        REQUIRE(report.at("conservation").at("enemies").at(family) == true);
    const auto& activity = report.at("units").at("neutrophil").at("activity");
    REQUIRE(activity.at("observed_ticks").get<u64>() > 0);
    REQUIRE(activity.at("overlapping") == true);
    REQUIRE(activity.at("exact_subtick_durations") == false);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation,
                 "disabled combat telemetry is unavailable and preserves the gameplay replay",
                 "[balance][observability][determinism]") {
    const auto config = observable_config();
    auto spec = observable_spec();
    const auto enabled = balance::execute(spec,config);
    spec["combat_metrics"] = false;
    const auto disabled = balance::execute(spec,config);
    INFO(disabled.at("diagnostics").dump());
    REQUIRE(disabled.at("summary").at("valid") == true);
    REQUIRE(disabled.at("state_hash") == enabled.at("state_hash"));
    REQUIRE(disabled.at("state") == enabled.at("state"));
    REQUIRE(disabled.at("economy") == enabled.at("economy"));
    REQUIRE(disabled.at("combat").at("enabled") == false);
    REQUIRE(disabled.at("combat").at("damage").is_null());
    REQUIRE(disabled.at("combat").at("pools").is_null());
    REQUIRE(disabled.at("units").at("neutrophil").at("density_removed").is_null());
    REQUIRE_FALSE(disabled.at("units").at("neutrophil").contains("lifecycle"));
    REQUIRE(disabled.at("combat").at("enemy_lifetimes").empty());
    REQUIRE(disabled.at("conservation").at("economy").at("reconciled") == true);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation,
                 "suppressed projectile and mucus output invalidate balance evidence",
                 "[balance][observability][capacity]") {
    auto config = observable_config();
    auto spec = observable_spec();
    const char* pool = "projectiles";
    SECTION("a projectile magazine exceeds the finite round pool") {
        config.sim.capacities.max_projectiles = 1;
    }
    SECTION("a mucus burst exceeds the finite droplet pool") {
        pool = "fluid";
        spec = observable_spec("goblet_cell");
        config.sim.capacities.max_fluid_particles = 2;
        auto& goblet = config.towers.mechanics[static_cast<u32>(TowerType::GobletCell)];
        goblet.swarm.speed = 0;
        goblet.swarm.search_radius = 100;
        goblet.swarm.attach_radius = 10;
        goblet.mucus_bomber.droplets = 7;
        goblet.mucus_bomber.chase_seconds = 0.01f;
    }
    const auto report = balance::execute(spec,config);
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("status") == "invalid");
    REQUIRE(report.at("combat").at("pools").at(pool).at("denied").get<u64>() > 0);
    const auto& counts = report.at("combat").at("pools").at(pool);
    REQUIRE(counts.at("requested").get<u64>() == counts.at("spawned").get<u64>()+counts.at("denied").get<u64>());
    REQUIRE(counts.at("highwater") == counts.at("capacity"));
    REQUIRE(report.at("diagnostics").at("errors").dump().find("capacity") != std::string::npos);
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation,
                 "player deployment capacity rejection remains distinct from lost emitted output",
                 "[balance][observability][capacity]") {
    auto config = observable_config();
    config.sim.capacities.max_swarmers = 1;
    auto spec = observable_spec();
    spec["actions"][0]["quantity"] = 2;
    spec["actions"][0]["required"] = false;
    const auto report = balance::execute(spec,config);
    INFO(report.at("diagnostics").dump());
    REQUIRE(report.at("summary").at("valid") == true);
    REQUIRE(report.at("summary").at("deploy_capacity_rejected") == 1);
    REQUIRE(report.at("actions")[0].at("reason") == "cell_capacity");
    REQUIRE(report.at("combat").at("pools").at("cells").at("spawned") == 1);
    REQUIRE(report.at("combat").at("pools").at("cells").at("denied") == 0);
}
