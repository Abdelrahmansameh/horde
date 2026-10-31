#include "BalanceTestIsolation.h"
#include "game/balance/BalanceRun.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "game/session/LevelSession.h"
#include "game/session/RunSetup.h"
#include "sim/hostile/HostileAttacks.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace immune;
using namespace immune::game;
using namespace immune::sim;
using Json=config::Json;

namespace {
Json level_document() {
    return Json::parse(R"JSON({"schema":2,"name":"level_scaling_fixture","region":"capillary",
      "world":{"min":[0,0],"max":[500,100],"cell_size":1},
      "economy":{"starting_atp":777,"income_multiplier":1.7},
      "vessels":[{"id":"main","points":[{"p":[4,50],"w":50},{"p":[496,50],"w":50}]}],
      "spawn_points":[{"id":"p0","pos":[100,50],"radius":2,"lane_id":"main"}],
      "objectives":[{"id":"organ","pos":[480,50],"half_extents":[3,20]}],
      "placement_zones":[{"min":[12,30],"max":[488,70]}],
      "waves":[{"name":"fixture","prep_time":0.1,"spawns":[{"family":"virus","count":8,"duration":0.1,"spawn_point_id":"p0"}]}]
    })JSON");
}
LevelDef parse(const Json& j) {
    LevelDef level;const auto result=LevelLoader{}.load_string(j.dump(),level);
    INFO(result.error);REQUIRE(result.ok);return level;
}
Json paid_spec(const Json& level) {
    return Json{{"schema",1},{"level_document",level},{"seed",82017},{"max_ticks",1800},
        {"sample_ticks",30},{"opening_atp",333},{"controller",{{"policy","none"}}},
        {"progression",{{"memory_cells",0},{"antibodies",0}}},
        {"actions",Json::array({{{"tick",1},{"kind","deploy"},{"tower","neutrophil"},{"position",{140,50}},{"quantity",10}},
                               {{"tick",2},{"kind","deploy"},{"tower","neutrophil"},{"position",{280,50}},{"quantity",5}},
                               {{"tick",3},{"kind","start_wave"}}})}};
}
}

TEST_CASE("level enemy scaling defaults serialize identically and authored values round trip", "[level][enemy-scaling]") {
    auto original=parse(level_document());
    CHECK(original.enemy_scaling.durability_multiplier==1);
    CHECK(original.enemy_scaling.damage_multiplier==1);
    const auto legacy=level_to_json(original);
    CHECK(legacy.find("enemy_scaling")==std::string::npos);
    auto explicit_default=level_document();explicit_default["enemy_scaling"]={{"durability_multiplier",1},{"damage_multiplier",1}};
    CHECK(level_to_json(parse(explicit_default))==legacy);
    original.enemy_scaling={4.125f,2.25f};
    const auto encoded=level_to_json(original);const auto restored=parse(Json::parse(encoded));
    CHECK(level_equal(original,restored));CHECK(level_to_json(restored)==encoded);
    auto different=restored;different.enemy_scaling.damage_multiplier=2.5f;
    CHECK_FALSE(level_equal(restored,different));
}

TEST_CASE("level enemy scaling rejects nonfinite nonpositive and excessive authored values", "[level][enemy-scaling]") {
    for (const char* key:{"durability_multiplier","damage_multiplier"}) {
        for (const double bad:{-1.0,0.0,0.009,64.001,1.0e100}) {
            auto j=level_document();j["enemy_scaling"]={{key,bad}};LevelDef ignored;
            CHECK_FALSE(LevelLoader{}.load_string(j.dump(),ignored).ok);
        }
        auto j=level_document();j["enemy_scaling"]={{key,nullptr}};LevelDef ignored;
        CHECK_FALSE(LevelLoader{}.load_string(j.dump(),ignored).ok);
    }
    EnemyScaling invalid;invalid.damage_multiplier=std::numeric_limits<f32>::infinity();
    CHECK_FALSE(valid_enemy_scaling(invalid));
    invalid.damage_multiplier=std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(valid_enemy_scaling(invalid));
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation,"level pressure is fresh per-world tuning and does not stack on reapply", "[level][session][enemy-scaling]") {
    auto base=default_game_config();EnemyRoster enemies;apply_enemy_config(enemies,base.enemies);
    auto level=parse(level_document());
    const auto ordinary=make_level_sim_desc(base,enemies,level,73);
    const auto unchanged=Json(dump_game_config(base)).dump();
    level.enemy_scaling={4,2};auto scaled=make_level_sim_desc(base,enemies,level,73);
    for (u32 f=0;f<kFamilyCount;++f) {
        CHECK(scaled.chaff_tuning.family[f].base_density==Catch::Approx(ordinary.chaff_tuning.family[f].base_density*4));
        CHECK(scaled.chaff_tuning.family[f].max_speed==ordinary.chaff_tuning.family[f].max_speed);
        CHECK(scaled.chaff_tuning.family[f].radius==ordinary.chaff_tuning.family[f].radius);
        CHECK(scaled.chaff_tuning.family[f].contact_spacing==ordinary.chaff_tuning.family[f].contact_spacing);
        CHECK(scaled.hostile_tuning.family[f].toxin_damage==ordinary.hostile_tuning.family[f].toxin_damage);
        CHECK(scaled.hostile_tuning.harpoon[f].pulse_damage==ordinary.hostile_tuning.harpoon[f].pulse_damage);
    }
    CHECK(scaled.hostile_tuning.damage_taken_mult==Catch::Approx(ordinary.hostile_tuning.damage_taken_mult*2));
    scaled.max_chaff=128;scaled.max_swarmers=32;scaled.max_projectiles=128;scaled.max_fluid_particles=32;
    SimWorld world;world.init(scaled,nullptr);REQUIRE(LevelLoader{}.instantiate(level,world).ok);
    TowerSystem towers;Economy economy;ActiveAbilitySystem abilities;
    towers.register_systems(world);enemies.register_systems(world);abilities.register_systems(world);
    RunTuning run;run.config=base;run.tree.hostile_damage_taken_mult=.6f;
    for (int repeat=0;repeat<3;++repeat) {
        apply_run_tuning(base,run,level,world,towers,enemies,economy,abilities);
        CHECK(world.hostile().tuning().damage_taken_mult==Catch::Approx(1.2f));
        for (u32 f=0;f<kFamilyCount;++f)
            CHECK(world.chaff_system().tuning().family[f].base_density==Catch::Approx(base.enemies.families[f].behavior.base_density*4));
        for (u32 f=0;f<kFamilyCount;++f)
            CHECK(enemy_config().families[f].behavior.base_density==base.enemies.families[f].behavior.base_density);
    }
    level.enemy_scaling={};apply_run_tuning(base,run,level,world,towers,enemies,economy,abilities);
    CHECK(world.hostile().tuning().damage_taken_mult==Catch::Approx(.6f));
    for (u32 f=0;f<kFamilyCount;++f)
        CHECK(world.chaff_system().tuning().family[f].base_density==base.enemies.families[f].behavior.base_density);
    CHECK(Json(dump_game_config(base)).dump()==unchanged);
}

