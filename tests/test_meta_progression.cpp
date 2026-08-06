// Tests for game/meta/MetaProgression.cpp: the antibody-memory currency loop
// (DESIGN.md §7.2/§7.3) -- earn-on-every-run, purchase spending unlocks,
// versioned JSON save/load round-trips and rejects future versions, and the
// loadout-modifier combination rule. Owner: Wave 5A.
#include "game/meta/MetaProgression.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::game;

namespace {

/// Same scratch-dir convention as test_render_screenshots.cpp: files land in
/// $TEMP, not under assets/, since they're throwaway test fixtures.
std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

AntibodyMemory make_memory(const std::string& id, MemoryTier tier, u32 cost, bool unlocked) {
    AntibodyMemory m;
    m.id = id;
    m.display_name = id;
    m.tier = tier;
    m.cost = cost;
    m.unlocked = unlocked;
    return m;
}

} // namespace

// ---- earn_for_run -----------------------------------------------------------

TEST_CASE("earn_for_run credits currency on a loss with some waves cleared", "[meta]") {
    RunResult result;
    result.waves_cleared = 3;
    result.chaff_killed_total = 500;
    result.elites_killed = 1;
    result.bosses_killed = 0;
    result.won = false;

    const u32 earned = MetaProgression::earn_for_run(result);
    REQUIRE(earned > 0);
}

TEST_CASE("earn_for_run credits currency even on a total wipe", "[meta]") {
    RunResult result; // everything default/zero, including won = false.
    const u32 earned = MetaProgression::earn_for_run(result);
    REQUIRE(earned > 0); // kBaseRunReward alone guarantees this.
}

TEST_CASE("earn_for_run rewards more for a better performance", "[meta]") {
    RunResult worse;
    worse.waves_cleared = 1;

    RunResult better;
    better.waves_cleared = 5;
    better.chaff_killed_total = 4000;
    better.elites_killed = 3;
    better.bosses_killed = 1;
    better.won = true;

    REQUIRE(MetaProgression::earn_for_run(better) > MetaProgression::earn_for_run(worse));
}

// ---- purchase ----------------------------------------------------------------

TEST_CASE("purchase spends currency and unlocks a global-baseline memory", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(1000);

    // Directly exercise purchase() via from_json since memories_ has no
    // public mutator outside load paths -- build a small save blob.
    const std::string json = R"JSON({
      "version": 1,
      "antibody_points": 1000,
      "memories": [
        { "id": "baseline_income", "display_name": "Baseline Income",
          "tier": 0, "cost": 100, "unlocked": false }
      ]
    })JSON";
    std::string err;
    REQUIRE(meta.from_json(json, err));
    REQUIRE(meta.antibody_points() == 1000);

    const auto result = meta.purchase("baseline_income");
    REQUIRE(result == MetaProgression::PurchaseResult::Ok);
    REQUIRE(meta.antibody_points() == 900);

    bool found_unlocked = false;
    for (const auto& m : meta.memories()) {
        if (m.id == "baseline_income") found_unlocked = m.unlocked;
    }
    REQUIRE(found_unlocked);
}

TEST_CASE("purchase fails without enough currency and spends nothing", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();

    const std::string json = R"JSON({
      "version": 1,
      "antibody_points": 10,
      "memories": [
        { "id": "pricey", "tier": 2, "cost": 500, "unlocked": false }
      ]
    })JSON";
    std::string err;
    REQUIRE(meta.from_json(json, err));

    const auto result = meta.purchase("pricey");
    REQUIRE(result == MetaProgression::PurchaseResult::InsufficientFunds);
    REQUIRE(meta.antibody_points() == 10);
}

TEST_CASE("purchase of a roster-unlock memory also unlocks its tower", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    REQUIRE_FALSE(meta.tower_unlocked(TowerType::BCell));

    const std::string json = R"JSON({
      "version": 1,
      "antibody_points": 200,
      "memories": [
        { "id": "unlock_bcell", "tier": 1, "cost": 150, "unlocked": false,
          "unlocks_tower": 4 }
      ]
    })JSON";
    std::string err;
    REQUIRE(meta.from_json(json, err));
    REQUIRE(static_cast<u32>(TowerType::BCell) == 4);

    REQUIRE(meta.purchase("unlock_bcell") == MetaProgression::PurchaseResult::Ok);
    REQUIRE(meta.tower_unlocked(TowerType::BCell));
}

