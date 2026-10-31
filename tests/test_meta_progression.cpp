#include "TreeTestHelpers.h"
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
    for (u32 i = 0; i < kTreeNodeCount; ++i) REQUIRE(m.level(static_cast<TreeNode>(i)) == 0);
}

// ---- Run payouts ------------------------------------------------------------

TEST_CASE("every run pays Memory Cells, even a wipe on the first wave", "[meta]") {
    const MetaConfig cfg;
    REQUIRE(default_game_config().meta.base_run_reward == cfg.base_run_reward);
    const RunReward wipe = MetaProgression::reward_for_run(RunResult{}, cfg, false);
    REQUIRE(wipe.memory_cells > 0);
    REQUIRE(wipe.memory_cells >= tree_node_cost(cfg, kTreeRoot, 0).memory_cells);
    REQUIRE(wipe.antibodies == 0);
    auto first_loss = fresh();
    first_loss.record_run("first_encounter", RunResult{}, cfg);
    REQUIRE(first_loss.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
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

TEST_CASE("losses bank permanent growth that can be spent before the first clear", "[meta][progression]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    const u32 starter_cost = m.next_cost(kTreeRoot, cfg).memory_cells;
    const u32 floor = MetaProgression::reward_for_run(RunResult{}, cfg, false).memory_cells;
    REQUIRE(floor > 0);
    const u32 attempts = (starter_cost + floor - 1) / floor;
    for (u32 i = 0; i < attempts; ++i) {
        const auto reward = m.record_run("opening", RunResult{}, cfg);
        CHECK(reward.memory_cells == floor);
        CHECK(reward.antibodies == 0);
        CHECK_FALSE(reward.first_clear);
    }
    CHECK_FALSE(m.level_cleared("opening"));
    CHECK(m.antibodies() == 0);
    REQUIRE(m.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    const auto trigger_cost = m.next_cost(TreeNode::NeutrophilSquadSize, cfg);
    CHECK(trigger_cost.antibodies == 0);
    m.credit(trigger_cost.memory_cells);
    REQUIRE(m.purchase(TreeNode::NeutrophilSquadSize, cfg) == MetaProgression::PurchaseResult::Ok);
    CHECK(m.antibodies() == 0);
}

TEST_CASE("a first clear affords an explicit paid starter and a useful discovery choice", "[meta][progression]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    RunResult won;
    won.won = true;
    const auto reward = m.record_run("first", won, cfg);
    REQUIRE(reward.antibodies == 1);
    const auto starter_cost = m.next_cost(kTreeRoot, cfg);
    REQUIRE(reward.memory_cells >= starter_cost.memory_cells);
    const std::string before = m.to_json();
    const auto choices = m.discovery_offers(cfg);
    REQUIRE(choices.size() == 5); // Four cell choices and the innate cell's capstone.
    CHECK(m.to_json() == before);
    for (const auto& choice : choices) {
        CHECK(choice.cost.memory_cells == 0);
        CHECK(choice.cost.antibodies == 1);
        CHECK(choice.prerequisite == kTreeRoot);
        CHECK(choice.prerequisite_cost.memory_cells == starter_cost.memory_cells);
        MetaProgression selected = m;
        REQUIRE(selected.purchase(choice.prerequisite, cfg) == MetaProgression::PurchaseResult::Ok);
        REQUIRE(selected.purchase(choice.node, cfg) == MetaProgression::PurchaseResult::Ok);
        CHECK(selected.level(choice.node) == 1);
        CHECK(selected.antibodies() == 0);
    }
}

TEST_CASE("every reachable discovery choice history leaves an affordable tool through ten first clears",
          "[meta][progression][discovery]") {
    const MetaConfig cfg;
    std::vector<TreeNode> tools;
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto node = static_cast<TreeNode>(i);
        const auto kind = tree_node(node).kind;
        if (kind == TreeNodeKind::TowerRoot || kind == TreeNodeKind::AbilityRoot || kind == TreeNodeKind::Capstone)
            tools.push_back(node);
    }
    REQUIRE(tools.size() == 12);
    u32 examined = 0;
    for (u32 mask = 0; mask < (1u << tools.size()); ++mask) {
        u32 owned = 0;
        for (u32 bits = mask; bits; bits >>= 1) owned += bits & 1;
        if (owned >= 10) continue;
        MetaProgression m = fresh();
        m.credit(m.next_cost(kTreeRoot, cfg).memory_cells, owned);
        REQUIRE(m.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
        u32 remaining = mask;
        for (usize pass = 0; pass < tools.size() && remaining; ++pass) {
            bool bought = false;
            for (usize i = 0; i < tools.size(); ++i) if (remaining & (1u << i)) {
                if (m.purchase(tools[i], cfg) != MetaProgression::PurchaseResult::Ok) continue;
                remaining &= ~(1u << i);
                bought = true;
            }
            if (!bought) break;
        }
        // An ability/capstone without its related cell is not reachable by
        // legal choices. All legal subsets are checked, independent of order.
        if (remaining) continue;
        ++examined;
        REQUIRE(m.memory_cells() == 0);
        REQUIRE(m.antibodies() == 0);
        RunResult won;
        won.won = true;
        const auto reward = m.record_run("next_first_clear", won, cfg);
        REQUIRE(reward.antibodies == 1);
        const auto choices = m.discovery_offers(cfg);
        REQUIRE_FALSE(choices.empty());
        for (const auto& choice : choices) {
            CHECK(choice.prerequisite == TreeNode::Count);
            CHECK(choice.cost.memory_cells == 0);
            CHECK(choice.cost.antibodies == 1);
            CHECK(m.level(choice.node) == 0);
            MetaProgression selected = m;
            REQUIRE(selected.purchase(choice.node, cfg) == MetaProgression::PurchaseResult::Ok);
        }
    }
    CHECK(examined > 500);
}

TEST_CASE("discovery purchases and first-clear identity survive save replay without duplicate Antibodies",
          "[meta][progression][save]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    RunResult won;
    won.won = true;
    m.record_run("first", won, cfg);
    REQUIRE(m.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::MacrophageRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    MetaProgression restored;
    std::string error;
    REQUIRE(restored.from_json(m.to_json(), error));
    CHECK(restored.to_json() == m.to_json());
    CHECK(restored.tower_unlocked(TowerType::Macrophage));
    CHECK(restored.spent_antibodies() == 1);
    const auto repeat = restored.record_run("first", won, cfg);
    CHECK(repeat.antibodies == 0);
    CHECK_FALSE(repeat.first_clear);
    CHECK(restored.discovery_offers(cfg).empty());
    const auto next = restored.record_run("second", won, cfg);
    CHECK(next.antibodies == 1);
    REQUIRE(restored.purchase(TreeNode::FeverUnlock, cfg) == MetaProgression::PurchaseResult::Ok);
    CHECK(restored.ability_unlocked(AbilityId::FeverResponse));
}

TEST_CASE("older paid capstone ledgers retain their full refund after discovery prices change",
          "[meta][progression][save]") {
    MetaConfig cfg;
    cfg.respec_cost = 25;
    MetaProgression m = fresh();
    m.credit(500, 1);
    REQUIRE(m.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::NeutrophilCapstone, cfg) == MetaProgression::PurchaseResult::Ok);
    auto historical = config::Json::parse(m.to_json());
    // v3 stored the exact paid ledger. Earlier capstones cost another150 MC.
    historical["spent_memory_cells"] = m.spent_memory_cells() + 150;
    historical["memory_cells"] = m.memory_cells() - 150;
    MetaProgression restored;
    std::string error;
    REQUIRE(restored.from_json(historical.dump(), error));
    CHECK(restored.level(TreeNode::NeutrophilCapstone) == 1);
    REQUIRE(restored.respec(cfg));
    CHECK(restored.memory_cells() == 500 - cfg.respec_cost);
    CHECK(restored.antibodies() == 1);
}

// ---- Purchases ----------------------------------------------------------------

TEST_CASE("a tower root costs an Antibody once the node it grows from is owned", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(0, 1);
    // Cell unlocks grow directly from Round Damage, outside the economy branch.
    REQUIRE(m.check_purchase(TreeNode::GobletRoot, cfg) == MetaProgression::PurchaseResult::Locked);
    buy_levels(m, TreeNode::NeutrophilRoundDamage, 1, cfg);
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
    // Round Damage is the only starting purchase, even with ample currencies.
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const TreeNode n = static_cast<TreeNode>(i);
        CAPTURE(tree_node(n).key);
        REQUIRE(m.check_purchase(n, cfg) == (n == TreeNode::NeutrophilRoundDamage
                    ? MetaProgression::PurchaseResult::Ok : MetaProgression::PurchaseResult::Locked));
    }
    // One level opens each independent branch; no maximum-level gate is needed.
    buy_levels(m, TreeNode::NeutrophilRoundDamage, 1, cfg);
    for (TreeNode n : {TreeNode::BoneMarrowReserve, TreeNode::NeutrophilSquadSize,
                       TreeNode::NeutrophilCapstone,
                       TreeNode::CytotoxicRoot, TreeNode::MacrophageRoot,
                       TreeNode::GobletRoot, TreeNode::FibroblastRoot}) {
        REQUIRE(m.check_purchase(n, cfg) == MetaProgression::PurchaseResult::Ok);
    }
    for (TreeNode n : {TreeNode::ComplementUnlock, TreeNode::FeverUnlock, TreeNode::ClotUnlock}) {
        REQUIRE(m.check_purchase(n, cfg) == MetaProgression::PurchaseResult::Locked);
    }
    REQUIRE(m.check_purchase(TreeNode::EfficientClearance, cfg) == MetaProgression::PurchaseResult::Locked);
    REQUIRE(m.check_purchase(TreeNode::FieldRequisition, cfg) == MetaProgression::PurchaseResult::Locked);
    // The economy entry opens only its own economic lines.
    buy_levels(m, TreeNode::BoneMarrowReserve, 1, cfg);
    REQUIRE(m.check_purchase(TreeNode::EfficientClearance, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::FieldRequisition, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::SystemicPotency, cfg) == MetaProgression::PurchaseResult::Locked);
    buy_levels(m, TreeNode::EfficientClearance, 1, cfg);
    REQUIRE(m.check_purchase(TreeNode::SystemicPotency, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::Homeostasis, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::FeverUnlock, cfg) == MetaProgression::PurchaseResult::Locked);
}

TEST_CASE("each active ability opens directly from its related cell discovery", "[meta]") {
    struct Path { TreeNode cell, ability; };
    const Path paths[] = {
        {TreeNode::CytotoxicRoot, TreeNode::ComplementUnlock},
        {TreeNode::MacrophageRoot, TreeNode::FeverUnlock},
        {TreeNode::FibroblastRoot, TreeNode::ClotUnlock},
    };
    const MetaConfig cfg;
    for (const Path& p : paths) {
        CAPTURE(tree_node(p.ability).key);
        MetaProgression m = fresh();
        m.credit(100000, 2);
        buy_levels(m, kTreeRoot, 1, cfg);
        REQUIRE(m.check_purchase(p.ability, cfg) == MetaProgression::PurchaseResult::Locked);
        REQUIRE(m.purchase(p.cell, cfg) == MetaProgression::PurchaseResult::Ok);
        REQUIRE(m.check_purchase(p.ability, cfg) == MetaProgression::PurchaseResult::Ok);
        const u64 memory = m.memory_cells();
        const u32 before = m.antibodies();
        REQUIRE(m.purchase(p.ability, cfg) == MetaProgression::PurchaseResult::Ok);
        REQUIRE(m.antibodies() == before - 1);
        REQUIRE(m.memory_cells() == memory);
    }
}

TEST_CASE("a stat line needs its tower's root and costs more each level", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(10000);
    REQUIRE(m.check_purchase(TreeNode::GobletSlowStrength, cfg) ==
            MetaProgression::PurchaseResult::Locked);
    // The Neutrophil is owned from the start, so Round Damage is available.
    REQUIRE(m.check_purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Ok);
    const u32 c0 = m.next_cost(TreeNode::NeutrophilRoundDamage, cfg).memory_cells;
    REQUIRE(m.purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Ok);
    const u32 c1 = m.next_cost(TreeNode::NeutrophilRoundDamage, cfg).memory_cells;
    REQUIRE(c1 > c0);
    REQUIRE(m.memory_cells() == 10000 - c0);
    REQUIRE(m.level(TreeNode::NeutrophilRoundDamage) == 1);
}

TEST_CASE("a refused purchase changes nothing", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(5);
    const std::string before = m.to_json();
    REQUIRE(m.purchase(TreeNode::NeutrophilRoundDamage, cfg) ==
            MetaProgression::PurchaseResult::NeedMemoryCells);
    REQUIRE(m.to_json() == before);
}

TEST_CASE("a stat line stops at its max level", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    buy_levels(m, TreeNode::NeutrophilRoundDamage, 1, cfg);
    buy_levels(m, TreeNode::NeutrophilSquadSize, 1, cfg);
    const u8 max = tree_node(TreeNode::NeutrophilAccuracy).max_level;
    buy_levels(m, TreeNode::NeutrophilAccuracy, max, cfg);
    m.credit(100000);
    REQUIRE(m.purchase(TreeNode::NeutrophilAccuracy, cfg) == MetaProgression::PurchaseResult::Maxed);
    REQUIRE(m.next_cost(TreeNode::NeutrophilAccuracy, cfg).memory_cells == 0);
}

TEST_CASE("a capstone is a one Antibody discovery without a stat or Memory Cell gate", "[meta]") {
    MetaConfig cfg;
    // Legacy config fields cannot reintroduce the retired capstone grind.
    cfg.capstone_threshold = 100;
    cfg.capstone_memory_cells = 100000;
    MetaProgression m = fresh();
    m.credit(m.next_cost(kTreeRoot, cfg).memory_cells, 1);
    REQUIRE(m.check_purchase(TreeNode::NeutrophilCapstone, cfg) == MetaProgression::PurchaseResult::Locked);
    REQUIRE(m.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.memory_cells() == 0);
    REQUIRE(m.check_purchase(TreeNode::NeutrophilCapstone, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::NeutrophilCapstone, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.memory_cells() == 0);
    REQUIRE(m.antibodies() == 0);
}

TEST_CASE("an ability line needs the ability's root", "[meta]") {
    const MetaConfig cfg;
    MetaProgression m = fresh();
    m.credit(10000, 2);
    REQUIRE(m.check_purchase(TreeNode::ClotDuration, cfg) == MetaProgression::PurchaseResult::Locked);
    // The clot grows from the Fibroblast; its own lines still need the ability.
    buy_levels(m, TreeNode::NeutrophilRoundDamage, 1, cfg);
    REQUIRE(m.purchase(TreeNode::FibroblastRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.check_purchase(TreeNode::ClotDuration, cfg) == MetaProgression::PurchaseResult::Locked);
    REQUIRE(m.purchase(TreeNode::ClotUnlock, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.ability_unlocked(AbilityId::FibrinClot));
    REQUIRE(m.purchase(TreeNode::ClotDuration, cfg) == MetaProgression::PurchaseResult::Ok);
}

TEST_CASE("respec refunds everything spent minus the fee and keeps the Neutrophil", "[meta]") {
    MetaConfig cfg;
    cfg.respec_cost = 25;
    MetaProgression m = fresh();
    REQUIRE_FALSE(m.can_respec(cfg)); // nothing bought yet
    m.credit(1000, 2);
    REQUIRE(m.purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::BoneMarrowReserve, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::CytotoxicRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::CytotoxicDrain, cfg) == MetaProgression::PurchaseResult::Ok);

    REQUIRE(m.respec(cfg));
    REQUIRE(m.memory_cells() == 1000 - 25);
    REQUIRE(m.antibodies() == 2);
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::CytotoxicT));
    REQUIRE(m.level(TreeNode::BoneMarrowReserve) == 0);
    REQUIRE(m.level(TreeNode::NeutrophilRoundDamage) == 0);
    REQUIRE(m.check_purchase(TreeNode::BoneMarrowReserve, cfg) == MetaProgression::PurchaseResult::Locked);
    REQUIRE(m.check_purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE_FALSE(m.can_respec(cfg));
}

// ---- Save / load --------------------------------------------------------------

TEST_CASE("save/load round-trips currencies, the tree and progress", "[meta]") {
    MetaConfig cfg;
    cfg.capstone_threshold = 1;
    MetaProgression m = fresh();
    m.credit(5000, 4);
    for (TreeNode n : {TreeNode::NeutrophilRoundDamage, TreeNode::BoneMarrowReserve,
                       TreeNode::FieldRequisition,
                       TreeNode::GobletRoot, TreeNode::GobletSlowStrength,
                       TreeNode::GobletSlowDuration, TreeNode::GobletSlowDuration, TreeNode::GobletSplashRadius,
                       TreeNode::GobletWeakness, TreeNode::GobletCapstone, TreeNode::EfficientClearance,
                       TreeNode::Homeostasis, TreeNode::MacrophageRoot, TreeNode::MacrophageHealth,
                       TreeNode::FeverUnlock}) {
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
    REQUIRE(m.level(kTreeRoot) == 0);
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
    REQUIRE(m.level(kTreeRoot) == 0);
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
        R"({"version": 3, "memory_cells": 9, "tree": {"no.such.node": 3, "hub.rapid_metabolism": 5, "neutrophil.unlock": 1, "hub.homeostasis": 2}})",
        err));
    REQUIRE(m.level(TreeNode::Homeostasis) == 2);
    REQUIRE(m.memory_cells() == 9);
    REQUIRE(m.to_json().find("hub.rapid_metabolism") == std::string::npos);
    REQUIRE(m.to_json().find("neutrophil.unlock") == std::string::npos);
    REQUIRE(m.level(kTreeRoot) == 0);
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil));
}

