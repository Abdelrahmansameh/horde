// tests/test_ui_hud.cpp — the in-match HUD from a hand-written model, no GL
// and no game: what the canvas's five states show, and which intents each
// control produces. The screen never sees SimWorld, so none is needed.
#include "gui/core/Gui.h"
#include "gui/widgets/Widgets.h"
#include "platform/FileIO.h"
#include "platform/Input.h"
#include "ui/hud/HudModel.h"
#include "ui/hud/HudParts.h"
#include "ui/hud/HudScreen.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace immune;
using namespace immune::ui;

namespace {

struct Harness {
    gui::Gui gui;
    std::unique_ptr<HudScreen> hud;
    platform::InputState input;  // nothing pressed: only button clicks act
    std::vector<Intent> out;

    Harness() {
        REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                          platform::asset_path("config/ui_theme.json")}));
        gui.set_viewport(Vec2{1920, 1080});
        gui.set_projection([](Vec2 w) { return w * 5.0f; });
        hud = std::make_unique<HudScreen>(gui);
    }

    void frame(const HudModel& m) {
        pointer_frame(m, Vec2{-100, -100});
    }

    void pointer_frame(const HudModel& m, Vec2 pos, bool down = false,
                       bool pressed = false, bool released = false, f32 dt = 1.0f / 60.0f) {
        hud->sync(m, Vec2{50, 50});
        gui::PointerInput p;
        p.pos = pos;
        p.down[0] = down;
        p.pressed[0] = pressed;
        p.released[0] = released;
        gui.frame(p, dt);
        out.clear();
        hud->handle_input(input, Vec2{50, 50}, gui.wants_pointer(), out);
    }

    void drag(const HudModel& m, const char* path, Vec2 delta) {
        REQUIRE(w(path) != nullptr);
        const Vec2 start = w(path)->center();
        pointer_frame(m, start, true, true);
        pointer_frame(m, start + delta, true);
        pointer_frame(m, start + delta, true);
        pointer_frame(m, start + delta, false, false, true);
        settle(m);
    }

    // A drag as real pointers deliver it: many small moves, one per frame.
    void sweep(const HudModel& m, const char* path, std::vector<Vec2> legs, f32 step = 6.0f) {
        REQUIRE(w(path) != nullptr);
        Vec2 at = w(path)->center();
        pointer_frame(m, at, true, true);
        for (const Vec2 leg : legs) {
            const int steps = std::max(1, static_cast<int>(std::max(std::abs(leg.x), std::abs(leg.y)) / step));
            for (int i = 0; i < steps; ++i) {
                at += leg / static_cast<f32>(steps);
                pointer_frame(m, at, true);
            }
        }
        pointer_frame(m, at, false, false, true);
        settle(m);
    }

    void click(const HudModel& m, const char* path) {
        REQUIRE(w(path) != nullptr);
        const Vec2 at = w(path)->center();
        pointer_frame(m, at, true, true);
        pointer_frame(m, at, false, false, true);
        settle(m);
    }

    void settle(const HudModel& m, int frames = 90) {
        for (int i = 0; i < frames; ++i) frame(m);
    }

    Rect visible_bounds(const char* path) {
        REQUIRE(w(path) != nullptr);
        const Rect r = w(path)->rect();
        const Vec2 vp = gui.viewport();
        return Rect{Vec2{std::max(0.0f, r.min.x), std::max(0.0f, r.min.y)},
                    Vec2{std::min(vp.x, r.max.x), std::min(vp.y, r.max.y)}};
    }

    gui::Widget* w(const char* path) { return gui.find(path); }
    bool shown(const char* path) {
        gui::Widget* x = gui.find(path);
        if (x == nullptr) return false;
        for (gui::Widget* p = x; p != nullptr; p = p->parent()) {
            if (!p->visible) return false;
        }
        return true;
    }
    usize count(IntentKind k) const {
        return static_cast<usize>(std::count_if(out.begin(), out.end(), [k](const Intent& i) { return i.kind == k; }));
    }
};

HudModel fixture() {
    HudModel m;
    m.integrity01 = 0.72f;
    m.atp = 150;
    m.phase = HudWavePhase::Spawning;
    m.wave_number = 3;
    m.preview.valid = true;
    m.preview.wave_number = 4;
    m.preview.families = {{PathogenFamily::Bacteria, 320}, {PathogenFamily::Virus, 180}};
    m.preview.total = 500;
    m.preview.elites = 1;
    const u32 costs[kTowerTypeCount] = {70, 180, 130, 160, 120};
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        m.cards[i].unlocked = true;
        m.cards[i].cost = costs[i];
        m.cards[i].range = 12.0f;
    }
    for (u32 i = 0; i < game::kAbilityCount; ++i) {
        m.abilities[i].unlocked = true;
        m.abilities[i].ready = true;
        m.abilities[i].radius = 10.0f;
        m.abilities[i].needs_target = static_cast<game::AbilityId>(i) != game::AbilityId::FeverResponse;
    }
    HudTower t;
    t.id = EntityId{42};
    t.type = TowerType::CytotoxicT;
    t.world = Vec2{100, 60};
    t.range = 14.0f;
    t.health = 214.0f;
    t.health_max = 380.0f;
    t.refund = 91;
    m.towers.push_back(t);
    return m;
}

} // namespace

TEST_CASE("HUD: the dock shows unlocked towers in canvas order, greyed when unaffordable", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    h.frame(m);
    auto* atp_label = dynamic_cast<gui::Label*>(h.w("hud/dock/atp/label"));
    REQUIRE(atp_label != nullptr);
    CHECK(atp_label->text() == "ATP");
    CHECK(h.w("hud/dock/atp/rate") == nullptr);
    REQUIRE(h.shown("hud/dock/neutrophil"));
    CHECK(h.w("hud/dock/neutrophil")->enabled);        // 70 <= 150
    CHECK_FALSE(h.w("hud/dock/macrophage")->enabled);  // 180 > 150
    auto* cost_label = dynamic_cast<gui::Label*>(h.w("hud/dock/macrophage/cost"));
    REQUIRE(cost_label != nullptr);
    CHECK(cost_label->text() == "180");
    CHECK_FALSE(h.shown("hud/dock/macrophage/short"));

    // Hotkeys follow the dock: Attack then Control.
    CHECK(dock_slot(TowerType::Neutrophil) == 0);
    CHECK(dock_slot(TowerType::CytotoxicT) == 1);
    CHECK(dock_slot(TowerType::Macrophage) == 2);
    CHECK(dock_tower(4) == TowerType::Fibroblast);

    // A locked tower is not in the dock; a group with none left disappears.
    m.cards[static_cast<usize>(TowerType::GobletCell)].unlocked = false;
    m.cards[static_cast<usize>(TowerType::Fibroblast)].unlocked = false;
    h.frame(m);
    CHECK_FALSE(h.shown("hud/dock/goblet_cell"));
    CHECK_FALSE(h.shown("hud/dock/fibroblast"));
}

TEST_CASE("HUD: clicking a card arms placement; again, or Escape, disarms", "[ui][hud]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    REQUIRE(h.gui.click("hud/dock/neutrophil"));
    CHECK(h.hud->has_build_cursor());
    CHECK(h.hud->build_cursor() == TowerType::Neutrophil);
    h.frame(m);
    auto* card = dynamic_cast<gui::Button*>(h.w("hud/dock/neutrophil"));
    REQUIRE(card != nullptr);
    CHECK(card->nudge.x == 14.0f);
    CHECK(h.shown("hud/hint"));
    REQUIRE(h.gui.click("hud/dock/neutrophil"));
    CHECK_FALSE(h.hud->has_build_cursor());

    // An unaffordable card denies instead of arming.
    REQUIRE(h.gui.click("hud/dock/macrophage"));
    CHECK_FALSE(h.hud->has_build_cursor());

    REQUIRE(h.gui.click("hud/dock/neutrophil"));
    CHECK(h.hud->cancel());
    CHECK_FALSE(h.hud->has_build_cursor());
    CHECK_FALSE(h.hud->cancel());  // nothing left: Escape may open the menu
}

