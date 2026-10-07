// Tests for game/meta/MetaProgression.cpp: the two-currency Strengthen
// Immunity loop (PROGRESSION.md §3) -- Memory Cells on every run, Antibodies
// once per level on its first clear, tree purchases and their gates, respec,
// and versioned JSON save/load (round-trip, v1/v2 migration, future-version
// rejection).
#include "game/config/GameConfig.h"
#include "game/meta/MetaProgression.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

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

MetaProgression fresh() {
    MetaProgression m;
    m.reset_to_new_game();
    return m;
}

/// Buys `n` levels of `node`, crediting enough Memory Cells first. Stops (and
/// fails the test) the moment a purchase is refused.
void buy_levels(MetaProgression& m, TreeNode node, u32 n, const MetaConfig& cfg) {
    for (u32 i = 0; i < n; ++i) {
        m.credit(m.next_cost(node, cfg).memory_cells);
        REQUIRE(m.purchase(node, cfg) == MetaProgression::PurchaseResult::Ok);
    }
}

} // namespace

// ---- A new campaign ----------------------------------------------------------

TEST_CASE("a new campaign owns the Neutrophil, nothing else, and no currency", "[meta]") {
    const MetaProgression m = fresh();
    REQUIRE(m.memory_cells() == 0);
    REQUIRE(m.antibodies() == 0);
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::Macrophage));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::CytotoxicT));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::GobletCell));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::Fibroblast));
    for (u32 a = 0; a < kAbilityCount; ++a) {
        REQUIRE_FALSE(m.ability_unlocked(static_cast<AbilityId>(a)));
    }
    REQUIRE(unlocked_tower_mask(m.levels()) == (1u << static_cast<u32>(TowerType::Neutrophil)));
    REQUIRE(unlocked_ability_mask(m.levels()) == 0u);
}

// ---- Run payouts ------------------------------------------------------------

TEST_CASE("every run pays Memory Cells, even a wipe on the first wave", "[meta]") {
    const MetaConfig cfg;
    const RunReward wipe = MetaProgression::reward_for_run(RunResult{}, cfg, false);
    REQUIRE(wipe.memory_cells > 0);
    REQUIRE(wipe.antibodies == 0);
}

TEST_CASE("a better run pays more Memory Cells", "[meta]") {
    const MetaConfig cfg;
    RunResult worse;
    worse.waves_cleared = 1;
    RunResult better;
    better.waves_cleared = 5;
    better.chaff_killed_total = 4000;
    better.won = true;
    REQUIRE(MetaProgression::reward_for_run(better, cfg, false).memory_cells >
            MetaProgression::reward_for_run(worse, cfg, false).memory_cells);
}

TEST_CASE("Antibodies are paid on a level's first clear only", "[meta]") {
    MetaConfig cfg;
    cfg.first_clear_antibodies = 1;
    MetaProgression m = fresh();

    RunResult lost;
    lost.waves_cleared = 4;
    const RunReward r0 = m.record_run("capillary_1", lost, cfg);
    REQUIRE(r0.memory_cells > 0);
    REQUIRE(r0.antibodies == 0);
    REQUIRE_FALSE(m.level_cleared("capillary_1"));

    RunResult won;
    won.waves_cleared = 6;
    won.won = true;
    const RunReward r1 = m.record_run("capillary_1", won, cfg);
    REQUIRE(r1.first_clear);
    REQUIRE(r1.antibodies == 1);
    REQUIRE(m.level_cleared("capillary_1"));
    REQUIRE(m.antibodies() == 1);

    // A replay of the same level, won again: Memory Cells yes, Antibody no.
    const RunReward r2 = m.record_run("capillary_1", won, cfg);
    REQUIRE_FALSE(r2.first_clear);
    REQUIRE(r2.antibodies == 0);
    REQUIRE(r2.memory_cells == r1.memory_cells);
    REQUIRE(m.antibodies() == 1);
    REQUIRE(m.memory_cells() == u64{r0.memory_cells} + r1.memory_cells + r2.memory_cells);
    REQUIRE(m.progress().levels_completed == 1);
}

