// tests/test_ui_hud.cpp — the in-match HUD from a hand-written model, no GL
// and no game: what the canvas's five states show, and which intents each
// control produces. The screen never sees SimWorld, so none is needed.
#include "gui/core/Gui.h"
#include "gui/widgets/Widgets.h"
#include "platform/FileIO.h"
#include "platform/Input.h"
#include "ui/hud/HudModel.h"
#include "ui/hud/HudScreen.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
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
        hud->sync(m, Vec2{50, 50});
        gui::PointerInput p;
        p.pos = Vec2{-100, -100};
        gui.frame(p, 1.0f / 60.0f);
        out.clear();
        hud->handle_input(input, Vec2{50, 50}, false, out);
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
    m.income_per_second = 4.0f;
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
    REQUIRE(h.shown("hud/dock/neutrophil"));
    CHECK(h.w("hud/dock/neutrophil")->enabled);        // 70 <= 150
    CHECK_FALSE(h.w("hud/dock/macrophage")->enabled);  // 180 > 150
    auto* short_label = dynamic_cast<gui::Label*>(h.w("hud/dock/macrophage/short"));
    REQUIRE(short_label != nullptr);
    CHECK(short_label->text() == "30 ATP short");
    CHECK(h.shown("hud/dock/macrophage/afford"));

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
    CHECK(h.shown("hud/dock/neutrophil/tag"));
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

TEST_CASE("HUD: abilities arm a cast cursor; Fever casts at once", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    h.frame(m);
    REQUIRE(h.gui.click("hud/abilities/histamine/cell/button"));
    CHECK(h.hud->has_cast_cursor());
    CHECK(h.hud->cast_cursor() == game::AbilityId::HistamineFlare);
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
    auto* clock = dynamic_cast<gui::Label*>(h.w("hud/prep/clock/time"));
    REQUIRE(clock != nullptr);
    CHECK(clock->text() == "0:18");
    REQUIRE(h.gui.click("hud/prep/send"));
    h.frame(m);
    CHECK(h.count(IntentKind::StartWaveEarly) == 1);
}

TEST_CASE("HUD: critical integrity, the wave-cleared toast, the next-wave panel", "[ui][hud]") {
    Harness h;
    HudModel m = fixture();
    h.frame(m);
    CHECK_FALSE(h.shown("hud/failing"));
    CHECK_FALSE(h.shown("hud/vignette"));
    m.integrity01 = 0.12f;
    h.frame(m);
    CHECK(h.shown("hud/failing"));
    CHECK(h.shown("hud/vignette"));
    auto* pct = dynamic_cast<gui::Label*>(h.w("hud/organ/value"));
    REQUIRE(pct != nullptr);
    CHECK(pct->text() == "12%");

    // The next-wave rows, biggest family first, plus the elite pill.
    auto* title = dynamic_cast<gui::Label*>(h.w("hud/wave/title"));
    REQUIRE(title != nullptr);
    CHECK(title->text() == "Wave 4");
    auto* first = dynamic_cast<gui::Label*>(h.w("hud/wave/row0/name"));
    REQUIRE(first != nullptr);
    CHECK(first->text() == "Bacteria");
    CHECK(h.shown("hud/wave/elite"));
    CHECK_FALSE(h.shown("hud/wave/row2"));

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

TEST_CASE("HUD: hidden, it draws nothing and takes no clicks", "[ui][hud]") {
    Harness h;
    const HudModel m = fixture();
    h.frame(m);
    h.hud->set_visible(false);
    h.frame(m);
    CHECK_FALSE(h.gui.click("hud/dock/neutrophil"));
    gui::DrawList dl;
    h.gui.draw(dl);
    // Only the (hidden) tooltip layer remains: no geometry at all.
    CHECK(dl.vertices().empty());
}
