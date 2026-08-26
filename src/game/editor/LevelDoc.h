// game/editor/LevelDoc.h — the level editor's document. NEW MODULE.
//
// RATIONALE
// The editor's document is a `LevelDef` and nothing else. This class wraps one
// with the three things direct manipulation needs and a plain struct does not
// have: selection, undo, and operations that keep the document's internal
// references consistent.
//
// HEADLESS BY CONSTRUCTION
// No ImGui, no GL, no window -- the same split game/gym established, for the
// same reason: every operation here is a pure function of (document, argument),
// so the whole edit model unit-tests with no screen. ui/editor is a typist for
// this class exactly as ui/GymPanel is a typist for the gym language.
//
// UNDO IS WHOLE-DOCUMENT SNAPSHOTS
// A level is a few kilobytes of POD-ish vectors and edits happen at human
// speed, so a stack of complete LevelDefs costs well under a megabyte and is
// obviously correct. Command objects with hand-written inverses would be the
// "proper" answer and would be the wrong one: one class of bug per operation,
// paid to save memory nobody needs.
//
// ID HYGIENE IS THE DOCUMENT'S JOB
// Renaming a spawn point rewrites the `spawn_point_id` of every wave entry that
// named it; renaming a lane rewrites every vessel and squad path that referred
// to it. An editor that lets you break your own references is a text editor
// with extra steps, and the loader would reject the result.
#pragma once

#include "game/editor/LevelValidate.h"   // ElementKind, ElementRef
#include "game/level/Level.h"

#include <string>
#include <vector>

namespace immune::game {

/// What a hit test found under the cursor, and what the outliner selects.
/// `ElementRef` (LevelValidate.h) is shared with the validator on purpose: a
/// validation row and a click in the viewport must produce the same selection.
using Selection = std::vector<ElementRef>;

class LevelDoc {
public:
    // ---- Document ---------------------------------------------------------

    /// Replaces the document. Clears selection and the whole undo stack --
    /// undoing across a file open would be nonsense.
    void set_document(LevelDef def, std::string source_path = {});
    const LevelDef& def() const { return def_; }

    /// Mutable access for the inspector's direct field edits. Callers MUST
    /// bracket a run of these in begin_gesture/end_gesture; the ops below do it
    /// for themselves.
    LevelDef& mutable_def() { return def_; }

    const std::string& source_path() const { return source_path_; }
    void set_source_path(std::string p) { source_path_ = std::move(p); }

    /// True when the document differs from the last set_document/mark_saved.
    /// Compared with level_equal(), so float noise below the writer's own
    /// three-decimal quantum never lights the dirty marker.
    bool dirty() const;
    void mark_saved();

    // ---- Undo -------------------------------------------------------------

    /// Brackets a run of edits into ONE undo entry. `label` becomes the menu
    /// text ("Undo move point"). Nesting is counted, not re-entrant: the
    /// outermost pair is what commits.
    void begin_gesture(std::string label);
    void end_gesture();
    bool in_gesture() const { return gesture_depth_ > 0; }

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    /// "move point", or empty when the stack is empty. For the Edit menu.
    const std::string& undo_label() const;
    const std::string& redo_label() const;

    // ---- Selection --------------------------------------------------------

    const Selection& selection() const { return selection_; }
    void select(ElementRef r);          ///< Replaces the selection.
    void select_add(ElementRef r);      ///< Shift-click: toggles membership.
    void clear_selection() { selection_.clear(); }
    bool is_selected(ElementRef r) const;
    /// The single selected element, or an invalid ref when 0 or >1 are selected.
    ElementRef primary() const;

    // ---- Hit testing ------------------------------------------------------

    /// What is under `world`, with points beating bodies so a control point
    /// sitting on a filled shape stays grabbable. `pick_radius` is in world
    /// units; the canvas derives it from a fixed screen-pixel radius so handles
    /// stay the same apparent size at every zoom.
    ElementRef hit_test(Vec2 world, f32 pick_radius) const;
    /// Every element whose representative point falls inside `rect`. Marquee.
    Selection hit_test_rect(const Rect& rect) const;

