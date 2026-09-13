// sim/flowfield/LaneConnectivity.h — "would this block sever the lane?" check.
// Header-only for the same reason ObstacleRaster.h is: src's CMakeLists lists
// sim sources explicitly and is orchestrator-owned, so this module must not
// add a .cpp.
//
// Grown out of TowerSystem.cpp's WouldBlockAllPaths heuristic, back when a
// tower footprint was solid ground; the Fibrin Clot active ability
// (game/abilities) then needed the identical question answered for a rotated
// bar, and today it is the only caller -- towers no longer block anything.
// The shape is a predicate rather than a Rect: the caller says which cells
// its block covers, and this only needs a bounding box to know where to look.
//
// WHY A LOCAL WINDOW. A global scratch re-bake would need the level's spawn
// point list and a full flow solve, neither of which is affordable inside a
// placement query. This tests connectivity in a bounded window around the
// block instead, on the theory that a block can only wall off "every lane" by
// fully closing the local vessel cross-section it sits in; if a walkable
// detour exists within a modest margin, the (real, unedited) flow field
// already had a path through there and will keep routing through it after
// the rebake. Anchors are seeded only from points currently `reachable()`
// per the live FlowField, so a block inside an already-dead-end pocket is
// never wrongly rejected.
//
// DIFFERENTIAL, not absolute: the window is flooded twice from the same seed,
// once over the mask as it stands and once with the block applied, and only
// anchors the block NEWLY cut off count. Asking the second flood alone "did
// every anchor stay connected" answers the wrong question, because a window
// can contain walls the block had nothing to do with -- a level with an
// authored obstacle inside a lane (game/level Level.h) puts border anchors on
// either side of an island whose way round lies outside the window, so an
// absolute test calls every placement near one a lane-severing one and the
// lane becomes unbuildable for no reason.
//
// The one thing this cannot do is see a full lane cut for what it is once it
// has happened: an incremental FlowField rebake seeds from the window's own
// boundary costs, so a genuinely severed lane leaves the cells upstream of
// the cut pointing back at the window edge rather than at the cut. That is
// why a runtime block is rejected up front rather than allowed and lived
// with.
#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "sim/flowfield/FlowField.h"

#include <vector>

namespace immune::sim {

/// True if marking every cell for which `blocked(x, y)` holds as non-walkable
/// would disconnect walkable, currently-reachable cells on the border of a
/// window around `bounds` from one another. `bounds` is the world-space box
/// the predicate can be true inside; cells outside it are never asked.
template <typename BlockedFn>
bool would_sever_lane(const TissueMask& mask, const FlowField& flow, const Rect& bounds,
                      BlockedFn&& blocked) {
    if (mask.width() <= 0 || mask.height() <= 0) return false;

    const f32 cs = math::max(mask.cell_size(), 1e-3f);
    const f32 pad = cs * 14.0f;
    const IVec2 c0raw = mask.world_to_cell(bounds.min - Vec2{pad, pad});
    const IVec2 c1raw = mask.world_to_cell(bounds.max + Vec2{pad, pad});
    const i32 x0 = math::clamp(c0raw.x, 0, mask.width() - 1);
    const i32 y0 = math::clamp(c0raw.y, 0, mask.height() - 1);
    const i32 x1 = math::clamp(c1raw.x, 0, mask.width() - 1);
    const i32 y1 = math::clamp(c1raw.y, 0, mask.height() - 1);
    const i32 w = x1 - x0 + 1;
    const i32 h = y1 - y0 + 1;
    if (w <= 0 || h <= 0) return false;

    // Transient BFS scratch only -- never holds anything meaningful across
    // calls, so reusing capacity via function-local statics is safe. One set
    // per predicate type, which is fine: each caller is its own instantiation.
    static thread_local std::vector<u32> visited_gen;
    static thread_local u32 gen = 0;
    static thread_local std::vector<IVec2> stack;
    static thread_local std::vector<IVec2> anchors;
    static thread_local std::vector<u8> reached_before;
    static thread_local std::vector<u8> reached_after;
    const usize cell_count = static_cast<usize>(w) * static_cast<usize>(h);
    if (visited_gen.size() < cell_count) visited_gen.assign(cell_count, 0);
    ++gen;
    stack.clear();
    anchors.clear();

    auto local_index = [&](i32 x, i32 y) {
        return static_cast<usize>(y - y0) * static_cast<usize>(w) + static_cast<usize>(x - x0);
    };

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const bool on_border = (x == x0 || x == x1 || y == y0 || y == y1);
            if (!on_border || !mask.walkable(x, y)) continue;
            if (!flow.reachable(mask.cell_to_world(x, y))) continue;
            anchors.push_back(IVec2{x, y});
        }
    }
    if (anchors.size() < 2) return false; // nothing that could be disconnected

    const IVec2 offsets[4] = {IVec2{1, 0}, IVec2{-1, 0}, IVec2{0, 1}, IVec2{0, -1}};
    auto flood_from_first_anchor = [&](bool apply_block, std::vector<u8>& reached) {
        auto open = [&](i32 x, i32 y) {
            if (!mask.in_range(x, y) || !mask.walkable(x, y)) return false;
            return !(apply_block && blocked(x, y));
        };
        reached.assign(anchors.size(), 0u);
        ++gen;
        stack.clear();
        if (!open(anchors[0].x, anchors[0].y)) return;
        stack.push_back(anchors[0]);
        visited_gen[local_index(anchors[0].x, anchors[0].y)] = gen;
        while (!stack.empty()) {
            const IVec2 cur = stack.back();
            stack.pop_back();
            for (const IVec2& o : offsets) {
                const i32 nx = cur.x + o.x;
                const i32 ny = cur.y + o.y;
                if (nx < x0 || nx > x1 || ny < y0 || ny > y1 || !open(nx, ny)) continue;
                const usize idx = local_index(nx, ny);
                if (visited_gen[idx] == gen) continue;
                visited_gen[idx] = gen;
                stack.push_back(IVec2{nx, ny});
            }
        }
        for (usize i = 0; i < anchors.size(); ++i) {
            reached[i] = visited_gen[local_index(anchors[i].x, anchors[i].y)] == gen ? 1u : 0u;
        }
    };

    flood_from_first_anchor(/*apply_block=*/false, reached_before);
    flood_from_first_anchor(/*apply_block=*/true, reached_after);

    for (usize i = 1; i < anchors.size(); ++i) {
        if (reached_before[i] != 0u && reached_after[i] == 0u) return true;
    }
    return false;
}

/// The axis-aligned case.
inline bool would_sever_lane(const TissueMask& mask, const FlowField& flow, const Rect& footprint) {
    const IVec2 f0 = mask.world_to_cell(footprint.min);
    const IVec2 f1 = mask.world_to_cell(footprint.max);
    return would_sever_lane(mask, flow, footprint, [&](i32 x, i32 y) {
        return x >= f0.x && x <= f1.x && y >= f0.y && y <= f1.y;
    });
}

} // namespace immune::sim
