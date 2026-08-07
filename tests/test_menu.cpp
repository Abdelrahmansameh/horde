// Front-end screen coverage: main menu and level select (ui/Menu.h).
//
// These drive the REAL ImGui stack against a headless GL context, the same way
// tests/test_render_gl.cpp verifies the render passes, and then read the
// framebuffer back. A menu that compiles but draws nothing is exactly the
// failure that "it builds and doesn't crash" misses, so the pixel checks here
// are the point, not decoration.
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "ui/Hud.h"
#include "ui/Menu.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

using namespace immune;

namespace {

struct HeadlessUi {
    platform::Window window;
    platform::InputState input;
    ui::Hud hud;
    render::Renderer renderer;
    bool ok = false;

    HeadlessUi(i32 w = 1280, i32 h = 720) {
        if (!platform::create_headless_gl(window, w, h)) return;
        render::RendererDesc rd;
        rd.framebuffer_width = w;
        rd.framebuffer_height = h;
        if (!renderer.init(rd)) return;
        input.bind_defaults();
        ok = hud.init(window, input);
    }

    ~HeadlessUi() {
        if (ok) hud.shutdown();
        renderer.shutdown();
        window.destroy();
    }

    /// Fraction of pixels that are not the cleared background. A drawn panel
    /// covers a real share of the screen; a no-op draw covers none of it.
    f32 draw_one_frame(const std::function<void()>& build) {
        render::Camera camera;
        camera.set_viewport(window.width(), window.height());
        renderer.begin_frame(camera, 0.0f);
        hud.begin_frame(input);
        build();
        hud.render();

        std::vector<u8> pixels;
        i32 pw = 0, ph = 0;
        if (!renderer.read_pixels(pixels, pw, ph)) return -1.0f;

        usize lit = 0;
        for (usize i = 0; i + 3 < pixels.size(); i += 4) {
            if (pixels[i] > 24 || pixels[i + 1] > 24 || pixels[i + 2] > 24) ++lit;
        }
        const usize total = pixels.size() / 4;
        return total == 0 ? -1.0f : static_cast<f32>(lit) / static_cast<f32>(total);
    }
};

std::vector<ui::LevelEntry> sample_levels() {
    return {
        {"assets/levels/skin_1_breach.json", "skin_1_breach", "skin", 1},
        {"assets/levels/capillary_2_forking_vessels.json", "capillary_2", "capillary", 2},
        {"assets/levels/organ_chamber_1_lymph_node_core.json", "organ_core", "organ_chamber", 5},
    };
}

} // namespace

TEST_CASE("the main menu actually draws pixels", "[ui][menu]") {
    HeadlessUi ui;
    if (!ui.ok) {
        WARN("headless GL/ImGui unavailable; skipping");
        return;
    }
    ui::Menu menu;
    const f32 coverage = ui.draw_one_frame([&] { menu.build_main_menu(1280, 720); });
    REQUIRE(coverage > 0.0f);
    // A centred 420x300 panel over 1280x720 is ~13.7% of the screen; require a
    // clear fraction of that so an empty or collapsed window fails.
    REQUIRE(coverage > 0.02f);
}

TEST_CASE("level select draws its entries", "[ui][menu]") {
    HeadlessUi ui;
    if (!ui.ok) {
        WARN("headless GL/ImGui unavailable; skipping");
        return;
    }
    ui::Menu menu;
    const auto levels = sample_levels();
    const f32 coverage = ui.draw_one_frame([&] { menu.build_level_select(levels, 1280, 720); });
    REQUIRE(coverage > 0.02f);
}

TEST_CASE("level select survives an empty level list", "[ui][menu]") {
    // The "no levels found" path is what a broken asset root looks like, so it
    // must render an explanation rather than crash or draw an empty box.
    HeadlessUi ui;
    if (!ui.ok) {
        WARN("headless GL/ImGui unavailable; skipping");
        return;
    }
    ui::Menu menu;
    const std::vector<ui::LevelEntry> none;
    const f32 coverage = ui.draw_one_frame([&] { menu.build_level_select(none, 1280, 720); });
    REQUIRE(coverage > 0.02f);
}

TEST_CASE("menus report no action when nothing is clicked", "[ui][menu]") {
    // The default every frame must be None -- anything else would fire a
    // transition on its own and make the front end unusable.
    HeadlessUi ui;
    if (!ui.ok) {
        WARN("headless GL/ImGui unavailable; skipping");
        return;
    }
    ui::Menu menu;
    ui::MenuResult main_result, select_result;
    const auto levels = sample_levels();
    ui.draw_one_frame([&] {
        main_result = menu.build_main_menu(1280, 720);
        select_result = menu.build_level_select(levels, 1280, 720);
    });
    REQUIRE(main_result.action == ui::MenuAction::None);
    REQUIRE(select_result.action == ui::MenuAction::None);
}