TEST_CASE("a saved level above a node's max is clamped", "[meta]") {
    MetaProgression m;
    std::string err;
    REQUIRE(m.from_json(R"({"version": 3, "tree": {"neutrophil.accuracy": 200}})", err));
    REQUIRE(m.level(TreeNode::NeutrophilAccuracy) == tree_node(TreeNode::NeutrophilAccuracy).max_level);
}

TEST_CASE("an earlier ability purchase survives changed parent requirements", "[meta]") {
    MetaProgression m;
    std::string err;
    REQUIRE(m.from_json(R"({"version": 3, "memory_cells": 1000, "tree": {"ability.clot.unlock": 1}})", err));
    REQUIRE(m.ability_unlocked(AbilityId::FibrinClot));
    REQUIRE_FALSE(m.tower_unlocked(TowerType::Fibroblast));
    REQUIRE(m.level(kTreeRoot) == 0);
    REQUIRE(m.purchase(TreeNode::ClotCooldown, MetaConfig{}) == MetaProgression::PurchaseResult::Ok);
}

TEST_CASE("a save that bought the removed Histamine Flare gets its spend back", "[meta]") {
    MetaProgression m;
    std::string err;
    REQUIRE(m.from_json(R"({"version": 3, "memory_cells": 10, "antibodies": 0,
        "spent_memory_cells": 100, "spent_antibodies": 2,
        "tree": {"ability.histamine.unlock": 1, "ability.histamine.cooldown": 2,
                 "ability.histamine.radius": 1, "ability.fever.unlock": 1}})", err));
    // Cooldown ranks 1-2 cost 24 + 42, Radius rank 1 costs 24 (120 weight).
    CHECK(m.memory_cells() == 10 + 24 + 42 + 24);
    CHECK(m.antibodies() == 1);
    CHECK(m.spent_memory_cells() == 100 - 90);
    CHECK(m.spent_antibodies() == 1);
    CHECK(m.ability_unlocked(AbilityId::FeverResponse));
    CHECK(m.to_json().find("histamine") == std::string::npos);
}

