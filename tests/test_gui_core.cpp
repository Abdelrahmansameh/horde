// tests/test_gui_core.cpp — the widget tree without GL: flex layout,
// anchoring, the pointer state machine (hover, click, deny, drag, capture,
// blocking), automation by path, the theme file, and the canvas's loops.
#include "gui/anim/Anim.h"
#include "gui/core/Gui.h"
#include "gui/widgets/Widgets.h"
#include "platform/FileIO.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace immune;
using namespace immune::gui;
using Catch::Approx;

namespace {

/// A widget with a fixed size, for layout tests.
std::unique_ptr<Widget> box(const std::string& id, f32 w, f32 h) {
    auto b = std::make_unique<Widget>(id);
    b->layout.width = Size::px(w);
    b->layout.height = Size::px(h);
    return b;
}

Gui& shared_gui() {
    static Gui gui;
    static bool ok = gui.init(Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                          platform::asset_path("config/ui_theme.json")});
    REQUIRE(ok);
    return gui;
}

PointerInput at(Vec2 p) {
    PointerInput in;
    in.pos = p;
    return in;
}

PointerInput press(Vec2 p) {
    PointerInput in = at(p);
    in.pressed[0] = in.down[0] = true;
    return in;
}

PointerInput hold(Vec2 p) {
    PointerInput in = at(p);
    in.down[0] = true;
    return in;
}

PointerInput release(Vec2 p) {
    PointerInput in = at(p);
    in.released[0] = true;
    return in;
}

/// Records every event a widget sees.
class Probe : public Widget {
public:
    explicit Probe(std::string id) : Widget(std::move(id)) { interactive = true; }
    std::vector<EventType> seen;
    Vec2 dragged{0.0f, 0.0f};
    void on_event(Event& e) override {
        seen.push_back(e.type);
        if (e.type == EventType::Drag) dragged += e.delta;
    }
    int count(EventType t) const {
        int n = 0;
        for (EventType s : seen) n += s == t;
        return n;
    }
};

} // namespace

TEST_CASE("Row layout places children with padding, gap and cross alignment", "[gui][core]") {
    Widget row;
    row.layout.axis = Axis::Row;
    row.layout.padding = Insets::all(10);
    row.layout.gap = 5;
    row.layout.align = Align::Center;
    Widget& a = row.add(box("a", 40, 20));
    Widget& b = row.add(box("b", 60, 50));
    const Vec2 s = row.measure(Vec2{kUnbounded, kUnbounded});
    CHECK(s == Vec2{10 + 40 + 5 + 60 + 10, 10 + 50 + 10});
    row.arrange(Rect{Vec2{100, 100}, Vec2{100 + s.x, 100 + s.y}});
    CHECK(a.rect().min == Vec2{110, 125});  // centred in 50 px of cross space
    CHECK(b.rect().min == Vec2{155, 110});
}

TEST_CASE("Grow and Fill share the spare main-axis space", "[gui][core]") {
    Widget col;
    col.layout.axis = Axis::Column;
    col.layout.width = Size::px(200);
    col.layout.height = Size::px(300);
    col.layout.align = Align::Stretch;
    Widget& top = col.add(box("top", 50, 40));
    Widget& mid = col.add(std::make_unique<Widget>("mid"));
    mid.layout.height = Size::fill();
    Widget& bot = col.add(box("bot", 50, 60));
    bot.layout.grow = 1;  // grows as well: equal share of the spare 200 px
    col.measure(Vec2{1920, 1080});
    col.arrange(Rect{Vec2{0, 0}, Vec2{200, 300}});
    CHECK(top.rect().size() == Vec2{200, 40});
    CHECK(mid.rect().size().y == Approx(100));
    CHECK(bot.rect().size().y == Approx(160));
    CHECK(bot.rect().max.y == Approx(300));
}

TEST_CASE("Justify distributes free space", "[gui][core]") {
    Widget row;
    row.layout.axis = Axis::Row;
    row.layout.width = Size::px(300);
    row.layout.justify = Justify::SpaceBetween;
    Widget& a = row.add(box("a", 50, 10));
    Widget& b = row.add(box("b", 50, 10));
    Widget& c = row.add(box("c", 50, 10));
    row.measure(Vec2{1000, 1000});
    row.arrange(Rect{Vec2{0, 0}, Vec2{300, 10}});
    CHECK(a.rect().min.x == 0);
    CHECK(b.rect().min.x == Approx(125));
    CHECK(c.rect().max.x == Approx(300));
    row.layout.justify = Justify::Center;
    row.arrange(Rect{Vec2{0, 0}, Vec2{300, 10}});
    CHECK(a.rect().min.x == Approx(75));
}

