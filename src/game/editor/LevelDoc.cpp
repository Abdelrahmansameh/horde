// game/editor/LevelDoc.cpp — see LevelDoc.h for the three design decisions
// (headless, snapshot undo, id hygiene owned here).
#include "game/editor/LevelDoc.h"

#include "core/Math.h"
#include "game/level/LevelWriter.h"      // level_equal, for the dirty flag
#include "sim/flowfield/ObstacleRaster.h"  // dist_sq_to_segment, point_in_polygon

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <utility>

namespace immune::game {
namespace {

const std::string kEmpty;

/// Squared distance from `p` to the polyline through `pts`.
f32 dist_sq_to_polyline(const std::vector<Vec2>& pts, Vec2 p) {
    if (pts.empty()) return 1e30f;
    if (pts.size() == 1) return math::length_sq(p - pts[0]);
    f32 best = 1e30f;
    for (usize i = 0; i + 1 < pts.size(); ++i) {
        best = math::min(best, sim::detail::dist_sq_to_segment(p, pts[i], pts[i + 1]));
    }
    return best;
}

std::vector<Vec2> vessel_positions(const Vessel& v) {
    std::vector<Vec2> out;
    out.reserve(v.points.size());
    for (const VesselPoint& p : v.points) out.push_back(p.position);
    return out;
}

std::vector<Vec2> obstacle_positions(const ObstacleDef& o) {
    std::vector<Vec2> out;
    out.reserve(o.points.size());
    for (const VesselPoint& p : o.points) out.push_back(p.position);
    return out;
}

/// Centre of an obstacle, whichever fields its shape actually uses.
Vec2 obstacle_center(const ObstacleDef& o) {
    if (o.shape == ObstacleShape::Disc || o.shape == ObstacleShape::Box) return o.position;
    if (o.points.empty()) return o.position;
    Vec2 sum{0.0f, 0.0f};
    for (const VesselPoint& p : o.points) sum += p.position;
    return sum / static_cast<f32>(o.points.size());
}

bool rect_contains(const Rect& r, Vec2 p) {
    return p.x >= math::min(r.min.x, r.max.x) && p.x <= math::max(r.min.x, r.max.x) &&
           p.y >= math::min(r.min.y, r.max.y) && p.y <= math::max(r.min.y, r.max.y);
}

/// Is `p` inside the obstacle's solid body? Uses the same analytic tests the
/// rasterizer's carve functions use, so the editor's picking cannot disagree
/// with what actually got carved.
bool obstacle_contains_point(const ObstacleDef& o, Vec2 p) {
    switch (o.shape) {
    case ObstacleShape::Disc:
        return math::length_sq(p - o.position) <= o.radius * o.radius;
    case ObstacleShape::Capsule:
        if (o.points.size() < 2) return false;
        return sim::detail::dist_sq_to_segment(p, o.points[0].position, o.points[1].position) <=
               o.radius * o.radius;
    case ObstacleShape::Box: {
        const f32 c = std::cos(-o.rotation);
        const f32 s = std::sin(-o.rotation);
        const Vec2 d = p - o.position;
        const Vec2 local{d.x * c - d.y * s, d.x * s + d.y * c};
        return std::fabs(local.x) <= o.half_extents.x && std::fabs(local.y) <= o.half_extents.y;
    }
    case ObstacleShape::Polygon: {
        const std::vector<Vec2> verts = obstacle_positions(o);
        if (verts.size() >= 3 && sim::detail::point_in_polygon(p, verts)) return true;
        if (o.radius <= 0.0f) return false;
        return dist_sq_to_polyline(verts, p) <= o.radius * o.radius;
    }
    case ObstacleShape::Ridge: {
        for (usize i = 0; i + 1 < o.points.size(); ++i) {
            const f32 r = math::max(o.points[i].width, o.points[i + 1].width) * 0.5f;
            if (sim::detail::dist_sq_to_segment(p, o.points[i].position,
                                                o.points[i + 1].position) <= r * r) {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

/// Zeroes the geometry fields the obstacle's shape does not read.
///
/// ObstacleDef is one flat struct sharing three geometry fields across five
/// shapes, and which ones a shape reads is documented on ObstacleShape. The
/// WRITER emits only the fields the shape reads, and the PARSER only fills the
/// ones the file mentions -- so a shape carrying a stale value in a field it
/// does not use is a document that silently changes when it round-trips
/// through a save. Normalising here keeps the in-memory document in exactly
/// the form the file format can express.
void normalize_obstacle(ObstacleDef& o) {
    switch (o.shape) {
    case ObstacleShape::Disc:
        o.points.clear();
        o.half_extents = Vec2{0.0f, 0.0f};
        o.rotation = 0.0f;
        break;
    case ObstacleShape::Capsule:
        o.position = Vec2{0.0f, 0.0f};
        o.half_extents = Vec2{0.0f, 0.0f};
        o.rotation = 0.0f;
        for (VesselPoint& p : o.points) p.width = 0.0f;
        break;
    case ObstacleShape::Box:
        o.points.clear();
        o.radius = 0.0f;
        break;
    case ObstacleShape::Polygon:
        // `radius` IS the polygon's `inflate`; it is read, and stays.
        o.position = Vec2{0.0f, 0.0f};
        o.half_extents = Vec2{0.0f, 0.0f};
        o.rotation = 0.0f;
        for (VesselPoint& p : o.points) p.width = 0.0f;
        break;
    case ObstacleShape::Ridge:
        // All of a ridge's geometry is in its per-point widths.
        o.position = Vec2{0.0f, 0.0f};
        o.half_extents = Vec2{0.0f, 0.0f};
        o.rotation = 0.0f;
        o.radius = 0.0f;
        break;
    }
}

} // namespace

std::string vessel_lane(const Vessel& v) { return v.lane_id.empty() ? v.id : v.lane_id; }

std::vector<std::string> lane_ids(const LevelDef& def) {
    std::vector<std::string> out;
    for (const Vessel& v : def.vessels) {
        const std::string lane = vessel_lane(v);
        if (std::find(out.begin(), out.end(), lane) == out.end()) out.push_back(lane);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Document / undo
// ---------------------------------------------------------------------------

void LevelDoc::set_document(LevelDef def, std::string source_path) {
    def_ = std::move(def);
    saved_ = def_;
    source_path_ = std::move(source_path);
    selection_.clear();
    undo_.clear();
    redo_.clear();
    gesture_depth_ = 0;
}

bool LevelDoc::dirty() const { return !level_equal(def_, saved_); }
void LevelDoc::mark_saved() { saved_ = def_; }

void LevelDoc::begin_gesture(std::string label) {
    if (gesture_depth_++ == 0) {
        gesture_base_ = def_;
        gesture_label_ = std::move(label);
    }
}

void LevelDoc::end_gesture() {
    if (gesture_depth_ == 0) return;
    if (--gesture_depth_ > 0) return;
    // Only commit an undo entry if the gesture actually changed something. A
    // click that selects but does not drag must not fill the undo stack with
    // no-ops the user then has to press Ctrl+Z through.
    if (level_equal(gesture_base_, def_)) return;
    undo_.push_back(Snapshot{std::move(gesture_base_), std::move(gesture_label_)});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void LevelDoc::push_undo(std::string label) {
    // Inside a gesture the outermost begin/end pair owns the entry; a nested op
    // must not add one of its own or a drag becomes one entry per mouse-move.
    if (gesture_depth_ > 0) return;
    undo_.push_back(Snapshot{def_, std::move(label)});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

bool LevelDoc::undo() {
    if (undo_.empty()) return false;
    Snapshot s = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(Snapshot{def_, s.label});
    def_ = std::move(s.def);
    selection_.clear();   // indices may no longer mean what they meant
    return true;
}

bool LevelDoc::redo() {
    if (redo_.empty()) return false;
    Snapshot s = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(Snapshot{def_, s.label});
    def_ = std::move(s.def);
    selection_.clear();
    return true;
}

const std::string& LevelDoc::undo_label() const {
    return undo_.empty() ? kEmpty : undo_.back().label;
}
const std::string& LevelDoc::redo_label() const {
    return redo_.empty() ? kEmpty : redo_.back().label;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void LevelDoc::select(ElementRef r) {
    selection_.clear();
    if (r.valid()) selection_.push_back(r);
}

void LevelDoc::select_add(ElementRef r) {
    if (!r.valid()) return;
    const auto it = std::find(selection_.begin(), selection_.end(), r);
    if (it != selection_.end()) selection_.erase(it);
    else selection_.push_back(r);
}

bool LevelDoc::is_selected(ElementRef r) const {
    return std::find(selection_.begin(), selection_.end(), r) != selection_.end();
}

ElementRef LevelDoc::primary() const {
    return selection_.size() == 1 ? selection_[0] : ElementRef{};
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

ElementRef LevelDoc::hit_test(Vec2 world, f32 pick_radius) const {
    const f32 pr2 = pick_radius * pick_radius;

    // PASS 1 -- handles. Points beat bodies unconditionally, so a control point
    // sitting on top of a filled shape is still grabbable. Within the pass the
    // nearest handle wins.
    ElementRef best;
    f32 best_d2 = pr2;
    const auto consider = [&](Vec2 p, ElementRef r) {
        const f32 d2 = math::length_sq(p - world);
        if (d2 > best_d2) return;
        best_d2 = d2;
        best = r;
    };

    for (usize i = 0; i < def_.vessels.size(); ++i) {
        for (usize k = 0; k < def_.vessels[i].points.size(); ++k) {
            consider(def_.vessels[i].points[k].position,
                     ElementRef{ElementKind::Vessel, static_cast<i32>(i), static_cast<i32>(k)});
        }
    }
    for (usize i = 0; i < def_.obstacles.size(); ++i) {
        for (usize k = 0; k < def_.obstacles[i].points.size(); ++k) {
            consider(def_.obstacles[i].points[k].position,
                     ElementRef{ElementKind::Obstacle, static_cast<i32>(i), static_cast<i32>(k)});
        }
    }
    for (usize i = 0; i < def_.squad_paths.size(); ++i) {
        for (usize k = 0; k < def_.squad_paths[i].points.size(); ++k) {
            consider(def_.squad_paths[i].points[k],
                     ElementRef{ElementKind::SquadPath, static_cast<i32>(i), static_cast<i32>(k)});
        }
    }
    // Zone corners, as sub-indices 0..3 (min, maxx-miny, max, minx-maxy).
    for (usize i = 0; i < def_.placement_zones.size(); ++i) {
        const Rect& r = def_.placement_zones[i];
        const Vec2 corners[4] = {r.min, Vec2{r.max.x, r.min.y}, r.max, Vec2{r.min.x, r.max.y}};
        for (i32 k = 0; k < 4; ++k) {
            consider(corners[k], ElementRef{ElementKind::Zone, static_cast<i32>(i), k});
        }
    }
    if (best.valid()) return best;

    // PASS 2 -- point-like elements (spawn points, objectives). Their whole
    // disc is grabbable, not just the centre, so a big spawn radius is easy to
    // hit; the nearest centre still wins a tie.
    for (usize i = 0; i < def_.spawn_points.size(); ++i) {
        const SpawnPoint& p = def_.spawn_points[i];
        const f32 rr = math::max(p.radius, pick_radius);
        if (math::length_sq(p.position - world) <= rr * rr) {
            consider(p.position, ElementRef{ElementKind::SpawnPoint, static_cast<i32>(i), -1});
            best_d2 = 1e30f;   // accept it regardless of pick_radius
            best = ElementRef{ElementKind::SpawnPoint, static_cast<i32>(i), -1};
        }
    }
    for (usize i = 0; i < def_.objectives.size(); ++i) {
        const ObjectivePoint& o = def_.objectives[i];
        // Square footprint (ObjectivePoint), so the hit test is Chebyshev --
        // otherwise the drawn corners would not be clickable.
        const f32 rr = math::max(o.radius, pick_radius);
        const Vec2 d = o.position - world;
        if (math::max(std::fabs(d.x), std::fabs(d.y)) <= rr) {
            best = ElementRef{ElementKind::Objective, static_cast<i32>(i), -1};
        }
    }
    if (best.valid()) return best;

    // PASS 3 -- obstacle bodies. Above zones and vessels because an obstacle is
    // a small thing deliberately placed on top of a big one.
    for (usize i = 0; i < def_.obstacles.size(); ++i) {
        if (!obstacle_contains_point(def_.obstacles[i], world)) continue;
        return ElementRef{ElementKind::Obstacle, static_cast<i32>(i), -1};
    }

    // PASS 4 -- squad path bands, then zone interiors, then vessel bodies. The
    // vessel is last because it is the biggest thing on screen and everything
    // else sits inside it.
    for (usize i = 0; i < def_.squad_paths.size(); ++i) {
        const SquadPathDef& sp = def_.squad_paths[i];
        const f32 hw = math::max(sp.half_width, pick_radius);
        if (dist_sq_to_polyline(sp.points, world) <= hw * hw) {
            return ElementRef{ElementKind::SquadPath, static_cast<i32>(i), -1};
        }
    }
    for (usize i = 0; i < def_.placement_zones.size(); ++i) {
        if (!rect_contains(def_.placement_zones[i], world)) continue;
        return ElementRef{ElementKind::Zone, static_cast<i32>(i), -1};
    }
    for (usize i = 0; i < def_.vessels.size(); ++i) {
        const Vessel& v = def_.vessels[i];
        if (v.points.size() < 2) continue;
        f32 max_half = 0.0f;
        for (const VesselPoint& p : v.points) max_half = math::max(max_half, p.width * 0.5f);
        if (dist_sq_to_polyline(vessel_positions(v), world) <= max_half * max_half) {
            return ElementRef{ElementKind::Vessel, static_cast<i32>(i), -1};
        }
    }
    return ElementRef{};
}

Selection LevelDoc::hit_test_rect(const Rect& rect) const {
    Selection out;
    const auto add_if_in = [&](Vec2 p, ElementRef r) {
        if (rect_contains(rect, p)) out.push_back(r);
    };
    for (usize i = 0; i < def_.vessels.size(); ++i) {
        for (usize k = 0; k < def_.vessels[i].points.size(); ++k) {
            add_if_in(def_.vessels[i].points[k].position,
                      ElementRef{ElementKind::Vessel, static_cast<i32>(i), static_cast<i32>(k)});
        }
    }
    for (usize i = 0; i < def_.obstacles.size(); ++i) {
        add_if_in(obstacle_center(def_.obstacles[i]),
                  ElementRef{ElementKind::Obstacle, static_cast<i32>(i), -1});
    }
    for (usize i = 0; i < def_.spawn_points.size(); ++i) {
        add_if_in(def_.spawn_points[i].position,
                  ElementRef{ElementKind::SpawnPoint, static_cast<i32>(i), -1});
    }
    for (usize i = 0; i < def_.objectives.size(); ++i) {
        add_if_in(def_.objectives[i].position,
                  ElementRef{ElementKind::Objective, static_cast<i32>(i), -1});
    }
    for (usize i = 0; i < def_.squad_paths.size(); ++i) {
        for (usize k = 0; k < def_.squad_paths[i].points.size(); ++k) {
            add_if_in(def_.squad_paths[i].points[k],
                      ElementRef{ElementKind::SquadPath, static_cast<i32>(i), static_cast<i32>(k)});
        }
    }
    return out;
}

bool LevelDoc::element_position(ElementRef r, Vec2& out) const {
    switch (r.kind) {
    case ElementKind::Vessel: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.vessels.size())) return false;
        const Vessel& v = def_.vessels[static_cast<usize>(r.index)];
        if (v.points.empty()) return false;
        if (r.sub >= 0 && r.sub < static_cast<i32>(v.points.size())) {
            out = v.points[static_cast<usize>(r.sub)].position;
        } else {
            out = v.points[v.points.size() / 2].position;
        }
        return true;
    }
    case ElementKind::Obstacle: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.obstacles.size())) return false;
        const ObstacleDef& o = def_.obstacles[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(o.points.size())) {
            out = o.points[static_cast<usize>(r.sub)].position;
        } else {
            out = obstacle_center(o);
        }
        return true;
    }
    case ElementKind::SpawnPoint:
        if (r.index < 0 || r.index >= static_cast<i32>(def_.spawn_points.size())) return false;
        out = def_.spawn_points[static_cast<usize>(r.index)].position;
        return true;
    case ElementKind::Objective:
        if (r.index < 0 || r.index >= static_cast<i32>(def_.objectives.size())) return false;
        out = def_.objectives[static_cast<usize>(r.index)].position;
        return true;
    case ElementKind::Zone: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.placement_zones.size())) return false;
        const Rect& z = def_.placement_zones[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < 4) {
            const Vec2 corners[4] = {z.min, Vec2{z.max.x, z.min.y}, z.max, Vec2{z.min.x, z.max.y}};
            out = corners[r.sub];
        } else {
            out = (z.min + z.max) * 0.5f;
        }
        return true;
    }
    case ElementKind::SquadPath: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.squad_paths.size())) return false;
        const SquadPathDef& sp = def_.squad_paths[static_cast<usize>(r.index)];
        if (sp.points.empty()) return false;
        if (r.sub >= 0 && r.sub < static_cast<i32>(sp.points.size())) {
            out = sp.points[static_cast<usize>(r.sub)];
        } else {
            out = sp.points[sp.points.size() / 2];
        }
        return true;
    }
    case ElementKind::World:
        out = def_.world_bounds.center();
        return true;
    default:
        return false;
    }
}