TEST_CASE("HUD: docking at any edge slides the same size menu offscreen leaving icons", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    REQUIRE(dock->dock_edge() == DockEdge::None);
    REQUIRE(h.shown("hud/dock/atp"));
    REQUIRE(h.shown("hud/dock/neutrophil/name"));
    gui::Widget* const first = h.w("hud/dock/neutrophil");
    const Vec2 floating_size = dock->size();
    const Vec2 vp = h.gui.viewport();
    const Rect floating = dock->rect();
    DockEdge edge = DockEdge::None;
    Vec2 delta{0, 0};
    SECTION("left") {
        edge = DockEdge::Left;
        delta = Vec2{8.0f - floating.min.x, 0};
    }
    SECTION("right") {
        edge = DockEdge::Right;
        delta = Vec2{vp.x - 8.0f - floating.max.x, 0};
    }
    SECTION("top") {
        edge = DockEdge::Top;
        delta = Vec2{600, 8.0f - floating.min.y};
    }
    SECTION("bottom") {
        edge = DockEdge::Bottom;
        delta = Vec2{600, vp.y - 8.0f - floating.max.y};
    }
    h.drag(m, "hud/dock/neutrophil", delta);
    CHECK(dock->dock_edge() == edge);
    CHECK_FALSE(h.hud->has_build_cursor());
    CHECK(h.w("hud/dock/neutrophil") == first);
    CHECK_FALSE(h.shown("hud/dock/atp"));
    CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
    CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
    CHECK(dock->reveal() < 0.02f);
    const bool horizontal = edge == DockEdge::Top || edge == DockEdge::Bottom;
    const Vec2 visible_size = h.visible_bounds("hud/dock").size();
    if (horizontal) CHECK(visible_size.y < 100.0f);
    else CHECK(visible_size.x < 100.0f);
    if (edge == DockEdge::Left) CHECK(dock->rect().min.x <= 0.0f);
    if (edge == DockEdge::Right) CHECK(dock->rect().max.x >= vp.x);
    if (edge == DockEdge::Top) CHECK(dock->rect().min.y <= 0.0f);
    if (edge == DockEdge::Bottom) CHECK(dock->rect().max.y >= vp.y);

    f32 previous = -1.0f;
    for (TowerType type : dock_order()) {
        const std::string path = "hud/dock/" + std::string(tower_id(type));
        REQUIRE(h.shown(path.c_str()));
        CHECK_FALSE(h.shown((path + "/name").c_str()));
        CHECK_FALSE(h.shown((path + "/cost").c_str()));
        CHECK_FALSE(h.shown((path + "/key").c_str()));
        CHECK(h.w(path.c_str())->size().x <= 72.0f);
        CHECK(h.w(path.c_str())->size().y <= 72.0f);
        const std::string icon_path = path + "/icon";
        REQUIRE(h.shown(icon_path.c_str()));
        const Rect bounds = h.w(icon_path.c_str())->rect();
        CHECK(bounds.min.x >= 0.0f);
        CHECK(bounds.min.y >= 0.0f);
        CHECK(bounds.max.x <= vp.x);
        CHECK(bounds.max.y <= vp.y);
        const Vec2 center = h.w(path.c_str())->center();
        const f32 position = horizontal ? center.x : center.y;
        CHECK(position > previous);
        previous = position;
    }
    // Entering and pressing in one frame captures the exposed icon directly,
    // without a preparatory hover, on each of the four screen edges.
    const Vec2 grab = first->center();
    h.pointer_frame(m, grab, true, true);
    CHECK(h.gui.pressed_widget() == first);
    CHECK(first->center().x == Catch::Approx(grab.x).margin(0.001f));
    CHECK(first->center().y == Catch::Approx(grab.y).margin(0.001f));
    h.pointer_frame(m, grab, false, false, true);
    CHECK(h.hud->build_cursor() == TowerType::Neutrophil);
    CHECK(dock->dock_edge() == edge);
    h.settle(m);
    CHECK(dock->reveal() < 0.02f);
}

TEST_CASE("HUD: an unaffordable card still drags the menu without selecting it", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    REQUIRE_FALSE(h.w("hud/dock/macrophage")->enabled);
    h.drag(m, "hud/dock/macrophage", Vec2{8.0f - dock->rect().min.x, 0});
    CHECK(dock->dock_edge() == DockEdge::Left);
    CHECK_FALSE(h.hud->has_build_cursor());
    CHECK(h.gui.pressed_widget() == nullptr);
}

TEST_CASE("HUD: docked icons keep live affordability and placement controls", "[ui][hud][dock]") {
    Harness h;
    HudModel m = fixture();
    m.atp = 69;
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    auto* card = dynamic_cast<gui::Button*>(h.w("hud/dock/neutrophil"));
    auto* icon = dynamic_cast<gui::Icon*>(h.w("hud/dock/neutrophil/icon"));
    REQUIRE(card != nullptr);
    REQUIRE(icon != nullptr);
    CHECK_FALSE(card->enabled);
    CHECK(icon->grayscale);
    CHECK_FALSE(card->tooltip.empty());
    h.click(m, "hud/dock/neutrophil");
    CHECK_FALSE(h.hud->has_build_cursor());

    m.atp = 70;
    h.frame(m);
    CHECK(card->enabled);
    CHECK_FALSE(icon->grayscale);
    h.click(m, "hud/dock/neutrophil");
    CHECK(h.hud->build_cursor() == TowerType::Neutrophil);
    CHECK(card->nudge == Vec2{0, 0});
    h.click(m, "hud/dock/neutrophil");
    CHECK_FALSE(h.hud->has_build_cursor());

    m.cards[static_cast<usize>(TowerType::Neutrophil)].allowed = false;
    h.frame(m);
    CHECK_FALSE(card->enabled);
    CHECK(icon->grayscale);
    CHECK(card->tooltip.find("level") != std::string::npos);
    CHECK_FALSE(h.shown("hud/dock/neutrophil/short"));
    h.click(m, "hud/dock/neutrophil");
    CHECK_FALSE(h.hud->has_build_cursor());
}

TEST_CASE("HUD: pulling a docked menu inward restores its full contents", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    const Vec2 floating_size = dock->size();
    gui::Widget* const card = h.w("hud/dock/neutrophil");
    for (int i = 0; i < 3; ++i) {
        h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
        REQUIRE(dock->dock_edge() == DockEdge::Left);
        h.drag(m, "hud/dock/neutrophil", Vec2{360, 0});
        CHECK(dock->dock_edge() == DockEdge::None);
        CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
        CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
        CHECK(h.w("hud/dock/neutrophil") == card);
        CHECK(h.shown("hud/dock/atp"));
        CHECK(h.shown("hud/dock/neutrophil/name"));
        CHECK(h.shown("hud/dock/neutrophil/cost"));
        for (TowerType type : dock_order()) {
            const std::string key_path = "hud/dock/" + std::string(tower_id(type)) + "/key";
            CHECK(h.shown(key_path.c_str()));
        }
        CHECK_FALSE(h.hud->has_build_cursor());
        CHECK(dock->rect().min.x >= 0.0f);
        CHECK(dock->rect().max.x <= h.gui.viewport().x);
        CHECK(dock->rect().min.y >= 0.0f);
        CHECK(dock->rect().max.y <= h.gui.viewport().y);
        h.frame(m);
        CHECK(dock->dock_edge() == DockEdge::None);
    }
}

