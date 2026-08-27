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
#include "game/meta/MetaProgression.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "sim/SimWorld.h"
#include "ui/GymPanel.h"
#include "ui/Hud.h"
#include "ui/Menu.h"
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
    void tick_sim();
    void render_frame();
    void enter_state(GameStateId id);
    bool load_level(const std::string& path);
    /// The half of load_level() from `SimDesc desc;` onward: builds a world
    /// from a LevelDef already in memory. Split out so the editor can Play the
    /// document it is editing without a file round-trip -- and so a level that
    /// has never been saved is still playable.
    bool load_level_def(const game::LevelDef& level, const std::string& source_path);
    /// Draws the editor and applies whatever it asked for. Runs inside Hud's
    /// ImGui frame, like every other window in ui/.
    void build_editor();
    /// Enters the editor on `path` (or a blank template when empty).
    void enter_editor(const std::string& path);
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
    /// Draws whichever front-end screen the current state calls for and
    /// applies the resulting MenuAction. Runs inside Hud's ImGui frame.
    void build_menus();
    /// Binds the live subsystems (and the app-level hooks the command layer
    /// cannot reach on its own: level loading, HUD overlays, the cursor) into
    /// one context for the gym panel. Rebuilt every frame rather than cached
    /// because `sim_` is re-inited on every level load, which would leave a
    /// cached context pointing at a world that no longer exists.
    game::GymContext make_gym_context();

    Options options_{};
    GameStateMachine state_;
    FixedClock clock_;
    Profiler profiler_;

    std::unique_ptr<JobSystem> jobs_;
    platform::Window window_;
    platform::InputState input_;
    render::Renderer renderer_;
    render::Camera camera_;
    audio::AudioEngine audio_;
    ui::Hud hud_;
    ui::Menu menu_;
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
    game::EnemyRoster enemies_;
    game::WaveDirector waves_;
    game::Economy economy_;
    game::MetaProgression meta_;
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
