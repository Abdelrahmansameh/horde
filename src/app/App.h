// app/App.h — interactive game loop. FROZEN CONTRACT.
//
// THE LOOP (plan decision 1)
//   clock.begin_frame();
//   input.poll();
//   while (clock.consume_tick()) { sim.tick(); }     // fixed 60 Hz
//   renderer.draw(clock.alpha());                    // variable rate
//
// The sim's tick count for a given wall-clock span is identical regardless of
// frame rate, so a 144 Hz machine and a 30 Hz machine play the same game.
// Nothing inside the sim step can observe the frame rate.
#pragma once

#include "app/Cli.h"
#include "app/EditorMode.h"
#include "app/GameState.h"
#include "audio/Audio.h"
#include "core/Clock.h"
#include "core/JobSystem.h"
#include "core/Profiler.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/autoplay/AutoPlayer.h"
#include "game/config/GameConfig.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/gym/GymCommands.h"
#include "game/level/Level.h"
#include "game/level/RenderSdf.h"
#include "game/meta/MetaProgression.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "gui/core/Gui.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "sim/SimWorld.h"
#include "ui/GymPanel.h"
#include "ui/DevUi.h"
#include "ui/Menu.h"
#include "ui/front/FrontEnd.h"
#include "ui/hud/HudScreen.h"
#include "ui/editor/EditorCanvas.h"
#include "ui/editor/EditorPanels.h"
#include "vfx/Particles.h"

#include <memory>
#include <vector>

namespace immune::app {

class App {
public:
    /// Creates window, GL context, renderer, audio, and the sim. False on any
    /// hard failure (message logged).
    bool init(const Options& options);
    void shutdown();

    /// Runs until quit. Returns the process exit code.
    int run();

private:
    void handle_input();
    void apply_intents(const std::vector<ui::Intent>& intents);
    /// Viewport, camera projection and pointer into the gui, then its frame
    /// (layout, events, animation). Merges its pointer capture into
    /// InputState so a click on the HUD never reaches the world.
    void run_gui_frame();
    /// Builds the HUD model from the live level and syncs the HUD to it.
    void sync_hud(Vec2 world_cursor);
    void tick_sim();
    void render_frame();
    void enter_state(GameStateId id);
    bool load_level(const std::string& path);
    /// The half of load_level() from `SimDesc desc;` onward: builds a world
    /// from a LevelDef already in memory. Split out so the editor can Play the
    /// document it is editing without a file round-trip -- and so a level that
    /// has never been saved is still playable.
    bool load_level_def(const game::LevelDef& level, const std::string& source_path);
    /// Draws the editor and applies whatever it asked for. Runs inside DevUi's
    /// ImGui frame, like every other window in ui/.
    void build_editor();
    /// Enters the editor on `path` (or a blank template when empty).
    void enter_editor(const std::string& path);
    /// Wheel zoom-to-cursor and middle-drag pan while a level is live. The
    /// level's own framing (load_level) is the zoom-out limit, so the widest
    /// view is exactly what the level author set; the zoom-in limit is a
    /// fixed multiple of that.
    void update_level_camera();
    /// Points the camera at the whole document with margin, and widens the
    /// clamp rect so the world rectangle's own edges stay reachable.
    void frame_editor_camera();
    /// Instantiates the editor's document and switches to InLevel, remembering
    /// that Stop should come back here rather than to the main menu. False if
    /// the document could not be instantiated, in which case nothing changed
    /// and the caller is still in whatever state it was in.
    bool editor_play(i32 from_wave);
    void editor_stop();
    /// Reads assets/config (or --config) into config_ and binds it for the gym
    /// console. False if any file is missing or malformed.
    bool load_tuning_config();
    /// Applies the loaded level's own schema-2 rules (economy overrides,
    /// allowed tower types) on top of the global tuning. Must run AFTER
    /// apply_tuning_config(), which overwrites wholesale.
    void apply_level_rules(const game::LevelDef& level);
    /// Pushes config_ into every system that can accept it at any time.
    /// Called at init, on every level load, and after a hot reload.
    void apply_tuning_config();
    /// Content-compares the config files and re-applies them if they changed.
    /// Off when --config pinned the directory.
    void poll_config_reload(f32 dt);
    /// Scans the levels directory once and fills `levels_`. Cheap enough to do
    /// at startup (a dozen small JSON parses) and keeps the level list a pure
    /// function of what is on disk rather than a hardcoded table.
    void discover_levels();
    /// Which out-of-match screen the current state shows (None in a live
    /// match and in the editor).
    ui::FrontScreen front_screen() const;
    /// The front end's plain data: the campaign with its unlocks, the run
    /// that just ended.
    ui::FrontModel front_model();
    /// Before gui.frame(): shows the screen the state calls for.
    void sync_front();
    /// After gui.frame(): takes what the player clicked and applies it.
    void finish_front();
    /// Applies a front-end click to the state machine.
    void apply_menu_result(const ui::MenuResult& r);
    /// Binds the live subsystems (and the app-level hooks the command layer
    /// cannot reach on its own: level loading, HUD overlays, the cursor) into
    /// one context for the gym panel. Rebuilt every frame rather than cached
    /// because `sim_` is re-inited on every level load, which would leave a
    /// cached context pointing at a world that no longer exists.
    game::GymContext make_gym_context();

