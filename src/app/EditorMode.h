// app/EditorMode.h — the level editor's session. NEW MODULE.
//
// RATIONALE
// game/editor owns the document; ui/editor owns the ImGui. This owns the things
// that are neither: the file lifecycle, the live bake, and the transition into
// and out of playtest.
//
// THE BAKE IS PRIVATE, AND THAT IS THE WHOLE TRICK
// LevelLoader::instantiate() needs a SimWorld, and SimWorld::init() is
// destructive -- it wipes agents, towers, economy and wave progress. Re-running
// it on every edit would be both slow and wrong. So the editor owns its own
// TissueMask/DistanceField/FlowField and fills them with
// LevelLoader::bake_geometry(), which is the SAME code instantiate() runs. The
// renderer's existing submit_tissue() takes exactly those three objects, so the
// editor draws the real bake with ZERO renderer changes and without a live
// world existing at all.
//
// CADENCE
// Gizmos move against the LevelDef alone at 60fps during a drag; the bake runs
// on gesture END. That is affordable because the rasterizer's sample rate is
// width-aware (sim/flowfield/TissueRaster.h): the shipped levels bake in
// 16-91 ms, where they used to take up to 617 ms.
#pragma once

#include "game/editor/LevelDoc.h"
#include "game/editor/LevelTemplates.h"
#include "game/editor/LevelValidate.h"
#include "game/level/Level.h"
#include "game/level/RenderSdf.h"
#include "sim/flowfield/FlowField.h"

#include <string>
#include <vector>

namespace immune::game { struct GeometryBakeDesc; }

namespace immune::app {

/// The editor's own geometry, parallel to what a SimWorld would hold.
struct EditorBake {
    sim::TissueMask mask;
    sim::DistanceField sdf;
    sim::FlowField flow;
    game::LaneOwnershipMap lanes;
    /// The smooth field the viewport draws the vessels from, from the same
    /// bake_geometry() call as the mask.
    game::RenderSdf render_sdf;
    game::GeometryBakeStats stats;
    bool valid = false;
    std::string error;
};

/// Non-UI editor session state. Everything here is drivable headlessly, which
/// is what lets tests/test_editor_panel drive a whole editing session on a
/// hidden GL context.
class EditorMode {
public:
    // ---- Lifecycle --------------------------------------------------------

    /// Opens `path`. False (with `last_error()` set) if it will not parse; the
    /// document is left untouched so a failed open cannot lose your work.
    bool open(const std::string& path);
    /// Starts a new document from a template. Never fails.
    void create(game::LevelTemplate t, const game::TemplateParams& params);
    /// Re-reads the document's source file, discarding edits.
    bool revert();

    /// Writes canonical JSON to `path` (or the document's source path when
    /// empty), backing up any existing file to `<path>.bak` first.
    ///
    /// Refuses when the document has validation ERRORS unless `force`. Warnings
    /// never block. False sets `last_error()`.
    bool save(const std::string& path = {}, bool force = false);

    /// True when saving would be refused without `force`.
    bool blocked_from_saving() const;

    // ---- Document ---------------------------------------------------------

    game::LevelDoc& doc() { return doc_; }
    const game::LevelDoc& doc() const { return doc_; }

    /// Re-runs the geometry bake, the lane map and the validator against the
    /// current document. Call on gesture end, not per frame.
    void rebake();
    /// Marks the bake stale without doing it. The next frame's begin_frame()
    /// picks it up, which coalesces several edits in one frame into one bake.
    void invalidate() { bake_dirty_ = true; }
    /// Runs a pending rebake if one is due. Call once per frame.
    void tick();

    const EditorBake& baked() const { return bake_; }
    const std::vector<game::Issue>& issues() const { return issues_; }
    u32 error_count() const { return error_count_; }
    u32 warning_count() const { return warning_count_; }

    const std::string& last_error() const { return last_error_; }
    /// Human-readable status for the title bar / status strip.
    std::string title() const;

    // ---- Bake configuration -----------------------------------------------

    /// The wall-cost/smoothing values the bake should use, so the editor's
    /// preview matches what the game will actually build at load. App fills
    /// this from assets/config; the defaults (all zero) are the geometry-only
    /// bake, which is all the validator needs.
    void set_bake_desc(const game::GeometryBakeDesc& d);

private:
    void refresh_validation();

    game::LevelDoc doc_;
    EditorBake bake_;
    std::vector<game::Issue> issues_;
    u32 error_count_ = 0;
    u32 warning_count_ = 0;
    bool bake_dirty_ = true;
    std::string last_error_;

    /// Stored by value rather than by reference to GameConfig: the editor
    /// outlives a level load, and a config hot-reload must not leave this
    /// pointing at freed tuning.
    f32 flow_smoothing_radius_ = 0.0f;
    f32 flow_wall_cost_ = 0.0f;
    f32 flow_wall_falloff_ = 0.0f;
    f32 flow_wall_exponent_ = 1.0f;
};

} // namespace immune::app