bool LevelDoc::element_bounds(ElementRef r, Rect& out) const {
    Vec2 p;
    switch (r.kind) {
    case ElementKind::Vessel: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.vessels.size())) return false;
        const Vessel& v = def_.vessels[static_cast<usize>(r.index)];
        if (v.points.empty()) return false;
        if (r.sub >= 0) break;   // a single point: fall through to the point path
        Vec2 lo = v.points[0].position, hi = lo;
        for (const VesselPoint& q : v.points) {
            const f32 h = q.width * 0.5f;
            lo = Vec2{math::min(lo.x, q.position.x - h), math::min(lo.y, q.position.y - h)};
            hi = Vec2{math::max(hi.x, q.position.x + h), math::max(hi.y, q.position.y + h)};
        }
        out = Rect{lo, hi};
        return true;
    }
    case ElementKind::Zone:
        if (r.index < 0 || r.index >= static_cast<i32>(def_.placement_zones.size())) return false;
        if (r.sub >= 0) break;
        out = def_.placement_zones[static_cast<usize>(r.index)];
        return true;
    case ElementKind::World:
        out = def_.world_bounds;
        return true;
    default:
        break;
    }
    if (!element_position(r, p)) return false;
    f32 pad = 6.0f;
    if (r.kind == ElementKind::SpawnPoint && r.index >= 0) {
        pad = def_.spawn_points[static_cast<usize>(r.index)].radius;
    } else if (r.kind == ElementKind::Objective && r.index >= 0) {
        pad = def_.objectives[static_cast<usize>(r.index)].radius;
    }
    out = Rect{p - Vec2{pad, pad}, p + Vec2{pad, pad}};
    return true;
}

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