// ---- Purchases ----------------------------------------------------------------

TEST_CASE("a tower root costs an Antibody once the node it grows from is owned", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(0, 1);
    // The Goblet Cell grows from Rapid Metabolism, one of the three cores.
    REQUIRE(m.check_purchase(TreeNode::GobletRoot, cfg) == MetaProgression::PurchaseResult::Locked);
    buy_levels(m, TreeNode::RapidMetabolism, 1, cfg);
    REQUIRE(m.purchase(TreeNode::GobletRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.tower_unlocked(TowerType::GobletCell));
    REQUIRE(m.antibodies() == 0);
    REQUIRE(m.purchase(TreeNode::GobletRoot, cfg) == MetaProgression::PurchaseResult::Maxed);
    REQUIRE(m.check_purchase(TreeNode::FibroblastRoot, cfg) == MetaProgression::PurchaseResult::NeedAntibodies);
}

TEST_CASE("a node opens when its parent is owned, at any level", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(100000, 5);
    // The three cores round the Neutrophil are open from the start; the rest is not.
    for (TreeNode n : {TreeNode::BoneMarrowReserve, TreeNode::EfficientClearance, TreeNode::RapidMetabolism}) {
        REQUIRE(m.check_purchase(n, cfg) == MetaProgression::PurchaseResult::Ok);
    }
    REQUIRE(m.check_purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Locked);
    REQUIRE(m.check_purchase(TreeNode::FibroblastRoot, cfg) == MetaProgression::PurchaseResult::Locked);
    // One level of Bone Marrow Reserve opens the attack towers' wedge.
    buy_levels(m, TreeNode::BoneMarrowReserve, 1, cfg);
    REQUIRE(m.check_purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::CytotoxicRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::MacrophageRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::FibroblastRoot, cfg) == MetaProgression::PurchaseResult::Locked);
    // An ability is two cheap nodes out: Efficient Clearance, then its gate.
    for (TreeNode n : {TreeNode::EfficientClearance, TreeNode::Homeostasis}) {
        REQUIRE(m.check_purchase(TreeNode::FeverUnlock, cfg) == MetaProgression::PurchaseResult::Locked);
        buy_levels(m, n, 1, cfg);
    }
    REQUIRE(m.purchase(TreeNode::FeverUnlock, cfg) == MetaProgression::PurchaseResult::Ok);
}

TEST_CASE("a stat line needs its tower's root and costs more each level", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(10000);
    REQUIRE(m.check_purchase(TreeNode::GobletSlowStrength, cfg) ==
            MetaProgression::PurchaseResult::Locked);
    // The Neutrophil is owned from the start; its lines open behind Bone
    // Marrow Reserve, one of the cores round it.
    REQUIRE(m.check_purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Locked);
    const u32 core = m.next_cost(TreeNode::BoneMarrowReserve, cfg).memory_cells;
    REQUIRE(m.purchase(TreeNode::BoneMarrowReserve, cfg) == MetaProgression::PurchaseResult::Ok);
    const u32 c0 = m.next_cost(TreeNode::NeutrophilRoundDamage, cfg).memory_cells;
    REQUIRE(m.purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Ok);
    const u32 c1 = m.next_cost(TreeNode::NeutrophilRoundDamage, cfg).memory_cells;
    REQUIRE(c1 > c0);
    REQUIRE(m.memory_cells() == 10000 - core - c0);
    REQUIRE(m.level(TreeNode::NeutrophilRoundDamage) == 1);
}

TEST_CASE("a refused purchase changes nothing", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(5);
    const std::string before = m.to_json();
    REQUIRE(m.purchase(TreeNode::BoneMarrowReserve, cfg) ==
            MetaProgression::PurchaseResult::NeedMemoryCells);
    REQUIRE(m.to_json() == before);
}

