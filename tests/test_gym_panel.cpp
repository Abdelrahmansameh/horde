// tests/test_gym_panel.cpp — ImGui smoke test for the gym level's control window.
//
// The panel's *behaviour* is game/gym's and is covered by test_gym_commands.cpp
// with no UI at all. What is left to check here is the one class of bug that
// file cannot see: an ImGui usage error. ImGui asserts on mismatched Begin/End,
// BeginChild/EndChild, BeginTabBar/EndTabBar, BeginTable/EndTable, and
// Push/PopStyleColor — and a window that only appears when a human opens it is
// exactly where such a bug ships unnoticed. So this stands up the same headless
// GL context --screenshot uses, opens the panel over the real gym level (so the
// tabs have portals, waves, elites, and towers to enumerate), and drives frames
// through every tab.
#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/gym/GymCommands.h"
#include "game/level/Level.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/FileIO.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Gl.h"
#include "render/Screenshot.h"
#include "sim/SimWorld.h"
#include "ui/GymPanel.h"
#include "ui/Hud.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace immune;

namespace {

/// A writable scratch directory outside the repo, matching
/// test_render_screenshots.cpp: these PNGs are manual-inspection artifacts.
std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

} // namespace

TEST_CASE("the gym panel builds and renders every tab", "[gym][ui][gl]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 1600, 900)) {
        WARN("no GL context available; skipping the gym panel UI smoke test");
        return;
    }

    platform::InputState input;
    input.bind_defaults();
    ui::Hud hud;
    REQUIRE(hud.init(window, input));

    // Over the real level where possible: the tabs enumerate portals, waves,
    // and elites, and an empty world would leave most of the draw code unrun.
    game::LevelLoader loader;
    game::LevelDef level;
    const std::string path = "assets/levels/gym.json";
    if (!platform::file_exists(path) || !loader.load_file(path, level).ok) {
        level = game::LevelLoader::default_test_level();
    }

    game::EnemyRoster roster;
    roster.load_defaults();
    sim::SimWorld world;
    sim::SimDesc desc;
    desc.world_bounds = level.world_bounds;
    roster.apply_to_tuning(desc.chaff_tuning);
    world.init(desc, nullptr);
    REQUIRE(loader.instantiate(level, world).ok);

    game::TowerSystem towers;
    towers.register_systems(world);
    game::WaveDirector waves;
    waves.set_waves(level.waves);
    waves.start(world);
    game::Economy economy;
    economy.configure(game::EconomyConfig{});
    game::ActiveAbilitySystem abilities;
    abilities.load_defaults();
    game::GymSpawnQueue spawns;
    // As app/ sets it up for the gym level: the objective is held so a leak
    // cannot end a session you are in the middle of setting up.
    game::GymToggles toggles;
    toggles.objective_invulnerable = (level.name == "gym");

    game::GymContext ctx;
    ctx.world = &world;
    ctx.towers = &towers;
    ctx.enemies = &roster;
    ctx.waves = &waves;
    ctx.economy = &economy;
    ctx.abilities = &abilities;
    ctx.spawns = &spawns;
    ctx.toggles = &toggles;

    ui::GymPanel panel;
    panel.set_level(level.name);
    if (level.name == "gym") {
        // The gym level is the one that brings its own control window up.
        CHECK(panel.visible());
    }
    panel.set_visible(true);

    // Commands run through the panel land in its log, which is where a
    // multi-line result gets split into rows.
    CHECK(panel.execute(ctx, "help"));
    CHECK(panel.execute(ctx, "portals"));
    CHECK_FALSE(panel.execute(ctx, "notacommand"));
    panel.print("banner line", true);

    // Every tab, two frames each: the first lays the widgets out, the second
    // runs them with state already in place. The second frame of each tab is
    // captured, so "does the panel actually look right" is answerable without a
    // human holding a key down -- the only way to review this window, since it
    // exists precisely to be looked at.
    static const char* const kTabNames[4] = {"horde", "defense", "waves", "world"};
    for (i32 tab = 0; tab < 4; ++tab) {
        for (int frame = 0; frame < 2; ++frame) {
            // Clear first: nothing else draws in this test, so without it each
            // capture would show every previous frame's widgets bleeding
            // through the panel's translucent background.
            glClearColor(0.02f, 0.03f, 0.04f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            panel.select_tab(tab);
            hud.begin_frame(input);
            panel.build(ctx);
            hud.render();
        }
        const std::string shot =
            scratch_path(std::string("gym_panel_") + kTabNames[tab] + ".png");
        CHECK(render::capture_framebuffer_png(shot, 1600, 900));
        std::printf("[gym panel] %s\n", shot.c_str());
    }

    panel.clear_log();
    hud.begin_frame(input);
    panel.build(ctx);
    hud.render();

    // Hidden is the default state elsewhere, and must cost nothing but an
    // early return.
    panel.set_visible(false);
    hud.begin_frame(input);
    panel.build(ctx);
    hud.render();
    CHECK_FALSE(panel.visible());

    hud.shutdown();
    window.destroy();
}

TEST_CASE("the gym panel only opens itself on the gym level", "[gym][ui]") {
    ui::GymPanel panel;
    CHECK_FALSE(panel.visible());

    panel.set_level("skin_1_breach");
    CHECK_FALSE(panel.visible());

    panel.set_level("gym");
    CHECK(panel.visible());

    // Opened by hand on some other level, it stays open across a level change.
    panel.set_visible(true);
    panel.set_level("capillary_test");
    CHECK(panel.visible());
}