std::string LevelDoc::unique_id(ElementKind kind, const std::string& prefix) const {
    std::unordered_set<std::string> taken;
    switch (kind) {
    case ElementKind::Vessel:
        for (const Vessel& v : def_.vessels) taken.insert(v.id);
        break;
    case ElementKind::Obstacle:
        for (const ObstacleDef& o : def_.obstacles) taken.insert(o.id);
        break;
    case ElementKind::SpawnPoint:
        for (const SpawnPoint& p : def_.spawn_points) taken.insert(p.id);
        break;
    case ElementKind::Objective:
        for (const ObjectivePoint& o : def_.objectives) taken.insert(o.id);
        break;
    case ElementKind::SquadPath:
        for (const SquadPathDef& s : def_.squad_paths) taken.insert(s.id);
        break;
    default:
        break;
    }
    if (!taken.count(prefix)) return prefix;
    for (i32 n = 2; n < 10000; ++n) {
        const std::string candidate = prefix + "_" + std::to_string(n);
        if (!taken.count(candidate)) return candidate;
    }
    return prefix;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

void LevelDoc::move_element(ElementRef r, Vec2 to) {
    push_undo("move");
    switch (r.kind) {
    case ElementKind::Vessel: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.vessels.size())) return;
        Vessel& v = def_.vessels[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(v.points.size())) {
            v.points[static_cast<usize>(r.sub)].position = to;
        } else if (!v.points.empty()) {
            const Vec2 delta = to - v.points[v.points.size() / 2].position;
            for (VesselPoint& p : v.points) p.position += delta;
        }
        return;
    }
    case ElementKind::Obstacle: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.obstacles.size())) return;
        ObstacleDef& o = def_.obstacles[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(o.points.size())) {
            o.points[static_cast<usize>(r.sub)].position = to;
        } else {
            const Vec2 delta = to - obstacle_center(o);
            o.position += delta;
            for (VesselPoint& p : o.points) p.position += delta;
        }
        return;
    }
    case ElementKind::SpawnPoint:
        if (r.index >= 0 && r.index < static_cast<i32>(def_.spawn_points.size())) {
            def_.spawn_points[static_cast<usize>(r.index)].position = to;
        }
        return;
    case ElementKind::Objective:
        if (r.index >= 0 && r.index < static_cast<i32>(def_.objectives.size())) {
            def_.objectives[static_cast<usize>(r.index)].position = to;
        }
        return;
    case ElementKind::Zone: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.placement_zones.size())) return;
        Rect& z = def_.placement_zones[static_cast<usize>(r.index)];
        if (r.sub == 0) z.min = to;
        else if (r.sub == 1) { z.max.x = to.x; z.min.y = to.y; }
        else if (r.sub == 2) z.max = to;
        else if (r.sub == 3) { z.min.x = to.x; z.max.y = to.y; }
        else {
            const Vec2 delta = to - (z.min + z.max) * 0.5f;
            z.min += delta;
            z.max += delta;
        }
        // A corner dragged past its opposite would invert the rect, which every
        // downstream `contains` test reads as empty. Normalise instead.
        const Rect fixed{Vec2{math::min(z.min.x, z.max.x), math::min(z.min.y, z.max.y)},
                         Vec2{math::max(z.min.x, z.max.x), math::max(z.min.y, z.max.y)}};
        z = fixed;
        return;
    }
    case ElementKind::SquadPath: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.squad_paths.size())) return;
        SquadPathDef& sp = def_.squad_paths[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(sp.points.size())) {
            sp.points[static_cast<usize>(r.sub)] = to;
        } else if (!sp.points.empty()) {
            const Vec2 delta = to - sp.points[sp.points.size() / 2];
            for (Vec2& p : sp.points) p += delta;
        }
        return;
    }
    default:
        return;
    }
}