    /// Where an element is, for gizmos, marquees and camera framing. False when
    /// the ref does not resolve (stale index after a delete).
    bool element_position(ElementRef r, Vec2& out) const;
    /// World-space bounds of an element, for frame-selection.
    bool element_bounds(ElementRef r, Rect& out) const;

    // ---- Operations -------------------------------------------------------
    // Each is one undo entry unless already inside a gesture. Each keeps ids
    // unique and cross-references intact.

    /// Moves whatever `r` names to `to` (a point moves; a body translates).
    void move_element(ElementRef r, Vec2 to);
    /// Translates every selected element by `delta`.
    void move_selection(Vec2 delta);

    i32 add_vessel(Vec2 a, Vec2 b, f32 width = 12.0f, VesselType type = VesselType::Artery);
    /// Appends a control point to vessel `vessel`, or inserts after `after`
    /// when `after >= 0`. Returns the new point index.
    i32 insert_vessel_point(i32 vessel, i32 after, Vec2 at);
    void set_vessel_point_width(i32 vessel, i32 point, f32 width);
    /// Retypes a vessel; the lane's hue follows.
    void set_vessel_type(i32 vessel, VesselType type);
    /// Renames a lane everywhere it is referred to.
    void rename_lane(const std::string& from, const std::string& to);

    i32 add_obstacle(ObstacleShape shape, Vec2 at, f32 size = 8.0f);
    /// Changes an obstacle's shape, migrating its geometry so the result is
    /// still a legal obstacle of the new shape rather than a half-filled one.
    void set_obstacle_shape(i32 obstacle, ObstacleShape shape);

    i32 add_spawn_point(Vec2 at);
    i32 add_objective(Vec2 at);
    i32 add_zone(const Rect& r);
    i32 add_squad_path(const std::vector<Vec2>& points, const std::string& lane_id = {});

    /// Renames an element, rewriting every reference to the old id. No-op if
    /// `to` is empty or already taken.
    void rename_element(ElementRef r, const std::string& to);

    /// Removes `r`. Refuses (returning false) to remove the last vessel, spawn
    /// point, objective or wave -- the loader rejects a level without them, so
    /// allowing it would only produce a document that cannot be saved.
    bool erase(ElementRef r);
    bool erase_selection();
    /// Duplicates `r` beside itself and selects the copy. Alt-drag's payload.
    ElementRef duplicate(ElementRef r);

    // ---- Waves ------------------------------------------------------------

    i32 add_wave();
    i32 duplicate_wave(i32 index);
    bool move_wave(i32 index, i32 to);
    i32 add_spawn_entry(i32 wave);

    // ---- Ids --------------------------------------------------------------

    /// `prefix`, `prefix_2`, `prefix_3`, ... -- the first that no element of
    /// the same kind is already using.
    std::string unique_id(ElementKind kind, const std::string& prefix) const;

private:
    void push_undo(std::string label);

    LevelDef def_{};
    LevelDef saved_{};
    std::string source_path_;
    Selection selection_;

    struct Snapshot {
        LevelDef def;
        std::string label;
    };
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    i32 gesture_depth_ = 0;
    /// The document as it stood when the outermost begin_gesture ran.
    LevelDef gesture_base_{};
    std::string gesture_label_;

    /// Cap on the undo stack. A level is kilobytes; 128 whole-document
    /// snapshots is comfortably under a megabyte and far more history than a
    /// human uses in a session.
    static constexpr usize kMaxUndo = 128;
};

/// Lane a vessel belongs to (its `lane_id`, or its `id` when unset). Mirrors
/// parse_vessel's rule; shared so the doc, the validator and the UI agree.
std::string vessel_lane(const Vessel& v);

/// Every distinct lane id in document order.
std::vector<std::string> lane_ids(const LevelDef& def);

} // namespace immune::game