TEST_CASE("a stat line stops at its max level", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    buy_levels(m, TreeNode::BoneMarrowReserve, 1, cfg);
    buy_levels(m, TreeNode::NeutrophilRoundDamage, 1, cfg);
    const u8 max = tree_node(TreeNode::NeutrophilAccuracy).max_level;
    buy_levels(m, TreeNode::NeutrophilAccuracy, max, cfg);
    m.credit(100000);
    REQUIRE(m.purchase(TreeNode::NeutrophilAccuracy, cfg) == MetaProgression::PurchaseResult::Maxed);
    REQUIRE(m.next_cost(TreeNode::NeutrophilAccuracy, cfg).memory_cells == 0);
}

TEST_CASE("a capstone needs points in its branch and both currencies", "[meta]") {
    MetaConfig cfg;
    cfg.capstone_threshold = 4;
    MetaProgression m = fresh();
    m.credit(100000, 5);
    REQUIRE(m.check_purchase(TreeNode::NeutrophilCapstone, cfg) == MetaProgression::PurchaseResult::Locked);
    // The capstone grows from Accuracy, and every line in the branch counts
    // toward the threshold, on its path or not.
    buy_levels(m, TreeNode::BoneMarrowReserve, 1, cfg);
    buy_levels(m, TreeNode::NeutrophilRoundDamage, 1, cfg);
    REQUIRE(m.check_purchase(TreeNode::NeutrophilCapstone, cfg) == MetaProgression::PurchaseResult::Locked);
    buy_levels(m, TreeNode::NeutrophilAccuracy, 1, cfg);
    REQUIRE(m.check_purchase(TreeNode::NeutrophilCapstone, cfg) ==
            MetaProgression::PurchaseResult::BelowThreshold);
    buy_levels(m, TreeNode::NeutrophilTriggerRate, 1, cfg);
    buy_levels(m, TreeNode::NeutrophilSquadSize, 1, cfg);
    REQUIRE(m.branch_points(TreeBranch::Neutrophil) == 4);
    const u64 mc = m.memory_cells();
    REQUIRE(m.purchase(TreeNode::NeutrophilCapstone, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.memory_cells() == mc - cfg.capstone_memory_cells);
    REQUIRE(m.antibodies() == 5 - cfg.capstone_antibodies);
}

TEST_CASE("an ability line needs the ability's root", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(10000, 1);
    REQUIRE(m.check_purchase(TreeNode::HistamineRadius, cfg) == MetaProgression::PurchaseResult::Locked);
    // Histamine Flare is behind Efficient Clearance and Systemic Potency.
    buy_levels(m, TreeNode::EfficientClearance, 1, cfg);
    buy_levels(m, TreeNode::SystemicPotency, 1, cfg);
    REQUIRE(m.purchase(TreeNode::HistamineUnlock, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.ability_unlocked(AbilityId::HistamineFlare));
    REQUIRE(m.purchase(TreeNode::HistamineRadius, cfg) == MetaProgression::PurchaseResult::Ok);
}

TEST_CASE("respec refunds everything spent minus the fee and keeps the Neutrophil", "[meta]") {
    MetaConfig cfg;
    cfg.respec_cost = 25;
    MetaProgression m = fresh();
    REQUIRE_FALSE(m.can_respec(cfg)); // nothing bought yet
    m.credit(1000, 2);
    REQUIRE(m.purchase(TreeNode::BoneMarrowReserve, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::CytotoxicRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::CytotoxicDrain, cfg) == MetaProgression::PurchaseResult::Ok);

    REQUIRE(m.respec(cfg));
    REQUIRE(m.memory_cells() == 1000 - 25);
    REQUIRE(m.antibodies() == 2);
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::CytotoxicT));
    REQUIRE(m.level(TreeNode::BoneMarrowReserve) == 0);
    REQUIRE_FALSE(m.can_respec(cfg));
}

// ---- Save / load --------------------------------------------------------------

