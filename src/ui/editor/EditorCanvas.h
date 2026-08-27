// ui/editor/EditorCanvas.h — the editor viewport. NEW MODULE.
//
// RATIONALE
// Everything a mouse does in the world lives here: camera control, tool modes,
// hit testing, dragging, snapping, and the gizmo pass. It owns no document
// state -- LevelDoc does -- and no file state -- EditorMode does.
//
// MODIFIER KEYS COME FROM IMGUI, NOT INPUTSTATE
// platform::InputState exposes no Ctrl/Shift/Alt and no text input, and it is a
// frozen contract. Since every line of canvas code lives in ui/ (which already
// links ImGui), reading ImGui::GetIO().KeyCtrl/KeyShift/KeyAlt costs nothing
// and avoids touching that header at all. ImGui is also already receiving every
// SDL event via the raw-event sink Hud installs, so its modifier state is
// authoritative and frame-coherent.
//
// CAMERA CONTROL DID NOT EXIST
// App only ever framed the camera at level load. Middle-drag pan and
// zoom-to-cursor are built here over the existing render::Camera, whose
// screen_to_world is exact.
#pragma once

#include "core/Types.h"
#include "game/editor/LevelDoc.h"

#include <string>
#include <vector>

namespace immune::platform { class InputState; }
namespace immune::render { class Camera; }
namespace immune::app { class EditorMode; }

namespace immune::ui {

/// Modal tools, single-key, paint-app conventions.
enum class EditorTool : u8 {
    Select = 0,   ///< V
    Pen,          ///< P -- draw vessel control points
    Width,        ///< W -- drag/scroll a point's lumen width
    Obstacle,     ///< B -- with a shape sub-picker
    Spawn,        ///< S
    Objective,    ///< O
    Zone,         ///< Z
    SquadPath,    ///< Q
    Count,
};

const char* tool_name(EditorTool t);
/// Single-character label for the toolbar button.
const char* tool_key(EditorTool t);

/// What the viewport can overlay on top of the real baked tissue.
struct ViewToggles {
    bool grid = true;
    bool world_bounds = true;
    bool vessels = true;
    bool obstacles = true;
    bool spawns = true;
    bool objectives = true;
    bool zones = true;
    bool squad_paths = true;
    bool derived_squad_paths = false;   ///< Ghosted; what the lane WOULD get
    bool labels = true;
    bool validation = true;
    bool flow_arrows = false;
    bool unreachable = false;           ///< Highlight tissue no spawn point can reach
};

class EditorCanvas {
public:
    /// Handles camera, tools and dragging, then draws every gizmo.
    ///
    /// Must run inside an ImGui frame (Hud owns begin_frame/render), like every
    /// other window in ui/. `camera` is mutated -- the canvas owns navigation.
    void build(app::EditorMode& editor, render::Camera& camera, platform::InputState& input);

    /// Draws just the toolbar row (tool buttons, grid, snap). Split out so the
    /// panel layer can place it wherever the dockspace puts it.
    void build_toolbar(app::EditorMode& editor);

    EditorTool tool() const { return tool_; }
    void set_tool(EditorTool t);
    game::ObstacleShape obstacle_shape() const { return obstacle_shape_; }

    ViewToggles& views() { return views_; }
    const ViewToggles& views() const { return views_; }

    f32 grid_size() const { return grid_size_; }
    void set_grid_size(f32 g) { grid_size_ = g; }
    bool snap_enabled() const { return snap_; }
    void set_snap_enabled(bool s) { snap_ = s; }

    /// World position of the cursor last frame, for the status bar.
    Vec2 cursor_world() const { return cursor_world_; }
    bool cursor_valid() const { return cursor_valid_; }

    /// Screen-space rect the world is actually VISIBLE in -- the dockspace's
    /// empty central node. The world renders to the whole framebuffer with the
    /// panels on top, so without this every frame-to-fit would centre the level
    /// behind the outliner. Cleared during playtest, where the HUD floats and
    /// the whole framebuffer is the viewport again.
    void set_viewport_rect(const Rect& screen_rect) {
        viewport_rect_ = screen_rect;
        has_viewport_rect_ = true;
    }
    void clear_viewport_rect() { has_viewport_rect_ = false; }

    /// Eases the camera to frame `r` over the next few frames. Used by
    /// frame-selection, frame-all, and clicking a validation row.
    void focus_on(const Rect& r);
    /// True while a focus ease is still running.
    bool focusing() const { return focus_active_; }

    /// Cancels any in-progress drag or multi-click tool (Escape).
    void cancel();

private:
    // ---- Sub-steps of build() ---------------------------------------------
    /// Tool keys, undo/redo, delete, frame, and the Alt+N view toggles. Read
    /// from ImGui rather than InputState, which exposes no modifiers.
    void handle_shortcuts(app::EditorMode& editor);
    void update_camera(render::Camera& camera, platform::InputState& input, bool hovering);
    void update_focus(render::Camera& camera);
    Vec2 snap_point(const app::EditorMode& editor, Vec2 world, bool suspend) const;
    void handle_tool(app::EditorMode& editor, render::Camera& camera, platform::InputState& input);
    void draw_gizmos(app::EditorMode& editor, const render::Camera& camera);

    EditorTool tool_ = EditorTool::Select;
    game::ObstacleShape obstacle_shape_ = game::ObstacleShape::Disc;
    ViewToggles views_;

    f32 grid_size_ = 1.0f;
    bool snap_ = true;

    Vec2 cursor_world_{0.0f, 0.0f};
    bool cursor_valid_ = false;

    // ---- Drag state -------------------------------------------------------
    bool dragging_ = false;
    /// Set once a drag has actually moved, so a click that merely selects does
    /// not open (and commit) an empty gesture.
    bool drag_moved_ = false;
    Vec2 drag_start_world_{0.0f, 0.0f};
    Vec2 drag_last_world_{0.0f, 0.0f};
    game::ElementRef drag_ref_;
    /// Select-tool drag that turns the grabbed element instead of moving it:
    /// set when the press landed on a rotation grip. The grip is drawn only for
    /// the selected element, so this can never start from a stale selection.
    bool rotating_ = false;

    /// Marquee rectangle in world space while the Select tool is band-boxing.
    bool marquee_ = false;
    Vec2 marquee_start_{0.0f, 0.0f};

    /// Points accumulated by a multi-click tool (Pen, Polygon, SquadPath).
    std::vector<Vec2> pending_;
    /// Vessel the Pen is extending, or -1 when it will start a new one.
    i32 pen_vessel_ = -1;

    Rect viewport_rect_{};
    bool has_viewport_rect_ = false;
    /// Set by focus_on(), consumed by the next update_focus().
    bool pending_recentre_ = false;

    /// Camera ease target.
    bool focus_active_ = false;
    Vec2 focus_center_{0.0f, 0.0f};
    f32 focus_height_ = 0.0f;

    /// Advances the validation halo pulse. Render-clock only, never the sim.
    f32 pulse_ = 0.0f;
};

} // namespace immune::ui