    // ---- Strengthen Immunity (game/meta) ------------------------------------
    /// A run with no meta-progression: the editor's playtests, the gym level,
    /// and anything under --sandbox. Every tower and ability unlocked, the
    /// tree's bonuses off, nothing paid out, nothing saved.
    bool sandbox_run() const;
    /// What the run so far is worth, as MetaProgression's input.
    game::RunResult current_run_result(bool won) const;
    /// Pays out the run that just ended (a clear or a wipe -- an abort pays
    /// nothing), saves, and fills last_run_ for the results screen.
    void finish_run(bool won);
    /// Writes meta_ to save_path_, unless loading it failed (see save_ok_).
    void save_meta();
    /// Loads save_path_ into meta_ at startup.
    void load_meta();

    Options options_{};
    GameStateMachine state_;
    FixedClock clock_;
    Profiler profiler_;

    std::unique_ptr<JobSystem> jobs_;
    platform::Window window_;
    platform::InputState input_;
    render::Renderer renderer_;
    render::Camera camera_;
    /// View height the level was framed at on load: the widest the player can
    /// zoom out to. Zero until a level is loaded (zoom disabled).
    f32 level_view_height_ = 0.0f;
    audio::AudioEngine audio_;
    /// ImGui: the developer tools (gym panel, level editor).
    ui::DevUi dev_ui_;
    /// The player-facing UI framework (src/gui, docs/UI_FRAMEWORK.md) and its
    /// in-match HUD. Declared after dev_ui_ so the HUD screen is destroyed first.
    gui::Gui gui_;
    std::unique_ptr<ui::HudScreen> hud_screen_;
    ui::HudModel hud_model_;
    /// Main menu, Strengthen Immunity, level select, pause and results
    /// (src/ui/front).
    std::unique_ptr<ui::FrontEnd> front_;
    /// The gym level's control window (game/gym). Opens itself on that level
    /// and is toggleable with ` or F2 anywhere; costs nothing while hidden.
    ui::GymPanel gym_panel_;
    /// The level editor (docs/LEVEL_EDITOR.md). Present in every interactive
    /// build; costs one LevelDoc and three empty grids while unused.
    EditorMode editor_;
    ui::EditorCanvas editor_canvas_;
    ui::EditorPanels editor_panels_;
    /// True while a playtest launched FROM the editor is running, so Escape and
    /// Stop return to editing rather than to the main menu.
    bool editor_playtest_ = false;
    /// Which wave that playtest should start from. Lets you test wave 7 without
    /// replaying waves 1-6.
    i32 editor_play_from_wave_ = 0;
    std::vector<ui::LevelEntry> levels_;
    /// levels_' campaign_NN_*.json files in order, with their thumbnails.
    std::vector<ui::CampaignLevel> campaign_;
    std::string current_level_path_;  ///< Path to the currently loaded level, for restart.
    /// The most recent level-instantiation failure, retained so editor Play can
    /// show the exact reason in its warning popup instead of only logging it.
    std::string last_level_load_error_;
    /// True once a level has actually been loaded into sim_. Guards the render
    /// path: the menu states run before any world exists, so the game passes
    /// must not be submitted against an uninitialised SimWorld.
    bool level_loaded_ = false;

    sim::SimWorld sim_;
    /// Tuning loaded from assets/config. Owned here because App is the only
    /// thing that outlives a level: a hot reload has to survive load_level().
    config::ConfigStore config_store_;
    game::GameConfig config_;
    /// Seconds until the next config poll. Polling every frame would stat
    /// seven files at 60 Hz for a file a human edits every few minutes.
    f32 config_poll_timer_ = 0.0f;

    game::TowerSystem towers_;
    game::LaneOwnershipMap lane_map_;
    /// The smooth distance field the tissue pass draws the level from, as
    /// instantiate() baked it (the same bake that wrote the sim's walkability,
    /// so the picture and the mask agree). The renderer caches its upload on
    /// the array's pointer.
    game::RenderSdf render_sdf_;
    game::EnemyRoster enemies_;
    game::WaveDirector waves_;
    game::Economy economy_;
    /// The player's persistent progress: both currencies and the tree.
    game::MetaProgression meta_;
    /// Where meta_ lives on disk (--save, else the per-user data dir).
    std::string save_path_;
    /// False when the save file exists but would not load (corrupt, or from
    /// a newer build). The game then plays on a fresh campaign but never
    /// writes, so the player's real save is not overwritten by accident.
    bool save_ok_ = true;
    /// What the last finished run paid, for the results screens.
    ui::RunSummary last_run_{};
    /// config_ with the purchased tree folded in: what every system is
    /// actually configured from this run. config_ itself stays the file's
    /// values, because the gym registry is bound to it.
    game::GameConfig run_config_;
    game::ActiveAbilitySystem abilities_;
    /// Remainder of any gym `spawn` too large for a single burst. Ticked with
    /// the sim so it streams in like a wave; empty and free in a normal run.
    game::GymSpawnQueue gym_spawns_;
    /// Per-tick gym settings, chiefly the gym level's default "the objective
    /// cannot be destroyed while you are experimenting". Applied after each
    /// tick and before the win/loss check.
    game::GymToggles gym_toggles_;

    /// The balance bot (game/autoplay), off unless the `autoplay` gym command
    /// turns it on. Present in the interactive build for one reason: a bot
    /// whose play cannot be watched cannot be trusted to produce balance
    /// numbers. Pair with `time 8` to watch a level at eight times speed.
    game::AutoPlayer bot_;
    bool autoplay_enabled_ = false;
    /// The level geometry the bot planned against, kept so a re-plan after a
    /// level load has something to plan from.
    game::LevelDef current_level_def_;

    /// Cosmetic only, and deliberately outside sim_: own RNG, render clock.
    vfx::ParticleSystem particles_;
    /// Reused across frames so build_instances() never reallocates.
    std::vector<vfx::ParticleInstance> particle_scratch_;

    std::vector<ui::Intent> intents_;
    bool running_ = false;
};

} // namespace immune::app
