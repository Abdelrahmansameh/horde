#include "config/Json.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "platform/FileIO.h"
#include "ui/front/FrontEnd.h"
#include "ui/front/VirusSwipe.h"
#include "ui/front/VirusSwipeConfig.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace immune;
using namespace immune::ui;

namespace {

struct SwipeFile {
    std::filesystem::path dir;
    std::string path;
    config::Json json;

    SwipeFile() {
        static u64 serial = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = std::filesystem::temp_directory_path() /
              ("immune-virus-swipe-" + std::to_string(stamp) + "-" + std::to_string(++serial));
        std::filesystem::create_directories(dir);
        path = (dir / "virus_swipe.json").string();
        const auto text = platform::read_text_file(platform::asset_path("config/virus_swipe.json"));
        REQUIRE(text.has_value());
        json = config::Json::parse(*text);
        write();
    }
    ~SwipeFile() {
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }
    void write() const { REQUIRE(platform::write_text_file(path, json.dump(2))); }
};

void check_same_config(const VirusSwipeConfig& actual, const VirusSwipeConfig& expected) {
    CHECK(actual.enabled == expected.enabled);
    CHECK(actual.virus_count == expected.virus_count);
    CHECK(actual.density == expected.density);
    CHECK(actual.duration_seconds == expected.duration_seconds);
    CHECK(actual.cover_fraction == expected.cover_fraction);
    CHECK(actual.hold_seconds == expected.hold_seconds);
    CHECK(actual.switch_fraction == expected.switch_fraction);
    CHECK(actual.min_size == expected.min_size);
    CHECK(actual.max_size == expected.max_size);
    CHECK(actual.edge_irregularity == expected.edge_irregularity);
    CHECK(actual.drift == expected.drift);
    CHECK(actual.stagger_seconds == expected.stagger_seconds);
    CHECK(actual.speed_variation == expected.speed_variation);
    CHECK(actual.path_spread == expected.path_spread);
    CHECK(actual.spin == expected.spin);
    CHECK(actual.seed == expected.seed);
    CHECK(actual.from_left == expected.from_left);
    for (int i = 0; i < 4; ++i) {
        CHECK(actual.tint[i] == expected.tint[i]);
    }
}

struct SwipeHarness {
    gui::Gui gui;
    std::unique_ptr<FrontEnd> front;
    FrontModel model;

    explicit SwipeHarness(const std::string& config_path) {
        REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                          platform::asset_path("config/ui_theme.json")}));
        gui.set_viewport(Vec2{1920, 1080});
        front = std::make_unique<FrontEnd>(gui, std::string{}, config_path);
        frame(FrontScreen::MainMenu);
        front->finish_transitions();
        frame(FrontScreen::MainMenu);
    }
    ~SwipeHarness() {
        front.reset();
        gui.shutdown();
    }
    void frame(FrontScreen screen, f32 dt = 1.0f / 60.0f) {
        front->show(screen, model);
        gui::PointerInput pointer;
        pointer.pos = Vec2{-100, -100};
        gui.frame(pointer, dt);
    }
    void advance(FrontScreen screen, int frames) {
        for (int i = 0; i < frames; ++i) frame(screen);
    }
};

void advance(VirusSwipe& swipe, int frames) {
    for (int i = 0; i < frames; ++i) swipe.update(1.0f / 60.0f);
}

} // namespace

TEST_CASE("virus swipe: the shipped transition loads as a dense gameplay horde", "[ui][menu][virus_swipe]") {
    VirusSwipeConfig cfg;
    std::string error = "old error";
    REQUIRE(load_virus_swipe_config(platform::asset_path("config/virus_swipe.json"), cfg, error));
    CHECK(error.empty());
    CHECK(cfg.enabled);
    CHECK(cfg.virus_count >= 500);
    CHECK(cfg.from_left);
    CHECK(cfg.duration_seconds > cfg.hold_seconds);
}

