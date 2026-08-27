// ui/editor/EditorCanvas.cpp — see EditorCanvas.h.
#include "ui/editor/EditorCanvas.h"

#include "app/EditorMode.h"
#include "core/Math.h"
#include "platform/Input.h"
#include "render/Camera.h"
#include "sim/flowfield/FlowField.h"
#include "ui/editor/EditorGizmos.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace immune::ui {
namespace {

using game::ElementKind;
using game::ElementRef;
using game::LevelDoc;

/// How far from a handle, in SCREEN pixels, still counts as grabbing it. Fixed
/// in pixels rather than world units so picking feels identical whether you are
/// framed on the whole level or on one control point.
constexpr f32 kPickPixels = 9.0f;
constexpr f32 kMinViewHeight = 8.0f;

Rect normalized(Vec2 a, Vec2 b) {
    return Rect{Vec2{math::min(a.x, b.x), math::min(a.y, b.y)},
                Vec2{math::max(a.x, b.x), math::max(a.y, b.y)}};
}

Rect expand(const Rect& r, f32 by) {
    return Rect{r.min - Vec2{by, by}, r.max + Vec2{by, by}};
}

/// How far out along the local +x axis the rotation grip sits, as a multiple of
/// that axis's half-extent.
constexpr f32 kGripReach = 1.6f;

/// The rotation grip of the elements that have one -- an objective, or a box
/// obstacle -- as centre + grip point, or false for everything else. ONE
/// function, called by both the drawing and the drag, so the handle can never
/// be drawn somewhere the press does not look for it.
bool rotation_grip(const game::LevelDef& d, const ElementRef& r, Vec2& center, Vec2& grip) {
    if (r.index < 0) return false;
    const usize i = static_cast<usize>(r.index);
    if (r.kind == ElementKind::Objective && i < d.objectives.size()) {
        const game::ObjectivePoint& o = d.objectives[i];
        center = o.position;
        grip = center + Vec2{std::cos(o.rotation), std::sin(o.rotation)} *
                            (o.half_extents.x * kGripReach);
        return true;
    }
    if (r.kind == ElementKind::Obstacle && i < d.obstacles.size() &&
        d.obstacles[i].shape == game::ObstacleShape::Box) {
        const game::ObstacleDef& o = d.obstacles[i];
        center = o.position;
        grip = center + Vec2{std::cos(o.rotation), std::sin(o.rotation)} *
                            (o.half_extents.x * kGripReach);
        return true;
    }
    return false;
}

/// Writes a rotation back to whichever element the grip belongs to. Same two
/// kinds as rotation_grip(); anything else is a no-op.
void set_element_rotation(LevelDoc& doc, const ElementRef& r, f32 radians) {
    game::LevelDef& d = doc.mutable_def();
    if (r.index < 0) return;
    const usize i = static_cast<usize>(r.index);
    if (r.kind == ElementKind::Objective && i < d.objectives.size()) {
        d.objectives[i].rotation = radians;
    } else if (r.kind == ElementKind::Obstacle && i < d.obstacles.size()) {
        d.obstacles[i].rotation = radians;
    }
}

} // namespace

const char* tool_name(EditorTool t) {
    switch (t) {
    case EditorTool::Select: return "Select";
    case EditorTool::Pen: return "Pen (vessel)";
    case EditorTool::Width: return "Width";
    case EditorTool::Obstacle: return "Obstacle";
    case EditorTool::Spawn: return "Spawn point";
    case EditorTool::Objective: return "Objective";
    case EditorTool::Zone: return "Placement zone";
    case EditorTool::SquadPath: return "Squad path";
    default: return "?";
    }
}

const char* tool_key(EditorTool t) {
    switch (t) {
    case EditorTool::Select: return "V";
    case EditorTool::Pen: return "P";
    case EditorTool::Width: return "W";
    case EditorTool::Obstacle: return "B";
    case EditorTool::Spawn: return "S";
    case EditorTool::Objective: return "O";
    case EditorTool::Zone: return "Z";
    case EditorTool::SquadPath: return "Q";
    default: return "?";
    }
}

void EditorCanvas::set_tool(EditorTool t) {
    if (tool_ == t) return;
    // Switching tools mid-gesture would leave half a polygon or a dangling pen
    // stroke on screen with no way to finish it.
    cancel();
    tool_ = t;
}

void EditorCanvas::cancel() {
    pending_.clear();
    pen_vessel_ = -1;
    dragging_ = false;
    drag_moved_ = false;
    marquee_ = false;
    drag_ref_ = ElementRef{};
}

void EditorCanvas::focus_on(const Rect& r) {
    const Vec2 size = r.size();
    focus_center_ = r.center();
    // Frame with headroom, and never zoom past the minimum: framing a single
    // control point would otherwise put the camera inside the geometry.
    focus_height_ = math::max(math::max(size.y, size.x * 0.6f) * 1.6f, kMinViewHeight * 2.0f);

    if (has_viewport_rect_) {
        // The world fills the framebuffer but only the central node is
        // uncovered, so fit to THAT and then shift the camera by the offset
        // between the two centres. Without the shift, "frame all" reliably
        // parks half the level under the outliner.
        const Vec2 vp = viewport_rect_.size();
        if (vp.x > 1.0f && vp.y > 1.0f) {
            const f32 fb_aspect = vp.x / vp.y;
            focus_height_ =
                math::max(math::max(size.y, size.x / math::max(fb_aspect, 0.1f)) * 1.15f,
                          kMinViewHeight * 2.0f);
        }
        pending_recentre_ = true;
    } else {
        pending_recentre_ = false;
    }
    focus_active_ = true;
}

