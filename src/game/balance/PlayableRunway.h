#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace immune::game::balance::detail {

/// Forward public route length from a projection until its first departure
/// from the playable rectangle. Off-board drains and later re-entry cannot
/// supply fighting room. `next_point` is the endpoint of the projected segment.
/// With no exit, retain the caller's existing full-route distance exactly.
inline f32 playable_runway(Vec2 projected, usize next_point,
                          const std::vector<Vec2>& points, const Rect& playable,
                          f32 full_remaining) {
    if (points.size() < 2 || next_point == 0 || next_point >= points.size() ||
        !playable.contains(projected) || !std::isfinite(full_remaining)) return 0.0f;
    f32 distance = 0.0f;
    Vec2 from = projected;
    for (usize i = next_point; i < points.size(); ++i) {
        const Vec2 to = points[i];
        const Vec2 delta = to - from;
        const f32 length = math::length(delta);
        if (playable.contains(to)) {
            distance += length;
            from = to;
            continue;
        }
        // The rectangle is convex and `from` is inside. Its first exiting
        // coordinate supplies the segment's exact forward clipping fraction.
        // Boundary touches followed by an inward bend remain legal; an
        // outward step from the boundary has zero remaining runway.
        f32 fraction = 1.0f;
        if (delta.x > 0.0f) fraction = std::min(fraction,(playable.max.x-from.x)/delta.x);
        if (delta.x < 0.0f) fraction = std::min(fraction,(playable.min.x-from.x)/delta.x);
        if (delta.y > 0.0f) fraction = std::min(fraction,(playable.max.y-from.y)/delta.y);
        if (delta.y < 0.0f) fraction = std::min(fraction,(playable.min.y-from.y)/delta.y);
        return distance + length * std::clamp(fraction,0.0f,1.0f);
    }
    return std::max(0.0f,full_remaining);
}

} // namespace immune::game::balance::detail
