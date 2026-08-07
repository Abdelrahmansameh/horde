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
#include "app/GameState.h"
#include "audio/Audio.h"
#include "core/Clock.h"
#include "core/JobSystem.h"
#include "core/Profiler.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/level/Level.h"
#include "game/meta/MetaProgression.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "sim/SimWorld.h"
#include "ui/Hud.h"
#include "ui/Menu.h"
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
    /// Scans the levels directory once and fills `levels_`. Cheap enough to do
    /// at startup (a dozen small JSON parses) and keeps the level list a pure
    /// function of what is on disk rather than a hardcoded table.
    void discover_levels();
    /// Draws whichever front-end screen the current state calls for and
    /// applies the resulting MenuAction. Runs inside Hud's ImGui frame.
    void build_menus();

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
    std::vector<ui::LevelEntry> levels_;
    std::string current_level_path_;  ///< Path to the currently loaded level, for restart.
    /// True once a level has actually been loaded into sim_. Guards the render
    /// path: the menu states run before any world exists, so the game passes
    /// must not be submitted against an uninitialised SimWorld.
    bool level_loaded_ = false;

    sim::SimWorld sim_;
    game::TowerSystem towers_;
    game::LaneOwnershipMap lane_map_;
    game::EnemyRoster enemies_;
    game::WaveDirector waves_;
    game::Economy economy_;
    game::MetaProgression meta_;
    game::ActiveAbilitySystem abilities_;

    /// Cosmetic only, and deliberately outside sim_: own RNG, render clock.
    vfx::ParticleSystem particles_;
    /// Reused across frames so build_instances() never reallocates.
    std::vector<vfx::ParticleInstance> particle_scratch_;

    std::vector<ui::Intent> intents_;
    bool running_ = false;
};

} // namespace immune::app