TEST_CASE("purchase of an unknown id reports NotFound", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(500);
    REQUIRE(meta.purchase("does_not_exist") == MetaProgression::PurchaseResult::NotFound);
    REQUIRE(meta.antibody_points() == 500);
}

// ---- save / load round-trip --------------------------------------------------

TEST_CASE("save/load round-trips every field including currency and loadout", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(777);
    meta.unlock_tower(TowerType::NKCell);
    meta.record_level_complete("capillary_01");

    const std::string json = R"JSON({
      "version": 1,
      "antibody_points": 777,
      "loadout_slots": 2,
      "memories": [
        { "id": "mem_a", "display_name": "Memory A", "unlocked": true, "tier": 0,
          "cost": 50, "starting_atp_bonus": 25.0, "tower_cost_multiplier": 0.9,
          "income_multiplier": 1.1,
          "damage_vs_family": [1.2, 1.0, 1.0, 1.0, 1.0, 1.0] }
      ],
      "loadout": ["mem_a"]
    })JSON";
    std::string err;
    REQUIRE(meta.from_json(json, err));
    REQUIRE(meta.selected_loadout().size() == 1); // set via the "loadout" field above

    const std::string dumped = meta.to_json();

    MetaProgression reloaded;
    std::string err2;
    REQUIRE(reloaded.from_json(dumped, err2));

    REQUIRE(reloaded.antibody_points() == meta.antibody_points());
    REQUIRE(reloaded.max_loadout_slots() == meta.max_loadout_slots());
    REQUIRE(reloaded.memories().size() == meta.memories().size());
    REQUIRE(reloaded.memories()[0].id == "mem_a");
    REQUIRE(reloaded.memories()[0].unlocked);
    REQUIRE(reloaded.memories()[0].cost == 50);
    REQUIRE(std::abs(reloaded.memories()[0].starting_atp_bonus - 25.0f) < 1e-4f);
    REQUIRE(reloaded.selected_loadout() == meta.selected_loadout());
}

TEST_CASE("save then load via the filesystem round-trips", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(321);
    meta.record_level_complete("floodplain_02");

    const std::string path = scratch_path("immune_meta_progression_test.json");
    REQUIRE(meta.save(path));

    MetaProgression reloaded;
    REQUIRE(reloaded.load(path));
    REQUIRE(reloaded.antibody_points() == 321);
    REQUIRE(reloaded.progress().levels_completed == 1);
    REQUIRE(reloaded.progress().completed_level_ids.size() == 1);
    REQUIRE(reloaded.progress().completed_level_ids[0] == "floodplain_02");
}

TEST_CASE("loading a future-versioned save fails loudly and leaves state untouched", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(99);
    meta.record_level_complete("baseline_level");

    const std::string future_json = R"JSON({
      "version": 999,
      "antibody_points": 123456
    })JSON";
    std::string err;
    const bool ok = meta.from_json(future_json, err);

    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());
    // State from before the failed load must be completely untouched.
    REQUIRE(meta.antibody_points() == 99);
    REQUIRE(meta.progress().levels_completed == 1);
}