void LevelDoc::move_selection(Vec2 delta) {
    push_undo("move selection");
    for (const ElementRef& r : selection_) {
        Vec2 p;
        if (!element_position(r, p)) continue;
        // Inside a gesture push_undo is a no-op, so this cannot stack entries.
        move_element(r, p + delta);
    }
}

i32 LevelDoc::add_vessel(Vec2 a, Vec2 b, f32 width, VesselType type) {
    push_undo("add vessel");
    Vessel v;
    v.id = unique_id(ElementKind::Vessel, "vessel");
    v.lane_id = v.id;
    v.type = type;
    v.points.push_back(VesselPoint{a, width});
    v.points.push_back(VesselPoint{b, width});
    def_.vessels.push_back(std::move(v));
    return static_cast<i32>(def_.vessels.size()) - 1;
}

i32 LevelDoc::insert_vessel_point(i32 vessel, i32 after, Vec2 at) {
    if (vessel < 0 || vessel >= static_cast<i32>(def_.vessels.size())) return -1;
    push_undo("insert point");
    Vessel& v = def_.vessels[static_cast<usize>(vessel)];
    // Inherit the width of the point being extended from, so inserting a
    // midpoint on a uniform run does not pinch the lumen.
    f32 width = 12.0f;
    if (!v.points.empty()) {
        const usize src = after >= 0 ? static_cast<usize>(math::clamp(
                                           after, 0, static_cast<i32>(v.points.size()) - 1))
                                     : v.points.size() - 1;
        width = v.points[src].width;
    }
    if (after < 0 || after >= static_cast<i32>(v.points.size()) - 1) {
        v.points.push_back(VesselPoint{at, width});
        return static_cast<i32>(v.points.size()) - 1;
    }
    // Midpoint insert averages its neighbours' widths, which is what makes
    // clicking a segment's midpoint diamond feel like subdividing rather than
    // like adding a bump.
    width = (v.points[static_cast<usize>(after)].width +
             v.points[static_cast<usize>(after) + 1].width) *
            0.5f;
    v.points.insert(v.points.begin() + after + 1, VesselPoint{at, width});
    return after + 1;
}