TEST_CASE("virus swipe: invalid configuration retains every previously applied value", "[ui][menu][virus_swipe]") {
    SwipeFile file;
    VirusSwipeConfig previous;
    std::string error;
    REQUIRE(load_virus_swipe_config(file.path, previous, error));
    previous.virus_count = 37;
    previous.duration_seconds = 2.75f;
    previous.seed = 98765;
    VirusSwipeConfig cfg = previous;

    std::vector<config::Json> invalid;
    auto with = [&](const char* key, config::Json value) {
        config::Json j = file.json;
        j[key] = std::move(value);
        invalid.push_back(std::move(j));
    };
    with("virus_count", 0);
    with("virus_count", 50001);
    with("virus_count", 12.5);
    with("density", 0.0);
    with("density", 4.1);
    with("duration_seconds", -1.0);
    with("duration_seconds", 1.0e100);
    with("cover_fraction", 1.0);
    with("hold_seconds", 20.0);
    with("switch_fraction", -0.1);
    with("switch_fraction", 1.1);
    with("min_size", 301.0);
    config::Json inverted_sizes = file.json;
    inverted_sizes["min_size"] = 200.0;
    inverted_sizes["max_size"] = 100.0;
    invalid.push_back(std::move(inverted_sizes));
    with("edge_irregularity", -1.0);
    with("drift", 101.0);
    with("stagger_seconds", -0.1);
    with("stagger_seconds", 1.1);
    with("speed_variation", 0.9);
    with("path_spread", 301.0);
    with("spin", 13.0);
    with("seed", -1);
    with("enabled", "yes");
    with("from_left", 1);
    with("icon", "pathogen_virus"); // Removed menu-icon path must not silently return.
    with("background", "#15672F"); // The transition has no backdrop.
    with("tint", "green");
    with("duraton_seconds", 0.5); // A typo must not silently use a default.
    config::Json missing = file.json;
    missing.erase("density");
    invalid.push_back(std::move(missing));
    invalid.push_back(config::Json::array());

    for (const config::Json& j : invalid) {
        INFO(j.dump());
        REQUIRE(platform::write_text_file(file.path, j.dump()));
        CHECK_FALSE(load_virus_swipe_config(file.path, cfg, error));
        CHECK_FALSE(error.empty());
        check_same_config(cfg, previous);
    }
    REQUIRE(platform::write_text_file(file.path, "{ invalid json"));
    CHECK_FALSE(load_virus_swipe_config(file.path, cfg, error));
    check_same_config(cfg, previous);
    CHECK_FALSE(load_virus_swipe_config((file.dir / "missing.json").string(), cfg, error));
    check_same_config(cfg, previous);

    file.write();
    REQUIRE(load_virus_swipe_config(file.path, cfg, error));
    CHECK(error.empty());
    CHECK(cfg.virus_count == file.json.at("virus_count").get<i32>());
}

TEST_CASE("virus swipe: full cover waits for the destination before revealing", "[ui][menu][virus_swipe]") {
    VirusSwipe swipe;
    VirusSwipeConfig cfg;
    cfg.duration_seconds = 1.0f;
    cfg.cover_fraction = 0.5f;
    cfg.hold_seconds = 0.1f;
    CHECK_FALSE(swipe.active());
    CHECK_FALSE(swipe.ready_to_reveal());
    swipe.start(cfg);
    REQUIRE(swipe.active());
    advance(swipe, 24);
    CHECK_FALSE(swipe.ready_to_reveal());
    advance(swipe, 24);
    REQUIRE(swipe.ready_to_reveal());
    advance(swipe, 180);
    CHECK(swipe.active());
    CHECK(swipe.ready_to_reveal());

    swipe.reveal();
    advance(swipe, 6);
    CHECK(swipe.active());
    advance(swipe, 120);
    CHECK_FALSE(swipe.active());
    CHECK_FALSE(swipe.ready_to_reveal());
    swipe.finish();
    CHECK_FALSE(swipe.active());

    swipe.start(cfg);
    CHECK(swipe.active());
    CHECK_FALSE(swipe.ready_to_reveal());
    swipe.finish();
    CHECK_FALSE(swipe.active());
}

TEST_CASE("virus swipe: authored screen swap lands in the covered hold", "[ui][menu][virus_swipe]") {
    for (f32 fraction : {0.0f, 0.5f, 1.0f}) {
        CAPTURE(fraction);
        VirusSwipeConfig cfg;
        cfg.duration_seconds = 2.0f;
        cfg.cover_fraction = 0.5f;
        cfg.hold_seconds = 0.4f;
        cfg.switch_fraction = fraction;
        VirusSwipe swipe;
        swipe.start(cfg);
        // Coverage finishes at .8s; screen swap is authored inside .8..1.2s.
        const f32 swap_time = 0.8f + 0.4f * fraction;
        swipe.update(swap_time - 0.01f);
        CHECK_FALSE(swipe.ready_to_reveal());
        swipe.update(0.02f);
        REQUIRE(swipe.ready_to_reveal());
        swipe.reveal();
        const f32 remaining = 0.4f * (1.0f - fraction) + 0.8f;
        swipe.update(remaining - 0.03f);
        CHECK(swipe.active());
        swipe.update(0.04f);
        CHECK_FALSE(swipe.active());
    }
}

