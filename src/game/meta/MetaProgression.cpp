// game/meta/MetaProgression.cpp — currencies, tree purchases, campaign
// progress, and versioned JSON save/load. See MetaProgression.h for the
// contract and ImmunityTree.h for what the tree's nodes mean.
#include "game/meta/MetaProgression.h"

#include "game/config/GameConfig.h"
#include "platform/FileIO.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace immune::game {

namespace {
using json = nlohmann::json;

/// A saved tower index, read in the numbering of save `version`, as today's
/// index. kTowerTypeCount means "no such tower any more".
u32 migrate_tower_index(u32 idx, i32 version) {
    if (version < 2) {
        // v1 had six slots; slot 2 was cut and 3..5 moved down one.
        if (idx == 2) return kTowerTypeCount;
        if (idx > 2 && idx < 6) return idx - 1;
        if (idx >= 6) return kTowerTypeCount;
    }
    return idx < kTowerTypeCount ? idx : kTowerTypeCount;
}
} // namespace

const char* purchase_result_text(MetaProgression::PurchaseResult r) {
    using R = MetaProgression::PurchaseResult;
    switch (r) {
    case R::Ok: return "ok";
    case R::Maxed: return "already maxed";
    case R::Locked: return "own the node it grows from first";
    case R::BelowThreshold: return "needs more points in this branch";
    case R::NeedMemoryCells: return "not enough Memory Cells";
    case R::NeedAntibodies: return "not enough Antibodies";
    }
    return "?";
}

void MetaProgression::reset_to_new_game() {
    levels_ = TreeLevels{};
    // The one thing a new campaign owns (PROGRESSION.md §1): innate immunity.
    levels_[TreeNode::NeutrophilRoot] = 1;
    progress_ = CampaignProgress{};
    memory_cells_ = 0;
    antibodies_ = 0;
    spent_memory_cells_ = 0;
    spent_antibodies_ = 0;
}

bool MetaProgression::tower_unlocked(TowerType type) const {
    const TreeNode root = tower_root(type);
    return root != TreeNode::Count && levels_.owned(root);
}

bool MetaProgression::ability_unlocked(AbilityId id) const {
    const TreeNode root = ability_root(id);
    return root != TreeNode::Count && levels_.owned(root);
}

// ---- Runs ------------------------------------------------------------------

RunReward MetaProgression::reward_for_run(const RunResult& r, const MetaConfig& cfg,
                                          bool first_clear) {
    // PROGRESSION.md §3.1: paid on EVERY run, cleared or failed, scaled by
    // performance -- never gated to zero by `won`.
    RunReward out;
    u64 mc = cfg.base_run_reward;
    mc += static_cast<u64>(r.waves_cleared) * cfg.per_wave_reward;
    if (cfg.chaff_per_point > 0) mc += r.chaff_killed_total / cfg.chaff_per_point;
    mc += static_cast<u64>(r.elites_killed) * cfg.per_elite_reward;
    mc += static_cast<u64>(r.bosses_killed) * cfg.per_boss_reward;
    if (r.won) mc += cfg.win_bonus;
    out.memory_cells = static_cast<u32>(mc > 0xFFFFFFFFull ? 0xFFFFFFFFull : mc);
    // §3.2: once per level, on its first clear, whatever the grade.
    out.first_clear = r.won && first_clear;
    out.antibodies = out.first_clear ? cfg.first_clear_antibodies : 0u;
    return out;
}

RunReward MetaProgression::record_run(const std::string& level_id, const RunResult& result,
                                      const MetaConfig& cfg) {
    const bool first = result.won && !level_cleared(level_id);
    const RunReward reward = reward_for_run(result, cfg, first);
    credit(reward.memory_cells, reward.antibodies);
    if (result.won) record_level_complete(level_id);
    return reward;
}

bool MetaProgression::level_cleared(const std::string& level_id) const {
    for (const auto& id : progress_.completed_level_ids) {
        if (id == level_id) return true;
    }
    return false;
}