TEST_CASE("HUD: a slow short pull frees an expanded menu from every edge", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    const Rect floating = dock->rect();
    const Vec2 vp = h.gui.viewport();
    DockEdge edge = DockEdge::None;
    Vec2 delta{0, 0};
    Vec2 inward{0, 0};
    SECTION("left") {
        edge = DockEdge::Left;
        delta = Vec2{8.0f - floating.min.x, 0};
        inward = Vec2{1, 0};
    }
    SECTION("right") {
        edge = DockEdge::Right;
        delta = Vec2{vp.x - 8.0f - floating.max.x, 0};
        inward = Vec2{-1, 0};
    }
    SECTION("top") {
        edge = DockEdge::Top;
        delta = Vec2{600, 8.0f - floating.min.y};
        inward = Vec2{0, 1};
    }
    SECTION("bottom") {
        edge = DockEdge::Bottom;
        delta = Vec2{600, vp.y - 8.0f - floating.max.y};
        inward = Vec2{0, -1};
    }
    h.drag(m, "hud/dock/neutrophil", delta);
    REQUIRE(dock->dock_edge() == edge);
    h.drag(m, "hud/dock/neutrophil", inward * 24.0f);
    REQUIRE(dock->dock_edge() == edge);
    REQUIRE(dock->reveal() > 0.98f);

    // Real slow drags arrive as small pointer deltas. No individual move is
    // large enough to cross the old per-event 24-pixel detachment threshold.
    const Vec2 start = h.w("hud/dock/neutrophil")->center();
    h.pointer_frame(m, start, true, true);
    for (int distance = 2; distance <= 12; distance += 2) {
        h.pointer_frame(m, start + inward * static_cast<f32>(distance), true);
    }
    REQUIRE(dock->dock_edge() == DockEdge::None);
    const Vec2 detached = dock->rect().min;
    for (int distance = 14; distance <= 20; distance += 2) {
        h.pointer_frame(m, start + inward * static_cast<f32>(distance), true);
        const Vec2 expected = detached + inward * static_cast<f32>(distance - 12);
        CHECK(dock->rect().min.x == Catch::Approx(expected.x).margin(0.001f));
        CHECK(dock->rect().min.y == Catch::Approx(expected.y).margin(0.001f));
        CHECK(dock->dock_edge() == DockEdge::None);
    }
    h.pointer_frame(m, start + inward * 20.0f, false, false, true);
    h.settle(m);
    CHECK(dock->dock_edge() == DockEdge::None);
    CHECK(h.shown("hud/dock/atp"));
    CHECK(h.shown("hud/dock/neutrophil/name"));
    CHECK_FALSE(h.hud->has_build_cursor());
}

TEST_CASE("HUD: a continuous slow pull opens and frees a compact menu", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    REQUIRE(dock->reveal() < 0.02f);
    const Vec2 start = h.w("hud/dock/neutrophil")->center();
    const f32 travel = dock->size().x + 30.0f;
    h.pointer_frame(m, start, true, true);
    f32 distance = 2.0f;
    for (; distance <= travel; distance += 2.0f) {
        h.pointer_frame(m, start + Vec2{distance, 0}, true);
    }
    REQUIRE(dock->dock_edge() == DockEdge::None);
    h.pointer_frame(m, start + Vec2{distance - 2.0f, 0}, false, false, true);
    h.settle(m);
    CHECK(dock->dock_edge() == DockEdge::None);
    CHECK(h.shown("hud/dock/atp"));
    CHECK(h.shown("hud/dock/neutrophil/name"));
    CHECK_FALSE(h.hud->has_build_cursor());
}

TEST_CASE("HUD: a docked menu can move to the opposite edge in one drag", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    const Vec2 floating_size = dock->size();
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    h.drag(m, "hud/dock/neutrophil", Vec2{h.gui.viewport().x, 0});
    CHECK(dock->dock_edge() == DockEdge::Right);
    CHECK(dock->rect().max.x >= h.gui.viewport().x);
    CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
    CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
    CHECK(h.visible_bounds("hud/dock").size().x < 100.0f);
    CHECK_FALSE(h.shown("hud/dock/atp"));
    CHECK_FALSE(h.shown("hud/dock/neutrophil/name"));
    CHECK_FALSE(h.hud->has_build_cursor());
}

TEST_CASE("HUD: a held inward drag reveals the menu continuously without resizing", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    const Vec2 floating_size = dock->size();
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    const f32 pinned_x = dock->rect().min.x;
    const f32 before_slide = dock->rect().min.y;
    h.drag(m, "hud/dock/neutrophil", Vec2{0, 96});
    CHECK(dock->dock_edge() == DockEdge::Left);
    CHECK(dock->rect().min.x == Catch::Approx(pinned_x).margin(0.001f));
    CHECK(dock->rect().min.y > before_slide);

    const Vec2 start = h.w("hud/dock/neutrophil")->center();
    h.pointer_frame(m, start, true, true);
    f32 previous_reveal = dock->reveal();
    f32 previous_x = dock->rect().min.x;
    for (int distance = 8; distance <= 120; distance += 8) {
        h.pointer_frame(m, start + Vec2{static_cast<f32>(distance), 0}, true);
        CHECK(dock->dock_edge() == DockEdge::Left);
        CHECK(dock->reveal() > previous_reveal);
        CHECK(dock->reveal() < 1.0f);
        CHECK(dock->rect().min.x > previous_x);
        CHECK(dock->rect().min.x - previous_x < 20.0f);
        CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
        CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
        previous_reveal = dock->reveal();
        previous_x = dock->rect().min.x;
    }
    h.pointer_frame(m, start + Vec2{120, 0}, false, false, true);
    h.settle(m);
    CHECK(dock->dock_edge() == DockEdge::Left);
    CHECK_FALSE(h.hud->has_build_cursor());

    // Pulling the whole panel clear of its edge restores normal floating moves.
    h.drag(m, "hud/dock/neutrophil", Vec2{360, 0});
    CHECK(dock->dock_edge() == DockEdge::None);
    h.drag(m, "hud/dock/neutrophil", Vec2{200, -60});
    CHECK(dock->dock_edge() == DockEdge::None);
    CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
    CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
    CHECK(h.shown("hud/dock/neutrophil/name"));
    CHECK(h.shown("hud/dock/neutrophil/cost"));
    CHECK_FALSE(h.hud->has_build_cursor());
}

TEST_CASE("HUD: edge docking begins while the pointer is held and slides closed", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    const Vec2 floating_size = dock->size();
    const Vec2 start = h.w("hud/dock/neutrophil")->center();
    const Vec2 end = start + Vec2{8.0f - dock->rect().min.x, 0};
    h.pointer_frame(m, start, true, true);
    h.pointer_frame(m, end, true);
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    CHECK(dock->reveal() > 0.0f);
    CHECK(h.gui.pressed_widget() != nullptr);
    const f32 initial_x = dock->rect().min.x;
    h.pointer_frame(m, end, true);
    CHECK(dock->reveal() > 0.0f);
    CHECK(dock->reveal() < 1.0f);
    for (int i = 0; i < 30; ++i) {
        h.pointer_frame(m, end, true);
        CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
        CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
    }
    CHECK(dock->reveal() < 0.05f);
    CHECK(dock->rect().min.x < initial_x);
    CHECK_FALSE(h.hud->has_build_cursor());
    h.pointer_frame(m, end, false, false, true);
    h.settle(m);
    CHECK(dock->dock_edge() == DockEdge::Left);
}