TEST_CASE("virus swipe: individual lag breaks up rigid wall movement", "[ui][menu][virus_swipe]") {
    gui::Gui gui;
    REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                      platform::asset_path("config/ui_theme.json")}));
    auto& uniform = gui.layer(gui::LayerId::Modal).emplace<VirusSwipe>("uniform");
    auto& staggered = gui.layer(gui::LayerId::Modal).emplace<VirusSwipe>("staggered");
    const Vec2 viewport{1920, 1080};
    uniform.arrange(Rect{Vec2{0, 0}, viewport});
    staggered.arrange(Rect{Vec2{0, 0}, viewport});
    VirusSwipeConfig cfg;
    cfg.virus_count = 256;
    cfg.min_size = cfg.max_size = 32;
    cfg.drift = cfg.edge_irregularity = cfg.spin = cfg.path_spread = 0;
    cfg.stagger_seconds = cfg.speed_variation = 0;
    uniform.start(cfg);
    cfg.stagger_seconds = 0.14f;
    cfg.speed_variation = 0.35f;
    staggered.start(cfg);
    uniform.update(0.3f);
    staggered.update(0.3f);
    auto positions = [&](VirusSwipe& swipe) {
        gui::DrawList draw;
        draw.reset(viewport, 1, 0.3f);
        swipe.draw_self(draw);
        std::map<f32, Vec2> result;
        for (usize i = 0; i < draw.vertices().size(); i += 4) {
            const auto& vertex = draw.vertices()[i];
            const u32 record = vertex.mode_record & 0x0FFFFFFFu;
            Vec2 center{};
            for (usize corner = 0; corner < 4; ++corner) center += draw.vertices()[i + corner].pos * 0.25f;
            // Phase is stable per virus, regardless of culling/draw order.
            result.emplace(draw.records()[record].geom[0], center);
        }
        return result;
    };
    const auto before = positions(uniform), after = positions(staggered);
    f32 smallest = 1.0e9f, largest = -1.0e9f;
    usize matched = 0;
    for (const auto& [phase, p] : before) {
        const auto it = after.find(phase);
        if (it == after.end()) continue;
        const f32 displacement = it->second.x - p.x;
        smallest = std::min(smallest, displacement);
        largest = std::max(largest, displacement);
        ++matched;
    }
    REQUIRE(matched >= 20);
    CHECK(largest - smallest > 80.0f);
}

TEST_CASE("virus swipe: viruses keep advancing on both sides of the crossover", "[ui][menu][virus_swipe]") {
    gui::Gui gui;
    REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                      platform::asset_path("config/ui_theme.json")}));
    auto& swipe = gui.layer(gui::LayerId::Modal).emplace<VirusSwipe>();
    const Vec2 viewport{1920, 1080};
    swipe.arrange(Rect{Vec2{0, 0}, viewport});
    VirusSwipeConfig cfg;
    cfg.virus_count = 1;
    cfg.min_size = cfg.max_size = 32;
    cfg.hold_seconds = cfg.drift = cfg.edge_irregularity = cfg.spin = cfg.path_spread = 0;
    cfg.speed_variation = 0.8f;
    swipe.start(cfg);
    auto position_x = [&] {
        gui::DrawList draw;
        draw.reset(viewport, 1, 0);
        swipe.draw_self(draw);
        REQUIRE(draw.vertices().size() == 4);
        f32 x = 0;
        for (const auto& vertex : draw.vertices()) x += vertex.pos.x * 0.25f;
        return x;
    };
    const f32 cover = cfg.duration_seconds * cfg.cover_fraction;
    const f32 reveal = cfg.duration_seconds * (1.0f - cfg.cover_fraction);
    swipe.update(cover * 0.96f);
    const f32 near_middle = position_x();
    swipe.update(cover * 0.03f);
    const f32 approaching = position_x();
    CHECK(approaching > near_middle + 1.0f);
    swipe.update(cover * 0.011f);
    REQUIRE(swipe.ready_to_reveal());
    const f32 crossover = position_x();
    CHECK(crossover > approaching + 1.0f);
    swipe.reveal();
    swipe.update(reveal * 0.015f);
    CHECK(position_x() > crossover + 1.0f);
}

