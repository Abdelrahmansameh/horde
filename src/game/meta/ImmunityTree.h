// game/meta/ImmunityTree.h — the Strengthen Immunity skill tree: its nodes,
// what they cost, what they unlock, and what they do to a run.
//
// RATIONALE (PROGRESSION.md, DESIGN.md §7.2)
//  - One connected tree bought between runs with two currencies: Memory Cells
//    (every run pays some) fund every leveled node; Antibodies (a level's first
//    clear) fund the milestones -- tower unlocks, ability unlocks, capstones.
//  - The tree is a fixed CATALOG compiled in here, not data. Its shape and
//    content are the design (PROGRESSION.md §5/§6 name every line); only the
//    pacing -- what a level costs, what a run pays -- is a balance knob, and
//    that lives in meta.json (game/config/GameConfig.h's MetaConfig).
//  - What a node DOES is expressed as an edit to the tuning config: a run
//    starts from the loaded config, apply_immunity_tree() folds the player's
//    purchases into a copy of it, and every system is configured from that
//    copy exactly as it would be from the file. No system learns that a tree
//    exists, which is what keeps tests, benches and headless modes -- none of
//    which apply the tree -- running the untouched baseline.
//  - Towers no longer have in-run tiers (PROGRESSION.md §7). The copy's tier-1
//    row IS the tower, and it is written over tiers 2 and 3 with upgrade_cost
//    0, so nothing in a tree-applied run can buy or reach a tier.
//
// This header is pure: no I/O, no globals, no sim. MetaProgression owns the
// purchased levels and the currencies; this owns what they mean.
#pragma once

#include "core/Types.h"
#include "game/abilities/ActiveAbilities.h"
#include "sim/Immunity.h"

#include <string_view>