void EditorCanvas::update_focus(render::Camera& camera) {
    if (pending_recentre_ && has_viewport_rect_) {
        // Applied once the camera knows its new height: the offset is measured
        // in world units through screen_to_world, so it is exact at any zoom.
        pending_recentre_ = false;
        const Vec2 fb_centre{static_cast<f32>(camera.viewport().x) * 0.5f,
                             static_cast<f32>(camera.viewport().y) * 0.5f};
        const Vec2 vp_centre = viewport_rect_.center();
        const f32 saved_h = camera.view_height();
        camera.set_view_height(focus_height_);
        const Vec2 a = camera.screen_to_world(fb_centre);
        const Vec2 b = camera.screen_to_world(vp_centre);
        camera.set_view_height(saved_h);
        focus_center_ += a - b;
    }
    if (!focus_active_) return;
    // Exponential ease rather than a jump: a camera that teleports when you
    // click a validation row loses you your sense of where the level is.
    const f32 k = 0.22f;
    const Vec2 c = camera.center() + (focus_center_ - camera.center()) * k;
    const f32 h = camera.view_height() + (focus_height_ - camera.view_height()) * k;
    camera.set_center(c);
    camera.set_view_height(h);
    if (math::length(focus_center_ - c) < 0.25f && std::fabs(focus_height_ - h) < 0.25f) {
        camera.set_center(focus_center_);
        camera.set_view_height(focus_height_);
        focus_active_ = false;
    }
}

void EditorCanvas::update_camera(render::Camera& camera, platform::InputState& input,
                                 bool hovering) {
    const ImGuiIO& io = ImGui::GetIO();

    // Pan: middle-drag, or Space+left-drag for trackpads with no middle button.
    const bool pan_key = ImGui::IsKeyDown(ImGuiKey_Space);
    if (hovering && (input.mouse_down(platform::MouseButton::Middle) ||
                     (pan_key && input.mouse_down(platform::MouseButton::Left)))) {
        const Vec2 d = input.mouse_delta();
        if (math::length_sq(d) > 0.0f) {
            // Convert the pixel delta through the camera rather than by a fixed
            // scale, so panning tracks the cursor exactly at any zoom.
            const Vec2 a = camera.screen_to_world(input.mouse_pos());
            const Vec2 b = camera.screen_to_world(input.mouse_pos() - d);
            camera.set_center(camera.center() + (b - a));
            focus_active_ = false;   // any manual nudge cancels an ease
        }
    }

    // Zoom to cursor: the world point under the pointer must not move.
    if (hovering && !io.WantCaptureMouse) {
        const f32 wheel = input.wheel();
        if (std::fabs(wheel) > 1e-4f) {
            const Vec2 before = camera.screen_to_world(input.mouse_pos());
            const f32 factor = std::pow(0.85f, wheel);
            const Rect b = camera.bounds();
            const f32 max_h = math::max(b.size().y, b.size().x) * 4.0f;
            camera.set_view_height(
                math::clamp(camera.view_height() * factor, kMinViewHeight, max_h));
            const Vec2 after = camera.screen_to_world(input.mouse_pos());
            camera.set_center(camera.center() + (before - after));
            focus_active_ = false;
        }
    }
}

Vec2 EditorCanvas::snap_point(const app::EditorMode& editor, Vec2 world, bool suspend) const {
    if (!snap_ || suspend) return world;

    const LevelDoc& doc = editor.doc();
    const f32 wpp = 1.0f;   // snapping radii are world-space by design here
    (void)wpp;

    // Vertex snap beats grid snap: welding a branch onto a parent's endpoint is
    // the thing you most need to be exact, and a grid can be coarser than the
    // distance that matters.
    const f32 vertex_radius = math::max(grid_size_, 0.5f) * 1.5f;
    f32 best = vertex_radius * vertex_radius;
    Vec2 result = world;
    bool found = false;
    for (const game::Vessel& v : doc.def().vessels) {
        for (const game::VesselPoint& p : v.points) {
            const f32 d2 = math::length_sq(p.position - world);
            if (d2 >= best) continue;
            best = d2;
            result = p.position;
            found = true;
        }
    }
    for (const game::ObstacleDef& o : doc.def().obstacles) {
        for (const game::VesselPoint& p : o.points) {
            const f32 d2 = math::length_sq(p.position - world);
            if (d2 >= best) continue;
            best = d2;
            result = p.position;
            found = true;
        }
    }
    if (found) return result;

    if (grid_size_ <= 0.0f) return world;
    return Vec2{std::round(world.x / grid_size_) * grid_size_,
                std::round(world.y / grid_size_) * grid_size_};
}