TEST_CASE("the shared hostile damage factor reaches every pathogen family", "[sim][hostile][enemy-scaling]") {
    for (u32 family=0;family<kFamilyCount;++family) {
        const auto measure=[&](f32 factor) {
            ChaffBuffers chaff;chaff.reserve(8);ChaffSpawnParams p;p.family=static_cast<PathogenFamily>(family);
            p.position={30,40};p.density=8;REQUIRE(chaff.spawn(p).valid());
            SpatialHash hash;hash.configure({{{0,0},{80,80}},4});hash.rebuild(chaff.pos_x.data(),chaff.pos_y.data(),chaff.count());
            SwarmerBuffers allies;allies.reserve(4);SwarmerProfile profile;profile.max_health=100;profile.size=2;
            allies.set_profile(0,profile);SwarmerSpawnParams cell;cell.position={31,40};cell.profile=0;
            REQUIRE(allies.spawn(cell));FriendlyTowerList towers;HostileSystem hostile;HostileTuning tuning;
            tuning.enabled=true;tuning.damage_taken_mult=factor;
            tuning.family[family].aura_dps=8;tuning.family[family].aura_radius=5;
            hostile.set_tuning(tuning);
            return hostile.update(chaff,hash,allies,towers,TissueMask{},.25f,nullptr).swarmer_damage;
        };
        const f32 ordinary=measure(1);REQUIRE(ordinary>0);
        CHECK(measure(2)==Catch::Approx(ordinary*2));
    }
}

TEST_CASE("level durability preserves paid kill income per agent through repeated economy rules", "[level][session][economy][enemy-scaling]") {
    auto level=parse(level_document());EconomyConfig base;base.starting_atp=100;base.atp_per_density=10;
    Economy economy;TowerSystem towers;
    for (f32 factor:{1.0f,4.0f,4.0f,1.0f}) {
        level.enemy_scaling.durability_multiplier=factor;apply_level_rules(level,base,economy,towers);
        const auto opening=economy.atp();economy.credit_kills(10*factor);
        CHECK(economy.atp()-opening==170);
    }
}

TEST_CASE_METHOD(immune::test::BalanceConfigIsolation,"native wallet overrides compensate durability and default pressure is replay identical", "[balance][level][enemy-scaling]") {
    auto base=default_game_config();base.sim.hostile.enabled=false;base.economy.atp_per_density=10;
    for (auto& family:base.enemies.families) family.chaff.replication_rate=0;
    auto plain=paid_spec(level_document());const auto before=balance::execute(plain,base);
    REQUIRE(before["summary"]["valid"].get<bool>());REQUIRE(before["summary"]["kills"]==8);
    auto identity=plain;identity["level_document"]["enemy_scaling"]={{"durability_multiplier",1},{"damage_multiplier",1}};
    const auto same=balance::execute(identity,base);
    CHECK(before["state_hash"]==same["state_hash"]);CHECK(before["state"]==same["state"]);
    CHECK(before["economy"]==same["economy"]);CHECK(before["summary"]==same["summary"]);
    auto stronger=plain;stronger["level_document"]["enemy_scaling"]={{"durability_multiplier",4},{"damage_multiplier",2}};
    const auto after=balance::execute(stronger,base);
    REQUIRE(after["summary"]["valid"].get<bool>());REQUIRE(after["summary"]["kills"]==8);
    CHECK(after["summary"]["denied_spawns"]==0);CHECK(after["summary"]["clearing_timeout_advances"]==0);
    CHECK(after["economy"]["opening_atp"]==333);
    CHECK(before["economy"]["damage_income"]==after["economy"]["damage_income"]);
    CHECK(after["summary"]["density_removed"].get<double>()==Catch::Approx(before["summary"]["density_removed"].get<double>()*4));
}