TEST_CASE("merged firing cycle preserves old ranks and actual paid refunds", "[meta][progression]") {
    MetaProgression m;
    std::string error;
    REQUIRE(m.from_json(R"({"version":3,"memory_cells":47,"antibodies":0,
        "spent_memory_cells":55,"spent_antibodies":0,
        "tree":{"neutrophil.round_damage":1,"neutrophil.trigger_rate":1}})", error));
    CHECK(m.level(kTreeRoot) == 2);
    CHECK(m.memory_cells() == 47);
    CHECK(m.spent_memory_cells() == 55);
    CHECK(m.to_json().find("neutrophil.trigger_rate") == std::string::npos);
    MetaProgression loaded;
    REQUIRE(loaded.from_json(m.to_json(), error));
    CHECK(loaded.to_json() == m.to_json());
    REQUIRE(loaded.respec(MetaConfig{}));
    CHECK(loaded.memory_cells() == 77);

    auto priced = fresh();
    const u32 prices[] = {30, 53, 75, 98, 120};
    for (u32 price : prices) {
        REQUIRE(priced.next_cost(kTreeRoot, MetaConfig{}).memory_cells == price);
        priced.credit(price);
        REQUIRE(priced.purchase(kTreeRoot, MetaConfig{}) == MetaProgression::PurchaseResult::Ok);
    }
    CHECK(priced.level(kTreeRoot) == 5);
    CHECK(priced.check_purchase(kTreeRoot, MetaConfig{}) == MetaProgression::PurchaseResult::Maxed);
    // Degranulation Cycle II grows from Granule Capacity.
    buy_levels(priced, TreeNode::NeutrophilSquadSize, 1, MetaConfig{});
    u32 previous_price = prices[4];
    for (unsigned stage = 2; stage <= 4; ++stage) {
        const auto node = immune::test::tree_stage(kTreeRoot, stage);
        REQUIRE(node != TreeNode::Count);
        for (u8 rank = 0; rank < tree_node(node).max_level; ++rank) {
            const auto cost = priced.next_cost(node, MetaConfig{});
            REQUIRE(cost.memory_cells > previous_price);
            previous_price = cost.memory_cells;
            priced.credit(cost.memory_cells);
            REQUIRE(priced.purchase(node, MetaConfig{}) == MetaProgression::PurchaseResult::Ok);
        }
    }
}