/// Nearest point on any vessel centerline, for the spawn/objective snap. An
/// off-lumen spawn point is the single most common authoring error, so the
/// tools that place them default to landing on the lane.
namespace {
bool nearest_centerline(const game::LevelDef& def, Vec2 world, f32 max_dist, Vec2& out) {
    f32 best = max_dist * max_dist;
    bool found = false;
    for (const game::Vessel& v : def.vessels) {
        std::vector<Vec2> pts;
        std::vector<f32> widths;
        for (const game::VesselPoint& p : v.points) {
            pts.push_back(p.position);
            widths.push_back(p.width);
        }
        std::vector<Vec2> curve;
        sample_vessel_curve(pts, widths, 8, curve, nullptr);
        for (Vec2 c : curve) {
            const f32 d2 = math::length_sq(c - world);
            if (d2 >= best) continue;
            best = d2;
            out = c;
            found = true;
        }
    }
    return found;
}
} // namespace

void EditorCanvas::build_toolbar(app::EditorMode& editor) {
    for (i32 i = 0; i < static_cast<i32>(EditorTool::Count); ++i) {
        const EditorTool t = static_cast<EditorTool>(i);
        if (i) ImGui::SameLine();
        const bool active = tool_ == t;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button(tool_key(t), ImVec2(28, 0))) set_tool(t);
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", tool_name(t), tool_key(t));
    }

    if (tool_ == EditorTool::Obstacle) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        const char* shapes[] = {"disc", "capsule", "box", "polygon", "ridge"};
        int cur = static_cast<int>(obstacle_shape_);
        if (ImGui::Combo("##shape", &cur, shapes, IM_ARRAYSIZE(shapes))) {
            obstacle_shape_ = static_cast<game::ObstacleShape>(cur);
            pending_.clear();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Obstacle shape (1-5)");
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::DragFloat("grid", &grid_size_, 0.1f, 0.0f, 64.0f, "%.2f");
    ImGui::SameLine();
    ImGui::Checkbox("snap", &snap_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold Alt to suspend snapping");

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    if (ImGui::Button("Frame all")) {
        focus_on(expand(editor.doc().def().world_bounds, 4.0f));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Home");
}

void EditorCanvas::handle_tool(app::EditorMode& editor, render::Camera& camera,
                               platform::InputState& input) {
    const ImGuiIO& io = ImGui::GetIO();
    LevelDoc& doc = editor.doc();

    const bool alt = io.KeyAlt;
    const bool shift = io.KeyShift;
    const bool ctrl = io.KeyCtrl;
    const Vec2 raw = camera.screen_to_world(input.mouse_pos());
    const Vec2 world = snap_point(editor, raw, alt);
    cursor_world_ = world;
    cursor_valid_ = true;

    const f32 pick = kPickPixels * Gizmos(ImGui::GetBackgroundDrawList(), camera).world_per_pixel();

    const bool lmb_down = input.mouse_pressed(platform::MouseButton::Left);
    const bool lmb_held = input.mouse_down(platform::MouseButton::Left);
    const bool lmb_up = input.mouse_released(platform::MouseButton::Left);
    const bool rmb_down = input.mouse_pressed(platform::MouseButton::Right);

    // Space is the pan modifier, so it must not also start a tool gesture.
    if (ImGui::IsKeyDown(ImGuiKey_Space)) return;

    switch (tool_) {
    case EditorTool::Select: {
        // A press on the selected element's rotation grip turns it instead of
        // starting a move -- checked before hit_test, since the grip sits
        // outside the element and would otherwise pick whatever is under it.
        Vec2 grip_center{}, grip_point{};
        const bool grip_hit = lmb_down && rotation_grip(doc.def(), doc.primary(), grip_center,
                                                        grip_point) &&
                              math::length_sq(grip_point - raw) <= pick * pick;
        if (grip_hit) {
            dragging_ = true;
            rotating_ = true;
            drag_moved_ = false;
            drag_ref_ = doc.primary();
        } else if (lmb_down) {
            const ElementRef hit = doc.hit_test(world, pick);
            if (!hit.valid()) {
                marquee_ = true;
                marquee_start_ = raw;
                if (!shift) doc.clear_selection();
                break;
            }
            if (shift) {
                doc.select_add(hit);
            } else if (!doc.is_selected(hit)) {
                doc.select(hit);
            }
            // Alt-drag duplicates: the copy becomes the thing you are moving,
            // so the original stays where it was.
            if (alt) {
                const ElementRef copy = doc.duplicate(hit);
                if (copy.valid()) doc.select(copy);
            }
            dragging_ = true;
            drag_moved_ = false;
            drag_ref_ = doc.primary();
            drag_start_world_ = world;
            drag_last_world_ = world;
        } else if (dragging_ && rotating_ && lmb_held) {
            Vec2 c{}, unused{};
            if (rotation_grip(doc.def(), drag_ref_, c, unused)) {
                const Vec2 d = raw - c;
                if (math::length_sq(d) > 1e-6f) {
                    if (!drag_moved_) {
                        doc.begin_gesture("rotate");
                        drag_moved_ = true;
                    }
                    f32 ang = std::atan2(d.y, d.x);
                    // Snapping is angular here, on the same toggle (and the
                    // same Alt escape hatch) that snaps positions to the grid.
                    if (snap_ && !alt) {
                        constexpr f32 kStep = math::kPi / 12.0f;   // 15 degrees
                        ang = std::round(ang / kStep) * kStep;
                    }
                    set_element_rotation(doc, drag_ref_, ang);
                }
            }
        } else if (dragging_ && lmb_held) {
            const Vec2 delta = world - drag_last_world_;
            if (math::length_sq(delta) > 1e-8f) {
                if (!drag_moved_) {
                    // Open the gesture only once the drag really moves, so a
                    // plain click never lands an empty entry on the undo stack.
                    doc.begin_gesture("move");
                    drag_moved_ = true;
                }
                doc.move_selection(delta);
                drag_last_world_ = world;
            }
        } else if (marquee_ && lmb_held) {
            // nothing to do; the rectangle is drawn in draw_gizmos
        }

        if (lmb_up) {
            if (marquee_) {
                const Rect r = normalized(marquee_start_, raw);
                for (const ElementRef& e : doc.hit_test_rect(r)) doc.select_add(e);
                marquee_ = false;
            }
            if (dragging_ && drag_moved_) {
                doc.end_gesture();
                editor.invalidate();
            }
            dragging_ = false;
            drag_moved_ = false;
            rotating_ = false;
        }
        break;
    }

    case EditorTool::Pen: {
        if (lmb_down) {
            if (pen_vessel_ < 0) {
                // Clicking an existing endpoint EXTENDS that vessel rather than
                // starting a parallel one on top of it.
                const ElementRef hit = doc.hit_test(world, pick);
                if (hit.kind == ElementKind::Vessel && hit.sub >= 0) {
                    const game::Vessel& v = doc.def().vessels[static_cast<usize>(hit.index)];
                    if (hit.sub == static_cast<i32>(v.points.size()) - 1) {
                        pen_vessel_ = hit.index;
                        doc.begin_gesture("draw vessel");
                        break;
                    }
                }
                pending_.push_back(world);
                if (pending_.size() == 2) {
                    doc.begin_gesture("draw vessel");
                    pen_vessel_ = doc.add_vessel(pending_[0], pending_[1]);
                    pending_.clear();
                    doc.select(ElementRef{ElementKind::Vessel, pen_vessel_, -1});
                }
            } else {
                doc.insert_vessel_point(pen_vessel_, -1, world);
            }
        }
        // Enter or right-click finishes the stroke.
        if (rmb_down || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            if (pen_vessel_ >= 0) {
                doc.end_gesture();
                editor.invalidate();
            }
            cancel();
        }
        break;
    }

    case EditorTool::Width: {
        if (lmb_down) {
            const ElementRef hit = doc.hit_test(world, pick);
            if (hit.kind == ElementKind::Vessel && hit.sub >= 0) {
                doc.select(hit);
                drag_ref_ = hit;
                dragging_ = true;
                drag_moved_ = false;
                drag_start_world_ = raw;
            }
        } else if (dragging_ && lmb_held && drag_ref_.kind == ElementKind::Vessel &&
                   drag_ref_.sub >= 0) {
            if (!drag_moved_) {
                doc.begin_gesture("set width");
                drag_moved_ = true;
            }
            const game::Vessel& v = doc.def().vessels[static_cast<usize>(drag_ref_.index)];
            // Width follows the distance from the point, doubled: you are
            // dragging the lumen EDGE, and the edge is half the width away.
            const f32 w = math::max(
                math::length(raw - v.points[static_cast<usize>(drag_ref_.sub)].position) * 2.0f,
                0.2f);
            if (ctrl) {
                // Ctrl sets the whole vessel, which is what you want when a
                // lane should be uniform.
                for (usize k = 0; k < v.points.size(); ++k) {
                    doc.set_vessel_point_width(drag_ref_.index, static_cast<i32>(k), w);
                }
            } else if (shift) {
                // Shift smooths: blend neighbours toward the dragged value so a
                // taper stays a taper instead of growing a step.
                const i32 n = static_cast<i32>(v.points.size());
                for (i32 k = 0; k < n; ++k) {
                    const f32 falloff =
                        1.0f / (1.0f + static_cast<f32>(std::abs(k - drag_ref_.sub)));
                    doc.set_vessel_point_width(
                        drag_ref_.index, k,
                        math::lerp(v.points[static_cast<usize>(k)].width, w, falloff));
                }
            } else {
                doc.set_vessel_point_width(drag_ref_.index, drag_ref_.sub, w);
            }
        }
        if (lmb_up && dragging_) {
            if (drag_moved_) {
                doc.end_gesture();
                editor.invalidate();
            }
            dragging_ = false;
            drag_moved_ = false;
        }
        break;
    }

    case EditorTool::Obstacle: {
        if (obstacle_shape_ == game::ObstacleShape::Polygon ||
            obstacle_shape_ == game::ObstacleShape::Ridge) {
            // Multi-click shapes: accumulate, then commit on Enter/right-click.
            if (lmb_down) pending_.push_back(world);
            const usize need = obstacle_shape_ == game::ObstacleShape::Polygon ? 3u : 2u;
            if ((rmb_down || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) &&
                pending_.size() >= need) {
                const i32 idx = doc.add_obstacle(obstacle_shape_, pending_[0], 8.0f);
                game::ObstacleDef& o = doc.mutable_def().obstacles[static_cast<usize>(idx)];
                o.points.clear();
                for (Vec2 p : pending_) {
                    o.points.push_back(game::VesselPoint{
                        p, obstacle_shape_ == game::ObstacleShape::Ridge ? 8.0f : 0.0f});
                }
                doc.select(ElementRef{ElementKind::Obstacle, idx, -1});
                editor.invalidate();
                cancel();
            } else if (rmb_down) {
                cancel();
            }
            break;
        }
        // Drag shapes: press sets the anchor, release sizes them.
        if (lmb_down) {
            drag_start_world_ = world;
            dragging_ = true;
            drag_moved_ = false;
        } else if (dragging_ && lmb_up) {
            const f32 size = math::max(math::length(world - drag_start_world_), 2.0f);
            const i32 idx = doc.add_obstacle(obstacle_shape_, drag_start_world_, size);
            if (obstacle_shape_ == game::ObstacleShape::Capsule) {
                game::ObstacleDef& o = doc.mutable_def().obstacles[static_cast<usize>(idx)];
                o.points[0].position = drag_start_world_;
                o.points[1].position = world;
                o.radius = math::max(size * 0.35f, 1.0f);
            }
            doc.select(ElementRef{ElementKind::Obstacle, idx, -1});
            editor.invalidate();
            dragging_ = false;
        }
        break;
    }

    case EditorTool::Spawn:
    case EditorTool::Objective: {
        if (!lmb_down) break;
        // Snap onto the lane by default. Alt (which already suspends grid snap)
        // also suspends this, for the rare deliberately-offset case.
        Vec2 place = world;
        if (!alt) {
            Vec2 online;
            if (nearest_centerline(doc.def(), raw, 60.0f, online)) place = online;
        }
        const i32 idx = tool_ == EditorTool::Spawn ? doc.add_spawn_point(place)
                                                   : doc.add_objective(place);
        doc.select(ElementRef{tool_ == EditorTool::Spawn ? ElementKind::SpawnPoint
                                                         : ElementKind::Objective,
                              idx, -1});
        editor.invalidate();
        break;
    }

    case EditorTool::Zone: {
        if (lmb_down) {
            drag_start_world_ = world;
            dragging_ = true;
        } else if (dragging_ && lmb_up) {
            const Rect r = normalized(drag_start_world_, world);
            if (r.size().x > 1.0f && r.size().y > 1.0f) {
                const i32 idx = doc.add_zone(r);
                doc.select(ElementRef{ElementKind::Zone, idx, -1});
            }
            dragging_ = false;
        }
        break;
    }

    case EditorTool::SquadPath: {
        if (lmb_down) pending_.push_back(world);
        if ((rmb_down || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) && pending_.size() >= 2) {
            const i32 idx = doc.add_squad_path(pending_);
            doc.select(ElementRef{ElementKind::SquadPath, idx, -1});
            cancel();
        } else if (rmb_down) {
            cancel();
        }
        break;
    }

    default:
        break;
    }
}

void EditorCanvas::handle_shortcuts(app::EditorMode& editor) {
    const ImGuiIO& io = ImGui::GetIO();
    // Never while a text field has focus: typing "v" into a level name must not
    // also switch to the Select tool.
    if (io.WantTextInput) return;

    game::LevelDoc& doc = editor.doc();

    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift ? doc.redo() : doc.undo()) editor.invalidate();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            if (doc.redo()) editor.invalidate();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D, false) && doc.primary().valid()) {
            const game::ElementRef c = doc.duplicate(doc.primary());
            if (c.valid()) doc.select(c);
            editor.invalidate();
        }
        return;   // Ctrl chords are never tool keys
    }

    struct Bind { ImGuiKey key; EditorTool tool; };
    static const Bind kBinds[] = {
        {ImGuiKey_V, EditorTool::Select},   {ImGuiKey_P, EditorTool::Pen},
        {ImGuiKey_W, EditorTool::Width},    {ImGuiKey_B, EditorTool::Obstacle},
        {ImGuiKey_S, EditorTool::Spawn},    {ImGuiKey_O, EditorTool::Objective},
        {ImGuiKey_Z, EditorTool::Zone},     {ImGuiKey_Q, EditorTool::SquadPath},
    };
    for (const Bind& b : kBinds) {
        if (ImGui::IsKeyPressed(b.key, false)) set_tool(b.tool);
    }

    // 1-5 pick the obstacle shape while that tool is live. Scoped to the tool
    // so they stay free for whatever else wants them later.
    if (tool_ == EditorTool::Obstacle) {
        for (i32 i = 0; i < 5; ++i) {
            if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false)) continue;
            obstacle_shape_ = static_cast<game::ObstacleShape>(i);
            pending_.clear();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !doc.selection().empty()) {
        if (doc.erase_selection()) editor.invalidate();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !doc.selection().empty()) {
        Rect r;
        bool any = false;
        for (const game::ElementRef& e : doc.selection()) {
            Rect b;
            if (!doc.element_bounds(e, b)) continue;
            if (!any) { r = b; any = true; }
            else {
                r.min = Vec2{math::min(r.min.x, b.min.x), math::min(r.min.y, b.min.y)};
                r.max = Vec2{math::max(r.max.x, b.max.x), math::max(r.max.y, b.max.y)};
            }
        }
        if (any) focus_on(r);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        const Rect wb = doc.def().world_bounds;
        focus_on(expand(wb, wb.size().x * 0.05f));
    }
    if (io.KeyAlt) {
        // Alt+N view toggles, in the same order the View menu lists them.
        bool* toggles[] = {&views_.grid,       &views_.world_bounds, &views_.vessels,
                           &views_.obstacles,  &views_.spawns,       &views_.objectives,
                           &views_.zones,      &views_.squad_paths};
        for (i32 i = 0; i < 8; ++i) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false)) {
                *toggles[i] = !*toggles[i];
            }
        }
    }
}