void LevelDoc::set_vessel_point_width(i32 vessel, i32 point, f32 width) {
    if (vessel < 0 || vessel >= static_cast<i32>(def_.vessels.size())) return;
    Vessel& v = def_.vessels[static_cast<usize>(vessel)];
    if (point < 0 || point >= static_cast<i32>(v.points.size())) return;
    push_undo("set width");
    v.points[static_cast<usize>(point)].width = math::max(width, 0.1f);
}

void LevelDoc::set_vessel_type(i32 vessel, VesselType type) {
    if (vessel < 0 || vessel >= static_cast<i32>(def_.vessels.size())) return;
    push_undo("set vessel type");
    def_.vessels[static_cast<usize>(vessel)].type = type;
}

void LevelDoc::rename_lane(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return;
    push_undo("rename lane");
    for (Vessel& v : def_.vessels) {
        if (vessel_lane(v) == from) v.lane_id = to;
    }
    // Every reference, not just the vessels: a spawn point or squad path left
    // pointing at the old name is a level the loader rejects.
    for (SpawnPoint& p : def_.spawn_points) {
        if (p.lane_id == from) p.lane_id = to;
    }
    for (SquadPathDef& sp : def_.squad_paths) {
        if (sp.lane_id == from) sp.lane_id = to;
    }
}

i32 LevelDoc::add_obstacle(ObstacleShape shape, Vec2 at, f32 size) {
    push_undo("add obstacle");
    ObstacleDef o;
    o.id = unique_id(ElementKind::Obstacle, "obstacle");
    o.shape = shape;
    o.position = at;
    switch (shape) {
    case ObstacleShape::Disc:
        o.radius = size;
        break;
    case ObstacleShape::Capsule:
        o.radius = size * 0.5f;
        o.points.push_back(VesselPoint{at - Vec2{size, 0.0f}, 0.0f});
        o.points.push_back(VesselPoint{at + Vec2{size, 0.0f}, 0.0f});
        break;
    case ObstacleShape::Box:
        o.half_extents = Vec2{size, size * 0.4f};
        break;
    case ObstacleShape::Polygon:
        o.points.push_back(VesselPoint{at + Vec2{-size, -size * 0.6f}, 0.0f});
        o.points.push_back(VesselPoint{at + Vec2{size, -size * 0.3f}, 0.0f});
        o.points.push_back(VesselPoint{at + Vec2{0.0f, size * 0.8f}, 0.0f});
        break;
    case ObstacleShape::Ridge:
        // Ridge points MUST carry a positive width -- parse_obstacle rejects a
        // ridge whose points do not, and a zero-width one would be a level that
        // saves and then refuses to load.
        o.points.push_back(VesselPoint{at - Vec2{size, 0.0f}, size * 0.6f});
        o.points.push_back(VesselPoint{at, size * 0.8f});
        o.points.push_back(VesselPoint{at + Vec2{size, 0.0f}, size * 0.6f});
        break;
    }
    normalize_obstacle(o);
    def_.obstacles.push_back(std::move(o));
    return static_cast<i32>(def_.obstacles.size()) - 1;
}