void MetaProgression::record_level_complete(const std::string& level_id) {
    if (level_cleared(level_id)) return;
    progress_.completed_level_ids.push_back(level_id);
    ++progress_.levels_completed;
}

// ---- The tree ----------------------------------------------------------------

TreeCost MetaProgression::next_cost(TreeNode node, const MetaConfig& cfg) const {
    const u8 cur = levels_[node];
    if (cur >= tree_node(node).max_level) return TreeCost{};
    return tree_node_cost(cfg, node, cur);
}

MetaProgression::PurchaseResult MetaProgression::check_purchase(TreeNode node,
                                                                const MetaConfig& cfg) const {
    const TreeNodeDef& d = tree_node(node);
    if (levels_[node] >= d.max_level) return PurchaseResult::Maxed;
    // A tree (§4): a node opens once its parent is owned, at any level.
    if (d.parent != TreeNode::Count && !levels_.owned(d.parent)) return PurchaseResult::Locked;
    if (d.kind == TreeNodeKind::Capstone && branch_points(d.branch) < cfg.capstone_threshold) {
        return PurchaseResult::BelowThreshold;
    }

    const TreeCost c = next_cost(node, cfg);
    if (antibodies_ < c.antibodies) return PurchaseResult::NeedAntibodies;
    if (memory_cells_ < c.memory_cells) return PurchaseResult::NeedMemoryCells;
    return PurchaseResult::Ok;
}

MetaProgression::PurchaseResult MetaProgression::purchase(TreeNode node, const MetaConfig& cfg) {
    const PurchaseResult r = check_purchase(node, cfg);
    if (r != PurchaseResult::Ok) return r;
    const TreeCost c = next_cost(node, cfg);
    memory_cells_ -= c.memory_cells;
    antibodies_ -= c.antibodies;
    spent_memory_cells_ += c.memory_cells;
    spent_antibodies_ += c.antibodies;
    ++levels_[node];
    return PurchaseResult::Ok;
}

bool MetaProgression::can_respec(const MetaConfig& cfg) const {
    const bool bought_anything = spent_memory_cells_ > 0 || spent_antibodies_ > 0;
    return bought_anything && memory_cells_ + spent_memory_cells_ >= cfg.respec_cost;
}

bool MetaProgression::respec(const MetaConfig& cfg) {
    if (!can_respec(cfg)) return false;
    memory_cells_ = memory_cells_ + spent_memory_cells_ - cfg.respec_cost;
    antibodies_ += spent_antibodies_;
    spent_memory_cells_ = 0;
    spent_antibodies_ = 0;
    levels_ = TreeLevels{};
    levels_[TreeNode::NeutrophilRoot] = 1;
    return true;
}

// ---- Versioned JSON save/load ----------------------------------------------

std::string MetaProgression::to_json() const {
    json j;
    j["version"] = kSaveVersion;
    j["memory_cells"] = memory_cells_;
    j["antibodies"] = antibodies_;
    j["spent_memory_cells"] = spent_memory_cells_;
    j["spent_antibodies"] = spent_antibodies_;

    // Only nodes the player actually has, keyed by name: the catalog can grow
    // or reorder without invalidating a save.
    json tree = json::object();
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const u8 lv = levels_.level[i];
        if (lv > 0) tree[tree_node(static_cast<TreeNode>(i)).key] = lv;
    }
    j["tree"] = std::move(tree);

    json prog;
    prog["levels_completed"] = progress_.levels_completed;
    prog["completed_level_ids"] = progress_.completed_level_ids;
    prog["unlocked_regions"] = progress_.unlocked_regions;
    j["progress"] = std::move(prog);

    return j.dump(2);
}