void EditorCanvas::build(app::EditorMode& editor, render::Camera& camera,
                         platform::InputState& input) {
    const ImGuiIO& io = ImGui::GetIO();
    pulse_ += io.DeltaTime;

    handle_shortcuts(editor);

    // A click on a panel must never also drop a control point in the world.
    // Hud::begin_frame already publishes ImGui's capture flags into InputState;
    // this is the same guard the HUD's build cursor uses.
    const bool hovering = !io.WantCaptureMouse;

    update_camera(camera, input, hovering);
    update_focus(camera);

    if (hovering && !io.WantCaptureKeyboard) {
        handle_tool(editor, camera, input);
    } else {
        cursor_valid_ = false;
    }

    draw_gizmos(editor, camera);
}

void EditorCanvas::draw_gizmos(app::EditorMode& editor, const render::Camera& camera) {
    const LevelDoc& doc = editor.doc();
    const game::LevelDef& d = doc.def();
    // Background draw list: under every ImGui window, over the rendered world.
    Gizmos g(ImGui::GetBackgroundDrawList(), camera);
    const f32 wpp = g.world_per_pixel();

    if (views_.grid && grid_size_ > 0.0f) {
        // Only when the grid is coarse enough to read; at full-level framing a
        // 1-unit grid is a grey wash that hides the level.
        const f32 px = grid_size_ / wpp;
        if (px > 6.0f) {
            const Rect vis = camera.visible_bounds();
            const f32 x0 = std::floor(vis.min.x / grid_size_) * grid_size_;
            const f32 y0 = std::floor(vis.min.y / grid_size_) * grid_size_;
            for (f32 x = x0; x <= vis.max.x; x += grid_size_) {
                g.line(Vec2{x, vis.min.y}, Vec2{x, vis.max.y}, gizmo_color::grid(), 1.0f);
            }
            for (f32 y = y0; y <= vis.max.y; y += grid_size_) {
                g.line(Vec2{vis.min.x, y}, Vec2{vis.max.x, y}, gizmo_color::grid(), 1.0f);
            }
        }
    }

    if (views_.world_bounds) {
        g.rect(d.world_bounds, gizmo_color::world_bounds(), 2.0f);
        const Vec2 c[4] = {d.world_bounds.min, Vec2{d.world_bounds.max.x, d.world_bounds.min.y},
                           d.world_bounds.max, Vec2{d.world_bounds.min.x, d.world_bounds.max.y}};
        for (Vec2 p : c) g.handle(p, gizmo_color::world_bounds(), false, 4.0f);
    }

    if (views_.zones) {
        for (usize i = 0; i < d.placement_zones.size(); ++i) {
            const bool conc = i < d.placement_zone_tags.size() &&
                              d.placement_zone_tags[i].concentrated;
            const u32 col = conc ? gizmo_color::zone_concentrated() : gizmo_color::zone();
            const bool sel = doc.is_selected(ElementRef{ElementKind::Zone, static_cast<i32>(i), -1});
            g.rect(d.placement_zones[i], sel ? gizmo_color::selection() : col, sel ? 2.5f : 1.5f);
            if (sel) {
                const Rect& r = d.placement_zones[i];
                const Vec2 corners[4] = {r.min, Vec2{r.max.x, r.min.y}, r.max,
                                         Vec2{r.min.x, r.max.y}};
                for (i32 k = 0; k < 4; ++k) g.handle(corners[k], gizmo_color::selection(), true);
            }
            if (views_.labels && conc) {
                g.label(d.placement_zones[i].min, "concentrated", col);
            }
        }
    }

    if (views_.squad_paths) {
        for (usize i = 0; i < d.squad_paths.size(); ++i) {
            const game::SquadPathDef& sp = d.squad_paths[i];
            const bool sel =
                doc.is_selected(ElementRef{ElementKind::SquadPath, static_cast<i32>(i), -1});
            std::vector<f32> hw(sp.points.size(), sp.half_width);
            g.ribbon(sp.points, hw, IM_COL32(255, 235, 140, 30));
            g.polyline(sp.points, sel ? gizmo_color::selection() : gizmo_color::squad_path(),
                       sel ? 2.5f : 1.8f);
            for (usize k = 0; k < sp.points.size(); ++k) {
                const bool psel = doc.is_selected(
                    ElementRef{ElementKind::SquadPath, static_cast<i32>(i), static_cast<i32>(k)});
                g.handle(sp.points[k], psel ? gizmo_color::selection() : gizmo_color::squad_path(),
                         psel);
            }
            if (views_.labels && !sp.points.empty()) {
                g.label(sp.points[0], sp.id, gizmo_color::squad_path());
            }
        }
    }

    if (views_.vessels) {
        std::vector<Vec2> pts;
        std::vector<f32> widths;
        std::vector<Vec2> curve;
        std::vector<f32> half;
        for (usize i = 0; i < d.vessels.size(); ++i) {
            const game::Vessel& v = d.vessels[i];
            pts.clear();
            widths.clear();
            for (const game::VesselPoint& p : v.points) {
                pts.push_back(p.position);
                widths.push_back(p.width);
            }
            sample_vessel_curve(pts, widths, 12, curve, &half);

            const bool sel =
                doc.is_selected(ElementRef{ElementKind::Vessel, static_cast<i32>(i), -1});
            g.ribbon(curve, half, gizmo_color::vessel_ribbon());
            g.polyline(curve, sel ? gizmo_color::selection() : gizmo_color::vessel(),
                       sel ? 2.5f : 1.6f);

            // Midpoint diamonds: click to subdivide. A different silhouette
            // from a control point so the two never read alike.
            for (usize k = 0; k + 1 < v.points.size(); ++k) {
                g.diamond((v.points[k].position + v.points[k + 1].position) * 0.5f,
                          gizmo_color::vessel(), false, 3.5f);
            }
            for (usize k = 0; k < v.points.size(); ++k) {
                const ElementRef pr{ElementKind::Vessel, static_cast<i32>(i), static_cast<i32>(k)};
                const bool psel = doc.is_selected(pr);
                g.handle(v.points[k].position,
                         psel ? gizmo_color::selection() : gizmo_color::vessel(), psel);
                // The width ring only while the Width tool is live or the point
                // is selected -- drawn always, it doubles every lane's outline.
                if (psel || tool_ == EditorTool::Width) {
                    g.ring(v.points[k].position, v.points[k].width * 0.5f,
                           IM_COL32(120, 200, 255, 110), 1.2f, 32);
                }
            }
            if (views_.labels && !v.points.empty()) {
                g.label(v.points[0].position, v.id + " [" + game::vessel_lane(v) + "]",
                        gizmo_color::vessel());
            }
        }
    }

    if (views_.obstacles) {
        for (usize i = 0; i < d.obstacles.size(); ++i) {
            const game::ObstacleDef& o = d.obstacles[i];
            const bool sel =
                doc.is_selected(ElementRef{ElementKind::Obstacle, static_cast<i32>(i), -1});
            const u32 col = sel ? gizmo_color::selection() : gizmo_color::obstacle();
            const f32 th = sel ? 2.5f : 1.6f;

            switch (o.shape) {
            case game::ObstacleShape::Disc:
                g.ring(o.position, o.radius, col, th);
                g.handle(o.position, col, sel);
                break;
            case game::ObstacleShape::Capsule:
                if (o.points.size() >= 2) {
                    g.capsule(o.points[0].position, o.points[1].position, o.radius, col, th);
                }
                break;
            case game::ObstacleShape::Box: {
                const f32 c = std::cos(o.rotation);
                const f32 s = std::sin(o.rotation);
                const Vec2 hx{o.half_extents.x * c, o.half_extents.x * s};
                const Vec2 hy{-o.half_extents.y * s, o.half_extents.y * c};
                const std::vector<Vec2> quad = {o.position - hx - hy, o.position + hx - hy,
                                                o.position + hx + hy, o.position - hx + hy};
                g.polyline(quad, col, th, true);
                g.handle(o.position, col, sel);
                if (sel) {
                    // Rotation handle, out along the box's local +x.
                    const Vec2 grip = o.position + hx * 1.6f;
                    g.line(o.position, grip, col, 1.2f);
                    g.handle(grip, col, true, 5.0f);
                }
                break;
            }
            case game::ObstacleShape::Polygon: {
                std::vector<Vec2> verts;
                for (const game::VesselPoint& p : o.points) verts.push_back(p.position);
                g.polyline(verts, col, th, true);
                if (o.radius > 0.0f && views_.labels) {
                    g.label(verts.empty() ? o.position : verts[0],
                            "inflate " + std::to_string(o.radius).substr(0, 4), col);
                }
                break;
            }
            case game::ObstacleShape::Ridge: {
                std::vector<Vec2> verts;
                for (const game::VesselPoint& p : o.points) verts.push_back(p.position);
                g.polyline(verts, col, th);
                for (const game::VesselPoint& p : o.points) {
                    g.ring(p.position, p.width * 0.5f, IM_COL32(255, 150, 90, 110), 1.2f, 24);
                }
                break;
            }
            }
            // Per-point handles for every shape that has points.
            for (usize k = 0; k < o.points.size(); ++k) {
                const ElementRef pr{ElementKind::Obstacle, static_cast<i32>(i),
                                    static_cast<i32>(k)};
                g.handle(o.points[k].position,
                         doc.is_selected(pr) ? gizmo_color::selection() : gizmo_color::obstacle(),
                         doc.is_selected(pr));
            }
        }
    }

    if (views_.spawns) {
        for (usize i = 0; i < d.spawn_points.size(); ++i) {
            const game::SpawnPoint& p = d.spawn_points[i];
            const bool sel =
                doc.is_selected(ElementRef{ElementKind::SpawnPoint, static_cast<i32>(i), -1});
            const u32 col = sel ? gizmo_color::selection() : gizmo_color::spawn();
            g.disc(p.position, p.radius, IM_COL32(120, 255, 160, 34));
            g.ring(p.position, p.radius, col, sel ? 2.5f : 1.6f);
            g.handle(p.position, col, sel);
            if (views_.labels) g.label(p.position, p.id, col);
        }
    }

    if (views_.objectives) {
        for (usize i = 0; i < d.objectives.size(); ++i) {
            const game::ObjectivePoint& o = d.objectives[i];
            const bool sel =
                doc.is_selected(ElementRef{ElementKind::Objective, static_cast<i32>(i), -1});
            const u32 col = sel ? gizmo_color::selection() : gizmo_color::objective();
            // A turned quad, not a ring: the objective's footprint IS an
            // oriented rectangle (game::ObjectivePoint). Same corner
            // construction as a box obstacle, so the two read alike.
            const f32 c = std::cos(o.rotation);
            const f32 sn = std::sin(o.rotation);
            const Vec2 ax{o.half_extents.x * c, o.half_extents.x * sn};
            const Vec2 ay{-o.half_extents.y * sn, o.half_extents.y * c};
            const std::vector<Vec2> quad = {o.position - ax - ay, o.position + ax - ay,
                                            o.position + ax + ay, o.position - ax + ay};
            g.convex_fill(quad, IM_COL32(255, 120, 190, 40));
            g.polyline(quad, col, sel ? 2.5f : 1.8f, true);
            g.handle(o.position, col, sel);
            if (sel) {
                // Rotation grip, out along the local +x. Draggable: see
                // rotation_grip() and the Select tool.
                const Vec2 grip = o.position + ax * kGripReach;
                g.line(o.position, grip, col, 1.2f);
                g.handle(grip, col, true, 5.0f);
            }
            if (views_.labels) g.label(o.position, o.id, col);
        }
    }

    // ---- In-progress tool feedback ----------------------------------------

    if (!pending_.empty()) {
        g.polyline(pending_, gizmo_color::hover(), 1.8f);
        for (Vec2 p : pending_) g.handle(p, gizmo_color::hover(), true);
        if (cursor_valid_) {
            g.dashed_line(pending_.back(), cursor_world_, gizmo_color::hover(),
                          math::max(grid_size_, 1.0f) * 2.0f, 1.4f);
        }
    }
    if (marquee_ && cursor_valid_) {
        const Rect r = normalized(marquee_start_, cursor_world_);
        g.rect_filled(r, IM_COL32(255, 214, 64, 24));
        g.rect(r, gizmo_color::selection(), 1.4f);
    }
    if (dragging_ && tool_ == EditorTool::Zone && cursor_valid_) {
        const Rect r = normalized(drag_start_world_, cursor_world_);
        g.rect_filled(r, IM_COL32(150, 160, 255, 30));
        g.rect(r, gizmo_color::zone(), 1.6f);
    }
    if (dragging_ && tool_ == EditorTool::Obstacle && cursor_valid_) {
        const f32 size = math::length(cursor_world_ - drag_start_world_);
        if (obstacle_shape_ == game::ObstacleShape::Disc) {
            g.ring(drag_start_world_, size, gizmo_color::obstacle(), 1.6f);
        } else if (obstacle_shape_ == game::ObstacleShape::Capsule) {
            g.capsule(drag_start_world_, cursor_world_, math::max(size * 0.35f, 1.0f),
                      gizmo_color::obstacle(), 1.6f);
        } else if (obstacle_shape_ == game::ObstacleShape::Box) {
            g.rect(normalized(drag_start_world_, cursor_world_), gizmo_color::obstacle(), 1.6f);
        }
    }

    // Cursor crosshair for the placing tools, so you can see exactly where the
    // snap has put the point before you commit it.
    if (cursor_valid_ && tool_ != EditorTool::Select) {
        const f32 s = 8.0f * wpp;
        g.line(cursor_world_ - Vec2{s, 0.0f}, cursor_world_ + Vec2{s, 0.0f}, gizmo_color::hover(),
               1.0f);
        g.line(cursor_world_ - Vec2{0.0f, s}, cursor_world_ + Vec2{0.0f, s}, gizmo_color::hover(),
               1.0f);
    }

    // ---- Validation halos --------------------------------------------------

    if (views_.validation) {
        for (const game::Issue& issue : editor.issues()) {
            if (!issue.has_anchor) continue;
            const u32 col = issue.severity == game::Issue::Severity::Error ? gizmo_color::error()
                                                                          : gizmo_color::warning();
            g.halo(issue.anchor, math::max(10.0f * wpp * 1.5f, 6.0f), col, pulse_);
        }
    }
}

} // namespace immune::ui