namespace immune::game {

struct GameConfig;
struct MetaConfig;

/// The five tower branches plus the hub, in PROGRESSION.md §4's drawing order.
enum class TreeBranch : u8 {
    Hub = 0,
    Neutrophil,
    CytotoxicT,
    Macrophage,
    GobletCell,
    Fibroblast,
    Count,
};
inline constexpr u32 kTreeBranchCount = static_cast<u32>(TreeBranch::Count);

enum class TreeNodeKind : u8 {
    TowerRoot = 0,   ///< Unlocks a tower. Antibodies. No prerequisite.
    Stat,            ///< A tower's leveled line. Memory Cells. Needs the root.
    Capstone,        ///< A branch's unique effect. Both currencies. Needs root + threshold.
    Economy,         ///< A hub line. Memory Cells. No prerequisite.
    AbilityRoot,     ///< Unlocks an active ability. Antibodies. No prerequisite.
    AbilityStat,     ///< An ability's leveled line. Memory Cells. Needs the ability root.
};

/// Every node, in catalog order. The order is the UI's order within a branch
/// and nothing else: saves key nodes by TreeNodeDef::key, never by index.
enum class TreeNode : u16 {
    // ---- Hub: economy lines (§6) ----
    BoneMarrowReserve = 0,
    RapidMetabolism,
    EfficientClearance,
    FieldRequisition,
    SystemicPotency,
    CellularResilience,
    RapidDeployment,
    EliteResponse,
    Homeostasis,
    MembraneResilience,
    // ---- Hub: abilities (§6) ----
    ComplementUnlock,
    ComplementCooldown,
    ComplementPotency,
    ComplementChain,
    HistamineUnlock,
    HistamineCooldown,
    HistamineRadius,
    HistaminePotency,
    FeverUnlock,
    FeverCooldown,
    FeverMagnitude,
    FeverDuration,
    ClotUnlock,
    ClotCooldown,
    ClotDuration,
    ClotWidth,
    // ---- Neutrophil (§5.1) ----
    NeutrophilRoot,
    NeutrophilRoundDamage,
    NeutrophilVolleyCadence,
    NeutrophilTriggerRate,
    NeutrophilAggroRange,
    NeutrophilSquadSize,
    NeutrophilAccuracy,
    NeutrophilVitality,
    NeutrophilHealth,
    NeutrophilCapstone,
    // ---- Cytotoxic T (§5.2) ----
    CytotoxicRoot,
    CytotoxicDrain,
    CytotoxicAttachSpeed,
    CytotoxicCadence,
    CytotoxicSearch,
    CytotoxicSquadSize,
    CytotoxicStamina,
    CytotoxicHealth,
    CytotoxicCapstone,
    // ---- Macrophage (§5.3) ----
    MacrophageRoot,
    MacrophageArms,
    MacrophageGrabSpeed,
    MacrophageCaptives,
    MacrophageCadence,
    MacrophageSearch,
    MacrophageBodyCount,
    MacrophageBodyMass,
    MacrophageHealth,
    MacrophageWall,
    MacrophageCapstone,
    // ---- Goblet Cell (§5.4) ----
    GobletRoot,
    GobletSlowStrength,
    GobletSplashRadius,
    GobletDroplets,
    GobletSlowDuration,
    GobletWeakness,
    GobletCadence,
    GobletHealth,
    GobletCapstone,
    // ---- Fibroblast (§5.5) ----
    FibroblastRoot,
    FibroblastScarHealth,
    FibroblastReinforce,
    FibroblastMaxScars,
    FibroblastScarSize,
    FibroblastBuildRadius,
    FibroblastInflammation,
    FibroblastCadence,
    FibroblastHealth,
    FibroblastCapstone,
    Count,
};
inline constexpr u32 kTreeNodeCount = static_cast<u32>(TreeNode::Count);

struct TreeNodeDef {
    /// Stable identifier: save files, the console, and tests speak this, so
    /// renaming or reordering nodes never breaks a save.
    const char* key = "";
    const char* name = "";
    /// What ONE level does, in the player's words ("+15% round damage").
    const char* effect = "";
    TreeNodeKind kind = TreeNodeKind::Stat;
    TreeBranch branch = TreeBranch::Hub;
    /// The ability a hub ability node belongs to; AbilityId::Count otherwise.
    AbilityId ability = AbilityId::Count;
    u8 max_level = 1;
    /// Percent of the standard Memory Cell curve this line costs.
    u16 cost_weight = 100;
};

const TreeNodeDef& tree_node(TreeNode node);
/// Looks a node up by key. False if there is no such node.
bool find_tree_node(std::string_view key, TreeNode& out);

/// The tower a branch is about; TowerType::Count for the hub.
TowerType branch_tower(TreeBranch branch);
TreeBranch tower_branch(TowerType type);
/// The root node that unlocks a tower / an ability, and a branch's capstone.
TreeNode tower_root(TowerType type);
TreeNode ability_root(AbilityId id);
TreeNode branch_capstone(TreeBranch branch);
const char* branch_name(TreeBranch branch);

/// Levels bought, indexed by TreeNode. A root or capstone is level 0 or 1.
struct TreeLevels {
    u8 level[kTreeNodeCount] = {};

    u8 operator[](TreeNode n) const { return level[static_cast<u32>(n)]; }
    u8& operator[](TreeNode n) { return level[static_cast<u32>(n)]; }
    bool owned(TreeNode n) const { return (*this)[n] > 0; }
};

/// What the next level of `node` costs, given `current` levels already bought.
struct TreeCost {
    u32 memory_cells = 0;
    u32 antibodies = 0;
};
TreeCost tree_node_cost(const MetaConfig& cfg, TreeNode node, u8 current);

/// Stat levels bought in a tower branch -- the capstone threshold's measure
/// (PROGRESSION.md §4.1: "points spent somewhere in this branch").
u32 branch_points(const TreeLevels& levels, TreeBranch branch);

/// Bitmasks over TowerType / AbilityId of what the tree has unlocked.
u32 unlocked_tower_mask(const TreeLevels& levels);
u32 unlocked_ability_mask(const TreeLevels& levels);

/// Everything a purchased tree does to a run that is not a config number:
/// the world-wide sim modifiers, the hostile pass's damage multiplier, and
/// the two unlock masks. Filled by apply_immunity_tree().
struct TreeEffects {
    sim::ImmunityTuning immunity{};
    f32 hostile_damage_taken_mult = 1.0f;
    u32 tower_mask = 0;
    u32 ability_mask = 0;
};

/// Folds `levels` into `cfg` (a COPY of the loaded config -- never the one
/// the registry is bound to) and returns the rest. See the file header for
/// what "folding" means for towers, economy and abilities. Idempotent only in
/// the sense that it must be applied to a fresh copy each time.
TreeEffects apply_immunity_tree(const TreeLevels& levels, GameConfig& cfg);

} // namespace immune::game