TEST_CASE("HUD: hovering and leaving docked icons never moves or expands the menu", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    const Vec2 floating_size = dock->size();
    const Rect floating = dock->rect();
    const Vec2 vp = h.gui.viewport();
    DockEdge edge = DockEdge::None;
    Vec2 delta{0, 0};
    SECTION("left") {
        edge = DockEdge::Left;
        delta = Vec2{8.0f - floating.min.x, 0};
    }
    SECTION("right") {
        edge = DockEdge::Right;
        delta = Vec2{vp.x - 8.0f - floating.max.x, 0};
    }
    SECTION("top") {
        edge = DockEdge::Top;
        delta = Vec2{600, 8.0f - floating.min.y};
    }
    SECTION("bottom") {
        edge = DockEdge::Bottom;
        delta = Vec2{600, vp.y - 8.0f - floating.max.y};
    }
    h.drag(m, "hud/dock/neutrophil", delta);
    REQUIRE(dock->dock_edge() == edge);
    REQUIRE(dock->reveal() < 0.02f);
    const Vec2 closed_position = dock->rect().min;
    for (TowerType type : dock_order()) {
        const std::string path = "hud/dock/" + std::string(tower_id(type)) + "/icon";
        const Vec2 at = h.w(path.c_str())->center();
        for (int i = 0; i < 12; ++i) {
            h.pointer_frame(m, at);
            CHECK(dock->reveal() < 0.02f);
            CHECK(dock->rect().min.x == Catch::Approx(closed_position.x).margin(0.001f));
            CHECK(dock->rect().min.y == Catch::Approx(closed_position.y).margin(0.001f));
            CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
            CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
        }
    }
    CHECK(dock->dock_edge() == edge);
    CHECK_FALSE(h.shown("hud/dock/atp"));
    CHECK_FALSE(h.shown("hud/dock/neutrophil/name"));
    CHECK_FALSE(h.shown("hud/dock/neutrophil/key"));
    CHECK_FALSE(h.hud->has_build_cursor());
    h.settle(m);
    CHECK(dock->reveal() < 0.02f);
    CHECK(dock->rect().min.x == Catch::Approx(closed_position.x).margin(0.001f));
    CHECK(dock->rect().min.y == Catch::Approx(closed_position.y).margin(0.001f));
    h.click(m, "hud/dock/neutrophil");
    CHECK(h.hud->build_cursor() == TowerType::Neutrophil);
    CHECK(dock->reveal() < 0.02f);
}

TEST_CASE("HUD: explicit pulls and flicks determine the drawer's persistent state", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    bool fast = false;
    bool opening = false;
    f32 distance = 24.0f;
    SECTION("a slow shallow release closes") { fast = false; }
    SECTION("a quick inward flick stays open") { fast = opening = true; }
    SECTION("a deliberate inward pull stays open") { distance = 180.0f; opening = true; }
    const Vec2 start = h.w("hud/dock/neutrophil")->center();
    const Vec2 end = start + Vec2{distance, 0};
    h.pointer_frame(m, start, true, true);
    h.pointer_frame(m, end, true);
    REQUIRE(dock->reveal() > 0.0f);
    if (distance < 100.0f) REQUIRE(dock->reveal() < 0.5f);
    else REQUIRE(dock->reveal() > 0.5f);
    if (!fast) {
        // Holding still discards the earlier movement's release velocity.
        for (int i = 0; i < 15; ++i) h.pointer_frame(m, end, true);
    }
    h.pointer_frame(m, end, false, false, true);
    h.settle(m, 12);
    CHECK(dock->dock_edge() == DockEdge::Left);
    if (opening) CHECK(dock->reveal() > 0.8f);
    else CHECK(dock->reveal() < 0.05f);
    CHECK_FALSE(h.hud->has_build_cursor());
    // Leaving the pointer away for 1.5 seconds never changes the chosen state.
    h.settle(m);
    if (opening) {
        CHECK(dock->reveal() > 0.98f);
        CHECK(h.shown("hud/dock/atp"));
        CHECK(h.shown("hud/dock/neutrophil/name"));
        CHECK(h.shown("hud/dock/neutrophil/key"));
    } else {
        CHECK(dock->reveal() < 0.02f);
    }
}

TEST_CASE("HUD: a slow first contact with an edge still settles closed", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    const Vec2 floating_size = dock->size();
    const Vec2 start = h.w("hud/dock/neutrophil")->center();
    const Vec2 end = start + Vec2{8.0f - dock->rect().min.x, 0};
    h.pointer_frame(m, start, true, true);
    // Ten logical pixels over 0.2 seconds is a slow placement, not a flick.
    h.pointer_frame(m, end, true, false, false, 0.2f);
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    h.pointer_frame(m, end, false, false, true);
    h.settle(m, 18);
    CHECK(dock->dock_edge() == DockEdge::Left);
    CHECK(dock->reveal() < 0.05f);
    CHECK(dock->size().x == Catch::Approx(floating_size.x).margin(0.001f));
    CHECK(dock->size().y == Catch::Approx(floating_size.y).margin(0.001f));
    CHECK_FALSE(h.hud->has_build_cursor());
    h.settle(m);
    CHECK(dock->reveal() < 0.02f);
}

TEST_CASE("HUD: an outward flick closes a drawer opened by an explicit inward flick", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    h.drag(m, "hud/dock/neutrophil", Vec2{24, 0});
    REQUIRE(dock->reveal() > 0.98f);
    const Vec2 start = h.w("hud/dock/neutrophil")->center();
    const Vec2 end = start - Vec2{24, 0};
    h.pointer_frame(m, start, true, true);
    h.pointer_frame(m, end, true);
    REQUIRE(dock->reveal() > 0.5f);
    h.pointer_frame(m, end, false, false, true);
    h.settle(m, 12);
    CHECK(dock->dock_edge() == DockEdge::Left);
    CHECK(dock->reveal() < 0.1f);
    CHECK_FALSE(h.hud->has_build_cursor());
    h.settle(m);
    CHECK(dock->reveal() < 0.02f);
}

TEST_CASE("HUD: docked menus stay attached when the viewport and UI scale change", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    DockEdge edge = DockEdge::None;
    SECTION("right rail") {
        edge = DockEdge::Right;
        h.drag(m, "hud/dock/neutrophil", Vec2{h.gui.viewport().x - 8.0f - dock->rect().max.x, 0});
    }
    SECTION("bottom rail") {
        edge = DockEdge::Bottom;
        h.drag(m, "hud/dock/neutrophil", Vec2{600, h.gui.viewport().y - 8.0f - dock->rect().max.y});
    }
    REQUIRE(dock->dock_edge() == edge);
    for (const auto& viewport : std::vector<std::pair<Vec2, f32>>{
             {Vec2{1280, 720}, 1.5f}, {Vec2{2560, 1080}, 0.75f}}) {
        h.gui.set_viewport(viewport.first, viewport.second);
        h.settle(m);
        const Vec2 vp = h.gui.viewport();
        CHECK(dock->dock_edge() == edge);
        if (edge == DockEdge::Right) {
            CHECK(dock->rect().min.x > vp.x - 100.0f);
            CHECK(dock->rect().max.x >= vp.x);
        } else {
            CHECK(dock->rect().min.y > vp.y - 100.0f);
            CHECK(dock->rect().max.y >= vp.y);
        }
        for (TowerType type : dock_order()) {
            const std::string path = "hud/dock/" + std::string(tower_id(type)) + "/icon";
            const Rect bounds = h.w(path.c_str())->rect();
            CHECK(bounds.min.x >= 0.0f);
            CHECK(bounds.min.y >= 0.0f);
            CHECK(bounds.max.x <= vp.x);
            CHECK(bounds.max.y <= vp.y);
        }
    }
}

