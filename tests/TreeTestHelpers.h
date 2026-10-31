#pragma once

#include "game/meta/ImmunityTree.h"

#include <algorithm>
#include <string>

// Test a former long upgrade as the same cumulative investment across its
// separately purchasable nodes. The catalog remains the source of each cap.
namespace immune::test {
inline game::TreeNode tree_stage(game::TreeNode first, unsigned stage) {
    if (stage == 1) return first;
    game::TreeNode result{};
    const auto key = std::string(game::tree_node(first).key) + ".stage" + std::to_string(stage);
    return game::find_tree_node(key, result) ? result : game::TreeNode::Count;
}

inline unsigned tree_track_max(game::TreeNode first) {
    unsigned total = 0;
    for (unsigned stage = 1; ; ++stage) {
        const auto node = tree_stage(first, stage);
        if (node == game::TreeNode::Count) return total;
        total += game::tree_node(node).max_level;
    }
}

inline void set_tree_track(game::TreeLevels& levels, game::TreeNode first, unsigned rank) {
    for (unsigned stage = 1; ; ++stage) {
        const auto node = tree_stage(first, stage);
        if (node == game::TreeNode::Count) return;
        const auto value = std::min(rank, static_cast<unsigned>(game::tree_node(node).max_level));
        levels[node] = static_cast<immune::u8>(value);
        rank -= value;
    }
}
} // namespace immune::test