void LevelDoc::set_obstacle_shape(i32 obstacle, ObstacleShape shape) {
    if (obstacle < 0 || obstacle >= static_cast<i32>(def_.obstacles.size())) return;
    ObstacleDef& o = def_.obstacles[static_cast<usize>(obstacle)];
    if (o.shape == shape) return;
    push_undo("change obstacle shape");

    // Migrate rather than reinterpret. Each shape reads a different subset of
    // the three shared geometry fields (ObstacleShape's doc comment is the
    // authority), so switching without filling in what the new shape needs
    // produces an obstacle the parser rejects -- e.g. a box with zero extents,
    // or a ridge whose points have no widths.
    const Vec2 c = obstacle_center(o);
    f32 extent = o.radius > 0.0f ? o.radius : math::max(o.half_extents.x, o.half_extents.y);
    if (extent <= 0.0f && !o.points.empty()) {
        for (const VesselPoint& p : o.points) {
            extent = math::max(extent, math::length(p.position - c));
        }
    }
    if (extent <= 0.0f) extent = 8.0f;

    o.shape = shape;
    switch (shape) {
    case ObstacleShape::Disc:
        o.position = c;
        o.radius = extent;
        o.points.clear();
        break;
    case ObstacleShape::Capsule:
        o.radius = math::max(extent * 0.5f, 0.5f);
        if (o.points.size() != 2) {
            o.points.assign({VesselPoint{c - Vec2{extent, 0.0f}, 0.0f},
                             VesselPoint{c + Vec2{extent, 0.0f}, 0.0f}});
        }
        for (VesselPoint& p : o.points) p.width = 0.0f;
        break;
    case ObstacleShape::Box:
        o.position = c;
        o.half_extents = Vec2{math::max(extent, 0.5f), math::max(extent * 0.4f, 0.5f)};
        o.points.clear();
        break;
    case ObstacleShape::Polygon:
        if (o.points.size() < 3) {
            o.points.assign({VesselPoint{c + Vec2{-extent, -extent * 0.6f}, 0.0f},
                             VesselPoint{c + Vec2{extent, -extent * 0.3f}, 0.0f},
                             VesselPoint{c + Vec2{0.0f, extent * 0.8f}, 0.0f}});
        }
        for (VesselPoint& p : o.points) p.width = 0.0f;
        o.radius = 0.0f;   // spelled `inflate` for a polygon; default to none
        break;
    case ObstacleShape::Ridge:
        if (o.points.size() < 2) {
            o.points.assign({VesselPoint{c - Vec2{extent, 0.0f}, extent * 0.6f},
                             VesselPoint{c, extent * 0.8f},
                             VesselPoint{c + Vec2{extent, 0.0f}, extent * 0.6f}});
        }
        for (VesselPoint& p : o.points) {
            if (p.width <= 0.0f) p.width = math::max(extent * 0.6f, 0.5f);
        }
        break;
    }
    normalize_obstacle(o);
}

i32 LevelDoc::add_spawn_point(Vec2 at) {
    push_undo("add spawn point");
    SpawnPoint p;
    p.id = unique_id(ElementKind::SpawnPoint, "spawn");
    p.position = at;
    p.radius = 6.0f;
    def_.spawn_points.push_back(std::move(p));
    return static_cast<i32>(def_.spawn_points.size()) - 1;
}

i32 LevelDoc::add_objective(Vec2 at) {
    push_undo("add objective");
    ObjectivePoint o;
    o.id = unique_id(ElementKind::Objective, "objective");
    o.position = at;
    o.radius = 6.0f;
    def_.objectives.push_back(std::move(o));
    return static_cast<i32>(def_.objectives.size()) - 1;
}

i32 LevelDoc::add_zone(const Rect& r) {
    push_undo("add placement zone");
    def_.placement_zones.push_back(
        Rect{Vec2{math::min(r.min.x, r.max.x), math::min(r.min.y, r.max.y)},
             Vec2{math::max(r.min.x, r.max.x), math::max(r.min.y, r.max.y)}});
    // The tag vector is index-aligned with the zone vector, so it has to grow
    // in lockstep or every later zone's tag shifts by one.
    def_.placement_zone_tags.resize(def_.placement_zones.size());
    return static_cast<i32>(def_.placement_zones.size()) - 1;
}

i32 LevelDoc::add_squad_path(const std::vector<Vec2>& points, const std::string& lane_id) {
    push_undo("add squad path");
    SquadPathDef sp;
    sp.id = unique_id(ElementKind::SquadPath, "path");
    sp.lane_id = lane_id;
    sp.points = points;
    def_.squad_paths.push_back(std::move(sp));
    return static_cast<i32>(def_.squad_paths.size()) - 1;
}

void LevelDoc::rename_element(ElementRef r, const std::string& to) {
    if (to.empty()) return;
    push_undo("rename");
    switch (r.kind) {
    case ElementKind::Vessel: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.vessels.size())) return;
        Vessel& v = def_.vessels[static_cast<usize>(r.index)];
        const std::string old_lane = vessel_lane(v);
        const std::string old_id = v.id;
        v.id = to;
        // A vessel whose lane_id defaulted to its id keeps that relationship,
        // and every reference to the old lane follows.
        if (v.lane_id == old_id) {
            v.lane_id = to;
            for (SpawnPoint& p : def_.spawn_points) {
                if (p.lane_id == old_lane) p.lane_id = to;
            }
            for (SquadPathDef& sp : def_.squad_paths) {
                if (sp.lane_id == old_lane) sp.lane_id = to;
            }
        }
        for (Vessel& other : def_.vessels) {
            for (std::string& c : other.children) {
                if (c == old_id) c = to;
            }
        }
        return;
    }
    case ElementKind::SpawnPoint: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.spawn_points.size())) return;
        SpawnPoint& p = def_.spawn_points[static_cast<usize>(r.index)];
        const std::string old_id = p.id;
        p.id = to;
        // The reason this is the document's job: every wave entry naming the
        // old id would otherwise become a dangling reference the loader rejects.
        for (WaveDef& w : def_.waves) {
            for (SpawnEntry& e : w.spawns) {
                if (e.spawn_point_id == old_id) e.spawn_point_id = to;
            }
        }
        return;
    }
    case ElementKind::Objective:
        if (r.index >= 0 && r.index < static_cast<i32>(def_.objectives.size())) {
            def_.objectives[static_cast<usize>(r.index)].id = to;
        }
        return;
    case ElementKind::Obstacle:
        if (r.index >= 0 && r.index < static_cast<i32>(def_.obstacles.size())) {
            def_.obstacles[static_cast<usize>(r.index)].id = to;
        }
        return;
    case ElementKind::SquadPath:
        if (r.index >= 0 && r.index < static_cast<i32>(def_.squad_paths.size())) {
            def_.squad_paths[static_cast<usize>(r.index)].id = to;
        }
        return;
    default:
        return;
    }
}