TEST_CASE("HUD: compact menus omit locked units without changing their key order", "[ui][hud][dock]") {
    Harness h;
    HudModel m = fixture();
    m.cards[static_cast<usize>(TowerType::GobletCell)].unlocked = false;
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    CHECK_FALSE(h.shown("hud/dock/goblet_cell"));
    const auto* key = dynamic_cast<gui::Label*>(h.w("hud/dock/fibroblast/key/n"));
    REQUIRE(key != nullptr);
    CHECK(key->text() == "5");
    CHECK(h.w("hud/dock/neutrophil")->center().y < h.w("hud/dock/cytotoxic_t")->center().y);
    CHECK(h.w("hud/dock/cytotoxic_t")->center().y < h.w("hud/dock/macrophage")->center().y);
    CHECK(h.w("hud/dock/macrophage")->center().y < h.w("hud/dock/fibroblast")->center().y);
    const f32 locked_height = dock->size().y;
    m.cards[static_cast<usize>(TowerType::GobletCell)].unlocked = true;
    h.frame(m);
    CHECK(dock->size().y > locked_height);
    CHECK(h.w("hud/dock/macrophage")->center().y < h.w("hud/dock/goblet_cell")->center().y);
    CHECK(h.w("hud/dock/goblet_cell")->center().y < h.w("hud/dock/fibroblast")->center().y);
}

TEST_CASE("HUD: a top rail finds room beside preparation and clock panels", "[ui][hud][dock]") {
    Harness h;
    HudModel m = fixture();
    m.phase = HudWavePhase::Prep;
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    REQUIRE(h.shown("hud/prep"));
    // Drop beneath the preparation banner: the rail must seek a clear stretch.
    h.drag(m, "hud/dock/neutrophil",
           Vec2{h.w("hud/prep")->center().x - dock->center().x, 8.0f - dock->rect().min.y});
    REQUIRE(dock->dock_edge() == DockEdge::Top);
    CHECK(dock->rect().min.y <= 0.0f);
    for (const char* path : {"hud/prep", "hud/organ", "hud/controls"}) {
        REQUIRE(h.shown(path));
        CHECK_FALSE(h.visible_bounds("hud/dock").overlaps(h.w(path)->rect()));
    }
    h.click(m, "hud/dock/neutrophil");
    CHECK(h.hud->build_cursor() == TowerType::Neutrophil);
    CHECK(dock->dock_edge() == DockEdge::Top);
}

TEST_CASE("HUD: a bottom rail leaves abilities and placement instructions clear at large UI scale", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.gui.set_viewport(Vec2{1280, 720}, 1.5f);
    h.frame(m);
    REQUIRE(h.gui.viewport() == Vec2{1280, 720});
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    REQUIRE(h.shown("hud/abilities"));
    // Aim at the ability panel's edge so its occupied space affects docking.
    h.drag(m, "hud/dock/neutrophil",
           Vec2{h.w("hud/abilities")->center().x - dock->center().x,
                h.gui.viewport().y - 8.0f - dock->rect().max.y});
    REQUIRE(dock->dock_edge() == DockEdge::Bottom);
    CHECK_FALSE(h.visible_bounds("hud/dock").overlaps(h.w("hud/abilities")->rect()));
    h.click(m, "hud/dock/neutrophil");
    REQUIRE(h.hud->build_cursor() == TowerType::Neutrophil);
    REQUIRE(h.shown("hud/hint"));
    CHECK(dock->dock_edge() == DockEdge::Bottom);
    CHECK(dock->rect().max.y >= h.gui.viewport().y);
    CHECK_FALSE(h.visible_bounds("hud/dock").overlaps(h.w("hud/abilities")->rect()));
    CHECK_FALSE(h.visible_bounds("hud/dock").overlaps(h.w("hud/hint")->rect()));
    CHECK(h.w("hud/hint")->rect().max.y <= h.visible_bounds("hud/dock").min.y);
    // The selected compact control remains clickable while the hint is visible.
    h.click(m, "hud/dock/neutrophil");
    CHECK_FALSE(h.hud->has_build_cursor());
}

TEST_CASE("HUD: abilities arm a cast cursor; Fever casts at once", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    h.frame(m);
    REQUIRE(h.gui.click("hud/abilities/clot/cell/button"));
    CHECK(h.hud->has_cast_cursor());
    CHECK(h.hud->cast_cursor() == game::AbilityId::FibrinClot);
    // Arming an ability disarms a tower and vice versa.
    REQUIRE(h.gui.click("hud/dock/neutrophil"));
    CHECK_FALSE(h.hud->has_cast_cursor());
    CHECK(h.hud->has_build_cursor());

    REQUIRE(h.gui.click("hud/abilities/fever/cell/button"));
    h.frame(m);
    CHECK(h.count(IntentKind::CastAbility) == 1);
    CHECK(h.out.front().ability_id == game::AbilityId::FeverResponse);

    // Recharging: the cell shows seconds left and a click does nothing.
    m.abilities[0].ready = false;
    m.abilities[0].cooldown_remaining = 12.2f;
    m.abilities[0].cooldown_total = 90.0f;
    h.frame(m);
    auto* timer = dynamic_cast<gui::Label*>(h.w("hud/abilities/cascade/cell/button/timer"));
    REQUIRE(timer != nullptr);
    CHECK(timer->text() == "13s");
    REQUIRE(h.gui.click("hud/abilities/cascade/cell/button"));
    CHECK_FALSE(h.hud->has_cast_cursor());

    // None unlocked: no ability panel at all.
    for (HudAbility& a : m.abilities) a.unlocked = false;
    h.frame(m);
    CHECK_FALSE(h.shown("hud/abilities"));
}

TEST_CASE("HUD: dragging an ability moves the panel without arming it", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* bar = dynamic_cast<DragPanel*>(h.w("hud/abilities"));
    REQUIRE(bar != nullptr);
    const Rect start = bar->rect();
    h.drag(m, "hud/abilities/cascade/cell/button", Vec2{-300, -200});
    CHECK(bar->dock_edge() == DockEdge::None);
    CHECK_FALSE(h.hud->has_cast_cursor());
    CHECK(bar->rect().min.x == Catch::Approx(start.min.x - 300.0f).margin(0.5f));
    CHECK(bar->rect().min.y == Catch::Approx(start.min.y - 200.0f).margin(0.5f));
}