TEST_CASE("Adaptive Memory is earned through its connected branch and retains the exact paid ledger", "[meta][progression][adaptive-memory]") {
    auto m = fresh();
    const MetaConfig cfg;
    REQUIRE(m.check_purchase(TreeNode::AdaptiveMemory, cfg) == MetaProgression::PurchaseResult::Locked);
    for (const auto parent : {kTreeRoot, TreeNode::BoneMarrowReserve, TreeNode::EfficientClearance, TreeNode::SystemicPotency}) {
        m.credit(m.next_cost(parent, cfg).memory_cells);
        REQUIRE(m.purchase(parent, cfg) == MetaProgression::PurchaseResult::Ok);
    }
    const auto before = m.spent_memory_cells();
    u64 paid = 0;
    unsigned total_rank = 0;
    for (unsigned stage = 1; stage <= 4; ++stage) {
        const auto node = immune::test::tree_stage(TreeNode::AdaptiveMemory, stage);
        REQUIRE(node != TreeNode::Count);
        for (u8 rank = 1; rank <= tree_node(node).max_level; ++rank) {
            const auto cost = m.next_cost(node, cfg);
            REQUIRE(cost.antibodies == 0);
            REQUIRE(m.check_purchase(node, cfg) == MetaProgression::PurchaseResult::NeedMemoryCells);
            m.credit(cost.memory_cells);
            paid += cost.memory_cells;
            REQUIRE(m.purchase(node, cfg) == MetaProgression::PurchaseResult::Ok);
            CHECK(m.memory_cells() == 0);
            if (++total_rank == 10) CHECK(paid == 1750);
        }
    }
    CHECK(total_rank == 20);
    CHECK(paid == 6500);
    CHECK(m.spent_memory_cells() == before + paid);
    MetaProgression restored;
    std::string error;
    REQUIRE(restored.from_json(m.to_json(), error));
    CHECK(restored.to_json() == m.to_json());
    for (unsigned stage = 1; stage <= 4; ++stage) {
        const auto node = immune::test::tree_stage(TreeNode::AdaptiveMemory, stage);
        CHECK(restored.level(node) == tree_node(node).max_level);
    }
}

