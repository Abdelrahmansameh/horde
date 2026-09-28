// game/meta/MetaProgression.h — the persistent, cross-run half of the game:
// the two currencies, the Strengthen Immunity purchases, campaign progress,
// and save/load. Rewritten for PROGRESSION.md (DESIGN.md §7.2/§7.3).
//
// WHAT CHANGED
// The single "antibody points" currency and the per-run loadout of antibody
// memories are gone (DESIGN.md §7.3 cut the loadout outright). In their place:
//   - Memory Cells, paid by EVERY run -- a wipe on wave 1 still earns the
//     floor -- scaled by how far the run got (§3.1);
//   - Antibodies, paid exactly once per level, on its first clear (§3.2);
//   - the Strengthen Immunity tree (game/meta/ImmunityTree.h), which both buy
//     into. The catalog and what each node does live there; this class owns
//     only how many levels of each node the player has, and the rules for
//     buying one.
//
// Save data is versioned JSON. Loading an older version must migrate, not
// fail; loading a NEWER version must fail loudly rather than silently drop
// fields. Nodes are saved by key, never by index, so the catalog can grow.
#pragma once

#include "core/Types.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/meta/ImmunityTree.h"

#include <string>
#include <vector>

namespace immune::game {

struct MetaConfig;

/// Outcome of a single completed or failed run, as input to reward_for_run().
/// Deliberately independent of SimWorld/WaveDirector types so the payout is
/// testable without constructing a real sim; app/ fills it at run end.
struct RunResult {
    u32 waves_cleared = 0;
    u64 chaff_killed_total = 0;
    u32 elites_killed = 0;
    u32 bosses_killed = 0;
    /// True only on a full level clear. Memory Cells are paid on every run
    /// regardless (§3.1: never gated to zero); `won` adds the win bonus and is
    /// the only way to earn Antibodies.
    bool won = false;
};

/// What one run paid out, for the results screen.
struct RunReward {
    u32 memory_cells = 0;
    u32 antibodies = 0;
    /// This run was the level's first clear (the only kind that pays Antibodies).
    bool first_clear = false;
};

struct CampaignProgress {
    u32 levels_completed = 0;
    std::vector<std::string> completed_level_ids;
    std::vector<std::string> unlocked_regions;
};

class MetaProgression {
public:
    /// v3: Strengthen Immunity. v1/v2 saves (one currency, antibody memories,
    /// a loadout) migrate: their points become Memory Cells, their unlocked
    /// towers become owned tower roots, and the memories/loadout are dropped.
    /// v2 itself renumbered tower indices when a roster slot was cut.
    static constexpr i32 kSaveVersion = 3;

    // Memory Cell payout defaults. meta.json is the live source
    // (MetaConfig); these seed it and are what default_game_config() writes.
    static constexpr u32 kBaseRunReward = 10;      ///< Flat, every run, even a wipe on wave 0.
    static constexpr u32 kPerWaveReward = 15;      ///< Per wave fully cleared.
    static constexpr u32 kPerEliteReward = 8;      ///< Per elite defeated.
    static constexpr u32 kPerBossReward = 50;      ///< Per boss defeated.
    static constexpr u32 kChaffPerPoint = 200;     ///< Chaff killed per Memory Cell, coarse-grained.
    static constexpr u32 kWinBonus = 40;           ///< Flat bonus for a full clear.

    /// Pure: what a run pays. Memory Cells on every run (the base reward alone
    /// guarantees > 0), scaled by the performance signals, plus the win bonus
    /// on a clear. Antibodies only when `won && first_clear`.
    static RunReward reward_for_run(const RunResult& result, const MetaConfig& cfg,
                                    bool first_clear);

    /// A new campaign: no currency, no progress, and only the Neutrophil --
    /// the one tower the player starts with (PROGRESSION.md §1).
    void reset_to_new_game();

    // ---- Currencies ---------------------------------------------------------
    u64 memory_cells() const { return memory_cells_; }
    u32 antibodies() const { return antibodies_; }
    void credit(u64 memory_cells, u32 antibodies = 0) {
        memory_cells_ += memory_cells;
        antibodies_ += antibodies;
    }

    // ---- Runs ---------------------------------------------------------------
    /// Pays out a finished run on `level_id` and records a clear. The
    /// Antibody is paid only the first time `level_id` is cleared.
    RunReward record_run(const std::string& level_id, const RunResult& result,
                         const MetaConfig& cfg);
    bool level_cleared(const std::string& level_id) const;
    const CampaignProgress& progress() const { return progress_; }
    /// Marks a level cleared without paying anything. record_run() is the
    /// normal path; this exists for migration and tests.
    void record_level_complete(const std::string& level_id);

    // ---- The tree -----------------------------------------------------------
    const TreeLevels& levels() const { return levels_; }
    u8 level(TreeNode n) const { return levels_[n]; }
    bool tower_unlocked(TowerType type) const;
    bool ability_unlocked(AbilityId id) const;
    u32 branch_points(TreeBranch branch) const { return game::branch_points(levels_, branch); }

    enum class PurchaseResult : u8 {
        Ok = 0,
        Maxed,              ///< Already at max level (a root/capstone: already owned).
        Locked,             ///< Needs the branch's tower root or the ability's root first.
        BelowThreshold,     ///< A capstone before enough stat levels in its branch.
        NeedMemoryCells,
        NeedAntibodies,
    };
    /// Whether the next level of `node` can be bought right now, and why not.
    PurchaseResult check_purchase(TreeNode node, const MetaConfig& cfg) const;
    /// Buys the next level of `node`. Nothing changes on any non-Ok result.
    PurchaseResult purchase(TreeNode node, const MetaConfig& cfg);
    /// Price of the next level of `node` (zero-cost if already maxed).
    TreeCost next_cost(TreeNode node, const MetaConfig& cfg) const;

    /// A full respec (PROGRESSION.md §8's recommended default): every node back
    /// to zero except the Neutrophil, every currency spent on them refunded,
    /// minus MetaConfig::respec_cost Memory Cells. False (and nothing changes)
    /// if nothing has been bought or the refund could not cover the fee.
    bool respec(const MetaConfig& cfg);
    bool can_respec(const MetaConfig& cfg) const;
    u64 spent_memory_cells() const { return spent_memory_cells_; }
    u32 spent_antibodies() const { return spent_antibodies_; }

    /// Folds the purchased tree into `cfg` (a run-start COPY of the loaded
    /// config); see apply_immunity_tree().
    TreeEffects apply_to(GameConfig& cfg) const { return apply_immunity_tree(levels_, cfg); }

    // ---- Persistence --------------------------------------------------------
    /// Versioned JSON save/load. Returns false and leaves state untouched on a
    /// malformed or future-versioned file.
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    /// Serialized form, exposed for tests without touching the filesystem.
    std::string to_json() const;
    bool from_json(const std::string& json, std::string& out_error);

private:
    TreeLevels levels_{};
    CampaignProgress progress_;
    u64 memory_cells_ = 0;
    u32 antibodies_ = 0;
    /// Everything ever spent on the tree since the last respec -- exactly what
    /// a respec refunds.
    u64 spent_memory_cells_ = 0;
    u32 spent_antibodies_ = 0;
};

const char* purchase_result_text(MetaProgression::PurchaseResult r);

} // namespace immune::game
