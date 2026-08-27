// ui/editor/EditorPanels.h — the editor's docked panels. NEW MODULE.
//
// Outliner, inspector, wave timeline, validation, level settings, and the menu
// bar. Every one of them is a typist for game/editor's document operations --
// nothing here reaches past LevelDoc into the LevelDef except through the ops,
// so the UI cannot become a second way to edit a level that skips id hygiene
// and undo.
//
// The panels report what the user asked for through EditorRequest rather than
// acting on it, because file lifecycle and play/stop are app/ concerns and
// ui/ must not depend on app/'s state machine (the same rule ui/Menu.h follows).
#pragma once

#include "core/Types.h"
#include "game/editor/LevelTemplates.h"
#include "game/editor/LevelValidate.h"

#include <string>

namespace immune::app { class EditorMode; }
namespace immune::render { class Camera; }
namespace immune::game { class EnemyRoster; }

namespace immune::ui {

class EditorCanvas;

/// Something the panels want app/ to do that they cannot do themselves.
enum class EditorAction : u8 {
    None = 0,
    NewLevel,       ///< `template_choice` + `params` describe it
    OpenLevel,      ///< `path`
    Save,
    SaveAs,         ///< `path`
    SaveAnyway,     ///< Save with validation errors, deliberately
    Revert,
    Play,           ///< `wave_index` is where to start
    Stop,
    ExitToMenu,
    FocusIssue,     ///< `focus` names where to look
};

struct EditorRequest {
    EditorAction action = EditorAction::None;
    std::string path;
    game::LevelTemplate template_choice = game::LevelTemplate::StraightLane;
    game::TemplateParams params;
    i32 wave_index = 0;
    Rect focus{};
};

/// One entry in the editor's Open dialog. Filled by app/ from discover_levels().
struct EditorLevelEntry {
    std::string path;
    std::string display_name;
};

class EditorPanels {
public:
    /// Draws every panel and returns whatever the user asked for. Must run
    /// inside an ImGui frame.
    ///
    /// `playing` switches the menu bar between Play and Stop; `roster` fills
    /// the family/elite pickers from live data rather than a hardcoded list.
    EditorRequest build(app::EditorMode& editor, EditorCanvas& canvas, render::Camera& camera,
                        const std::vector<EditorLevelEntry>& levels, bool playing,
                        const game::EnemyRoster* roster, u32 max_chaff);

    /// Opens the New Level dialog on the next frame.
    void open_new_dialog() { show_new_ = true; }
    void open_open_dialog() { show_open_ = true; }
    void open_save_as_dialog() { show_save_as_ = true; }

    /// Shown in a modal when a save or open failed.
    void set_message(std::string m) { message_ = std::move(m); }

private:
    /// Docks the panels around an EMPTY central node, so the rendered world
    /// shows through the middle and stays clickable.
    void build_dockspace();
    EditorRequest draw_menu_bar(app::EditorMode& editor, EditorCanvas& canvas, bool playing);
    void draw_outliner(app::EditorMode& editor, EditorCanvas& canvas);
    void draw_inspector(app::EditorMode& editor);
    EditorRequest draw_waves(app::EditorMode& editor, const game::EnemyRoster* roster,
                             u32 max_chaff);
    EditorRequest draw_validation(app::EditorMode& editor);
    void draw_status_bar(app::EditorMode& editor, const EditorCanvas& canvas);
    EditorRequest draw_dialogs(app::EditorMode& editor,
                               const std::vector<EditorLevelEntry>& levels);

    bool show_new_ = false;
    bool show_open_ = false;
    bool show_save_as_ = false;
    bool show_ramp_ = false;
    bool show_settings_ = true;
    bool wave_table_view_ = false;

    i32 selected_wave_ = 0;
    char name_buf_[128] = {};
    char path_buf_[512] = {};
    std::string message_;

    // New-level dialog state.
    i32 new_template_ = 0;
    game::TemplateParams new_params_;
    /// When set, dragging one world-size component scales the other to keep
    /// `new_aspect_` (width / height, captured when the lock was engaged).
    bool new_lock_aspect_ = false;
    f32 new_aspect_ = 1.0f;

    // Wave ramp dialog state.
    game::WaveRampParams ramp_;

    /// Set when the outliner wants the canvas to frame the selection, so the
    /// request survives to the end of the frame.
    bool want_focus_selection_ = false;
    /// The default dock layout is built once; after that imgui.ini owns it.
    bool layout_built_ = false;
    /// Screen-space rect of the dockspace's empty central node -- the part of
    /// the framebuffer the panels are NOT covering.
    Rect central_{};
    bool has_central_ = false;
};

} // namespace immune::ui