TEST_CASE("Anchored children pin to a point of the parent", "[gui][core]") {
    Widget screen;
    screen.layout.width = Size::px(1920);
    screen.layout.height = Size::px(1080);
    Widget& corner = screen.add(box("dock", 400, 100));
    corner.layout.position = Position::Anchored;
    corner.layout.anchor = Vec2{0.5f, 1.0f};
    corner.layout.pivot = Vec2{0.5f, 1.0f};
    corner.layout.offset = Vec2{0, -24};
    Widget& pct = screen.add(std::make_unique<Widget>("pct"));
    pct.layout.position = Position::Anchored;
    pct.layout.width = Size::pct(0.25f);
    pct.layout.height = Size::px(10);
    screen.measure(Vec2{1920, 1080});
    screen.arrange(Rect{Vec2{0, 0}, Vec2{1920, 1080}});
    CHECK(corner.rect().min == Vec2{760, 1080 - 24 - 100});
    CHECK(pct.rect().size().x == Approx(480));
}

TEST_CASE("Paths address widgets and skip anonymous containers", "[gui][core]") {
    Widget root("hud");
    Widget& anon = root.add(std::make_unique<Widget>());
    Widget& dock = anon.add(std::make_unique<Widget>("dock"));
    Widget& card = dock.add(std::make_unique<Widget>("neutrophil"));
    CHECK(card.path() == "hud/dock/neutrophil");
    CHECK(root.find("dock/neutrophil") == &card);
    CHECK(root.find("dock/nope") == nullptr);
}

TEST_CASE("Labels size to their text and wrap to the offered width", "[gui][core]") {
    Gui& gui = shared_gui();
    Widget& host = gui.layer(LayerId::Popup).emplace<Widget>("labels");
    Label& l = host.emplace<Label>("l", "Organ integrity", gui.theme().text("label"));
    const Vec2 one = l.measure(Vec2{kUnbounded, kUnbounded});
    CHECK(one.x > 60);
    l.wrap = true;
    const Vec2 wrapped = l.measure(Vec2{one.x * 0.6f, kUnbounded});
    CHECK(wrapped.y > one.y * 1.5f);
    gui.layer(LayerId::Popup).remove(host);
    gui.frame(at(Vec2{-10, -10}), 0.0f);
}

TEST_CASE("Pointer: hover, click, capture and drag route to the right widget", "[gui][core]") {
    Gui& gui = shared_gui();
    gui.set_viewport(Vec2{1920, 1080});
    Widget& root = gui.layer(LayerId::Hud).emplace<Widget>("t1");
    root.layout.position = Position::Anchored;
    root.layout.width = Size::px(400);
    root.layout.height = Size::px(200);
    auto& probe = root.emplace<Probe>("probe");
    probe.layout.width = Size::px(100);
    probe.layout.height = Size::px(100);

    gui.frame(at(Vec2{50, 50}), 0.016f);
    CHECK(probe.hovered());
    CHECK(gui.wants_pointer());
    CHECK(probe.count(EventType::Enter) == 1);

    gui.frame(press(Vec2{50, 50}), 0.016f);
    CHECK(gui.pressed_widget() == &probe);
    gui.frame(release(Vec2{52, 50}), 0.016f);
    CHECK(probe.count(EventType::Click) == 1);

    // Press inside, release outside: no click, but the capture still ends.
    gui.frame(press(Vec2{50, 50}), 0.016f);
    gui.frame(hold(Vec2{300, 150}), 0.016f);
    gui.frame(release(Vec2{300, 150}), 0.016f);
    CHECK(probe.count(EventType::Click) == 1);
    CHECK(probe.count(EventType::DragStart) == 1);
    CHECK(probe.count(EventType::DragEnd) == 1);
    CHECK(probe.dragged.x == Approx(250));
    CHECK(gui.pressed_widget() == nullptr);

    // Off the widget: Leave, and the world gets the pointer again.
    gui.frame(at(Vec2{1000, 800}), 0.016f);
    CHECK(probe.count(EventType::Leave) >= 1);
    CHECK_FALSE(gui.wants_pointer());

    // Disabled: a click becomes a Deny.
    probe.enabled = false;
    gui.frame(press(Vec2{50, 50}), 0.016f);
    gui.frame(release(Vec2{50, 50}), 0.016f);
    CHECK(probe.count(EventType::Deny) == 1);
    CHECK(probe.count(EventType::Click) == 1);

    gui.layer(LayerId::Hud).remove(root);
    gui.frame(at(Vec2{-10, -10}), 0.0f);
}

