// ui/editor/EditorPanels.cpp — see EditorPanels.h.
//
// Every control here goes through a LevelDoc operation rather than poking the
// LevelDef, so the panels get id hygiene and undo for free and cannot become a
// second editing path that skips them. The two exceptions are the scalar field
// drags (radius, integrity, prep time), which write through mutable_def()
// bracketed by an explicit gesture -- ImGui's DragFloat has no "one edit"
// notion, so the gesture is opened on activation and closed on deactivation.
#include "ui/editor/EditorPanels.h"

#include "app/EditorMode.h"
#include "core/Math.h"
#include "game/editor/LevelDoc.h"
#include "game/enemies/EnemyRoster.h"
#include "game/towers/TowerSystem.h"   // tower_type_name, for the allowed list
#include "render/Camera.h"
#include "ui/editor/EditorCanvas.h"

#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder, for the default layout

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace immune::ui {
namespace {

using game::ElementKind;
using game::ElementRef;

const char* kVesselTypeNames[] = {"artery", "vein", "lymphatic", "nerve_adjacent", "mucosal_fold"};
const char* kObstacleShapeNames[] = {"disc", "capsule", "box", "polygon", "ridge"};
const char* kModifierNames[] = {"none", "fever", "swarm"};

/// Brackets a run of ImGui drag/input edits into ONE undo entry: the gesture
/// opens when the widget becomes active and closes when it is released. Without
/// this, dragging a radius slider for a second would push sixty undo entries.
void gesture_from_item(game::LevelDoc& doc, const char* label) {
    if (ImGui::IsItemActivated()) doc.begin_gesture(label);
    if (ImGui::IsItemDeactivatedAfterEdit()) doc.end_gesture();
    else if (ImGui::IsItemDeactivated() && doc.in_gesture()) doc.end_gesture();
}

/// A text field bound to a std::string, committing on Enter or focus loss.
bool id_field(const char* label, const std::string& current, char* buf, usize buf_size,
              std::string& out) {
    if (!ImGui::IsItemActive()) {
        std::snprintf(buf, buf_size, "%s", current.c_str());
    }
    if (ImGui::InputText(label, buf, buf_size, ImGuiInputTextFlags_EnterReturnsTrue)) {
        out = buf;
        return !out.empty() && out != current;
    }
    return false;
}

ImVec4 family_tint(PathogenFamily f, const game::EnemyRoster* roster) {
    if (roster) {
        const Vec4 c = roster->family(f).color;
        return ImVec4(c.x, c.y, c.z, 1.0f);
    }
    return f == PathogenFamily::Virus ? ImVec4(0.5f, 0.8f, 1.0f, 1.0f)
                                      : ImVec4(0.9f, 0.75f, 0.35f, 1.0f);
}

const char* family_label(PathogenFamily f, const game::EnemyRoster* roster) {
    if (roster) return roster->family(f).name;
    return f == PathogenFamily::Virus ? "virus" : "bacteria";
}

} // namespace

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

EditorRequest EditorPanels::draw_menu_bar(app::EditorMode& editor, EditorCanvas& canvas,
                                          bool playing) {
    EditorRequest req;
    game::LevelDoc& doc = editor.doc();

    if (!ImGui::BeginMainMenuBar()) return req;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New...", "Ctrl+N")) show_new_ = true;
        if (ImGui::MenuItem("Open...", "Ctrl+O")) show_open_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, !doc.source_path().empty())) {
            req.action = EditorAction::Save;
        }
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) show_save_as_ = true;
        if (editor.blocked_from_saving()) {
            // Explicit, and deliberately not the default: work in progress has
            // to be savable, but you should have to say so.
            if (ImGui::MenuItem("Save anyway (has errors)")) req.action = EditorAction::SaveAnyway;
        }
        if (ImGui::MenuItem("Revert", nullptr, false, !doc.source_path().empty())) {
            req.action = EditorAction::Revert;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit editor")) req.action = EditorAction::ExitToMenu;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        const std::string undo = doc.can_undo() ? "Undo " + doc.undo_label() : "Undo";
        const std::string redo = doc.can_redo() ? "Redo " + doc.redo_label() : "Redo";
        if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, doc.can_undo())) {
            doc.undo();
            editor.invalidate();
        }
        if (ImGui::MenuItem(redo.c_str(), "Ctrl+Shift+Z", false, doc.can_redo())) {
            doc.redo();
            editor.invalidate();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, doc.primary().valid())) {
            const ElementRef c = doc.duplicate(doc.primary());
            if (c.valid()) doc.select(c);
            editor.invalidate();
        }
        if (ImGui::MenuItem("Delete", "Del", false, !doc.selection().empty())) {
            doc.erase_selection();
            editor.invalidate();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ViewToggles& v = canvas.views();
        ImGui::MenuItem("Grid", "Alt+1", &v.grid);
        ImGui::MenuItem("World bounds", "Alt+2", &v.world_bounds);
        ImGui::MenuItem("Vessels", "Alt+3", &v.vessels);
        ImGui::MenuItem("Obstacles", "Alt+4", &v.obstacles);
        ImGui::MenuItem("Spawn points", "Alt+5", &v.spawns);
        ImGui::MenuItem("Objectives", "Alt+6", &v.objectives);
        ImGui::MenuItem("Placement zones", "Alt+7", &v.zones);
        ImGui::MenuItem("Squad paths", "Alt+8", &v.squad_paths);
        ImGui::Separator();
        ImGui::MenuItem("Game camera view", nullptr, &v.camera_frame);
        ImGui::MenuItem("Labels", nullptr, &v.labels);
        ImGui::MenuItem("Validation halos", nullptr, &v.validation);
        ImGui::Separator();
        if (ImGui::MenuItem("Frame selection", "F", false, !doc.selection().empty())) {
            want_focus_selection_ = true;
        }
        if (ImGui::MenuItem("Frame all", "Home")) {
            req.action = EditorAction::FocusIssue;
            req.focus = doc.def().world_bounds;
        }
        ImGui::Separator();
        ImGui::MenuItem("Level settings", nullptr, &show_settings_);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Level")) {
        if (ImGui::MenuItem("Wave ramp generator...")) show_ramp_ = true;
        if (ImGui::MenuItem("Bake derived squad paths to authored")) {
            // Materialises what build_squad_paths() would derive, so you can
            // hand-tune ONE without silently dropping the lane to a single
            // path -- which is what authoring one by hand does today.
            game::LevelLoader loader;
            const std::vector<sim::SquadPath> derived =
                loader.build_squad_paths(doc.def(), editor.baked().mask);
            if (!derived.empty()) {
                doc.begin_gesture("bake squad paths");
                for (const sim::SquadPath& p : derived) {
                    if (p.points.size() < 2) continue;
                    doc.add_squad_path(p.points, p.lane_id);
                }
                doc.end_gesture();
                editor.invalidate();
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Play")) {
        if (!playing) {
            if (ImGui::MenuItem("Play", "F5")) {
                req.action = EditorAction::Play;
                req.wave_index = 0;
            }
            if (ImGui::MenuItem("Play from selected wave", "Shift+F5")) {
                req.action = EditorAction::Play;
                req.wave_index = selected_wave_;
            }
        } else if (ImGui::MenuItem("Stop", "Shift+F5")) {
            req.action = EditorAction::Stop;
        }
        ImGui::EndMenu();
    }

    // Title, right-aligned-ish: the dirty marker belongs where a title bar
    // would put it, not buried in a panel.
    const std::string title = editor.title();
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);
    // ASCII only: the system font Hud loads has no em-dash glyph, so a literal
    // one renders as a bare '?' in the one place the level's name is shown.
    ImGui::TextUnformatted(("IMMUNE editor - " + title).c_str());
    if (playing) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "[PLAYING]");
    }

    ImGui::EndMainMenuBar();
    return req;
}

