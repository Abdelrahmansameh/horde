// tests/test_editor_panel.cpp — ImGui smoke test for the level editor.
//
// The editor's BEHAVIOUR is game/editor's and is covered headlessly by
// test_level_edit / test_level_validate / test_level_templates. What is left to
// check here is the class of bug those cannot see: an ImGui usage error. ImGui
// asserts on mismatched Begin/End, BeginChild/EndChild, BeginTable/EndTable,
// BeginCombo/EndCombo and Push/PopStyleColor, and a panel that only appears
// when a human opens it is exactly where such a bug ships unnoticed.
//
// So this stands up the same headless GL context --screenshot uses, opens the
// editor over a real level, and drives frames through every tool, every
// inspector shape, and both wave views -- rendering the REAL baked tissue
// underneath, because that path (EditorMode's private bake -> submit_tissue) is
// itself the thing most worth proving works without a SimWorld.
//
// The captures are manual-inspection artifacts: this window exists to be looked
// at, and a PNG is the only way to review it without a human holding a key.
#include "app/EditorMode.h"
#include "game/editor/LevelDoc.h"
#include "game/editor/LevelTemplates.h"
#include "game/enemies/EnemyRoster.h"
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "platform/FileIO.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Gl.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "ui/Hud.h"
#include "ui/editor/EditorCanvas.h"
#include "ui/editor/EditorPanels.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace immune;

namespace {

std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

/// Everything a frame needs, so each test can drive frames without repeating
/// the setup.
struct Harness {
    platform::Window window;
    platform::InputState input;
    ui::Hud hud;
    render::Renderer renderer;
    render::Camera camera;
    app::EditorMode editor;
    ui::EditorCanvas canvas;
    ui::EditorPanels panels;
    game::EnemyRoster roster;
    std::vector<ui::EditorLevelEntry> levels;
    bool ok = false;

    Harness(i32 w = 1600, i32 h = 900) {
        if (!platform::create_headless_gl(window, w, h)) return;
        input.bind_defaults();
        if (!hud.init(window, input)) return;
        render::RendererDesc rd;
        rd.framebuffer_width = w;
        rd.framebuffer_height = h;
        if (!renderer.init(rd)) return;
        camera.set_viewport(w, h);
        roster.load_defaults();
        levels.push_back(ui::EditorLevelEntry{"assets/levels/plaque_field.json", "plaque_field"});
        ok = true;
    }

    ~Harness() {
        if (ok) renderer.shutdown();
        hud.shutdown();
    }

    /// Exactly what App::frame_editor_camera() does, including going through
    /// the canvas so the framing compensates for the docked panels -- otherwise
    /// the captures would not show what a real session shows.
    void frame_camera() {
        const Rect wb = editor.doc().def().world_bounds;
        const Vec2 pad = wb.size() * 0.25f;
        camera.set_bounds(Rect{wb.min - pad, wb.max + pad});
        camera.set_center(wb.center());
        camera.set_view_height(wb.size().y * 1.15f);
        canvas.focus_on(wb);
    }