TEST_CASE("Panels block the world by their shape; upper layers win", "[gui][core]") {
    Gui& gui = shared_gui();
    gui.set_viewport(Vec2{1920, 1080});
    Panel& disc = gui.layer(LayerId::Hud).emplace<Panel>("disc", gui.theme().shape("button.round"));
    disc.layout.position = Position::Anchored;
    disc.layout.width = Size::px(100);
    disc.layout.height = Size::px(100);
    disc.shape.wobble_amp = 0;
    disc.shape.shadow = kTransparent;

    gui.frame(at(Vec2{50, 50}), 0.016f);
    CHECK(gui.wants_pointer());
    CHECK(gui.hovered_widget() == nullptr);  // blocking, not interactive
    gui.frame(at(Vec2{4, 4}), 0.016f);  // inside the rect, outside the circle
    CHECK_FALSE(gui.wants_pointer());

    auto& over = gui.layer(LayerId::Modal).emplace<Probe>("over");
    over.layout.position = Position::Anchored;
    over.layout.width = Size::px(60);
    over.layout.height = Size::px(60);
    gui.frame(at(Vec2{30, 30}), 0.016f);
    CHECK(gui.hovered_widget() == &over);

    gui.layer(LayerId::Modal).remove(over);
    gui.layer(LayerId::Hud).remove(disc);
    gui.frame(at(Vec2{-10, -10}), 0.0f);
}

TEST_CASE("Buttons click, deny with a shake, and spring on hover", "[gui][core]") {
    Gui& gui = shared_gui();
    gui.set_viewport(Vec2{1920, 1080});
    std::vector<UiSound> sounds;
    gui.on_sound = [&](UiSound s) { sounds.push_back(s); };
    Button& b = gui.layer(LayerId::Hud).emplace<Button>("grow", gui.theme().shape("button.primary"));
    b.layout.position = Position::Anchored;
    b.layout.anchor = Vec2{0.5f, 0.5f};
    b.layout.pivot = Vec2{0.5f, 0.5f};
    b.layout.width = Size::px(200);
    b.layout.height = Size::px(64);
    int clicks = 0, denies = 0;
    b.on_click = [&] { ++clicks; };
    b.on_deny = [&] { ++denies; };

    const Vec2 c{960, 540};
    gui.frame(at(c), 0.016f);
    for (int i = 0; i < 30; ++i) gui.frame(at(c), 0.016f);
    CHECK(b.anim_transform.uniform_scale() == Approx(b.hover_scale).margin(0.005));

    CHECK(gui.click("grow"));
    CHECK(clicks == 1);
    b.enabled = false;
    CHECK(gui.click("grow"));
    CHECK(denies == 1);
    gui.frame(at(c), 0.03f);
    CHECK(b.anim_transform.tx != 0.0f);  // shaking
    CHECK(sounds.front() == UiSound::Hover);
    CHECK(std::count(sounds.begin(), sounds.end(), UiSound::Click) == 1);
    CHECK(std::count(sounds.begin(), sounds.end(), UiSound::Deny) == 1);

    CHECK_FALSE(gui.click("nope"));
    const std::string d = gui.dump();
    CHECK(d.find("grow") != std::string::npos);
    CHECK(d.find("disabled") != std::string::npos);

    gui.on_sound = nullptr;
    gui.layer(LayerId::Hud).remove(b);
    gui.frame(at(Vec2{-10, -10}), 0.0f);
}

TEST_CASE("Tooltips appear after the hover delay near the pointer", "[gui][core]") {
    Gui& gui = shared_gui();
    gui.set_viewport(Vec2{1920, 1080});
    auto& p = gui.layer(LayerId::Hud).emplace<Probe>("tip_target");
    p.layout.position = Position::Anchored;
    p.layout.offset = Vec2{500, 500};
    p.layout.width = Size::px(80);
    p.layout.height = Size::px(80);
    p.tooltip = "Needs 12 more ATP";
    gui.frame(at(Vec2{540, 540}), 0.1f);
    Widget* tip = gui.find("tooltip");
    REQUIRE(tip != nullptr);
    CHECK_FALSE(tip->visible);
    for (int i = 0; i < 6; ++i) gui.frame(at(Vec2{540, 540}), 0.1f);
    CHECK(tip->visible);
    CHECK(tip->rect().min.x > 540);
    CHECK(tip->rect().max.y < 540);
    gui.frame(at(Vec2{1500, 900}), 0.1f);
    CHECK_FALSE(tip->visible);
    gui.layer(LayerId::Hud).remove(p);
    gui.frame(at(Vec2{-10, -10}), 0.0f);
}