// ---------------------------------------------------------------------------
// Outliner
// ---------------------------------------------------------------------------

void EditorPanels::draw_outliner(app::EditorMode& editor, EditorCanvas& canvas) {
    game::LevelDoc& doc = editor.doc();
    const game::LevelDef& d = doc.def();

    if (!ImGui::Begin("Outliner")) {
        ImGui::End();
        return;
    }

    const auto row = [&](ElementRef r, const std::string& label) {
        const bool sel = doc.is_selected(r);
        if (ImGui::Selectable(label.c_str(), sel)) {
            if (ImGui::GetIO().KeyShift) doc.select_add(r);
            else doc.select(r);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            doc.select(r);
            want_focus_selection_ = true;
        }
    };

    // Lanes, then the vessels inside them: a lane is the unit an author thinks
    // in, and a level's vessels are grouped by lane_id rather than listed flat.
    if (ImGui::TreeNodeEx("Lanes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const std::string& lane : game::lane_ids(d)) {
            if (!ImGui::TreeNodeEx(lane.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            for (usize i = 0; i < d.vessels.size(); ++i) {
                if (game::vessel_lane(d.vessels[i]) != lane) continue;
                const ElementRef vr{ElementKind::Vessel, static_cast<i32>(i), -1};
                const std::string label =
                    d.vessels[i].id + "  (" +
                    std::string(game::vessel_type_to_string(d.vessels[i].type)) + ", " +
                    std::to_string(d.vessels[i].points.size()) + " pts)";
                row(vr, label + "##v" + std::to_string(i));
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    const auto section = [&](const char* title, ElementKind kind, usize count,
                             const std::function<std::string(usize)>& label) {
        const std::string header = std::string(title) + " (" + std::to_string(count) + ")";
        if (!ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
        for (usize i = 0; i < count; ++i) {
            row(ElementRef{kind, static_cast<i32>(i), -1},
                label(i) + "##" + title + std::to_string(i));
        }
        ImGui::TreePop();
    };

    section("Obstacles", ElementKind::Obstacle, d.obstacles.size(), [&](usize i) {
        const std::string id = d.obstacles[i].id.empty() ? std::to_string(i) : d.obstacles[i].id;
        return id + "  (" + game::obstacle_shape_to_string(d.obstacles[i].shape) + ")";
    });
    section("Spawn points", ElementKind::SpawnPoint, d.spawn_points.size(),
            [&](usize i) { return d.spawn_points[i].id; });
    section("Objectives", ElementKind::Objective, d.objectives.size(),
            [&](usize i) { return d.objectives[i].id; });
    section("Placement zones", ElementKind::Zone, d.placement_zones.size(), [&](usize i) {
        const bool c = i < d.placement_zone_tags.size() && d.placement_zone_tags[i].concentrated;
        return "zone " + std::to_string(i) + (c ? "  (concentrated)" : "");
    });
    section("Squad paths", ElementKind::SquadPath, d.squad_paths.size(),
            [&](usize i) { return d.squad_paths[i].id; });

    (void)canvas;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

void EditorPanels::draw_inspector(app::EditorMode& editor) {
    game::LevelDoc& doc = editor.doc();

    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }

    const ElementRef r = doc.primary();
    if (!r.valid()) {
        ImGui::TextDisabled(doc.selection().empty() ? "Nothing selected"
                                                    : "Multiple selection");
        ImGui::End();
        return;
    }

    game::LevelDef& d = doc.mutable_def();
    bool changed = false;

    switch (r.kind) {
    case ElementKind::Vessel: {
        if (r.index >= static_cast<i32>(d.vessels.size())) break;
        game::Vessel& v = d.vessels[static_cast<usize>(r.index)];
        ImGui::SeparatorText("Vessel");

        std::string new_id;
        if (id_field("id", v.id, name_buf_, sizeof(name_buf_), new_id)) {
            doc.rename_element(r, new_id);
        }
        ImGui::LabelText("lane", "%s", game::vessel_lane(v).c_str());

        int type = static_cast<int>(v.type);
        if (ImGui::Combo("type", &type, kVesselTypeNames, IM_ARRAYSIZE(kVesselTypeNames))) {
            doc.set_vessel_type(r.index, static_cast<game::VesselType>(type));
            changed = true;
        }

        ImGui::SeparatorText("Control points");
        for (usize k = 0; k < v.points.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            const bool sel = doc.is_selected(
                ElementRef{ElementKind::Vessel, r.index, static_cast<i32>(k)});
            if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.84f, 0.25f, 1.0f));
            ImGui::Text("%zu", k);
            if (sel) ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            if (ImGui::DragFloat2("##p", &v.points[k].position.x, 0.25f)) changed = true;
            gesture_from_item(doc, "move point");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (ImGui::DragFloat("w", &v.points[k].width, 0.25f, 0.1f, 400.0f, "%.1f")) {
                changed = true;
            }
            gesture_from_item(doc, "set width");
            ImGui::PopID();
        }
        break;
    }

    case ElementKind::Obstacle: {
        if (r.index >= static_cast<i32>(d.obstacles.size())) break;
        game::ObstacleDef& o = d.obstacles[static_cast<usize>(r.index)];
        ImGui::SeparatorText("Obstacle");

        std::string new_id;
        if (id_field("id", o.id, name_buf_, sizeof(name_buf_), new_id)) {
            doc.rename_element(r, new_id);
        }

        int shape = static_cast<int>(o.shape);
        if (ImGui::Combo("shape", &shape, kObstacleShapeNames, IM_ARRAYSIZE(kObstacleShapeNames))) {
            doc.set_obstacle_shape(r.index, static_cast<game::ObstacleShape>(shape));
            changed = true;
        }

        // SHAPE-AWARE: only the fields this shape actually reads. ObstacleDef
        // is one flat struct shared by five shapes, so showing all of them
        // would invite editing a value that does nothing.
        switch (o.shape) {
        case game::ObstacleShape::Disc:
            if (ImGui::DragFloat2("pos", &o.position.x, 0.25f)) changed = true;
            gesture_from_item(doc, "move obstacle");
            if (ImGui::DragFloat("radius", &o.radius, 0.1f, 0.1f, 500.0f)) changed = true;
            gesture_from_item(doc, "resize obstacle");
            break;
        case game::ObstacleShape::Capsule:
            if (o.points.size() >= 2) {
                if (ImGui::DragFloat2("a", &o.points[0].position.x, 0.25f)) changed = true;
                gesture_from_item(doc, "move obstacle");
                if (ImGui::DragFloat2("b", &o.points[1].position.x, 0.25f)) changed = true;
                gesture_from_item(doc, "move obstacle");
            }
            if (ImGui::DragFloat("radius", &o.radius, 0.1f, 0.1f, 500.0f)) changed = true;
            gesture_from_item(doc, "resize obstacle");
            break;
        case game::ObstacleShape::Box: {
            if (ImGui::DragFloat2("pos", &o.position.x, 0.25f)) changed = true;
            gesture_from_item(doc, "move obstacle");
            if (ImGui::DragFloat2("half extents", &o.half_extents.x, 0.1f, 0.1f, 500.0f)) {
                changed = true;
            }
            gesture_from_item(doc, "resize obstacle");
            // DEGREES in the UI and in the file; radians in the struct. Showing
            // radians here would be the single easiest way to author a box that
            // looks right in the inspector and wrong in the world.
            f32 deg = o.rotation * (180.0f / math::kPi);
            if (ImGui::DragFloat("rotation", &deg, 1.0f, -360.0f, 360.0f, "%.1f deg")) {
                o.rotation = deg * (math::kPi / 180.0f);
                changed = true;
            }
            gesture_from_item(doc, "rotate obstacle");
            break;
        }
        case game::ObstacleShape::Polygon:
            if (ImGui::DragFloat("inflate", &o.radius, 0.1f, 0.0f, 200.0f)) changed = true;
            gesture_from_item(doc, "inflate obstacle");
            ImGui::SeparatorText("Vertices");
            for (usize k = 0; k < o.points.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                if (ImGui::DragFloat2("##v", &o.points[k].position.x, 0.25f)) changed = true;
                gesture_from_item(doc, "move vertex");
                ImGui::PopID();
            }
            break;
        case game::ObstacleShape::Ridge:
            ImGui::SeparatorText("Points");
            for (usize k = 0; k < o.points.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                ImGui::SetNextItemWidth(150);
                if (ImGui::DragFloat2("##p", &o.points[k].position.x, 0.25f)) changed = true;
                gesture_from_item(doc, "move point");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                // A ridge point with width <= 0 makes the level fail to LOAD
                // (parse_obstacle), so the minimum is enforced here rather than
                // discovered later.
                if (ImGui::DragFloat("w", &o.points[k].width, 0.1f, 0.1f, 200.0f, "%.1f")) {
                    changed = true;
                }
                gesture_from_item(doc, "set width");
                ImGui::PopID();
            }
            break;
        }
        break;
    }

    case ElementKind::SpawnPoint: {
        if (r.index >= static_cast<i32>(d.spawn_points.size())) break;
        game::SpawnPoint& p = d.spawn_points[static_cast<usize>(r.index)];
        ImGui::SeparatorText("Spawn point");
        std::string new_id;
        if (id_field("id", p.id, name_buf_, sizeof(name_buf_), new_id)) {
            // Goes through the doc so every wave entry naming it follows.
            doc.rename_element(r, new_id);
        }
        if (ImGui::DragFloat2("pos", &p.position.x, 0.25f)) changed = true;
        gesture_from_item(doc, "move spawn point");
        if (ImGui::DragFloat("radius", &p.radius, 0.1f, 0.1f, 200.0f)) changed = true;
        gesture_from_item(doc, "resize spawn point");

        const std::vector<std::string> lanes = game::lane_ids(d);
        std::string current = p.lane_id.empty() ? "(auto)" : p.lane_id;
        if (ImGui::BeginCombo("lane", current.c_str())) {
            if (ImGui::Selectable("(auto)", p.lane_id.empty())) {
                doc.begin_gesture("set lane");
                p.lane_id.clear();
                doc.end_gesture();
            }
            for (const std::string& l : lanes) {
                if (!ImGui::Selectable(l.c_str(), p.lane_id == l)) continue;
                doc.begin_gesture("set lane");
                p.lane_id = l;
                doc.end_gesture();
            }
            ImGui::EndCombo();
        }
        break;
    }

    case ElementKind::Objective: {
        if (r.index >= static_cast<i32>(d.objectives.size())) break;
        game::ObjectivePoint& o = d.objectives[static_cast<usize>(r.index)];
        ImGui::SeparatorText("Objective");
        std::string new_id;
        if (id_field("id", o.id, name_buf_, sizeof(name_buf_), new_id)) {
            doc.rename_element(r, new_id);
        }
        if (ImGui::DragFloat2("pos", &o.position.x, 0.25f)) changed = true;
        gesture_from_item(doc, "move objective");
        if (ImGui::DragFloat2("half extents", &o.half_extents.x, 0.1f, 0.1f, 500.0f)) {
            changed = true;
        }
        gesture_from_item(doc, "resize objective");
        // DEGREES here and in the file, radians in the struct -- same contract
        // as an obstacle box, for the same reason.
        f32 obj_deg = o.rotation * (180.0f / math::kPi);
        if (ImGui::DragFloat("rotation", &obj_deg, 1.0f, -360.0f, 360.0f, "%.1f deg")) {
            o.rotation = obj_deg * (math::kPi / 180.0f);
            changed = true;
        }
        gesture_from_item(doc, "rotate objective");
        if (ImGui::DragFloat("integrity", &o.integrity, 1.0f, 1.0f, 10000.0f)) changed = true;
        gesture_from_item(doc, "set integrity");
        break;
    }

    case ElementKind::Zone: {
        if (r.index >= static_cast<i32>(d.placement_zones.size())) break;
        Rect& z = d.placement_zones[static_cast<usize>(r.index)];
        ImGui::SeparatorText("Placement zone");
        if (ImGui::DragFloat2("min", &z.min.x, 0.25f)) changed = true;
        gesture_from_item(doc, "resize zone");
        if (ImGui::DragFloat2("max", &z.max.x, 0.25f)) changed = true;
        gesture_from_item(doc, "resize zone");
        if (r.index < static_cast<i32>(d.placement_zone_tags.size())) {
            game::PlacementZoneTag& t = d.placement_zone_tags[static_cast<usize>(r.index)];
            if (ImGui::Checkbox("concentrated", &t.concentrated)) {
                doc.begin_gesture("tag zone");
                doc.end_gesture();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Generous buildable margin at a bend or convergence\n(DESIGN.md 4.3/4.7)");
            }
            if (ImGui::DragFloat("priority", &t.priority, 0.05f, 0.0f, 10.0f)) changed = true;
            gesture_from_item(doc, "set zone priority");
        }
        break;
    }

    case ElementKind::SquadPath: {
        if (r.index >= static_cast<i32>(d.squad_paths.size())) break;
        game::SquadPathDef& sp = d.squad_paths[static_cast<usize>(r.index)];
        ImGui::SeparatorText("Squad path");
        std::string new_id;
        if (id_field("id", sp.id, name_buf_, sizeof(name_buf_), new_id)) {
            doc.rename_element(r, new_id);
        }
        if (ImGui::DragFloat("half width", &sp.half_width, 0.1f, 0.1f, 200.0f)) changed = true;
        gesture_from_item(doc, "set half width");

        const std::vector<std::string> lanes = game::lane_ids(d);
        if (ImGui::BeginCombo("lane", sp.lane_id.empty() ? "(nearest)" : sp.lane_id.c_str())) {
            if (ImGui::Selectable("(nearest)", sp.lane_id.empty())) {
                doc.begin_gesture("set lane");
                sp.lane_id.clear();
                doc.end_gesture();
            }
            for (const std::string& l : lanes) {
                if (!ImGui::Selectable(l.c_str(), sp.lane_id == l)) continue;
                doc.begin_gesture("set lane");
                sp.lane_id = l;
                doc.end_gesture();
            }
            ImGui::EndCombo();
        }
        ImGui::SeparatorText("Points");
        for (usize k = 0; k < sp.points.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            if (ImGui::DragFloat2("##p", &sp.points[k].x, 0.25f)) changed = true;
            gesture_from_item(doc, "move path point");
            ImGui::PopID();
        }
        break;
    }

    default:
        ImGui::TextDisabled("No inspector for this element");
        break;
    }

    if (changed) editor.invalidate();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Level settings
// ---------------------------------------------------------------------------

namespace {
void draw_settings(app::EditorMode& editor, bool* open, Vec2 cam_center,
                   f32 cam_height) {
    if (!*open) return;
    game::LevelDoc& doc = editor.doc();
    game::LevelDef& d = doc.mutable_def();

    if (!ImGui::Begin("Level settings", open)) {
        ImGui::End();
        return;
    }

    static char name[128] = {};
    static char region[128] = {};
    if (!ImGui::IsAnyItemActive()) {
        std::snprintf(name, sizeof(name), "%s", d.name.c_str());
        std::snprintf(region, sizeof(region), "%s", d.region.c_str());
    }
    if (ImGui::InputText("name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue)) {
        doc.begin_gesture("rename level");
        d.name = name;
        doc.end_gesture();
    }
    if (ImGui::InputText("region", region, sizeof(region), ImGuiInputTextFlags_EnterReturnsTrue)) {
        doc.begin_gesture("set region");
        d.region = region;
        doc.end_gesture();
    }

    ImGui::SeparatorText("World");
    bool changed = false;
    if (ImGui::DragFloat2("min", &d.world_bounds.min.x, 1.0f)) changed = true;
    gesture_from_item(doc, "resize world");
    if (ImGui::DragFloat2("max", &d.world_bounds.max.x, 1.0f)) changed = true;
    gesture_from_item(doc, "resize world");
    if (ImGui::DragFloat("cell size", &d.cell_size, 0.05f, 0.1f, 8.0f, "%.2f")) changed = true;
    gesture_from_item(doc, "set cell size");

    const Vec2 ext = d.world_bounds.size();
    const f64 cells = d.cell_size > 0.0f
                          ? static_cast<f64>(ext.x / d.cell_size) * (ext.y / d.cell_size)
                          : 0.0;
    // The bake cost is what makes a cell_size choice felt, so show it here
    // rather than making the author discover it as sluggishness.
    ImGui::Text("%.0f x %.0f units, %.0fk cells", ext.x, ext.y, cells / 1000.0);
    ImGui::Text("bake %.1f ms (rasterize %.1f, sdf %.1f, flow %.1f)", editor.baked().stats.total_ms,
                editor.baked().stats.rasterize_ms, editor.baked().stats.sdf_ms,
                editor.baked().stats.flow_ms);

    ImGui::SeparatorText("Ambient drift");
    if (ImGui::DragFloat2("drift", &d.ambient_drift.x, 0.01f, -5.0f, 5.0f)) changed = true;
    gesture_from_item(doc, "set drift");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Overrides sim.json global drift when non-zero");
    }

    // ---- schema 2 ----------------------------------------------------------
    // Every field below defaults to "use the global", so a level that touches
    // none of them stays a schema-1 file (LevelWriter::needs_schema_2).

    if (ImGui::CollapsingHeader("Presentation")) {
        static char disp[128] = {};
        static char desc[512] = {};
        static char auth[128] = {};
        static char tagbuf[256] = {};
        if (!ImGui::IsAnyItemActive()) {
            std::snprintf(disp, sizeof(disp), "%s", d.display_name.c_str());
            std::snprintf(desc, sizeof(desc), "%s", d.description.c_str());
            std::snprintf(auth, sizeof(auth), "%s", d.author.c_str());
            std::string joined;
            for (const std::string& t : d.tags) {
                if (!joined.empty()) joined += ", ";
                joined += t;
            }
            std::snprintf(tagbuf, sizeof(tagbuf), "%s", joined.c_str());
        }
        if (ImGui::InputText("display name", disp, sizeof(disp),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            doc.begin_gesture("set display name");
            d.display_name = disp;
            doc.end_gesture();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Shown in level select instead of the filename");
        }
        if (ImGui::InputTextMultiline("description", desc, sizeof(desc), ImVec2(0, 54),
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            doc.begin_gesture("set description");
            d.description = desc;
            doc.end_gesture();
        }
        if (ImGui::InputText("author", auth, sizeof(auth),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            doc.begin_gesture("set author");
            d.author = auth;
            doc.end_gesture();
        }
        if (ImGui::DragInt("difficulty", &d.difficulty, 0.1f, 0, 10)) changed = true;
        gesture_from_item(doc, "set difficulty");
        if (ImGui::InputText("tags", tagbuf, sizeof(tagbuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            doc.begin_gesture("set tags");
            d.tags.clear();
            std::string cur;
            for (const char* c = tagbuf; *c != 0; ++c) {
                if (*c == ',') {
                    if (!cur.empty()) d.tags.push_back(cur);
                    cur.clear();
                } else if (*c != ' ' || !cur.empty()) {
                    cur += *c;
                }
            }
            if (!cur.empty()) d.tags.push_back(cur);
            doc.end_gesture();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Comma separated");
    }

    if (ImGui::CollapsingHeader("Camera framing")) {
        ImGui::TextDisabled("0 / unset = frame the whole level");
        bool has = d.camera.has_center;
        if (ImGui::Checkbox("custom centre", &has)) {
            doc.begin_gesture("set camera");
            d.camera.has_center = has;
            if (has) d.camera.center = d.world_bounds.center();
            doc.end_gesture();
        }
        if (d.camera.has_center) {
            if (ImGui::DragFloat2("centre", &d.camera.center.x, 1.0f)) changed = true;
            gesture_from_item(doc, "set camera");
        }
        if (ImGui::DragFloat("view height", &d.camera.view_height, 1.0f, 0.0f, 8000.0f)) {
            changed = true;
        }
        gesture_from_item(doc, "set camera");
        if (ImGui::DragFloat("min zoom", &d.camera.min_view_height, 1.0f, 0.0f, 8000.0f)) {
            changed = true;
        }
        gesture_from_item(doc, "set camera");
        if (ImGui::DragFloat("max zoom", &d.camera.max_view_height, 1.0f, 0.0f, 8000.0f)) {
            changed = true;
        }
        gesture_from_item(doc, "set camera");
        if (ImGui::Button("Use current view")) {
            doc.begin_gesture("set camera");
            d.camera.has_center = true;
            d.camera.center = cam_center;
            d.camera.view_height = cam_height;
            doc.end_gesture();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Capture what the viewport shows now as the level default.\n"
                              "Fitted to the full game window, so it may show a little\n"
                              "more than the docked panels leave visible here.");
        }
    }

    if (ImGui::CollapsingHeader("Rules")) {
        int atp = static_cast<int>(d.economy.starting_atp);
        if (ImGui::DragInt("starting ATP", &atp, 1.0f, 0, 100000)) {
            d.economy.starting_atp = static_cast<u32>(math::max(atp, 0));
            changed = true;
        }
        gesture_from_item(doc, "set starting atp");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = use economy.json");
        if (ImGui::DragFloat("income x", &d.economy.income_multiplier, 0.01f, 0.05f, 10.0f)) {
            changed = true;
        }
        gesture_from_item(doc, "set income multiplier");
        if (ImGui::DragFloat("survive seconds", &d.win.survive_seconds, 1.0f, 0.0f, 3600.0f)) {
            changed = true;
        }
        gesture_from_item(doc, "set win condition");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = win by clearing the wave table");

        ImGui::SeparatorText("Allowed towers");
        ImGui::TextDisabled("none checked = all buildable");
        for (u32 i = 0; i < kTowerTypeCount; ++i) {
            const char* tname = game::tower_type_name(static_cast<TowerType>(i));
            const auto it = std::find(d.allowed_towers.begin(), d.allowed_towers.end(), tname);
            bool on = it != d.allowed_towers.end();
            if (ImGui::Checkbox(tname, &on)) {
                doc.begin_gesture("set allowed towers");
                if (on) d.allowed_towers.push_back(tname);
                else d.allowed_towers.erase(it);
                doc.end_gesture();
            }
            if ((i % 2) == 0 && i + 1 < kTowerTypeCount) ImGui::SameLine(160.0f);
        }
    }

    if (ImGui::CollapsingHeader("Notes")) {
        static char notes[1024] = {};
        if (!ImGui::IsAnyItemActive()) {
            std::snprintf(notes, sizeof(notes), "%s", d.editor.notes.c_str());
        }
        // Survives a Save only because the schema has an `editor` block: the
        // parser drops unknown keys, so before it there was nowhere for an
        // annotation to live.
        if (ImGui::InputTextMultiline("##notes", notes, sizeof(notes), ImVec2(-1, 80),
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            doc.begin_gesture("set notes");
            d.editor.notes = notes;
            d.editor.present = true;
            doc.end_gesture();
        }
    }

    if (changed) editor.invalidate();
    ImGui::End();
}
} // namespace

// ---------------------------------------------------------------------------
// Waves
// ---------------------------------------------------------------------------

EditorRequest EditorPanels::draw_waves(app::EditorMode& editor, const game::EnemyRoster* roster,
                                       u32 max_chaff) {
    EditorRequest req;
    game::LevelDoc& doc = editor.doc();
    game::LevelDef& d = doc.mutable_def();

    if (!ImGui::Begin("Waves")) {
        ImGui::End();
        return req;
    }

    selected_wave_ = math::clamp(selected_wave_, 0, static_cast<i32>(d.waves.size()) - 1);

    // Wave strip.
    for (usize i = 0; i < d.waves.size(); ++i) {
        if (i) ImGui::SameLine();
        const bool sel = static_cast<i32>(i) == selected_wave_;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button(std::to_string(i + 1).c_str(), ImVec2(28, 0))) {
            selected_wave_ = static_cast<i32>(i);
        }
        if (sel) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.waves[i].name.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        selected_wave_ = doc.add_wave();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a wave, continuing the ramp");
    ImGui::SameLine();
    if (ImGui::Button("Ramp...")) show_ramp_ = true;
    ImGui::SameLine();
    ImGui::Checkbox("table", &wave_table_view_);

    if (d.waves.empty()) {
        ImGui::TextDisabled("no waves");
        ImGui::End();
        return req;
    }

    game::WaveDef& w = d.waves[static_cast<usize>(selected_wave_)];
    ImGui::Separator();

    bool changed = false;
    static char wname[128] = {};
    if (!ImGui::IsAnyItemActive()) std::snprintf(wname, sizeof(wname), "%s", w.name.c_str());
    ImGui::SetNextItemWidth(180);
    if (ImGui::InputText("name", wname, sizeof(wname), ImGuiInputTextFlags_EnterReturnsTrue)) {
        doc.begin_gesture("rename wave");
        w.name = wname;
        doc.end_gesture();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragFloat("prep", &w.prep_time, 0.1f, 0.0f, 120.0f, "%.1fs")) changed = true;
    gesture_from_item(doc, "set prep time");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    int atp = static_cast<int>(w.atp_reward);
    if (ImGui::DragInt("atp", &atp, 1.0f, 0, 100000)) {
        w.atp_reward = static_cast<u32>(math::max(atp, 0));
        changed = true;
    }
    gesture_from_item(doc, "set atp reward");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    int mod = static_cast<int>(w.modifier);
    if (ImGui::Combo("mod", &mod, kModifierNames, IM_ARRAYSIZE(kModifierNames))) {
        doc.begin_gesture("set modifier");
        w.modifier = static_cast<game::WaveModifier>(mod);
        doc.end_gesture();
    }
    ImGui::SameLine();
    if (ImGui::Button("Dup")) selected_wave_ = doc.duplicate_wave(selected_wave_);
    ImGui::SameLine();
    if (ImGui::Button("Del")) {
        if (doc.erase(ElementRef{ElementKind::Wave, selected_wave_, -1})) {
            selected_wave_ = math::max(selected_wave_ - 1, 0);
        }
        ImGui::End();
        return req;
    }
    ImGui::SameLine();
    if (ImGui::Button("<") && selected_wave_ > 0) {
        if (doc.move_wave(selected_wave_, selected_wave_ - 1)) --selected_wave_;
    }
    ImGui::SameLine();
    if (ImGui::Button(">") && selected_wave_ + 1 < static_cast<i32>(d.waves.size())) {
        if (doc.move_wave(selected_wave_, selected_wave_ + 1)) ++selected_wave_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Play from here")) {
        req.action = EditorAction::Play;
        req.wave_index = selected_wave_;
    }

    // ---- Budget readout ---------------------------------------------------
    // You should know a wave will blow the chaff cap BEFORE you play it.
    const game::WaveBudget budget = game::wave_budget(w);
    const bool over = max_chaff > 0 && budget.total_agents > max_chaff;
    ImGui::TextColored(over ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.7f, 0.75f, 0.8f, 1.0f),
                       "%u agents over %.1fs, peak %.0f/s%s", budget.total_agents,
                       budget.span_seconds, budget.peak_per_second,
                       over ? "   OVER max_chaff!" : "");
    if (max_chaff > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(cap %u)", max_chaff);
    }

    ImGui::Separator();

    if (wave_table_view_) {
        if (ImGui::BeginTable("spawns", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("family");
            ImGui::TableSetupColumn("count");
            ImGui::TableSetupColumn("start");
            ImGui::TableSetupColumn("dur");
            ImGui::TableSetupColumn("elite");
            ImGui::TableSetupColumn("spawn point");
            ImGui::TableSetupColumn("squad");
            ImGui::TableSetupColumn("paths");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (usize k = 0; k < w.spawns.size(); ++k) {
                game::SpawnEntry& e = w.spawns[k];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(k));

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                int fam = static_cast<int>(e.family);
                std::vector<const char*> fam_names;
                for (u32 f = 0; f < kFamilyCount; ++f) {
                    fam_names.push_back(family_label(static_cast<PathogenFamily>(f), roster));
                }
                if (ImGui::Combo("##f", &fam, fam_names.data(),
                                 static_cast<int>(fam_names.size()))) {
                    doc.begin_gesture("set family");
                    e.family = static_cast<PathogenFamily>(fam);
                    doc.end_gesture();
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                int count = static_cast<int>(e.count);
                if (ImGui::DragInt("##c", &count, 1.0f, 0, 1000000)) {
                    e.count = static_cast<u32>(math::max(count, 0));
                    changed = true;
                }
                gesture_from_item(doc, "set count");

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat("##s", &e.start_time, 0.1f, 0.0f, 600.0f, "%.1f")) {
                    changed = true;
                }
                gesture_from_item(doc, "set start time");

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                // parse_spawn_entry REJECTS duration <= 0, so the minimum is
                // enforced here rather than at load.
                if (ImGui::DragFloat("##d", &e.duration, 0.1f, 0.1f, 600.0f, "%.1f")) {
                    e.duration = math::max(e.duration, 0.1f);
                    changed = true;
                }
                gesture_from_item(doc, "set duration");

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                int elite = static_cast<int>(e.elite_id);
                if (roster && !roster->elites().empty()) {
                    std::vector<const char*> names{"(none)"};
                    for (const game::EliteDef& ed : roster->elites()) names.push_back(ed.name);
                    if (ImGui::Combo("##e", &elite, names.data(),
                                     static_cast<int>(names.size()))) {
                        doc.begin_gesture("set elite");
                        e.elite_id = static_cast<u16>(elite);
                        doc.end_gesture();
                    }
                } else {
                    if (ImGui::DragInt("##e", &elite, 1.0f, 0, 65535)) {
                        e.elite_id = static_cast<u16>(math::max(elite, 0));
                        changed = true;
                    }
                    gesture_from_item(doc, "set elite");
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##sp",
                                      e.spawn_point_id.empty() ? "(cycle all)"
                                                               : e.spawn_point_id.c_str())) {
                    if (ImGui::Selectable("(cycle all)", e.spawn_point_id.empty())) {
                        doc.begin_gesture("set spawn point");
                        e.spawn_point_id.clear();
                        doc.end_gesture();
                    }
                    for (const game::SpawnPoint& sp : d.spawn_points) {
                        if (!ImGui::Selectable(sp.id.c_str(), e.spawn_point_id == sp.id)) continue;
                        doc.begin_gesture("set spawn point");
                        e.spawn_point_id = sp.id;
                        doc.end_gesture();
                    }
                    ImGui::EndCombo();
                }

                // schema 2: per-entry squad size. The single most expressive
                // knob in the table and, before v2, a global -- 900 as 15x60
                // and 900 as 6x150 are different arrivals.
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                int ss = static_cast<int>(e.squad_size);
                if (ImGui::DragInt("##ss", &ss, 1.0f, 0, 100000)) {
                    e.squad_size = static_cast<u32>(math::max(ss, 0));
                    changed = true;
                }
                gesture_from_item(doc, "set squad size");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("0 = sim.squads.target_squad_size");
                }

                // schema 2: restrict this entry to a subset of the lane paths.
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                {
                    std::string label;
                    for (const std::string& n : e.squad_paths) {
                        if (!label.empty()) label += ",";
                        label += n;
                    }
                    if (ImGui::BeginCombo("##sp2", label.empty() ? "(all)" : label.c_str())) {
                        if (ImGui::Selectable("(all)", e.squad_paths.empty())) {
                            doc.begin_gesture("set squad paths");
                            e.squad_paths.clear();
                            doc.end_gesture();
                        }
                        for (const game::SquadPathDef& sp : d.squad_paths) {
                            const auto it = std::find(e.squad_paths.begin(), e.squad_paths.end(),
                                                      sp.id);
                            bool on = it != e.squad_paths.end();
                            if (!ImGui::Checkbox(sp.id.c_str(), &on)) continue;
                            doc.begin_gesture("set squad paths");
                            if (on) e.squad_paths.push_back(sp.id);
                            else e.squad_paths.erase(it);
                            doc.end_gesture();
                        }
                        if (d.squad_paths.empty()) {
                            ImGui::TextDisabled("this level authors no squad paths");
                        }
                        ImGui::EndCombo();
                    }
                }

                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) {
                    doc.erase(ElementRef{ElementKind::Wave, selected_wave_, static_cast<i32>(k)});
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else {
        // ---- Timeline -----------------------------------------------------
        // Bars on a seconds axis: start = start_time, length = duration,
        // colour = family (from the roster, so the editor speaks the game's
        // colour language).
        const f32 span = math::max(budget.span_seconds, 1.0f) * 1.1f;
        const f32 avail = ImGui::GetContentRegionAvail().x - 8.0f;
        const f32 row_h = 22.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // Seconds axis.
        for (i32 s = 0; s <= static_cast<i32>(span); ++s) {
            const f32 x = origin.x + (static_cast<f32>(s) / span) * avail;
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + 12.0f),
                        IM_COL32(150, 155, 165, 120));
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%ds", s);
            dl->AddText(ImVec2(x + 2.0f, origin.y), IM_COL32(150, 155, 165, 200), buf);
        }
        ImGui::Dummy(ImVec2(avail, 16.0f));

        for (usize k = 0; k < w.spawns.size(); ++k) {
            game::SpawnEntry& e = w.spawns[k];
            ImGui::PushID(static_cast<int>(k));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const f32 x0 = p.x + (e.start_time / span) * avail;
            const f32 x1 = p.x + ((e.start_time + e.duration) / span) * avail;
            const ImVec4 tint = family_tint(e.family, roster);

            dl->AddRectFilled(ImVec2(x0, p.y + 2.0f), ImVec2(math::max(x1, x0 + 4.0f), p.y + row_h - 2.0f),
                              ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.55f)), 3.0f);
            dl->AddRect(ImVec2(x0, p.y + 2.0f), ImVec2(math::max(x1, x0 + 4.0f), p.y + row_h - 2.0f),
                        ImGui::GetColorU32(tint), 3.0f);

            char label[192];
            std::snprintf(label, sizeof(label), "%s %u", family_label(e.family, roster), e.count);
            dl->AddText(ImVec2(x0 + 5.0f, p.y + 4.0f), IM_COL32(20, 22, 28, 255), label);

            // Whole-row hit area so hovering anywhere on the lane shows the
            // squad derivation, which is the part of a wave the JSON hides.
            ImGui::InvisibleButton("row", ImVec2(avail, row_h));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s x%u  start %.1fs  over %.1fs", family_label(e.family, roster),
                            e.count, e.start_time, e.duration);
                // SQUADS ARE EMERGENT, NOT AUTHORED. A SpawnEntry is a count,
                // not a group: WaveDirector chops the stream into squads of
                // sim.squads.target_squad_size and hands each to the next path
                // on the lane. None of that is visible in the file.
                if (e.elite_id != 0) {
                    ImGui::TextDisabled("elites are never squadded; spawns from the point disc");
                } else {
                    const u32 size = e.squad_size != 0 ? e.squad_size : 60;
                    const u32 squads = (e.count + size - 1) / size;
                    ImGui::TextDisabled("~%u squads x %u%s", squads, size,
                                        e.squad_size != 0 ? " (entry override)"
                                                          : " (sim.squads.target_squad_size)");
                    if (!e.squad_paths.empty()) {
                        std::string names;
                        for (const std::string& n : e.squad_paths) {
                            if (!names.empty()) names += " -> ";
                            names += n;
                        }
                        ImGui::TextDisabled("paths %s", names.c_str());
                    } else {
                        ImGui::TextDisabled("round-robin across the lane paths");
                    }
                    ImGui::TextDisabled("arrival shape only; replicators grow past it in-lane");
                }
                ImGui::EndTooltip();
            }
            // Drag the bar to move it in time.
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                if (!doc.in_gesture()) doc.begin_gesture("move spawn entry");
                e.start_time = math::max(
                    e.start_time + ImGui::GetIO().MouseDelta.x / avail * span, 0.0f);
                changed = true;
            }
            if (ImGui::IsItemDeactivated() && doc.in_gesture()) doc.end_gesture();
            ImGui::PopID();
        }

        if (ImGui::Button("+ entry")) doc.add_spawn_entry(selected_wave_);
    }

    if (changed) { /* wave edits need no geometry rebake */ }
    ImGui::End();
    return req;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

EditorRequest EditorPanels::draw_validation(app::EditorMode& editor) {
    EditorRequest req;
    game::LevelDoc& doc = editor.doc();

    if (!ImGui::Begin("Validation")) {
        ImGui::End();
        return req;
    }

    if (editor.issues().empty()) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1.0f), "No issues.");
    }
    for (const game::Issue& i : editor.issues()) {
        const bool err = i.severity == game::Issue::Severity::Error;
        ImGui::PushStyleColor(ImGuiCol_Text, err ? ImVec4(1.0f, 0.42f, 0.4f, 1.0f)
                                                 : ImVec4(1.0f, 0.78f, 0.3f, 1.0f));
        const std::string label = std::string(err ? "[error] " : "[warn]  ") + i.message;
        // Clicking a row selects the offending element and flies to it -- the
        // whole reason Issue carries a ref and an anchor.
        if (ImGui::Selectable(label.c_str())) {
            if (i.ref.valid()) doc.select(i.ref);
            if (i.has_anchor) {
                req.action = EditorAction::FocusIssue;
                req.focus = Rect{i.anchor - Vec2{20.0f, 20.0f}, i.anchor + Vec2{20.0f, 20.0f}};
            }
        }
        ImGui::PopStyleColor();
    }

    ImGui::End();
    return req;
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void EditorPanels::draw_status_bar(app::EditorMode& editor, const EditorCanvas& canvas) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    if (ImGui::Begin("##status", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        if (canvas.cursor_valid()) {
            ImGui::Text("x %.2f  y %.2f", canvas.cursor_world().x, canvas.cursor_world().y);
        } else {
            ImGui::TextDisabled("x --  y --");
        }
        ImGui::SameLine();
        ImGui::Text("| grid %.2f%s | %s", canvas.grid_size(),
                    canvas.snap_enabled() ? "" : " (off)", tool_name(canvas.tool()));
        ImGui::SameLine();
        ImGui::Text("| %zu selected", editor.doc().selection().size());
        ImGui::SameLine();
        ImGui::Text("| bake %.1f ms", editor.baked().stats.total_ms);
        ImGui::SameLine();
        if (editor.error_count() > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.4f, 1.0f), "| %u error%s",
                               editor.error_count(), editor.error_count() == 1 ? "" : "s");
            ImGui::SameLine();
        }
        if (editor.warning_count() > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "| %u warning%s",
                               editor.warning_count(), editor.warning_count() == 1 ? "" : "s");
            ImGui::SameLine();
        }
        if (editor.doc().dirty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "| unsaved");
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

EditorRequest EditorPanels::draw_dialogs(app::EditorMode& editor,
                                         const std::vector<EditorLevelEntry>& levels) {
    EditorRequest req;
    game::LevelDoc& doc = editor.doc();

    if (show_new_) {
        ImGui::OpenPopup("New level");
        show_new_ = false;
    }
    if (ImGui::BeginPopupModal("New level", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char nbuf[128] = "new_level";
        static char rbuf[128] = "capillary";
        ImGui::InputText("name", nbuf, sizeof(nbuf));
        ImGui::InputText("region", rbuf, sizeof(rbuf));

        std::vector<const char*> names;
        for (game::LevelTemplate t : game::all_level_templates()) {
            names.push_back(game::level_template_to_string(t));
        }
        ImGui::Combo("shape", &new_template_, names.data(), static_cast<int>(names.size()));
        const Vec2 prev_size = new_params_.world_size;
        if (ImGui::DragFloat2("world size", &new_params_.world_size.x, 4.0f, 64.0f, 4000.0f,
                              "%.0f") &&
            new_lock_aspect_ && new_aspect_ > 0.0f) {
            // Whichever component the user dragged drives the other one.
            if (new_params_.world_size.x != prev_size.x) {
                new_params_.world_size.y =
                    math::clamp(new_params_.world_size.x / new_aspect_, 64.0f, 4000.0f);
            } else if (new_params_.world_size.y != prev_size.y) {
                new_params_.world_size.x =
                    math::clamp(new_params_.world_size.y * new_aspect_, 64.0f, 4000.0f);
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(new_lock_aspect_ ? "[L]" : "[ ]")) {
            new_lock_aspect_ = !new_lock_aspect_;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock aspect ratio while scaling");
        // Track the ratio while unlocked so engaging the lock keeps what is on screen.
        if (!new_lock_aspect_ && new_params_.world_size.y > 0.0f) {
            new_aspect_ = new_params_.world_size.x / new_params_.world_size.y;
        }
        ImGui::DragFloat("cell size", &new_params_.cell_size, 0.05f, 0.1f, 8.0f, "%.2f");
        ImGui::DragFloat("lane width", &new_params_.lane_width, 1.0f, 4.0f, 400.0f, "%.0f");
        int wc = static_cast<int>(new_params_.wave_count);
        if (ImGui::DragInt("waves", &wc, 1.0f, 1, 40)) {
            new_params_.wave_count = static_cast<u32>(math::max(wc, 1));
        }
        // The number that decides whether this level will be pleasant to edit.
        const f64 cells = new_params_.cell_size > 0.0f
                              ? static_cast<f64>(new_params_.world_size.x / new_params_.cell_size) *
                                    (new_params_.world_size.y / new_params_.cell_size)
                              : 0.0;
        ImGui::TextDisabled("%.0fk cells", cells / 1000.0);
        if (cells > 500000.0) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "large: bakes will be slow");
        }

        if (ImGui::Button("Create", ImVec2(120, 0))) {
            req.action = EditorAction::NewLevel;
            req.template_choice = game::all_level_templates()[static_cast<usize>(new_template_)];
            req.params = new_params_;
            req.params.name = nbuf;
            req.params.region = rbuf;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (show_open_) {
        ImGui::OpenPopup("Open level");
        show_open_ = false;
    }
    if (ImGui::BeginPopupModal("Open level", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (doc.dirty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f),
                               "The current level has unsaved changes.");
        }
        ImGui::BeginChild("list", ImVec2(420, 320), true);
        for (const EditorLevelEntry& e : levels) {
            if (!ImGui::Selectable(e.display_name.c_str())) continue;
            req.action = EditorAction::OpenLevel;
            req.path = e.path;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndChild();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (show_save_as_) {
        ImGui::OpenPopup("Save as");
        std::snprintf(path_buf_, sizeof(path_buf_), "%s",
                      doc.source_path().empty()
                          ? ("assets/levels/" + doc.def().name + ".json").c_str()
                          : doc.source_path().c_str());
        show_save_as_ = false;
    }
    if (ImGui::BeginPopupModal("Save as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("path", path_buf_, sizeof(path_buf_));
        ImGui::TextDisabled("Overwrites are backed up to <path>.bak");
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            req.action = EditorAction::SaveAs;
            req.path = path_buf_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (show_ramp_) {
        ImGui::OpenPopup("Wave ramp");
        show_ramp_ = false;
    }
    if (ImGui::BeginPopupModal("Wave ramp", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Materialises real waves you then hand-edit.");
        ImGui::TextDisabled("This REPLACES the current table.");
        ImGui::Separator();
        int wc = static_cast<int>(ramp_.wave_count);
        if (ImGui::DragInt("waves", &wc, 1.0f, 1, 40)) {
            ramp_.wave_count = static_cast<u32>(math::max(wc, 1));
        }
        for (u32 f = 0; f < kFamilyCount; ++f) {
            ImGui::PushID(static_cast<int>(f));
            ImGui::SeparatorText(f == 0 ? "virus" : "bacteria");
            int a = static_cast<int>(ramp_.first_count[f]);
            int b = static_cast<int>(ramp_.last_count[f]);
            int fw = static_cast<int>(ramp_.first_wave[f]);
            if (ImGui::DragInt("first count", &a, 1.0f, 0, 100000)) {
                ramp_.first_count[f] = static_cast<u32>(math::max(a, 0));
            }
            if (ImGui::DragInt("last count", &b, 1.0f, 0, 100000)) {
                ramp_.last_count[f] = static_cast<u32>(math::max(b, 0));
            }
            if (ImGui::DragInt("joins at wave", &fw, 1.0f, 0, 40)) {
                ramp_.first_wave[f] = static_cast<u32>(math::max(fw, 0));
            }
            ImGui::PopID();
        }
        ImGui::SeparatorText("Pacing");
        ImGui::DragFloat("first prep", &ramp_.first_prep, 0.1f, 0.0f, 120.0f, "%.1fs");
        ImGui::DragFloat("last prep", &ramp_.last_prep, 0.1f, 0.0f, 120.0f, "%.1fs");
        int a1 = static_cast<int>(ramp_.first_atp);
        int a2 = static_cast<int>(ramp_.last_atp);
        if (ImGui::DragInt("first atp", &a1, 1.0f, 0, 100000)) {
            ramp_.first_atp = static_cast<u32>(math::max(a1, 0));
        }
        if (ImGui::DragInt("last atp", &a2, 1.0f, 0, 100000)) {
            ramp_.last_atp = static_cast<u32>(math::max(a2, 0));
        }
        ImGui::DragFloat("duration", &ramp_.duration, 0.1f, 0.1f, 120.0f, "%.1fs");
        ImGui::DragFloat("curve", &ramp_.curve, 0.05f, 0.2f, 4.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("1 = linear, >1 back-loads difficulty, <1 front-loads it");
        }

        if (ImGui::Button("Generate", ImVec2(120, 0))) {
            doc.begin_gesture("generate wave ramp");
            doc.mutable_def().waves = game::make_wave_ramp(ramp_);
            doc.end_gesture();
            selected_wave_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!message_.empty()) {
        ImGui::OpenPopup("Editor");
        if (ImGui::BeginPopupModal("Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", message_.c_str());
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                message_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    return req;
}

// ---------------------------------------------------------------------------

void EditorPanels::build_dockspace() {
    // A dockspace that leaves a HOLE in the middle rather than filling it: the
    // viewport is the rendered world underneath, not an ImGui window, so the
    // central node stays empty and click-through.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 menu_h = ImGui::GetFrameHeight();
    const f32 status_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - status_h));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("##editor_dockspace", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(3);

    const ImGuiID dock_id = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode |
                         ImGuiDockNodeFlags_NoDockingOverCentralNode);

    // The world renders to the WHOLE framebuffer and the panels sit on top of
    // it, so "frame all" centred on the framebuffer puts the level's middle
    // behind the outliner. Publishing the central node's rect lets the canvas
    // centre on the part you can actually see.
    if (const ImGuiDockNode* node = ImGui::DockBuilderGetCentralNode(dock_id)) {
        central_ = Rect{Vec2{node->Pos.x, node->Pos.y},
                        Vec2{node->Pos.x + node->Size.x, node->Pos.y + node->Size.y}};
        has_central_ = node->Size.x > 1.0f && node->Size.y > 1.0f;
    }

    if (!layout_built_) {
        layout_built_ = true;
        // Only lay out if the user has no saved arrangement, so imgui.ini keeps
        // whatever they dragged into place last session.
        if (ImGui::DockBuilderGetNode(dock_id) == nullptr ||
            ImGui::DockBuilderGetNode(dock_id)->IsEmpty()) {
            ImGui::DockBuilderRemoveNode(dock_id);
            ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

            ImGuiID centre = dock_id;
            const ImGuiID left = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f,
                                                            nullptr, &centre);
            const ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.24f,
                                                             nullptr, &centre);
            const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.30f,
                                                              nullptr, &centre);
            // Enough height to actually show the toolbar row. A hairline split
            // leaves the panel a tab header with no content -- present, and
            // useless.
            const ImGuiID top = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Up, 0.10f,
                                                           nullptr, &centre);

            ImGui::DockBuilderDockWindow("Tools", top);
            ImGui::DockBuilderDockWindow("Outliner", left);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Level settings", right);
            ImGui::DockBuilderDockWindow("Waves", bottom);
            ImGui::DockBuilderDockWindow("Validation", bottom);
            ImGui::DockBuilderFinish(dock_id);
        }
    }
    ImGui::End();
    (void)menu_h;
}

EditorRequest EditorPanels::build(app::EditorMode& editor, EditorCanvas& canvas,
                                  render::Camera& camera,
                                  const std::vector<EditorLevelEntry>& levels, bool playing,
                                  const game::EnemyRoster* roster, u32 max_chaff) {
    EditorRequest req = draw_menu_bar(editor, canvas, playing);
    if (!playing) {
        build_dockspace();
        if (has_central_) canvas.set_viewport_rect(central_);
    } else {
        canvas.clear_viewport_rect();
    }

    if (!playing) {
        // Toolbar lives in its own small window so the dockspace can put it
        // wherever the user wants without it fighting the viewport.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
        if (ImGui::Begin("Tools", nullptr, ImGuiWindowFlags_NoScrollbar)) {
            canvas.build_toolbar(editor);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        draw_outliner(editor, canvas);
        draw_inspector(editor);
        // Not camera.center()/view_height(): those describe the full
        // framebuffer, most of whose edges sit behind the docked panels. The
        // canvas converts to the framing that reproduces the visible viewport.
        Vec2 cap_center{0.0f, 0.0f};
        f32 cap_height = 0.0f;
        canvas.capture_framing(camera, cap_center, cap_height);
        draw_settings(editor, &show_settings_, cap_center, cap_height);

        const EditorRequest wave_req = draw_waves(editor, roster, max_chaff);
        if (wave_req.action != EditorAction::None) req = wave_req;

        const EditorRequest val_req = draw_validation(editor);
        if (val_req.action != EditorAction::None) req = val_req;

        draw_status_bar(editor, canvas);

        const EditorRequest dlg = draw_dialogs(editor, levels);
        if (dlg.action != EditorAction::None) req = dlg;
    }

    if (want_focus_selection_) {
        want_focus_selection_ = false;
        Rect r;
        bool any = false;
        for (const ElementRef& e : editor.doc().selection()) {
            Rect b;
            if (!editor.doc().element_bounds(e, b)) continue;
            if (!any) { r = b; any = true; }
            else {
                r.min = Vec2{math::min(r.min.x, b.min.x), math::min(r.min.y, b.min.y)};
                r.max = Vec2{math::max(r.max.x, b.max.x), math::max(r.max.y, b.max.y)};
            }
        }
        if (any) canvas.focus_on(r);
    }

    (void)camera;
    return req;
}

} // namespace immune::ui