bool LevelDoc::erase(ElementRef r) {
    switch (r.kind) {
    case ElementKind::Vessel: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.vessels.size())) return false;
        Vessel& v = def_.vessels[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(v.points.size())) {
            // Two points is the minimum a vessel can have; below that the
            // loader rejects it, so refuse rather than create a broken document.
            if (v.points.size() <= 2) return false;
            push_undo("delete point");
            v.points.erase(v.points.begin() + r.sub);
            return true;
        }
        if (def_.vessels.size() <= 1) return false;
        push_undo("delete vessel");
        const std::string gone = v.id;
        def_.vessels.erase(def_.vessels.begin() + r.index);
        for (Vessel& other : def_.vessels) {
            other.children.erase(
                std::remove(other.children.begin(), other.children.end(), gone),
                other.children.end());
        }
        return true;
    }
    case ElementKind::Obstacle: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.obstacles.size())) return false;
        ObstacleDef& o = def_.obstacles[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(o.points.size())) {
            // Per-shape minimums, straight from parse_obstacle's checks.
            const usize min_points = o.shape == ObstacleShape::Polygon ? 3
                                     : o.shape == ObstacleShape::Ridge ? 2
                                                                       : 2;
            if (o.points.size() <= min_points) return false;
            push_undo("delete obstacle point");
            o.points.erase(o.points.begin() + r.sub);
            return true;
        }
        push_undo("delete obstacle");
        def_.obstacles.erase(def_.obstacles.begin() + r.index);
        return true;
    }
    case ElementKind::SpawnPoint: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.spawn_points.size())) return false;
        if (def_.spawn_points.size() <= 1) return false;
        push_undo("delete spawn point");
        const std::string gone = def_.spawn_points[static_cast<usize>(r.index)].id;
        def_.spawn_points.erase(def_.spawn_points.begin() + r.index);
        // Wave entries that named it would become dangling references. Clearing
        // the field falls back to "any spawn point", which is the documented
        // meaning of an empty spawn_point_id and keeps the level loadable.
        for (WaveDef& w : def_.waves) {
            for (SpawnEntry& e : w.spawns) {
                if (e.spawn_point_id == gone) e.spawn_point_id.clear();
            }
        }
        return true;
    }
    case ElementKind::Objective:
        if (r.index < 0 || r.index >= static_cast<i32>(def_.objectives.size())) return false;
        if (def_.objectives.size() <= 1) return false;
        push_undo("delete objective");
        def_.objectives.erase(def_.objectives.begin() + r.index);
        return true;
    case ElementKind::Zone:
        if (r.index < 0 || r.index >= static_cast<i32>(def_.placement_zones.size())) return false;
        push_undo("delete placement zone");
        def_.placement_zones.erase(def_.placement_zones.begin() + r.index);
        if (r.index < static_cast<i32>(def_.placement_zone_tags.size())) {
            def_.placement_zone_tags.erase(def_.placement_zone_tags.begin() + r.index);
        }
        return true;
    case ElementKind::SquadPath: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.squad_paths.size())) return false;
        SquadPathDef& sp = def_.squad_paths[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(sp.points.size())) {
            if (sp.points.size() <= 2) return false;
            push_undo("delete path point");
            sp.points.erase(sp.points.begin() + r.sub);
            return true;
        }
        push_undo("delete squad path");
        def_.squad_paths.erase(def_.squad_paths.begin() + r.index);
        return true;
    }
    case ElementKind::Wave: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.waves.size())) return false;
        WaveDef& w = def_.waves[static_cast<usize>(r.index)];
        if (r.sub >= 0 && r.sub < static_cast<i32>(w.spawns.size())) {
            if (w.spawns.size() <= 1) return false;   // parse_wave needs a non-empty array
            push_undo("delete spawn entry");
            w.spawns.erase(w.spawns.begin() + r.sub);
            return true;
        }
        if (def_.waves.size() <= 1) return false;
        push_undo("delete wave");
        def_.waves.erase(def_.waves.begin() + r.index);
        for (usize i = 0; i < def_.waves.size(); ++i) {
            def_.waves[i].index = static_cast<u32>(i);
        }
        return true;
    }
    default:
        return false;
    }
}

bool LevelDoc::erase_selection() {
    if (selection_.empty()) return false;
    begin_gesture("delete selection");
    // Descending index order: erasing shifts everything after it, so ascending
    // order would invalidate the refs still to be processed.
    Selection ordered = selection_;
    std::sort(ordered.begin(), ordered.end(), [](const ElementRef& a, const ElementRef& b) {
        if (a.index != b.index) return a.index > b.index;
        return a.sub > b.sub;
    });
    bool any = false;
    for (const ElementRef& r : ordered) any = erase(r) || any;
    selection_.clear();
    end_gesture();
    return any;
}