TEST_CASE("virus swipe: every aspect ratio draws only gameplay viruses without a backing", "[ui][menu][virus_swipe]") {
    for (const Vec2 size : {Vec2{1920, 1080}, Vec2{3440, 1440}, Vec2{1080, 1920}}) {
        CAPTURE(size.x, size.y);
        gui::Gui gui;
        REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                          platform::asset_path("config/ui_theme.json")}));
        VirusSwipe& swipe = gui.layer(gui::LayerId::Modal).emplace<VirusSwipe>();
        VirusSwipeConfig cfg;
        cfg.virus_count = 1; // Sparse settings must leave transparent space.
        cfg.drift = cfg.edge_irregularity = 0.0f;
        swipe.arrange(Rect{Vec2{0, 0}, size});
        swipe.start(cfg);
        advance(swipe, 180);
        REQUIRE(swipe.ready_to_reveal());
        gui::DrawList draw;
        draw.reset(size, 1.0f, 0.0f);
        swipe.draw_self(draw);
        REQUIRE(draw.commands().size() == 1);
        CHECK(draw.commands()[0].kind == gui::CmdKind::GameplayVirus);
        REQUIRE(draw.vertices().size() == 4);
        for (const auto& vertex : draw.vertices()) {
            CHECK((vertex.mode_record >> 28u) == static_cast<u32>(gui::VertexMode::GameplayVirus));
            CHECK(std::isfinite(vertex.pos.x));
            CHECK(std::isfinite(vertex.pos.y));
        }
    }
}

TEST_CASE("virus swipe: Play hands off once under cover and unlocks the destination after reveal", "[ui][menu][virus_swipe]") {
    SwipeFile file;
    file.json["duration_seconds"] = 1.0;
    file.json["cover_fraction"] = 0.5;
    file.json["hold_seconds"] = 0.1;
    file.write();
    SwipeHarness h(file.path);
    REQUIRE(h.gui.click("menu/play"));
    REQUIRE(h.front->virus_swipe() != nullptr);
    REQUIRE(h.front->virus_swipe()->active());
    CHECK(h.front->current() == FrontScreen::MainMenu);
    CHECK(h.front->take_result().action == MenuAction::None);
    (void)h.gui.click("menu/play");
    (void)h.gui.click("menu/quit");
    CHECK(h.front->take_result().action == MenuAction::None);

    h.advance(FrontScreen::MainMenu, 24);
    CHECK(h.front->take_result().action == MenuAction::None);
    bool handed_off = false;
    for (int f = 0; f < 100; ++f) {
        h.frame(FrontScreen::MainMenu);
        const MenuResult result = h.front->take_result();
        if (result.action == MenuAction::None) continue;
        REQUIRE(result.action == MenuAction::OpenImmunityTree);
        REQUIRE_FALSE(handed_off);
        handed_off = true;
        CHECK(h.front->virus_swipe()->ready_to_reveal());
    }
    REQUIRE(handed_off);
    CHECK(h.front->current() == FrontScreen::MainMenu);
    CHECK(h.front->virus_swipe()->active());
    CHECK(h.front->take_result().action == MenuAction::None);

    h.frame(FrontScreen::Tree);
    CHECK(h.front->current() == FrontScreen::Tree);
    REQUIRE(h.gui.find("tree/play") != nullptr);
    (void)h.gui.click("tree/play");
    (void)h.gui.click("tree/back");
    CHECK(h.front->take_result().action == MenuAction::None);
    h.advance(FrontScreen::Tree, 120);
    REQUIRE_FALSE(h.front->virus_swipe()->active());
    CHECK(h.front->take_result().action == MenuAction::None);
    REQUIRE(h.gui.click("tree/play"));
    CHECK(h.front->take_result().action == MenuAction::OpenLevelSelect);
}

TEST_CASE("virus swipe: disabled Play navigates immediately", "[ui][menu][virus_swipe]") {
    SwipeFile file;
    file.json["enabled"] = false;
    file.write();
    SwipeHarness h(file.path);
    REQUIRE(h.gui.click("menu/play"));
    CHECK(h.front->take_result().action == MenuAction::OpenImmunityTree);
    CHECK(h.front->take_result().action == MenuAction::None);
    REQUIRE(h.front->virus_swipe() != nullptr);
    CHECK_FALSE(h.front->virus_swipe()->active());
}

TEST_CASE("virus swipe: external navigation cancels a pending Play without a stale handoff", "[ui][menu][virus_swipe]") {
    SwipeFile file;
    SwipeHarness h(file.path);
    REQUIRE(h.gui.click("menu/play"));
    h.advance(FrontScreen::MainMenu, 6);
    REQUIRE(h.front->virus_swipe()->active());
    h.frame(FrontScreen::None);
    CHECK_FALSE(h.front->virus_swipe()->active());
    CHECK(h.front->take_result().action == MenuAction::None);

    h.frame(FrontScreen::MainMenu);
    h.front->finish_transitions();
    h.frame(FrontScreen::MainMenu);
    REQUIRE(h.gui.click("menu/play"));
    CHECK(h.front->virus_swipe()->active());
    CHECK_FALSE(h.front->virus_swipe()->ready_to_reveal());
    CHECK(h.front->take_result().action == MenuAction::None);
}