TEST_CASE("Degranulation Cycle II needs both Granule Capacity and the fully grown first stage", "[meta][progression]") {
    auto m = fresh();
    const MetaConfig cfg{};
    const auto next = TreeNode::NeutrophilRoundDamageStage2;
    const auto branch = TreeNode::NeutrophilSquadSize;
    REQUIRE(tree_node(next).parent == branch);
    REQUIRE(tree_previous_stage(next) == kTreeRoot);
    m.credit(10000);
    CHECK(m.check_purchase(next, cfg) == MetaProgression::PurchaseResult::Locked);
    REQUIRE(m.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(branch, cfg) == MetaProgression::PurchaseResult::Ok);
    CHECK(m.check_purchase(next, cfg) == MetaProgression::PurchaseResult::Locked);
    for (u8 rank = 2; rank <= tree_node(kTreeRoot).max_level; ++rank) {
        REQUIRE(m.purchase(kTreeRoot, cfg) == MetaProgression::PurchaseResult::Ok);
        if (rank < tree_node(kTreeRoot).max_level)
            CHECK(m.check_purchase(next, cfg) == MetaProgression::PurchaseResult::Locked);
    }
    // Any rank of the branch parent is enough; it does not need to be maxed.
    CHECK(m.level(branch) == 1);
    CHECK(m.check_purchase(next, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(next, cfg) == MetaProgression::PurchaseResult::Ok);
    CHECK(m.check_purchase(TreeNode::NeutrophilRoundDamageStage3, cfg) == MetaProgression::PurchaseResult::Locked);

    auto no_branch = fresh();
    no_branch.credit(10000);
    buy_levels(no_branch, kTreeRoot, tree_node(kTreeRoot).max_level, cfg);
    CHECK(no_branch.check_purchase(next, cfg) == MetaProgression::PurchaseResult::Locked);
}

TEST_CASE("legacy long tracks migrate every purchased rank across stages", "[meta][progression]") {
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto first = static_cast<TreeNode>(i);
        // v3 predates all separately purchasable effect tracks.
        if (i >= static_cast<u32>(TreeNode::NeutrophilRelease)) continue;
        if (immune::test::tree_stage(first, 2) == TreeNode::Count) continue;
        const auto maximum = immune::test::tree_track_max(first);
        for (const auto original : {5u, 6u, maximum}) {
            CAPTURE(tree_node(first).key, original);
            MetaProgression m;
            std::string error;
            const auto saved = std::string("{\"version\":3,\"memory_cells\":77,\"spent_memory_cells\":999,\"tree\":{\"") +
                               tree_node(first).key + "\":" + std::to_string(original) + "}}";
            REQUIRE(m.from_json(saved, error));
            unsigned remaining = original;
            for (unsigned stage = 1; ; ++stage) {
                const auto node = immune::test::tree_stage(first, stage);
                if (node == TreeNode::Count) break;
                const auto expected = std::min(remaining, static_cast<unsigned>(tree_node(node).max_level));
                CHECK(m.level(node) == expected);
                remaining -= expected;
            }
            CHECK(m.memory_cells() == 77);
            CHECK(m.spent_memory_cells() == 999);
            MetaProgression round_trip;
            REQUIRE(round_trip.from_json(m.to_json(), error));
            CHECK(round_trip.to_json() == m.to_json());
        }
    }
}