TEST_CASE("save/load round-trips currencies, the tree and progress", "[meta]") {
    MetaConfig cfg;
    cfg.capstone_threshold = 1;
    MetaProgression m = fresh();
    m.credit(5000, 3);
    for (TreeNode n : {TreeNode::RapidMetabolism, TreeNode::GobletRoot, TreeNode::GobletSlowStrength,
                       TreeNode::GobletSlowDuration, TreeNode::GobletSlowDuration, TreeNode::GobletSplashRadius,
                       TreeNode::GobletWeakness, TreeNode::GobletCapstone, TreeNode::EfficientClearance,
                       TreeNode::Homeostasis, TreeNode::FeverUnlock}) {
        REQUIRE(m.purchase(n, cfg) == MetaProgression::PurchaseResult::Ok);
    }
    m.record_level_complete("capillary_1");

    const std::string path = scratch_path("immune_meta_roundtrip.json");
    REQUIRE(m.save(path));
    MetaProgression back;
    REQUIRE(back.load(path));
    REQUIRE(back.to_json() == m.to_json());
    REQUIRE(back.memory_cells() == m.memory_cells());
    REQUIRE(back.antibodies() == m.antibodies());
    REQUIRE(back.level(TreeNode::GobletSlowDuration) == 2);
    REQUIRE(back.level(TreeNode::GobletCapstone) == 1);
    REQUIRE(back.ability_unlocked(AbilityId::FeverResponse));
    REQUIRE(back.level_cleared("capillary_1"));
    REQUIRE(back.spent_antibodies() == m.spent_antibodies());
}

TEST_CASE("a v2 save migrates: points become Memory Cells, towers become roots", "[meta]") {
    MetaProgression m;
    std::string err;
    const std::string v2 = R"({
      "version": 2,
      "antibody_points": 321,
      "loadout_slots": 2,
      "unlocked_towers": [0, 1, 3],
      "memories": [{"id": "mem_a", "unlocked": true}],
      "loadout": ["mem_a"],
      "progress": {"levels_completed": 1, "completed_level_ids": ["capillary_1"]}
    })";
    REQUIRE(m.from_json(v2, err));
    REQUIRE(err.empty());
    REQUIRE(m.memory_cells() == 321);
    REQUIRE(m.antibodies() == 0);
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil));
    REQUIRE(m.tower_unlocked(TowerType::Macrophage));
    REQUIRE(m.tower_unlocked(TowerType::GobletCell));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::CytotoxicT));
    REQUIRE(m.level_cleared("capillary_1"));
}

TEST_CASE("a v1 save migrates its tower indices across the cut slot", "[meta]") {
    MetaProgression m;
    std::string err;
    // v1 slot 2 was cut; v1 slot 4 is today's slot 3 (the Goblet Cell).
    REQUIRE(m.from_json(R"({"version": 1, "antibody_points": 7, "unlocked_towers": [2, 4]})", err));
    REQUIRE(m.tower_unlocked(TowerType::GobletCell));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::CytotoxicT));
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil)); // always owned
}

TEST_CASE("load rejects a future version and leaves state untouched", "[meta]") {
    MetaProgression m = fresh();
    m.credit(42, 1);
    const std::string before = m.to_json();
    std::string err;
    REQUIRE_FALSE(m.from_json(R"({"version": 99, "memory_cells": 5})", err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(m.to_json() == before);
}

TEST_CASE("load rejects a missing version and malformed JSON", "[meta]") {
    MetaProgression m = fresh();
    std::string err;
    REQUIRE_FALSE(m.from_json(R"({"memory_cells": 5})", err));
    REQUIRE_FALSE(m.from_json("{not json", err));
    REQUIRE_FALSE(m.from_json(R"({"version": 3, "tree": [1, 2]})", err));
}

TEST_CASE("an unknown node key in a save is skipped, not fatal", "[meta]") {
    MetaProgression m;
    std::string err;
    REQUIRE(m.from_json(
        R"({"version": 3, "memory_cells": 9, "tree": {"no.such.node": 3, "hub.homeostasis": 2}})",
        err));
    REQUIRE(m.level(TreeNode::Homeostasis) == 2);
    REQUIRE(m.memory_cells() == 9);
}

TEST_CASE("a saved level above a node's max is clamped", "[meta]") {
    MetaProgression m;
    std::string err;
    REQUIRE(m.from_json(R"({"version": 3, "tree": {"neutrophil.accuracy": 200}})", err));
    REQUIRE(m.level(TreeNode::NeutrophilAccuracy) == tree_node(TreeNode::NeutrophilAccuracy).max_level);
}
