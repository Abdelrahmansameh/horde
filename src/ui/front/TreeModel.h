// ui/front/TreeModel.h — the Strengthen Immunity tree as plain data.
//
// Filled by app/UiBridge (make_tree_model) from MetaProgression and the tree
// catalog (game/meta/ImmunityTree); the screen never sees either, and the
// rules (what can be bought, what it costs, why not) are the game's.
#pragma once

#include "core/Types.h"

#include <array>
#include <string>
#include <vector>

namespace immune::ui {

/// What a node looks like, from the canvas's Tree artboard.
enum class TreeNodeState : u8 {
    Locked,     ///< A prerequisite is missing (dimmed).
    Short,      ///< Buyable but not affordable.
    Available,  ///< Can be bought now (white halo).
    Maxed,      ///< Every level bought.
};

enum class TreeNodeRole : u8 { Stat, TowerRoot, AbilityRoot, Capstone };

struct TreeNodeView {
    std::string key;            ///< The game's stable key ("neutrophil.round_damage").
    u32 node = 0;               ///< game::TreeNode as an integer (MenuResult::node).
    std::string name;
    std::string effect;         ///< What one level does.
    TreeNodeRole role = TreeNodeRole::Stat;
    TreeNodeState state = TreeNodeState::Locked;
    u8 level = 0;
    u8 max_level = 1;
    /// Price of the next level (zero when maxed).
    u32 cost_memory = 0;
    u32 cost_antibodies = 0;
    /// Why it cannot be bought yet, when Locked ("Needs Neutrophil").
    std::string requirement;
};

/// The five tower branches, in the canvas's column order.
inline constexpr std::array<const char*, 5> kTreeBranchIds = {"Neutrophil", "CytotoxicT", "Macrophage", "GobletCell",
                                                              "Fibroblast"};

struct TreeModel {
    std::vector<TreeNodeView> nodes;  ///< Catalog order.
    u64 memory_cells = 0;
    u32 antibodies = 0;
    /// Stat levels bought per tower branch (kTreeBranchIds order), toward the
    /// capstone threshold.
    std::array<u32, 5> branch_points{};
    u32 capstone_threshold = 6;
    bool can_respec = false;
    u32 respec_cost = 0;

    const TreeNodeView* find(const std::string& key) const {
        for (const TreeNodeView& n : nodes) {
            if (n.key == key) return &n;
        }
        return nullptr;
    }
};

} // namespace immune::ui