TEST_CASE("HUD: the ability panel docks at any edge leaving its cells as icons", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* bar = dynamic_cast<DragPanel*>(h.w("hud/abilities"));
    REQUIRE(bar != nullptr);
    REQUIRE(bar->dock_edge() == DockEdge::None);
    const Vec2 vp = h.gui.viewport();
    const Rect floating = bar->rect();
    DockEdge edge = DockEdge::None;
    Vec2 delta{0, 0};
    SECTION("left") {
        edge = DockEdge::Left;
        delta = Vec2{8.0f - floating.min.x, -200};
    }
    SECTION("right") {
        edge = DockEdge::Right;
        delta = Vec2{vp.x - 8.0f - floating.max.x, -200};
    }
    SECTION("top") {
        edge = DockEdge::Top;
        delta = Vec2{-300, 8.0f - floating.min.y};
    }
    SECTION("bottom") {
        edge = DockEdge::Bottom;
        delta = Vec2{-300, vp.y - 8.0f - floating.max.y};
    }
    h.drag(m, "hud/abilities/cascade/cell/button", delta);
    REQUIRE(bar->dock_edge() == edge);
    CHECK_FALSE(h.hud->has_cast_cursor());
    CHECK(bar->reveal() < 0.02f);
    const bool horizontal = edge == DockEdge::Top || edge == DockEdge::Bottom;
    // Along a side the panel stands on end, so its rail runs down that side.
    if (horizontal) CHECK(bar->size().x > bar->size().y);
    else CHECK(bar->size().y > bar->size().x);
    const Vec2 visible_size = h.visible_bounds("hud/abilities").size();
    if (horizontal) CHECK(visible_size.y < 100.0f);
    else CHECK(visible_size.x < 100.0f);

    f32 previous = -1.0f;
    for (const char* id : {"cascade", "fever", "clot"}) {
        const std::string button = std::string("hud/abilities/") + id + "/cell/button";
        INFO(button);
        REQUIRE(h.shown(button.c_str()));
        const Rect bounds = h.w(button.c_str())->rect();
        CHECK(bounds.size().x <= 72.0f);
        CHECK(bounds.size().y <= 72.0f);
        CHECK(bounds.min.x >= 0.0f);
        CHECK(bounds.min.y >= 0.0f);
        CHECK(bounds.max.x <= vp.x);
        CHECK(bounds.max.y <= vp.y);
        const f32 position = horizontal ? bounds.center().x : bounds.center().y;
        CHECK(position > previous);
        previous = position;
    }
    // A docked icon still casts, and the panel stays tucked away.
    h.click(m, "hud/abilities/cascade/cell/button");
    CHECK(h.hud->cast_cursor() == game::AbilityId::ComplementCascadeBurst);
    CHECK(bar->dock_edge() == edge);
    CHECK(bar->reveal() < 0.02f);

    // Pulling it back off the edge restores the full row.
    h.drag(m, "hud/abilities/fever/cell/button",
           Vec2{edge == DockEdge::Left ? 400.0f : edge == DockEdge::Right ? -400.0f : 0.0f,
                edge == DockEdge::Top ? 400.0f : edge == DockEdge::Bottom ? -400.0f : 0.0f});
    CHECK(bar->dock_edge() == DockEdge::None);
    CHECK(bar->size().x > bar->size().y);
    CHECK(h.w("hud/abilities/fever/cell/button")->rect().size().x > 80.0f);
}

TEST_CASE("HUD: a panel docks to the edge it rests on, including one it just left", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{8.0f - dock->rect().min.x, 0});
    REQUIRE(dock->dock_edge() == DockEdge::Left);
    // Out past the reach of the edge and back onto it, in one drag.
    h.sweep(m, "hud/dock/neutrophil", {Vec2{dock->size().x + 160.0f, 0}, Vec2{-400, 0}});
    CHECK(dock->dock_edge() == DockEdge::Left);
}

TEST_CASE("HUD: a throw follows the panel's recent motion, not the pointer's last twitch", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{400.0f - dock->rect().min.x, 0});
    REQUIRE(dock->dock_edge() == DockEdge::None);
    // Thrown left, stopping short of the edge with a two-pixel wobble back.
    Vec2 at = h.w("hud/dock/neutrophil")->center();
    h.pointer_frame(m, at, true, true);
    for (int i = 0; i < 10; ++i) {
        at.x -= 30.0f;
        h.pointer_frame(m, at, true);
    }
    REQUIRE(dock->dock_edge() == DockEdge::None);
    REQUIRE(dock->rect().min.x > 60.0f);
    at.x += 2.0f;
    h.pointer_frame(m, at, true);
    h.pointer_frame(m, at, false, false, true);
    h.settle(m);
    CHECK(dock->dock_edge() == DockEdge::Left);
}

namespace {
// Docks the ability panel on the bottom edge, centred on `x`.
DragPanel* dock_abilities_bottom(Harness& h, const HudModel& m, f32 x) {
    auto* bar = dynamic_cast<DragPanel*>(h.w("hud/abilities"));
    REQUIRE(bar != nullptr);
    h.drag(m, "hud/abilities/cascade/cell/button",
           Vec2{x - bar->center().x, h.gui.viewport().y - 8.0f - bar->rect().max.y});
    REQUIRE(bar->dock_edge() == DockEdge::Bottom);
    return bar;
}
}

TEST_CASE("HUD: pressing a panel onto a docked one toward its edge never squeezes it aside", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    DragPanel* bar = dock_abilities_bottom(h, m, h.gui.viewport().x * 0.5f);
    const Rect docked = bar->rect();
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    // Line up above the docked panel, then push straight down onto it.
    h.drag(m, "hud/dock/neutrophil", Vec2{docked.center().x - dock->center().x, 200.0f - dock->rect().min.y});
    REQUIRE(dock->dock_edge() == DockEdge::None);
    h.sweep(m, "hud/dock/neutrophil", {Vec2{0, h.gui.viewport().y}});
    CHECK(bar->dock_edge() == DockEdge::Bottom);
    CHECK(bar->rect().min.x == Catch::Approx(docked.min.x).margin(0.5f));
    CHECK(bar->rect().min.y == Catch::Approx(docked.min.y).margin(0.5f));
    CHECK(dock->dock_edge() == DockEdge::None);
    CHECK(dock->rect().max.y <= docked.min.y);
}

TEST_CASE("HUD: a rail dragged along an edge pushes a docked panel until it runs out of room", "[ui][hud][dock]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    const Vec2 vp = h.gui.viewport();
    DragPanel* bar = dock_abilities_bottom(h, m, vp.x * 0.5f);
    auto* dock = dynamic_cast<DragPanel*>(h.w("hud/dock"));
    REQUIRE(dock != nullptr);
    h.drag(m, "hud/dock/neutrophil", Vec2{60.0f - dock->rect().min.x, vp.y - 8.0f - dock->rect().max.y});
    REQUIRE(dock->dock_edge() == DockEdge::Bottom);
    REQUIRE(dock->rect().max.x < bar->rect().min.x);

    // Slide right into it: it gives way along the edge, staying on the near side.
    const f32 start_x = bar->rect().min.x;
    h.sweep(m, "hud/dock/neutrophil", {Vec2{bar->rect().min.x - dock->rect().max.x + 200.0f, 0}});
    CHECK(dock->dock_edge() == DockEdge::Bottom);
    CHECK(bar->dock_edge() == DockEdge::Bottom);
    CHECK(bar->rect().min.x > start_x + 100.0f);
    CHECK(bar->rect().min.x >= dock->rect().max.x);

    // Against the far corner the pushed panel holds and the rail stops.
    h.sweep(m, "hud/dock/neutrophil", {Vec2{vp.x, 0}});
    CHECK(bar->rect().max.x <= vp.x);
    CHECK(bar->rect().min.x >= dock->rect().max.x);
    CHECK(dock->dock_edge() == DockEdge::Bottom);
}

TEST_CASE("HUD: selecting a tower opens its popup; Sell emits the intent", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    h.frame(m);
    CHECK_FALSE(h.shown("inspect/popup"));
    h.hud->select(EntityId{42});
    h.frame(m);
    REQUIRE(h.shown("inspect/popup"));
    auto* value = dynamic_cast<gui::Label*>(h.w("inspect/popup/value"));
    REQUIRE(value != nullptr);
    CHECK(value->text() == "214 / 380");
    auto* refund = dynamic_cast<gui::Label*>(h.w("inspect/popup/sell/refund"));
    REQUIRE(refund != nullptr);
    CHECK(refund->text() == "91");
    // World-anchored: the popup sits up and right of the projected tower.
    CHECK(h.w("inspect/popup")->rect().min.x > 100 * 5.0f);

    REQUIRE(h.gui.click("inspect/popup/sell"));
    h.frame(m);
    REQUIRE(h.count(IntentKind::SellTower) == 1);
    CHECK(h.out.front().entity == EntityId{42});
    CHECK_FALSE(h.hud->selected().valid());

    // A tower that disappears (destroyed) drops its selection and overlays.
    h.hud->select(EntityId{42});
    m.towers.clear();
    h.frame(m);
    CHECK_FALSE(h.hud->selected().valid());
    CHECK(h.gui.find("world/ring42") == nullptr);
}