bool MetaProgression::from_json(const std::string& text, std::string& out_error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        out_error = std::string("MetaProgression JSON parse error: ") + e.what();
        return false;
    }

    if (!j.is_object()) {
        out_error = "MetaProgression JSON must be an object";
        return false;
    }
    // A missing "version" is an error, not a default (mirrors Level.h's
    // "schema" rule) -- a save file without a version is not a v1 save, it's
    // not a save file this code understands at all.
    if (!j.contains("version")) {
        out_error = "MetaProgression JSON missing required field 'version'";
        return false;
    }

    i32 version = 0;
    try {
        version = j.at("version").get<i32>();
    } catch (const std::exception& e) {
        out_error = std::string("MetaProgression JSON malformed 'version': ") + e.what();
        return false;
    }
    // Newer-than-supported must fail loudly, never silently drop fields.
    if (version > kSaveVersion) {
        out_error = "MetaProgression save version " + std::to_string(version) +
                     " is newer than supported version " + std::to_string(kSaveVersion) +
                     " -- refusing to load and silently drop fields";
        return false;
    }

    // Everything is built into locals first and only committed at the very
    // end, so a parse failure partway through leaves *this* untouched.
    try {
        TreeLevels levels{};
        u64 memory_cells = 0;
        u32 antibodies = 0;
        u64 spent_mc = 0;
        u32 spent_ab = 0;

        if (version >= 3) {
            memory_cells = j.value("memory_cells", u64{0});
            antibodies = j.value("antibodies", u32{0});
            spent_mc = j.value("spent_memory_cells", u64{0});
            spent_ab = j.value("spent_antibodies", u32{0});
            if (j.contains("tree")) {
                const json& tree = j.at("tree");
                if (!tree.is_object()) throw std::runtime_error("'tree' must be an object");
                for (auto it = tree.begin(); it != tree.end(); ++it) {
                    TreeNode n{};
                    // A node this build does not know (a newer catalog, or one
                    // since removed) is skipped rather than failing the load:
                    // the version gate above is what guards real format changes.
                    if (!find_tree_node(it.key(), n)) continue;
                    const u32 lv = it.value().get<u32>();
                    const u8 max = tree_node(n).max_level;
                    levels[n] = static_cast<u8>(lv < max ? lv : max);
                }
            }
        } else {
            // v1/v2: one currency and a set of unlocked tower indices. The
            // points become Memory Cells, the towers become owned roots.
            // Antibody memories and the loadout have no successor (DESIGN.md
            // §7.3) and are dropped.
            memory_cells = j.value("antibody_points", u64{0});
            if (j.contains("unlocked_towers")) {
                const json& arr = j.at("unlocked_towers");
                if (!arr.is_array()) throw std::runtime_error("'unlocked_towers' must be an array");
                for (const auto& idx_j : arr) {
                    const u32 idx = migrate_tower_index(idx_j.get<u32>(), version);
                    if (idx < kTowerTypeCount) levels[tower_root(static_cast<TowerType>(idx))] = 1;
                }
            }
        }
        // Whatever the file says, the campaign always owns its starting tower.
        levels[TreeNode::NeutrophilRoot] = 1;

        CampaignProgress progress;
        if (j.contains("progress")) {
            const json& pj = j.at("progress");
            progress.levels_completed = pj.value("levels_completed", u32{0});
            if (pj.contains("completed_level_ids")) {
                for (const auto& e : pj.at("completed_level_ids")) {
                    progress.completed_level_ids.push_back(e.get<std::string>());
                }
            }
            if (pj.contains("unlocked_regions")) {
                for (const auto& e : pj.at("unlocked_regions")) {
                    progress.unlocked_regions.push_back(e.get<std::string>());
                }
            }
        }

        // Every field parsed successfully -- commit atomically.
        levels_ = levels;
        memory_cells_ = memory_cells;
        antibodies_ = antibodies;
        spent_memory_cells_ = spent_mc;
        spent_antibodies_ = spent_ab;
        progress_ = std::move(progress);
    } catch (const std::exception& e) {
        out_error = std::string("MetaProgression JSON malformed: ") + e.what();
        return false;
    }

    out_error.clear();
    return true;
}

bool MetaProgression::save(const std::string& path) const {
    return platform::write_text_file(path, to_json());
}

bool MetaProgression::load(const std::string& path) {
    const auto text = platform::read_text_file(path);
    if (!text) return false;
    std::string err;
    return from_json(*text, err);
}

} // namespace immune::game