TEST_CASE("Meters ease their level; World widgets follow the projection", "[gui][core]") {
    Gui& gui = shared_gui();
    gui.set_viewport(Vec2{1920, 1080});
    Meter& m = gui.layer(LayerId::Hud).emplace<Meter>("bar", gui.theme().shape("bar.blood"));
    m.layout.position = Position::Anchored;
    m.layout.width = Size::px(300);
    m.layout.height = Size::px(26);
    m.set_level(0.72f, 0.3f);
    gui.frame(at(Vec2{-10, -10}), 0.1f);
    CHECK(m.level() > 0.0f);
    CHECK(m.level() < 0.72f);
    for (int i = 0; i < 5; ++i) gui.frame(at(Vec2{-10, -10}), 0.1f);
    CHECK(m.level() == Approx(0.72f));

    gui.set_projection([](Vec2 w) { return w * 2.0f; });
    Widget& pop = gui.layer(LayerId::World).add(box("popup", 100, 40));
    pop.layout.position = Position::World;
    pop.layout.world = Vec2{300, 200};
    pop.layout.pivot = Vec2{0.5f, 1.0f};
    gui.frame(at(Vec2{-10, -10}), 0.016f);
    CHECK(pop.rect().min == Vec2{550, 360});

    gui.set_projection(nullptr);
    gui.layer(LayerId::World).remove(pop);
    gui.layer(LayerId::Hud).remove(m);
    gui.frame(at(Vec2{-10, -10}), 0.0f);
}

TEST_CASE("The shipped theme parses and resolves names, aliases and bases", "[gui][core]") {
    Theme t;
    std::string err;
    INFO(err);
    REQUIRE(t.load(platform::asset_path("config/ui_theme.json"), &err));
    CHECK(t.color("plum") == rgb(0x4E1638));
    const ShapeDesc& host = t.shape("membrane.host");
    CHECK(host.band == 6.0f);
    CHECK(host.rim.a == Approx(0.55f));
    const ShapeDesc& player = t.shape("membrane.player");
    CHECK(player.band == 6.0f);  // inherited from its base
    CHECK(player.fill == t.color("lavender"));
    CHECK(t.text("label").uppercase);
    CHECK(t.text("label").font == FontId::NunitoExtraBold);
    CHECK(t.number("tooltip_delay") == Approx(0.45f));

    // A broken document leaves the previous theme in place.
    CHECK_FALSE(t.parse(R"({"colors": {"x": "nope"}})", &err));
    CHECK_FALSE(err.empty());
    CHECK(t.has_shape("membrane.host"));
    CHECK_FALSE(t.parse("{ not json", &err));
    // Runtime colours survive a reload.
    t.set_color("family_virus", rgb(0x3CD46C));
    REQUIRE(t.parse(R"({"colors": {"a": "#102030", "b": "a@0.5"}})", &err));
    CHECK(t.color("b").a == Approx(0.5f));
    CHECK(t.has_color("family_virus"));
    // Unknown names are loud.
    CHECK(t.color("missing") == Color{1, 0, 1, 1});
}

TEST_CASE("Tweens, springs and the canvas loops", "[gui][core]") {
    Tween tw(0.0f);
    tw.start(10.0f, 0.5f, Ease::Linear);
    tw.update(0.25f);
    CHECK(tw.value() == Approx(5.0f));
    tw.update(1.0f);
    CHECK(tw.done());
    CHECK(tw.value() == 10.0f);

    Spring sp(0.0f);
    sp.set_target(1.0f);
    for (int i = 0; i < 120; ++i) sp.update(1.0f / 60.0f);
    CHECK(sp.settled(1e-2f));
    sp.update(5.0f);  // one huge frame must not explode
    CHECK(sp.value() == Approx(1.0f).margin(1e-3));

    CHECK(loop::beat_scale(0.0f) == Approx(1.0f));
    CHECK(loop::beat_scale(0.12f * 1.4f) == Approx(1.06f));
    CHECK(loop::beat_scale(0.36f * 1.4f) == Approx(1.04f));
    CHECK(loop::halo_opacity(0.8f) == Approx(1.0f));
    CHECK(loop::pulse_opacity(0.5f) == Approx(0.5f));
    CHECK(loop::spin_rotation(2.0f) == Approx(math::kPi * 0.5f));
    CHECK(loop::wobble_rotation(1.1f) == Approx(1.5f * math::kPi / 180.0f));
    CHECK(loop::flow_dash_offset(0.9f) == Approx(-22.0f));
}