TEST_CASE("loading a malformed save fails loudly and leaves state untouched", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(55);

    std::string err;
    REQUIRE_FALSE(meta.from_json("{ not valid json", err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(meta.antibody_points() == 55);

    // Missing 'version' entirely is also an error, not a silent default.
    std::string err2;
    REQUIRE_FALSE(meta.from_json(R"({"antibody_points": 7})", err2));
    REQUIRE_FALSE(err2.empty());
    REQUIRE(meta.antibody_points() == 55);
}

TEST_CASE("loading a bad memory entry (missing id) fails and leaves state untouched", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(42);

    const std::string bad_json = R"JSON({
      "version": 1,
      "antibody_points": 5000,
      "memories": [ { "display_name": "no id here" } ]
    })JSON";
    std::string err;
    REQUIRE_FALSE(meta.from_json(bad_json, err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(meta.antibody_points() == 42);
}

TEST_CASE("loading an older save (missing new fields) migrates by defaulting them", "[meta]") {
    // A "v1 save from before Wave 5A" -- has version but none of the fields
    // this wave added. Must not fail; must default them.
    const std::string old_json = R"JSON({ "version": 1 })JSON";
    MetaProgression meta;
    std::string err;
    REQUIRE(meta.from_json(old_json, err));
    REQUIRE(meta.antibody_points() == 0);
    REQUIRE(meta.max_loadout_slots() == 2);
    REQUIRE(meta.memories().empty());
}

// ---- loadout modifier combination --------------------------------------------

TEST_CASE("combine_loadout_modifiers multiplies multipliers and sums flat bonuses", "[meta]") {
    std::vector<AntibodyMemory> pool;

    AntibodyMemory a = make_memory("a", MemoryTier::GlobalBaseline, 0, true);
    a.damage_vs_family[static_cast<u32>(PathogenFamily::Virus)] = 1.2f;
    a.starting_atp_bonus = 25.0f;
    a.tower_cost_multiplier = 0.9f;
    a.income_multiplier = 1.1f;
    pool.push_back(a);

    AntibodyMemory b = make_memory("b", MemoryTier::Specialized, 0, true);
    b.damage_vs_family[static_cast<u32>(PathogenFamily::Virus)] = 1.5f;
    b.starting_atp_bonus = 10.0f;
    b.tower_cost_multiplier = 0.95f;
    b.income_multiplier = 1.05f;
    pool.push_back(b);

    const std::vector<std::string> selected = {"a", "b"};
    const LoadoutModifiers mods = MetaProgression::combine_loadout_modifiers(pool, selected);

    const f32 expected_virus_dmg = 1.2f * 1.5f;
    const f32 expected_atp = 25.0f + 10.0f;
    const f32 expected_cost_mul = 0.9f * 0.95f;
    const f32 expected_income_mul = 1.1f * 1.05f;

    REQUIRE(std::abs(mods.damage_vs_family[static_cast<u32>(PathogenFamily::Virus)] - expected_virus_dmg) < 1e-4f);
    REQUIRE(std::abs(mods.starting_atp_bonus - expected_atp) < 1e-4f);
    REQUIRE(std::abs(mods.tower_cost_multiplier - expected_cost_mul) < 1e-4f);
    REQUIRE(std::abs(mods.income_multiplier - expected_income_mul) < 1e-4f);

    // Untouched families stay at the neutral 1.0 multiplier.
    REQUIRE(std::abs(mods.damage_vs_family[static_cast<u32>(PathogenFamily::Bacteria)] - 1.0f) < 1e-4f);
}

TEST_CASE("combine_loadout_modifiers on an empty loadout is fully neutral", "[meta]") {
    std::vector<AntibodyMemory> pool;
    const LoadoutModifiers mods = MetaProgression::combine_loadout_modifiers(pool, {});
    for (u32 i = 0; i < kFamilyCount; ++i) {
        REQUIRE(std::abs(mods.damage_vs_family[i] - 1.0f) < 1e-4f);
    }
    REQUIRE(mods.starting_atp_bonus == 0.0f);
    REQUIRE(mods.tower_cost_multiplier == 1.0f);
    REQUIRE(mods.income_multiplier == 1.0f);
}

TEST_CASE("compute_loadout_modifiers reflects the instance's selected_loadout", "[meta]") {
    MetaProgression meta;
    meta.reset_to_new_game();

    const std::string json = R"JSON({
      "version": 1,
      "memories": [
        { "id": "dmg_virus", "unlocked": true, "tier": 0,
          "damage_vs_family": [2.0, 1.0, 1.0, 1.0, 1.0, 1.0] }
      ]
    })JSON";
    std::string err;
    REQUIRE(meta.from_json(json, err));
    REQUIRE(meta.select_memory("dmg_virus"));

    const LoadoutModifiers mods = meta.compute_loadout_modifiers();
    REQUIRE(std::abs(mods.damage_vs_family[static_cast<u32>(PathogenFamily::Virus)] - 2.0f) < 1e-4f);
}