TEST_CASE("formerly capped veteran saves retain purchased ranks and can buy the next rank", "[meta][progression][veteran][adaptive-memory]") {
    MetaProgression restored;
    std::string error;
    REQUIRE(restored.from_json(R"({"version":3,"memory_cells":91,"antibodies":2,
      "spent_memory_cells":5478,"spent_antibodies":2,"tree":{
      "neutrophil.round_damage":12,"cytotoxic.unlock":1,"cytotoxic.drain":12,
      "macrophage.unlock":1,"macrophage.grab_speed":8,"hub.bone_marrow_reserve":1,
      "hub.efficient_clearance":1,"hub.systemic_potency":1,"hub.adaptive_memory":10}})", error));
    CHECK(restored.memory_cells() == 91);
    CHECK(restored.antibodies() == 2);
    CHECK(restored.spent_memory_cells() == 5478);
    CHECK(restored.spent_antibodies() == 2);
    for (const auto node : {TreeNode::NeutrophilRoundDamage, TreeNode::CytotoxicDrain,
                            TreeNode::MacrophageGrabSpeed, TreeNode::AdaptiveMemory}) {
        CAPTURE(tree_node(node).key);
        const u8 old_cap = node == TreeNode::MacrophageGrabSpeed ? 8 :
            node == TreeNode::AdaptiveMemory ? 10 : 12;
        unsigned total = 0;
        for (unsigned stage = 1; ; ++stage) {
            const auto part = immune::test::tree_stage(node, stage);
            if (part == TreeNode::Count) break;
            total += restored.level(part);
        }
        CHECK(total == old_cap);
        const auto next = immune::test::tree_stage(node, old_cap / 5 + 1);
        REQUIRE(next != TreeNode::Count);
        const auto cost = restored.next_cost(next, MetaConfig{});
        REQUIRE(cost.memory_cells > 0);
        REQUIRE(cost.antibodies == 0);
        const auto bank = restored.memory_cells();
        const auto spent = restored.spent_memory_cells();
        restored.credit(cost.memory_cells);
        REQUIRE(restored.purchase(next, MetaConfig{}) == MetaProgression::PurchaseResult::Ok);
        CHECK(restored.level(next) == old_cap % 5 + 1);
        CHECK(restored.memory_cells() == bank);
        CHECK(restored.spent_memory_cells() == spent + cost.memory_cells);
    }
    MetaProgression round_trip;
    REQUIRE(round_trip.from_json(restored.to_json(), error));
    CHECK(round_trip.to_json() == restored.to_json());
}