ElementRef LevelDoc::duplicate(ElementRef r) {
    const Vec2 offset{8.0f, 8.0f};
    switch (r.kind) {
    case ElementKind::Vessel: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.vessels.size())) return {};
        push_undo("duplicate vessel");
        Vessel v = def_.vessels[static_cast<usize>(r.index)];
        const std::string old_id = v.id;
        v.id = unique_id(ElementKind::Vessel, old_id);
        // A duplicate is a NEW lane unless it was explicitly grouped with
        // others; copying the lane_id would silently merge two vessels into one
        // lane and change how every wave on it reads.
        if (v.lane_id == old_id) v.lane_id = v.id;
        v.children.clear();
        for (VesselPoint& p : v.points) p.position += offset;
        def_.vessels.push_back(std::move(v));
        return ElementRef{ElementKind::Vessel, static_cast<i32>(def_.vessels.size()) - 1, -1};
    }
    case ElementKind::Obstacle: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.obstacles.size())) return {};
        push_undo("duplicate obstacle");
        ObstacleDef o = def_.obstacles[static_cast<usize>(r.index)];
        if (!o.id.empty()) o.id = unique_id(ElementKind::Obstacle, o.id);
        o.position += offset;
        for (VesselPoint& p : o.points) p.position += offset;
        def_.obstacles.push_back(std::move(o));
        return ElementRef{ElementKind::Obstacle, static_cast<i32>(def_.obstacles.size()) - 1, -1};
    }
    case ElementKind::SpawnPoint: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.spawn_points.size())) return {};
        push_undo("duplicate spawn point");
        SpawnPoint p = def_.spawn_points[static_cast<usize>(r.index)];
        p.id = unique_id(ElementKind::SpawnPoint, p.id);
        p.position += offset;
        def_.spawn_points.push_back(std::move(p));
        return ElementRef{ElementKind::SpawnPoint, static_cast<i32>(def_.spawn_points.size()) - 1,
                          -1};
    }
    case ElementKind::Objective: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.objectives.size())) return {};
        push_undo("duplicate objective");
        ObjectivePoint o = def_.objectives[static_cast<usize>(r.index)];
        o.id = unique_id(ElementKind::Objective, o.id);
        o.position += offset;
        def_.objectives.push_back(std::move(o));
        return ElementRef{ElementKind::Objective, static_cast<i32>(def_.objectives.size()) - 1, -1};
    }
    case ElementKind::Zone: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.placement_zones.size())) return {};
        push_undo("duplicate zone");
        Rect z = def_.placement_zones[static_cast<usize>(r.index)];
        z.min += offset;
        z.max += offset;
        const PlacementZoneTag tag =
            r.index < static_cast<i32>(def_.placement_zone_tags.size())
                ? def_.placement_zone_tags[static_cast<usize>(r.index)]
                : PlacementZoneTag{};
        def_.placement_zones.push_back(z);
        def_.placement_zone_tags.resize(def_.placement_zones.size());
        def_.placement_zone_tags.back() = tag;
        return ElementRef{ElementKind::Zone, static_cast<i32>(def_.placement_zones.size()) - 1, -1};
    }
    case ElementKind::SquadPath: {
        if (r.index < 0 || r.index >= static_cast<i32>(def_.squad_paths.size())) return {};
        push_undo("duplicate squad path");
        SquadPathDef sp = def_.squad_paths[static_cast<usize>(r.index)];
        sp.id = unique_id(ElementKind::SquadPath, sp.id);
        for (Vec2& p : sp.points) p += offset;
        def_.squad_paths.push_back(std::move(sp));
        return ElementRef{ElementKind::SquadPath, static_cast<i32>(def_.squad_paths.size()) - 1, -1};
    }
    case ElementKind::Wave:
        return ElementRef{ElementKind::Wave, duplicate_wave(r.index), -1};
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// Waves
// ---------------------------------------------------------------------------

i32 LevelDoc::add_wave() {
    push_undo("add wave");
    WaveDef w;
    w.index = static_cast<u32>(def_.waves.size());
    w.name = "wave_" + std::to_string(def_.waves.size() + 1);
    // Continue the ramp rather than starting from the struct defaults: a new
    // wave that is easier and pays less than the one before it is never what
    // an author wanted, and the validator would immediately warn about it.
    if (!def_.waves.empty()) {
        const WaveDef& prev = def_.waves.back();
        w.prep_time = math::max(prev.prep_time * 0.9f, 4.0f);
        w.atp_reward = prev.atp_reward + 10;
        w.spawns = prev.spawns;
        for (SpawnEntry& e : w.spawns) {
            e.count = static_cast<u32>(static_cast<f32>(e.count) * 1.3f) + 1;
        }
    } else {
        w.prep_time = 8.0f;
        w.atp_reward = 50;
        SpawnEntry e;
        e.family = PathogenFamily::Virus;
        e.count = 100;
        e.duration = 4.0f;
        w.spawns.push_back(e);
    }
    def_.waves.push_back(std::move(w));
    return static_cast<i32>(def_.waves.size()) - 1;
}

i32 LevelDoc::duplicate_wave(i32 index) {
    if (index < 0 || index >= static_cast<i32>(def_.waves.size())) return -1;
    push_undo("duplicate wave");
    WaveDef w = def_.waves[static_cast<usize>(index)];
    w.name += "_copy";
    def_.waves.insert(def_.waves.begin() + index + 1, std::move(w));
    for (usize i = 0; i < def_.waves.size(); ++i) def_.waves[i].index = static_cast<u32>(i);
    return index + 1;
}

bool LevelDoc::move_wave(i32 index, i32 to) {
    const i32 n = static_cast<i32>(def_.waves.size());
    if (index < 0 || index >= n || to < 0 || to >= n || index == to) return false;
    push_undo("reorder wave");
    WaveDef w = def_.waves[static_cast<usize>(index)];
    def_.waves.erase(def_.waves.begin() + index);
    def_.waves.insert(def_.waves.begin() + to, std::move(w));
    // `index` is not authorable -- array position IS the order the director
    // walks -- so it has to be restamped after any reorder.
    for (usize i = 0; i < def_.waves.size(); ++i) def_.waves[i].index = static_cast<u32>(i);
    return true;
}

i32 LevelDoc::add_spawn_entry(i32 wave) {
    if (wave < 0 || wave >= static_cast<i32>(def_.waves.size())) return -1;
    push_undo("add spawn entry");
    WaveDef& w = def_.waves[static_cast<usize>(wave)];
    SpawnEntry e;
    e.family = PathogenFamily::Virus;
    e.count = 50;
    e.duration = 4.0f;
    if (!w.spawns.empty()) {
        e.family = w.spawns.back().family;
        e.start_time = w.spawns.back().start_time + 1.0f;
    }
    w.spawns.push_back(e);
    return static_cast<i32>(w.spawns.size()) - 1;
}

} // namespace immune::game