TEST_CASE("HUD: clock controls, menu and Send now", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    h.frame(m);
    REQUIRE(h.gui.click("hud/controls/speed2"));
    REQUIRE(h.gui.click("hud/controls/menu"));
    h.frame(m);
    REQUIRE(h.out.size() == 2);
    CHECK(h.out[0].kind == IntentKind::SetTimeScale);
    CHECK(h.out[0].value == 2.0f);
    CHECK(h.out[1].kind == IntentKind::OpenMenu);

    // Pause remembers the speed it paused from.
    m.time_scale = 2.0f;
    h.frame(m);
    REQUIRE(h.gui.click("hud/controls/pause"));
    h.frame(m);
    REQUIRE(h.count(IntentKind::SetTimeScale) == 1);
    CHECK(h.out[0].value == 0.0f);
    m.time_scale = 0.0f;
    h.frame(m);
    REQUIRE(h.gui.click("hud/controls/pause"));
    h.frame(m);
    CHECK(h.out[0].value == 2.0f);

    // Prep: the banner and its Send now.
    CHECK_FALSE(h.shown("hud/prep"));
    m.phase = HudWavePhase::Prep;
    m.phase_time_remaining = 18.0f;
    m.prep_total = 20.0f;
    h.frame(m);
    REQUIRE(h.shown("hud/prep"));
    CHECK(h.w("hud/prep/clock") == nullptr);
    REQUIRE(h.gui.click("hud/prep/send"));
    h.frame(m);
    CHECK(h.count(IntentKind::StartWaveEarly) == 1);

    // The auto-start toggle.
    REQUIRE(h.gui.click("hud/controls/auto"));
    h.frame(m);
    CHECK(h.count(IntentKind::ToggleAutoStart) == 1);
}

TEST_CASE("HUD: critical integrity and the wave-cleared toast", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    h.frame(m);
    CHECK_FALSE(h.shown("hud/failing"));
    CHECK_FALSE(h.shown("hud/vignette"));
    m.integrity01 = 0.12f;
    h.frame(m);
    CHECK_FALSE(h.shown("hud/failing"));
    CHECK_FALSE(h.shown("hud/vignette"));
    auto* pct = dynamic_cast<gui::Label*>(h.w("hud/organ/value"));
    REQUIRE(pct != nullptr);
    CHECK(pct->text() == "12%");

    // Spawning -> Prep: "Wave 3 cleared".
    CHECK_FALSE(h.shown("hud/toast"));
    m.phase = HudWavePhase::Prep;
    m.wave_number = 4;
    h.frame(m);
    REQUIRE(h.shown("hud/toast"));
    auto* toast = dynamic_cast<gui::Label*>(h.w("hud/toast/text"));
    REQUIRE(toast != nullptr);
    CHECK(toast->text() == "Wave 3 cleared");
}

TEST_CASE("HUD: the next-wave composition panel is absent in every phase", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    for (HudWavePhase phase : {HudWavePhase::Prep, HudWavePhase::Spawning,
                              HudWavePhase::Clearing, HudWavePhase::Complete}) {
        m.phase = phase;
        h.frame(m);
        CHECK(h.w("hud/wave") == nullptr);
    }
    m.phase = HudWavePhase::Prep;
    m.all_waves_complete = true;
    h.frame(m);
    CHECK(h.w("hud/wave") == nullptr);
    m.all_waves_complete = false;
    m.preview.valid = false;
    h.frame(m);
    CHECK(h.w("hud/wave") == nullptr);
}

TEST_CASE("HUD: hidden, it draws nothing and takes no clicks", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    m.phase = HudWavePhase::Prep;
    m.spawn_points = {{Vec2{100, 160}, Vec2{1, 0}}};
    m.organ_points = {{Vec2{220, 160}, Vec2{1, 0}}};
    h.frame(m);
    h.hud->set_visible(false);
    h.frame(m);
    CHECK_FALSE(h.gui.click("hud/dock/neutrophil"));
    gui::DrawList dl;
    h.gui.draw(dl);
    // Only the (hidden) tooltip layer remains: no geometry at all.
    CHECK(dl.vertices().empty());
}

TEST_CASE("HUD: every entrance has a count-free marker during preparation", "[ui][hud][spawn]") {
    Harness h;
    HudModel m = fixture();
    m.phase = HudWavePhase::Prep;
    m.spawn_points = {{Vec2{100, 160}, Vec2{1, 0}}, {Vec2{220, 160}, Vec2{-1, 0}}};
    // Entrances belong to the level, even when there is no next-wave preview.
    m.preview = {};
    h.frame(m);
    REQUIRE(h.shown("world/spawn0"));
    REQUIRE(h.shown("world/spawn1"));
    CHECK(h.w("world/spawn0")->center().x < h.w("world/spawn1")->center().x);

    // Entrance markers never imply an enemy count or split.
    m.preview = fixture().preview;
    h.frame(m);
    const auto check_no_count = [](const auto& self, const gui::Widget& w) -> void {
        if (const auto* label = dynamic_cast<const gui::Label*>(&w)) {
            CHECK(label->text().find_first_of("0123456789") == std::string::npos);
        }
        for (const auto& child : w.children()) self(self, *child);
    };
    check_no_count(check_no_count, *h.w("world/spawn0"));
    check_no_count(check_no_count, *h.w("world/spawn1"));
    CHECK(h.w("world/spawn") == nullptr); // The old composition pill is gone.

    // A marker must not block deploying a cell beneath it.
    gui::PointerInput pointer;
    pointer.pos = h.w("world/spawn1")->center();
    h.gui.frame(pointer, 1.0f / 60.0f);
    CHECK_FALSE(h.gui.wants_pointer());

    for (HudWavePhase phase : {HudWavePhase::Spawning, HudWavePhase::Clearing, HudWavePhase::Complete}) {
        m.phase = phase;
        h.frame(m);
        CHECK_FALSE(h.shown("world/spawn0"));
        CHECK_FALSE(h.shown("world/spawn1"));
    }
    m.phase = HudWavePhase::Prep;
    ++m.wave_number;
    h.frame(m);
    CHECK(h.shown("world/spawn0"));
    CHECK(h.shown("world/spawn1"));
    m.all_waves_complete = true;
    h.frame(m);
    CHECK_FALSE(h.shown("world/spawn0"));
    CHECK_FALSE(h.shown("world/spawn1"));
}

TEST_CASE("HUD: entrance markers stay visible at map edges and follow the current level", "[ui][hud][spawn]") {
    Harness h;
    HudModel m = fixture();
    m.phase = HudWavePhase::Prep;
    m.spawn_points = {{Vec2{0, 80}, Vec2{1, 0}}, {Vec2{500, 500}, Vec2{-1, 0}},
                      {Vec2{200, 100}, Vec2{0, -1}}};
    h.frame(m);
    for (const char* path : {"world/spawn0", "world/spawn1", "world/spawn2"}) {
        REQUIRE(h.shown(path));
        const Rect r = h.w(path)->rect();
        CHECK(r.min.x >= 0.0f);
        CHECK(r.min.y >= 0.0f);
        CHECK(r.max.x <= h.gui.viewport().x);
        CHECK(r.max.y <= h.gui.viewport().y);
    }

    m.spawn_points.resize(1);
    m.spawn_points.front().world = Vec2{150, 160};
    h.frame(m);
    REQUIRE(h.shown("world/spawn0"));
    CHECK(h.w("world/spawn0")->center().x > 700.0f);
    CHECK(h.w("world/spawn1") == nullptr);
    CHECK(h.w("world/spawn2") == nullptr);
    m.spawn_points.clear();
    h.frame(m);
    CHECK(h.w("world/spawn0") == nullptr);
}