TEST_CASE("Progression cheats preserve save and reset invariants", "[meta]") {
    auto m = fresh();
    const auto cfg = default_game_config().meta;
    m.credit(100000, 100);
    REQUIRE(m.purchase(TreeNode::NeutrophilRoundDamage, cfg) == MetaProgression::PurchaseResult::Ok);
    REQUIRE(m.purchase(TreeNode::BoneMarrowReserve, cfg) == MetaProgression::PurchaseResult::Ok);
    m.debug_set_all_skills(true);
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto n = static_cast<TreeNode>(i);
        REQUIRE(m.level(n) == tree_node(n).max_level);
    }
    REQUIRE(m.spent_memory_cells() == 0);
    REQUIRE(m.spent_antibodies() == 0);
    m.debug_set_skill(kTreeRoot, 0);
    REQUIRE(m.level(kTreeRoot) == 0);
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil));
    m.debug_set_skill(TreeNode::BoneMarrowReserve, 999);
    REQUIRE(m.level(TreeNode::BoneMarrowReserve) == tree_node(TreeNode::BoneMarrowReserve).max_level);
    m.debug_set_level_cleared("campaign_01", true);
    m.debug_set_level_cleared("campaign_01", true);
    REQUIRE(m.progress().levels_completed == 1);
    m.debug_set_level_cleared("campaign_01", false);
    REQUIRE_FALSE(m.level_cleared("campaign_01"));
    REQUIRE(m.progress().levels_completed == 0);
    m.debug_set_currencies(1234, 56);
    MetaProgression loaded;
    std::string error;
    REQUIRE(loaded.from_json(m.to_json(), error));
    REQUIRE(loaded.to_json() == m.to_json());
    m.debug_set_level_cleared("campaign_02", true);
    m.debug_reset_campaign();
    REQUIRE(m.progress().completed_level_ids.empty());
    REQUIRE(m.memory_cells() == 1234);
    m.debug_set_all_skills(false);
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto n = static_cast<TreeNode>(i);
        REQUIRE(m.level(n) == 0);
    }
    REQUIRE(m.tower_unlocked(TowerType::Neutrophil));
    m.reset_to_new_game();
    REQUIRE(m.to_json() == fresh().to_json());
}


TEST_CASE("split ranks migrate with their effects, prices, and purchase requirements", "[meta][progression][stages]") {
    const MetaConfig cfg;
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto stage = static_cast<TreeNode>(i);
        const auto& d = tree_node(stage);
        if (d.effect_source == TreeNode::Count) continue;
        // v3 could not have contained any of the new split tracks.
        if (static_cast<u32>(d.effect_source) >= static_cast<u32>(TreeNode::NeutrophilRelease)) continue;
        CAPTURE(d.key);
        REQUIRE(d.max_level <= 5);
        const auto& source = tree_node(d.effect_source);
        REQUIRE(d.parent != TreeNode::Count);
        CHECK(tree_node_cost(cfg, stage, 0).memory_cells ==
              tree_node_cost(cfg, d.effect_source, d.rank_offset).memory_cells);

        MetaProgression m;
        std::string error;
        const std::string save = std::string(R"({"version":3,"memory_cells":100000,"spent_memory_cells":1234,"tree":{")") +
            source.key + "\":" + std::to_string(d.rank_offset + 1) + "}}";
        REQUIRE(m.from_json(save, error));
        CHECK(m.level(stage) == 1);
        CHECK(m.spent_memory_cells() == 1234);
        MetaProgression roundtrip;
        REQUIRE(roundtrip.from_json(m.to_json(), error));
        CHECK(roundtrip.to_json() == m.to_json());

        MetaProgression locked;
        locked.debug_set_currencies(100000, 100);
        const TreeNode previous = tree_previous_stage(stage);
        REQUIRE(previous != TreeNode::Count);
        locked.debug_set_skill(d.parent, 1);
        locked.debug_set_skill(previous, 1);
        if (tree_node(previous).max_level > 1)
            CHECK(locked.check_purchase(stage, cfg) == MetaProgression::PurchaseResult::Locked);
        locked.debug_set_skill(previous, tree_node(previous).max_level);
        CHECK(locked.check_purchase(stage, cfg) == MetaProgression::PurchaseResult::Ok);
    }
}


