// Explicit visual-evidence capture of the actual menu Play transition.
// Run [virus_swipe_capture] with IMMUNE_SWIPE_CAPTURE_DIR set, then encode
// frame_%04d.png at 60 fps. No desktop input or visible window is used.
#include "app/UiBridge.h"
#include "config/ConfigStore.h"
#include "game/config/GameConfig.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/enemies/EnemyRoster.h"
#include "gui/core/Gui.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Screenshot.h"
#include "ui/front/FrontEnd.h"
#include "ui/front/VirusSwipe.h"

#include <glad/glad.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace immune;

namespace {

struct SwipeCaptureUi {
    gui::Gui gui;
    gui::GlBackend coverage_renderer;
    GLuint coverage_fbo = 0, coverage_texture = 0, coverage_stencil = 0;
    std::unique_ptr<ui::FrontEnd> front;

    SwipeCaptureUi() {
        REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"),
                                          platform::asset_path("ui/icons"),
                                          platform::asset_path("config/ui_theme.json")}));
        REQUIRE(gui.init_renderer());
        REQUIRE(coverage_renderer.init());
        // The default window framebuffer may have no alpha channel. Use an
        // explicit RGBA target so uncovered pixels cannot read as opaque.
        glCreateTextures(GL_TEXTURE_2D, 1, &coverage_texture);
        glTextureStorage2D(coverage_texture, 1, GL_RGBA8, 1600, 900);
        glCreateRenderbuffers(1, &coverage_stencil);
        glNamedRenderbufferStorage(coverage_stencil, GL_DEPTH24_STENCIL8, 1600, 900);
        glCreateFramebuffers(1, &coverage_fbo);
        glNamedFramebufferTexture(coverage_fbo, GL_COLOR_ATTACHMENT0, coverage_texture, 0);
        glNamedFramebufferRenderbuffer(coverage_fbo, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, coverage_stencil);
        REQUIRE(glCheckNamedFramebufferStatus(coverage_fbo, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        gui.set_viewport(Vec2{1600, 900});
        front = std::make_unique<ui::FrontEnd>(gui);
    }
    ~SwipeCaptureUi() {
        front.reset();
        coverage_renderer.shutdown();
        glDeleteFramebuffers(1, &coverage_fbo);
        glDeleteRenderbuffers(1, &coverage_stencil);
        glDeleteTextures(1, &coverage_texture);
        gui.shutdown();
    }

    void render() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 1600, 900);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        gui.render(1600, 900);
        glFinish();
    }

    void verify_coverage(const std::string& capture_dir, int frame) {
        gui::DrawList swarm;
        swarm.reset(gui.viewport(), 1.0f / gui.scale(), gui.time());
        front->virus_swipe()->draw_self(swarm);
        GLint previous_draw = 0, previous_read = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
        glBindFramebuffer(GL_FRAMEBUFFER, coverage_fbo);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        coverage_renderer.render(swarm, nullptr, nullptr, 1600, 900, gui.scale());
        std::vector<u8> mask;
        REQUIRE(render::read_framebuffer_rgba(mask, 1600, 900));
        usize exposed = 0;
        for (usize pixel = 0; pixel < mask.size() / 4; ++pixel) {
            if (mask[pixel * 4 + 3] < 254) ++exposed;
        }
        std::fprintf(stderr, "[virus coverage] frame %d: %zu exposed / 1440000 pixels\n", frame, exposed);
        char file[64];
        std::snprintf(file, sizeof(file), "/handoff_%04d.png", frame);
        REQUIRE(render::write_png_rgba(capture_dir + file, mask.data(), 1600, 900));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
        // Tiny gaps are intentional; retain at least 99.9% opaque coverage
        // at the screen swap without forcing a heavier swarm.
        CHECK(exposed <= 1440);
    }
};

} // namespace

TEST_CASE("capture main-menu Play virus swipe at 60 fps",
          "[.][ui][virus_swipe_capture][gl]") {
    const char* capture_env = std::getenv("IMMUNE_SWIPE_CAPTURE_DIR");
    if (capture_env == nullptr || capture_env[0] == '\0') {
        WARN("Set IMMUNE_SWIPE_CAPTURE_DIR to opt into the 240-frame virus swipe capture");
        return;
    }
    const std::string capture_dir(capture_env);
    REQUIRE(platform::ensure_directory(capture_dir));

    platform::Window window;
    const bool context_ready = platform::create_headless_gl(window, 1600, 900);
    INFO(window.error());
    REQUIRE(context_ready);
    // Use the same loaded enemy material tuning as the interactive game.
    config::ConfigStore store;
    game::GameConfig tuning;
    std::string error;
    REQUIRE(game::load_game_config(store, platform::asset_path("config"), tuning, error));
    game::EnemyRoster roster;
    game::apply_enemy_config(roster, tuning.enemies);
    SwipeCaptureUi capture;
    constexpr f32 dt = 1.0f / 60.0f;
    gui::PointerInput pointer;
    pointer.pos = Vec2{-100.0f, -100.0f};
    ui::FrontModel menu_model;
    ui::FrontModel tree_model;
    tree_model.tree = app::make_screenshot_tree_model(app::ScreenshotTree::New);

    // Settle the opening menu and fill its real icon atlas before recording.
    for (int warmup = 0; warmup < 45; ++warmup) {
        capture.front->show(ui::FrontScreen::MainMenu, menu_model);
        capture.gui.frame(pointer, dt);
        REQUIRE(capture.front->take_result().action == ui::MenuAction::None);
        capture.render();
    }

    ui::FrontScreen screen = ui::FrontScreen::MainMenu;
    int navigations = 0;
    std::vector<u8> pixels;
    for (int frame = 0; frame < 240; ++frame) {
        CAPTURE(frame);
        const ui::FrontModel& model = screen == ui::FrontScreen::Tree ? tree_model : menu_model;
        capture.front->show(screen, model);
        if (frame == 60) REQUIRE(capture.gui.click("menu/play"));
        capture.gui.frame(pointer, dt);
        const ui::MenuResult result = capture.front->take_result();
        if (result.action != ui::MenuAction::None) {
            REQUIRE(result.action == ui::MenuAction::OpenImmunityTree);
            ++navigations;
            screen = ui::FrontScreen::Tree; // Installed by show() on the next frame.
        }
        capture.render();
        REQUIRE(render::read_framebuffer_rgba(pixels, 1600, 900));
        char filename[32];
        std::snprintf(filename, sizeof(filename), "frame_%04d.png", frame);
        REQUIRE(render::write_png_rgba(capture_dir + "/" + filename,
                                        pixels.data(), 1600, 900));
        // Measure the crossover itself. The next frame is already revealing
        // the destination as the viruses continue forward without a pause.
        if (result.action != ui::MenuAction::None) {
            capture.verify_coverage(capture_dir, frame);
        }
    }

    CHECK(navigations == 1);
    CHECK(capture.front->current() == ui::FrontScreen::Tree);
    REQUIRE(capture.front->virus_swipe() != nullptr);
    CHECK_FALSE(capture.front->virus_swipe()->active());
}