    /// One complete editor frame, exactly as App::render_frame drives it.
    void draw(bool playing = false) {
        editor.tick();
        glClearColor(0.02f, 0.03f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        renderer.begin_frame(camera, 0.0f);
        const app::EditorBake& b = editor.baked();
        if (b.valid && b.mask.width() > 0) {
            render::TissueDecor decor;
            decor.flow = &b.flow;
            if (!b.lanes.owner.empty() && !b.lanes.lane_types.empty()) {
                decor.lane_owner = b.lanes.owner.data();
                decor.lane_type = reinterpret_cast<const u8*>(b.lanes.lane_types.data());
                decor.lane_count = static_cast<u32>(b.lanes.lane_types.size());
                decor.lane_width = b.lanes.width;
                decor.lane_height = b.lanes.height;
            }
            renderer.submit_tissue(b.mask, b.sdf, 0.0f, &decor);
        }
        renderer.end_frame();

        hud.begin_frame(input);
        canvas.build(editor, camera, input);
        panels.build(editor, canvas, camera, levels, playing, &roster, 20000);
        hud.render();
    }

    void capture(const std::string& name) {
        const std::string p = scratch_path(name);
        CHECK(render::capture_framebuffer_png(p, window.width(), window.height()));
        std::printf("[editor] %s\n", p.c_str());
    }
};

} // namespace

TEST_CASE("the editor builds every tool and panel without an ImGui error",
          "[editor][ui][gl]") {
    Harness h;
    if (!h.ok) {
        WARN("no GL context available; skipping the editor UI smoke test");
        return;
    }

    REQUIRE(h.editor.open("assets/levels/plaque_field.json"));
    h.frame_camera();

    // A real level, so the outliner has lanes to group, the inspector has all
    // five obstacle shapes to switch between, and the wave panel has a table.
    REQUIRE(h.editor.doc().def().obstacles.size() == 5);
    REQUIRE(h.editor.error_count() == 0);

    // Two frames per tool: the first lays widgets out, the second runs them
    // with state already in place.
    for (i32 t = 0; t < static_cast<i32>(ui::EditorTool::Count); ++t) {
        const ui::EditorTool tool = static_cast<ui::EditorTool>(t);
        h.canvas.set_tool(tool);
        // Enough frames for the focus ease to settle; a capture mid-ease shows
        // a camera that is still moving.
        for (i32 f = 0; f < 30; ++f) h.draw();
        h.capture(std::string("editor_tool_") + ui::tool_key(tool) + ".png");
    }
}

TEST_CASE("the editor inspector draws every element kind", "[editor][ui][gl]") {
    Harness h;
    if (!h.ok) {
        WARN("no GL context available; skipping");
        return;
    }
    REQUIRE(h.editor.open("assets/levels/plaque_field.json"));
    h.frame_camera();
    h.canvas.set_tool(ui::EditorTool::Select);

    // Each kind's inspector branch reads different fields off the flat
    // ObstacleDef, so every one of them has to be exercised.
    const game::ElementRef refs[] = {
        {game::ElementKind::Vessel, 0, -1},   {game::ElementKind::Vessel, 0, 1},
        {game::ElementKind::Obstacle, 0, -1}, {game::ElementKind::Obstacle, 1, -1},
        {game::ElementKind::Obstacle, 2, -1}, {game::ElementKind::Obstacle, 3, -1},
        {game::ElementKind::Obstacle, 4, -1}, {game::ElementKind::SpawnPoint, 0, -1},
        {game::ElementKind::Objective, 0, -1}, {game::ElementKind::Zone, 0, -1},
    };
    for (const game::ElementRef& r : refs) {
        h.editor.doc().select(r);
        for (i32 f = 0; f < 2; ++f) h.draw();
    }
    h.capture("editor_inspector.png");

    // And a squad path, which the shipped level does not author.
    h.editor.doc().add_squad_path({Vec2{40.0f, 130.0f}, Vec2{440.0f, 130.0f}}, "main");
    h.editor.doc().select({game::ElementKind::SquadPath, 0, -1});
    for (i32 f = 0; f < 2; ++f) h.draw();
}

TEST_CASE("the editor wave panel draws both views", "[editor][ui][gl]") {
    Harness h;
    if (!h.ok) {
        WARN("no GL context available; skipping");
        return;
    }
    REQUIRE(h.editor.open("assets/levels/plaque_field.json"));
    h.frame_camera();
    REQUIRE(h.editor.doc().def().waves.size() >= 3);

    // Timeline view (the default).
    for (i32 f = 0; f < 3; ++f) h.draw();
    h.capture("editor_waves_timeline.png");
}

TEST_CASE("the editor draws every starter template", "[editor][ui][gl]") {
    // The New Level flow is the first thing a new author touches, so every
    // template has to survive being drawn as well as being validated.
    Harness h;
    if (!h.ok) {
        WARN("no GL context available; skipping");
        return;
    }
    for (game::LevelTemplate t : game::all_level_templates()) {
        h.editor.create(t, game::TemplateParams{});
        h.frame_camera();
        INFO("template " << game::level_template_to_string(t));
        CHECK(h.editor.error_count() == 0);
        for (i32 f = 0; f < 30; ++f) h.draw();
        h.capture(std::string("editor_template_") +
                  std::to_string(static_cast<int>(t)) + ".png");
    }
}

TEST_CASE("the editor survives a full edit session", "[editor][ui][gl]") {
    // Drives the operations a real session performs, interleaved with frames,
    // so a crash from a stale selection index after a delete shows up here.
    Harness h;
    if (!h.ok) {
        WARN("no GL context available; skipping");
        return;
    }
    h.editor.create(game::LevelTemplate::Switchback, game::TemplateParams{});
    h.frame_camera();
    game::LevelDoc& doc = h.editor.doc();

    h.draw();
    doc.add_obstacle(game::ObstacleShape::Ridge, Vec2{200.0f, 100.0f}, 10.0f);
    h.editor.invalidate();
    h.draw();

    doc.select({game::ElementKind::Obstacle, 0, -1});
    h.draw();
    doc.set_obstacle_shape(0, game::ObstacleShape::Polygon);
    h.editor.invalidate();
    h.draw();

    // Delete the selected element and keep drawing: the selection now points at
    // an index that no longer exists.
    REQUIRE(doc.erase_selection());
    h.editor.invalidate();
    h.draw();

    doc.add_wave();
    doc.add_spawn_entry(0);
    h.draw();

    REQUIRE(doc.undo());
    h.editor.invalidate();
    h.draw();
    REQUIRE(doc.redo());
    h.editor.invalidate();
    h.draw();

    h.capture("editor_session.png");
    CHECK(h.editor.error_count() == 0);
}

TEST_CASE("the editor saves and reloads through the real file path",
          "[editor][ui][gl][io]") {
    Harness h;
    if (!h.ok) {
        WARN("no GL context available; skipping");
        return;
    }
    h.editor.create(game::LevelTemplate::Fork, game::TemplateParams{});
    h.frame_camera();
    h.draw();

    const std::string out = scratch_path("editor_roundtrip.json");
    REQUIRE(h.editor.save(out));
    REQUIRE_FALSE(h.editor.doc().dirty());

    // Saving runs its own round-trip check, so a pass here means the file on
    // disk parses back to the document that wrote it.
    app::EditorMode second;
    REQUIRE(second.open(out));
    REQUIRE(second.error_count() == 0);
    REQUIRE(game::level_equal(h.editor.doc().def(), second.doc().def()));

    SECTION("and refuses to save a document with errors") {
        // Drag the objective off the tissue: the bake-dependent validator sees
        // it, and Save is blocked until it is fixed or overridden.
        game::LevelDoc& doc = h.editor.doc();
        doc.move_element({game::ElementKind::Objective, 0, -1}, Vec2{5.0f, 5.0f});
        h.editor.rebake();
        REQUIRE(h.editor.error_count() > 0);
        REQUIRE(h.editor.blocked_from_saving());
        REQUIRE_FALSE(h.editor.save(out));
        REQUIRE_FALSE(h.editor.last_error().empty());
        // ...but says yes when asked explicitly, because work in progress has
        // to be savable.
        REQUIRE(h.editor.save(out, true));
    }
}
