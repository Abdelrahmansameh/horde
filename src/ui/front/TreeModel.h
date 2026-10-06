// ui/front/TreeModel.h — the Strengthen Immunity tree as plain data.
//
// Filled by app/UiBridge (make_tree_model) from MetaProgression and the tree
// catalog (game/meta/ImmunityTree); the screen never sees either, and the
// rules (what can be bought, what it costs, why not) are the game's.
#pragma once

#include "core/Types.h"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
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
    /// The node it grows from, which must be owned before it can be bought;
    /// "" for the root.
    std::string parent;
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
    /// Why it cannot be bought yet, when Locked ("Needs Bone Marrow Reserve").
    std::string requirement;
};

/// The five tower branches, in TreeModel::branch_points order.
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

    /// Which nodes are on the map yet, in `nodes` order. The tree uncovers
    /// as it grows: the root shows, every owned node shows, and so does every
    /// node whose parent is owned -- the next thing on offer. The rest stay
    /// hidden. (An owned node's ancestors always show too, so a save bought
    /// under older rules never leaves an island.)
    std::vector<bool> revealed() const {
        std::unordered_map<std::string_view, usize> index;
        for (usize i = 0; i < nodes.size(); ++i) index.emplace(nodes[i].key, i);
        auto parent_of = [&](usize i) -> usize {
            const auto it = index.find(nodes[i].parent);
            return it == index.end() ? nodes.size() : it->second;
        };
        std::vector<bool> out(nodes.size(), false);
        for (usize i = 0; i < nodes.size(); ++i) {
            const usize p = parent_of(i);
            if (nodes[i].parent.empty() || nodes[i].level > 0 || (p < nodes.size() && nodes[p].level > 0)) {
                out[i] = true;
            }
            if (nodes[i].level == 0) continue;
            for (usize a = p, steps = 0; a < nodes.size() && steps < nodes.size(); a = parent_of(a), ++steps) {
                out[a] = true;
            }
        }
        return out;
    }
    bool revealed(const std::string& key) const {
        const std::vector<bool> shown = revealed();
        for (usize i = 0; i < nodes.size(); ++i) {
            if (nodes[i].key == key) return shown[i];
        }
        return false;
    }
};

} // namespace immune::ui