TEST_CASE("HUD: entrance markers clear nearby panels without losing their entrance", "[ui][hud][spawn]") {
    Harness h;
    HudModel m = fixture();
    m.phase = HudWavePhase::Prep;
    m.spawn_points = {{Vec2{0, 26}, Vec2{1, 0}}, {Vec2{192, 0}, Vec2{0, 1}}};
    h.frame(m);
    REQUIRE(h.shown("world/spawn0"));
    REQUIRE(h.shown("world/spawn1"));
    REQUIRE(h.shown("hud/prep"));
    const Rect organ = h.w("hud/organ")->rect();
    const Rect prep = h.w("hud/prep")->rect();
    const gui::Widget& left = *h.w("world/spawn0");
    const gui::Widget& top = *h.w("world/spawn1");

    // A small panel overlap should move the badge just beyond that panel,
    // keeping left and top entrances recognizable at their respective edges.
    CHECK(left.center().x < 100.0f);
    CHECK(top.center().x > 950.0f);
    CHECK(top.center().x < 970.0f);
    CHECK(top.center().y > prep.max.y + 34.0f);
    for (const gui::Widget* marker : {&left, &top}) {
        CHECK_FALSE(marker->rect().overlaps(organ));
        CHECK_FALSE(marker->rect().overlaps(prep));
        gui::PointerInput pointer;
        pointer.pos = marker->center();
        h.gui.frame(pointer, 1.0f / 60.0f);
        CHECK_FALSE(h.gui.wants_pointer());
    }
}

TEST_CASE("HUD: organ path arrows appear only during preparation", "[ui][hud][organ]") {
    Harness h;
    HudModel m = fixture();
    m.spawn_points = {{Vec2{100, 160}, Vec2{1, 0}}};
    m.organ_points = {{Vec2{220, 160}, Vec2{1, 0}}, {Vec2{300, 120}, Vec2{0, -1}}};
    m.preview = {};
    for (HudWavePhase phase : {HudWavePhase::Spawning, HudWavePhase::Clearing, HudWavePhase::Complete}) {
        m.phase = HudWavePhase::Prep;
        h.frame(m);
        REQUIRE(h.shown("world/organ0"));
        REQUIRE(h.shown("world/organ1"));
        CHECK(h.w("world/organ0") != h.w("world/spawn0"));
        CHECK(h.w("world/organ0")->center().x < h.w("world/organ1")->center().x);
        const auto* right = dynamic_cast<OrganMarker*>(h.w("world/organ0"));
        const auto* up = dynamic_cast<OrganMarker*>(h.w("world/organ1"));
        REQUIRE(right != nullptr);
        REQUIRE(up != nullptr);
        CHECK(right->direction.x > 0.99f);
        CHECK(right->direction.y == 0.0f);
        CHECK(up->direction.x == 0.0f);
        CHECK(up->direction.y < -0.99f);
        m.phase = phase;
        h.frame(m);
        CHECK_FALSE(h.shown("world/organ0"));
        CHECK_FALSE(h.shown("world/organ1"));
        CHECK_FALSE(h.shown("world/spawn0"));
    }

    m.phase = HudWavePhase::Prep;
    m.all_waves_complete = true;
    h.frame(m);
    CHECK_FALSE(h.shown("world/organ0"));
    CHECK_FALSE(h.shown("world/organ1"));
    m.all_waves_complete = false;
    m.organ_points.resize(1);
    m.organ_points.front().world = Vec2{180, 160};
    h.frame(m);
    REQUIRE(h.shown("world/organ0"));
    CHECK(h.w("world/organ0")->center().x < 1000.0f);
    CHECK(h.w("world/organ1") == nullptr);
    m.organ_points.clear();
    h.frame(m);
    CHECK(h.w("world/organ0") == nullptr);
}

TEST_CASE("HUD: organ destinations stay accessible at screen edges and beside panels", "[ui][hud][organ]") {
    Harness h;
    HudModel m = fixture();
    m.phase = HudWavePhase::Prep;
    m.organ_points = {{Vec2{0, 26}, Vec2{-1, 0}}, {Vec2{500, 140}, Vec2{1, 0}}};
    h.frame(m);
    REQUIRE(h.shown("world/organ0"));
    REQUIRE(h.shown("world/organ1"));
    CHECK(h.w("world/organ0")->center().x < 100.0f);
    for (const char* path : {"world/organ0", "world/organ1"}) {
        const gui::Widget& marker = *h.w(path);
        const Rect bounds = marker.rect();
        CHECK(bounds.min.x >= 0.0f);
        CHECK(bounds.min.y >= 0.0f);
        CHECK(bounds.max.x <= h.gui.viewport().x);
        CHECK(bounds.max.y <= h.gui.viewport().y);
        for (const char* panel : {"hud/organ", "hud/dock", "hud/abilities"}) {
            if (h.shown(panel)) CHECK_FALSE(bounds.overlaps(h.w(panel)->rect()));
        }
        gui::PointerInput pointer;
        pointer.pos = marker.center();
        h.gui.frame(pointer, 1.0f / 60.0f);
        CHECK_FALSE(h.gui.wants_pointer());
    }
}

TEST_CASE("HUD: map arrows have a maximum avoidance distance when the dock blocks their location", "[ui][hud][spawn][organ]") {
    Harness h;
    HudModel m = fixture();
    m.phase = HudWavePhase::Prep;
    const char* path = nullptr;
    SECTION("enemy entrance") {
        m.spawn_points = {{Vec2{192, 122}, Vec2{1, 0}}};
        path = "world/spawn0";
    }
    SECTION("organ destination") {
        m.organ_points = {{Vec2{192, 122}, Vec2{1, 0}}};
        path = "world/organ0";
    }
    h.frame(m);
    REQUIRE(h.shown(path));
    const Vec2 preferred = h.w(path)->center();
    gui::Widget& dock = *h.w("hud/dock");
    REQUIRE_FALSE(h.w(path)->rect().overlaps(dock.rect()));

    // Drag the large dock directly over the marker. No position within the
    // avoidance radius can clear it, so keeping the location takes priority.
    dock.layout.anchor = Vec2{0, 0};
    dock.layout.pivot = Vec2{0.5f, 0.5f};
    dock.layout.offset = preferred;
    dock.layout.width = gui::Size::px(500.0f);
    h.frame(m);
    REQUIRE(dock.rect().size().x * 0.5f > 180.0f);
    REQUIRE(dock.rect().size().y * 0.5f > 180.0f);
    const Vec2 shift = h.w(path)->center() - preferred;
    CHECK(shift.x * shift.x + shift.y * shift.y <= 180.0f * 180.0f + 0.1f);
    CHECK(h.w(path)->rect().overlaps(dock.rect()));

    // Avoidance is recomputed from the map location each frame, so it cannot
    // accumulate into a larger drift while the panel remains in the way.
    for (u32 i = 0; i < 5; ++i) h.frame(m);
    const Vec2 later_shift = h.w(path)->center() - preferred;
    CHECK(later_shift.x * later_shift.x + later_shift.y * later_shift.y <= 180.0f * 180.0f + 0.1f);
}