TEST_CASE("legacy combined purchases migrate each split effect once", "[meta][progression][migration]") {
    struct Split { TreeNode old_line, new_line; unsigned offset; };
    using N = TreeNode;
    const Split splits[] = {
        {N::NeutrophilRoundDamage, N::NeutrophilRelease, 0},
        {N::NeutrophilRoundDamage, N::NeutrophilGranuleDamage, 12},
        {N::NeutrophilVitality, N::NeutrophilSpeed, 0},
        {N::CytotoxicStamina, N::CytotoxicStaminaHealth, 0},
        {N::MacrophageArms, N::MacrophageIntakeRadius, 0},
        {N::MacrophageCaptives, N::MacrophageCaptivesRadius, 0},
        {N::MacrophageGrabSpeed, N::MacrophageGrabReach, 4},
        {N::MacrophageGrabSpeed, N::MacrophageMobility, 4},
        {N::MacrophageWall, N::MacrophageWallHealth, 0},
        {N::FibroblastInflammation, N::FibroblastRegrowth, 0},
        {N::AdaptiveMemory, N::AdaptiveWork, 0},
        {N::SystemicPotency, N::CytokineDrain, 0},
        {N::SystemicPotency, N::CytokineGobletSpeed, 0},
        {N::AdaptiveMemory, N::AdaptiveDrain, 0},
        {N::AdaptiveMemory, N::AdaptiveGobletSpeed, 0},
    };
    for (const auto& split : splits) {
        const auto total = std::min(immune::test::tree_track_max(split.old_line), 15u);
        for (const int version : {3, 4}) {
            CAPTURE(tree_node(split.old_line).key, tree_node(split.new_line).key, version);
            std::string fields;
            if (version == 3) {
                fields = std::string("\"") + tree_node(split.old_line).key + "\":" + std::to_string(total);
            } else {
                unsigned remaining = total;
                for (unsigned stage = 1; remaining; ++stage) {
                    const auto node = immune::test::tree_stage(split.old_line, stage);
                    REQUIRE(node != TreeNode::Count);
                    const auto rank = std::min(remaining, static_cast<unsigned>(tree_node(node).max_level));
                    if (!fields.empty()) fields += ",";
                    fields += std::string("\"") + tree_node(node).key + "\":" + std::to_string(rank);
                    remaining -= rank;
                }
            }
            const auto save = std::string("{\"version\":") + std::to_string(version) +
                ",\"memory_cells\":57,\"spent_memory_cells\":999,\"tree\":{" + fields + "}}";
            MetaProgression restored;
            std::string error;
            REQUIRE(restored.from_json(save, error));
            unsigned remaining = total > split.offset ? total - split.offset : 0;
            for (unsigned stage = 1; ; ++stage) {
                const auto node = immune::test::tree_stage(split.new_line, stage);
                if (node == TreeNode::Count) break;
                const auto expected = std::min(remaining, static_cast<unsigned>(tree_node(node).max_level));
                CHECK(restored.level(node) == expected);
                remaining -= expected;
            }
            CHECK(restored.memory_cells() == 57);
            CHECK(restored.spent_memory_cells() == 999);
            MetaProgression roundtrip;
            REQUIRE(roundtrip.from_json(restored.to_json(), error));
            CHECK(roundtrip.to_json() == restored.to_json());
        }
    }
    // A current save can intentionally leave a new split track unbought.
    MetaProgression current;
    std::string error;
    REQUIRE(current.from_json(R"({"version":5,"tree":{"neutrophil.round_damage":5}})", error));
    CHECK(current.level(N::NeutrophilRelease) == 0);
}

TEST_CASE("late split effects migrate only ranks that previously had them", "[meta][progression][migration]") {
    for (const auto rank : {4u, 5u, 12u, 13u, 20u}) {
        CAPTURE(rank);
        const auto save = std::string("{\"version\":3,\"tree\":{\"neutrophil.round_damage\":") + std::to_string(rank) +
            ",\"macrophage.grab_speed\":" + std::to_string(rank) + "}}";
        MetaProgression restored;
        std::string error;
        REQUIRE(restored.from_json(save, error));
        auto total = [&](TreeNode first) {
            unsigned sum = 0;
            for (unsigned stage = 1; ; ++stage) {
                const auto n = immune::test::tree_stage(first, stage);
                if (n == TreeNode::Count) return sum;
                sum += restored.level(n);
            }
        };
        CHECK(total(TreeNode::NeutrophilGranuleDamage) == (rank > 12 ? rank - 12 : 0));
        const auto old_grab = std::min(rank, 16u);
        CHECK(total(TreeNode::MacrophageGrabReach) == (old_grab > 4 ? old_grab - 4 : 0));
        CHECK(total(TreeNode::MacrophageMobility) == (old_grab > 4 ? old_grab - 4 : 0));
    }
}
