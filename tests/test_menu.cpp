// tests/test_menu.cpp — the out-of-match screens (ui/front/FrontEnd): main
// menu, the Strengthen Immunity tree, campaign level select, pause and
// results, from hand-built models (the tree from a real MetaProgression).
//
// The logic tests need no GL: they build the widget trees, click widgets by
// path (what the gym's `ui click` does) and check the MenuResult each click
// reports. A GL test then renders every screen to PNGs for review
// (front_*.png in the working directory) and checks it drew.
#include "app/UiBridge.h"
#include "core/Clock.h"
#include "game/config/GameConfig.h"
#include "game/level/Level.h"
#include "game/meta/MetaProgression.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/widgets/Widgets.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Screenshot.h"
#include "ui/front/FrontEnd.h"
#include "ui/front/FrontModel.h"
#include "ui/front/LevelCell.h"
#include "ui/front/TreeScreen.h"

#include <glad/glad.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::ui;

namespace {

struct Harness {
    gui::Gui gui;
    std::unique_ptr<FrontEnd> front;

    explicit Harness(bool gl = false) {
        REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                          platform::asset_path("config/ui_theme.json")}));
        if (gl) REQUIRE(gui.init_renderer());
        gui.set_viewport(Vec2{1920, 1080});
        front = std::make_unique<FrontEnd>(gui);
    }
    ~Harness() {
        front.reset();
        gui.shutdown();
    }

    void frame(FrontScreen s, const FrontModel& m, f32 dt = 1.0f / 60.0f) {
        front->show(s, m);
        gui::PointerInput p;
        p.pos = Vec2{-100, -100};
        gui.frame(p, dt);
    }
    /// Shows `s` and lets its transition finish.
    void open(FrontScreen s, const FrontModel& m) {
        frame(s, m);
        front->finish_transitions();
        frame(s, m);
    }
    MenuResult click(const std::string& path) {
        INFO(path);
        REQUIRE(gui.click(path));
        return front->take_result();
    }
    bool shown(const std::string& path) {
        gui::Widget* x = gui.find(path);
        if (x == nullptr) return false;
        for (gui::Widget* p = x; p != nullptr; p = p->parent()) {
            if (!p->visible) return false;
        }
        return true;
    }
};

/// Ten campaign levels, the first `cleared` of them cleared. Their indices
/// into app's level list are deliberately not 0..9 (other levels sit between
/// them there).
FrontModel campaign(usize cleared) {
    static const char* kNames[10] = {"First Bend", "Island Climb", "Two Chambers", "Twin Channels", "Ring Road",
                                     "Circuit Board", "Chevron Sieve", "Blob Field", "The Funnel", "Core Breach"};
    FrontModel m;
    for (usize i = 0; i < 10; ++i) {
        CampaignLevel l;
        l.number = static_cast<u32>(i + 1);
        l.name = kNames[i];
        l.level_index = 100 + i * 2;
        l.cleared = i < cleared;
        l.thumb.lanes.push_back(LevelThumb::Lane{{Vec2{-0.2f, 0.3f}, Vec2{0.5f, 0.3f}, Vec2{0.5f, 0.7f},
                                                  Vec2{1.2f, 0.7f}},
                                                 0.14f});
        l.thumb.obstacles.push_back(LevelThumb::Obstacle{{Vec2{0.3f, 0.45f}, Vec2{0.45f, 0.45f},
                                                          Vec2{0.45f, 0.6f}, Vec2{0.38f, 0.52f},
                                                          Vec2{0.3f, 0.6f}}});
        l.thumb.spawns.push_back(Vec2{0.05f, 0.3f});
        m.campaign.push_back(l);
    }
    for (usize i = 0; i < m.campaign.size(); ++i) m.campaign[i].locked = !campaign_unlocked(m.campaign, i);
    return m;
}

} // namespace

TEST_CASE("campaign gating: each level opens when the one before is cleared", "[ui][menu]") {
    FrontModel m = campaign(3);
    CHECK_FALSE(m.campaign[0].locked);
    CHECK_FALSE(m.campaign[3].locked);   // the next to play
    CHECK(m.campaign[4].locked);
    CHECK(m.campaign[9].locked);
    CHECK(campaign_frontier(m.campaign) == 3);
    CHECK(campaign_frontier(campaign(0).campaign) == 0);
    // Everything cleared: the frontier stays on the last level.
    CHECK(campaign_frontier(campaign(10).campaign) == 9);
}

TEST_CASE("the shipped campaign: ten levels in file order, gated, with thumbnails", "[ui][menu]") {
    std::vector<LevelEntry> entries;
    std::vector<game::LevelDef> defs;
    for (const std::string& path : platform::list_files(platform::asset_path("levels"), ".json")) {
        game::LevelLoader loader;
        game::LevelDef def;
        if (!loader.load_file(path, def).ok) continue;
        LevelEntry e;
        e.path = path;
        e.level_id = def.name;
        e.display_name = def.display_name.empty() ? def.name : def.display_name;
        entries.push_back(e);
        defs.push_back(std::move(def));
    }
    std::vector<CampaignLevel> c = app::make_campaign(entries, defs);
    REQUIRE(c.size() == 10);
    CHECK(c[0].name == "First Bend");
    CHECK(c[9].name == "Core Breach");
    for (usize i = 0; i < c.size(); ++i) {
        CAPTURE(c[i].name);
        CHECK(c[i].number == i + 1);
        CHECK(app::is_campaign_level(entries[c[i].level_index].path));
        CHECK(c[i].locked == (i > 0));
        // Every level has a lane in its thumbnail, inside the square.
        REQUIRE_FALSE(c[i].thumb.lanes.empty());
        for (const LevelThumb::Lane& l : c[i].thumb.lanes) {
            CHECK(l.width > 0.01f);
            CHECK(l.width < 0.5f);
        }
        CHECK_FALSE(c[i].thumb.spawns.empty());
    }
    // Clearing the first opens the second, and nothing past it.
    entries[c[0].level_index].cleared = true;
    app::refresh_campaign(c, entries);
    CHECK_FALSE(c[1].locked);
    CHECK(c[2].locked);
    // First Bend's lane enters from the left along the top of the thumbnail
    // (the world is y-up, the thumbnail y-down), as in the canvas.
    const std::vector<Vec2>& lane = c[0].thumb.lanes.front().points;
    CHECK(lane.front().x < 0.1f);
    CHECK(lane.front().y < 0.5f);
    CHECK(lane.back().y > 0.5f);
}

TEST_CASE("main menu: Play Game goes to Strengthen Immunity, Quit quits", "[ui][menu]") {
    Harness h;
    h.open(FrontScreen::MainMenu, FrontModel{});
    CHECK(h.front->current() == FrontScreen::MainMenu);
    CHECK(h.shown("menu/title"));
    CHECK(h.shown("menu/mascot"));
    // Nothing clicked, nothing reported: a default of anything else would
    // fire a transition on its own.
    CHECK(h.front->take_result().action == MenuAction::None);
    CHECK(h.click("menu/play").action == MenuAction::OpenImmunityTree);
    CHECK(h.click("menu/quit").action == MenuAction::Quit);
}

TEST_CASE("level select: picks up at the frontier; locked levels refuse; Play starts", "[ui][menu]") {
    Harness h;
    const FrontModel m = campaign(3);
    h.open(FrontScreen::LevelSelect, m);
    for (int n = 1; n <= 10; ++n) CHECK(h.shown("levels/cell" + std::to_string(n)));
    // Level 4 is next: selected on arrival, gold halo, Play on offer.
    CHECK(h.front->selected_level() == 3);
    auto* cell4 = dynamic_cast<LevelCell*>(h.gui.find("levels/cell4"));
    REQUIRE(cell4 != nullptr);
    CHECK(cell4->frontier);
    CHECK(cell4->selected);
    CHECK(h.shown("levels/play"));

    // A locked level shakes and says why; the selection does not move.
    auto* cell6 = dynamic_cast<LevelCell*>(h.gui.find("levels/cell6"));
    REQUIRE(cell6 != nullptr);
    CHECK_FALSE(cell6->enabled);
    CHECK(cell6->tooltip.find("Ring Road") != std::string::npos);
    CHECK(h.click("levels/cell6").action == MenuAction::None);
    CHECK(h.front->selected_level() == 3);

    // A cleared level selects on the first click and starts on the second.
    CHECK(h.click("levels/cell2").action == MenuAction::None);
    CHECK(h.front->selected_level() == 1);
    const MenuResult again = h.click("levels/cell2");
    CHECK(again.action == MenuAction::StartLevel);
    CHECK(again.level_index == m.campaign[1].level_index);

    const MenuResult play = h.click("levels/play");
    CHECK(play.action == MenuAction::StartLevel);
    CHECK(play.level_index == m.campaign[1].level_index);
    CHECK(h.click("levels/back").action == MenuAction::OpenImmunityTree);

    // The gym's `ui level <n>` path.
    CHECK(h.front->select_level(0));
    CHECK_FALSE(h.front->select_level(7));
    CHECK(h.front->selected_level() == 0);
}

TEST_CASE("level select: an empty campaign says why instead of showing an empty map", "[ui][menu]") {
    Harness h;
    h.open(FrontScreen::LevelSelect, FrontModel{});
    CHECK(h.shown("levels/empty"));
    CHECK_FALSE(h.shown("levels/play"));
    CHECK(h.front->selected_level() == -1);
}

TEST_CASE("results: level cleared pays out and offers the next level", "[ui][menu]") {
    Harness h;
    FrontModel m = campaign(1);
    m.level_name = "First Bend";
    m.campaign_slot = 0;
    m.unlocked_next = true;
    m.run.valid = true;
    m.run.memory_cells = 203;
    m.run.antibodies = 1;
    m.run.first_clear = true;
    h.open(FrontScreen::Victory, m);
    CHECK(h.shown("victory/panel/rewards/memory"));
    CHECK(h.shown("victory/panel/rewards/antibody"));
    CHECK(h.shown("victory/panel/rewards/unlocked"));
    auto* unlocked = dynamic_cast<gui::Label*>(h.gui.find("victory/panel/rewards/unlocked/name"));
    REQUIRE(unlocked != nullptr);
    CHECK(unlocked->text() == "Island Climb unlocked");

    CHECK(h.click("victory/panel/buttons/tree").action == MenuAction::OpenImmunityTree);
    const MenuResult next = h.click("victory/panel/buttons/next");
    CHECK(next.action == MenuAction::StartLevel);
    CHECK(next.level_index == m.campaign[1].level_index);
    CHECK(h.click("victory/panel/buttons/replay").action == MenuAction::RestartLevel);

    // A replay of a cleared level: no Antibody row, no unlock row.
    m.run.antibodies = 0;
    m.unlocked_next = false;
    h.open(FrontScreen::Victory, m);
    CHECK(h.shown("victory/panel/rewards/memory"));
    CHECK_FALSE(h.shown("victory/panel/rewards/antibody"));
    CHECK_FALSE(h.shown("victory/panel/rewards/unlocked"));

    // The last campaign level has no next one: Levels instead.
    FrontModel last = campaign(10);
    last.campaign_slot = 9;
    last.run.valid = true;
    h.open(FrontScreen::Victory, last);
    CHECK_FALSE(h.shown("victory/panel/buttons/next"));
    CHECK(h.click("victory/panel/buttons/levels").action == MenuAction::OpenLevelSelect);
}

TEST_CASE("results: level failed still pays, and offers retry", "[ui][menu]") {
    Harness h;
    FrontModel m = campaign(3);
    m.level_name = "Twin Channels";
    m.campaign_slot = 3;
    m.run.valid = true;
    m.run.memory_cells = 70;
    h.open(FrontScreen::Defeat, m);
    auto* title = dynamic_cast<gui::Label*>(h.gui.find("defeat/panel/title"));
    REQUIRE(title != nullptr);
    CHECK(title->text() == "Level failed");
    CHECK(h.shown("defeat/panel/rewards/memory"));
    CHECK(h.click("defeat/panel/buttons/tree").action == MenuAction::OpenImmunityTree);
    CHECK(h.click("defeat/panel/buttons/retry").action == MenuAction::RestartLevel);
    CHECK(h.click("defeat/panel/buttons/levels").action == MenuAction::OpenLevelSelect);
}

TEST_CASE("results: a playtest goes back to the editor, a sandbox run says it paid nothing", "[ui][menu]") {
    Harness h;
    for (FrontScreen s : {FrontScreen::Victory, FrontScreen::Defeat}) {
        FrontModel m;
        m.playtest = true;
        m.run.valid = true;
        m.run.sandbox = true;
        h.open(s, m);
        const std::string root = front_screen_name(s);
        CHECK_FALSE(h.shown(root + "/panel/rewards"));
        CHECK_FALSE(h.shown(root + "/panel/buttons/tree"));
        CHECK(h.click(root + "/panel/buttons/restart").action == MenuAction::RestartLevel);
        CHECK(h.click(root + "/panel/buttons/editor").action == MenuAction::BackToEditor);
    }
    FrontModel sandbox;
    sandbox.run.valid = true;
    sandbox.run.sandbox = true;
    h.open(FrontScreen::Defeat, sandbox);
    CHECK(h.shown("defeat/panel/rewards/sandbox"));
    CHECK_FALSE(h.shown("defeat/panel/rewards/memory"));
}

TEST_CASE("pause: resume, restart, abandon", "[ui][menu]") {
    Harness h;
    FrontModel m;
    m.level_name = "Ring Road";
    h.open(FrontScreen::Pause, m);
    CHECK(h.click("pause/panel/buttons/resume").action == MenuAction::Resume);
    CHECK(h.click("pause/panel/buttons/restart").action == MenuAction::RestartLevel);
    CHECK(h.click("pause/panel/buttons/menu").action == MenuAction::Back);
}

TEST_CASE("screens crossfade; a screen on its way out takes no clicks", "[ui][menu]") {
    Harness h;
    h.open(FrontScreen::MainMenu, FrontModel{});
    h.frame(FrontScreen::LevelSelect, campaign(0));
    // Both trees exist mid-transition; only the incoming one is live.
    REQUIRE(h.gui.find("menu/play") != nullptr);
    CHECK_FALSE(h.gui.click("menu/play"));
    CHECK(h.front->take_result().action == MenuAction::None);
    for (int i = 0; i < 40; ++i) h.frame(FrontScreen::LevelSelect, campaign(0));
    CHECK(h.gui.find("menu/play") == nullptr);
    CHECK(h.shown("levels/play"));

    // None fades the front end away (a level starting).
    for (int i = 0; i < 40; ++i) h.frame(FrontScreen::None, FrontModel{});
    CHECK(h.gui.find("levels") == nullptr);
    CHECK(h.front->current() == FrontScreen::None);
}

TEST_CASE("the front-end screens render (PNGs for review)", "[ui][menu][gl]") {
    platform::Window window;
    if (!platform::create_headless_gl(window, 1920, 1080)) {
        WARN("headless GL context unavailable in this environment; skipping");
        return;
    }
    Harness h(true);
    FrontModel results = campaign(1);
    results.level_name = "First Bend";
    results.campaign_slot = 0;
    results.unlocked_next = true;
    results.run.valid = true;
    results.run.memory_cells = 203;
    results.run.antibodies = 1;
    FrontModel tree;
    {
        const game::MetaConfig cfg;
        game::MetaProgression meta;
        meta.reset_to_new_game();
        meta.credit(900, 3);
        for (game::TreeNode n : {game::TreeNode::NeutrophilRoundDamage, game::TreeNode::NeutrophilRoundDamage,
                                 game::TreeNode::NeutrophilVolleyCadence, game::TreeNode::CytotoxicRoot,
                                 game::TreeNode::BoneMarrowReserve}) {
            meta.purchase(n, cfg);
        }
        tree.tree = app::make_tree_model(meta, cfg);
    }
    struct Shot { FrontScreen screen; FrontModel model; const char* file; };
    const Shot shots[] = {
        {FrontScreen::MainMenu, FrontModel{}, "front_menu.png"},
        {FrontScreen::Tree, tree, "front_tree.png"},
        {FrontScreen::LevelSelect, campaign(3), "front_levels.png"},
        {FrontScreen::Victory, results, "front_victory.png"},
        {FrontScreen::Defeat, results, "front_defeat.png"},
        {FrontScreen::Pause, results, "front_pause.png"},
    };
    for (const Shot& s : shots) {
        CAPTURE(s.file);
        h.open(s.screen, s.model);
        for (int i = 0; i < 10; ++i) h.frame(s.screen, s.model);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 1920, 1080);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        h.gui.render(1920, 1080);
        glFinish();
        std::vector<u8> px;
        render::read_framebuffer_rgba(px, 1920, 1080);
        render::write_png_rgba(s.file, px.data(), 1920, 1080);
        // Every screen covers the frame (a backdrop or a veil): no pixel
        // left at the black clear colour in the corners.
        const usize corners[4] = {0, 1919, 1919u * 1080u, 1920u * 1080u - 1};
        for (usize c : corners) {
            CAPTURE(c);
            CHECK(px[c * 4] + px[c * 4 + 1] + px[c * 4 + 2] > 0);
        }
        // Each stencil-clipped level thumbnail splits the batch (write the clip,
        // draw inside it, pop it), so the level map is the busiest screen.
        CHECK(h.gui.render_stats().draw_calls <= 64);
    }
}


// ---- Strengthen Immunity --------------------------------------------------------------

TEST_CASE("tree layout: every node of the game's tree has its place from the canvas", "[ui][menu][meta]") {
    TreeLayout layout;
    std::string err;
    REQUIRE(load_tree_layout(platform::asset_path("ui/tree_layout.json"), layout, &err));
    INFO(err);
    CHECK(layout.nodes.size() == game::kTreeNodeCount);
    for (u32 i = 0; i < game::kTreeNodeCount; ++i) {
        const char* key = game::tree_node(static_cast<game::TreeNode>(i)).key;
        CAPTURE(key);
        CHECK(layout.nodes.count(key) == 1);
    }
    // Every vessel but the two that loop out to the abilities belongs to a node.
    usize owned = 0;
    for (const TreeLayout::Vessel& v : layout.vessels) {
        if (!v.node.empty()) {
            ++owned;
            CHECK(layout.nodes.count(v.node) == 1);
        }
    }
    CHECK(owned == game::kTreeNodeCount);
    CHECK(layout.labels.size() == 14);
}

TEST_CASE("tree: select a node, Grow buys it, Play goes to the campaign", "[ui][menu][meta]") {
    Harness h;
    const game::MetaConfig cfg;
    game::MetaProgression meta;
    meta.reset_to_new_game();
    meta.credit(500, 1);
    FrontModel m;
    m.tree = app::make_tree_model(meta, cfg);
    h.open(FrontScreen::Tree, m);
    TreeScreen* tree = h.front->tree();
    REQUIRE(tree != nullptr);
    CHECK(h.shown("tree/info/grow"));
    // The Neutrophil comes owned; the first thing on offer is selected.
    const TreeNodeView* sel = m.tree.find(tree->selected());
    REQUIRE(sel != nullptr);
    CHECK(sel->state == TreeNodeState::Available);

    // A locked node (a capstone before its threshold) selects, says why,
    // and Grow refuses.
    CHECK(h.click("tree/neutrophil.capstone").action == MenuAction::None);
    CHECK(tree->selected() == "neutrophil.capstone");
    auto* req = dynamic_cast<gui::Label*>(h.gui.find("tree/info/words/req"));
    REQUIRE(req != nullptr);
    CHECK(req->visible);
    CHECK(req->text().find("6 points") != std::string::npos);
    CHECK(h.click("tree/info/grow").action == MenuAction::None);

    // Round Damage is buyable: Grow reports the purchase of that node.
    CHECK(h.click("tree/neutrophil.round_damage").action == MenuAction::None);
    auto* name = dynamic_cast<gui::Label*>(h.gui.find("tree/info/words/name"));
    REQUIRE(name != nullptr);
    CHECK(name->text() == "Round Damage");
    const MenuResult grow = h.click("tree/info/grow");
    CHECK(grow.action == MenuAction::PurchaseNode);
    CHECK(grow.node == static_cast<u32>(game::TreeNode::NeutrophilRoundDamage));

    // app/ buys it; the next model shows level 1 without losing the selection.
    REQUIRE(meta.purchase(game::TreeNode::NeutrophilRoundDamage, cfg) == game::MetaProgression::PurchaseResult::Ok);
    m.tree = app::make_tree_model(meta, cfg);
    h.frame(FrontScreen::Tree, m);
    CHECK(tree == h.front->tree());
    CHECK(tree->selected() == "neutrophil.round_damage");
    auto* level = dynamic_cast<gui::Label*>(h.gui.find("tree/info/words/level"));
    REQUIRE(level != nullptr);
    CHECK(level->text() == "1/5");

    CHECK(h.click("tree/play").action == MenuAction::OpenLevelSelect);
    CHECK(h.click("tree/back").action == MenuAction::Back);
    // Nothing refundable yet beyond one level, but respec is on offer once
    // something was bought and the fee is affordable.
    CHECK(h.click("tree/wallet/respec").action == MenuAction::Respec);
}

TEST_CASE("tree model: the game's rules, as the screen shows them", "[ui][menu][meta]") {
    const game::MetaConfig cfg;
    game::MetaProgression meta;
    meta.reset_to_new_game();
    TreeModel t = app::make_tree_model(meta, cfg);
    REQUIRE(t.nodes.size() == game::kTreeNodeCount);
    const TreeNodeView* neutrophil = t.find("neutrophil.unlock");
    REQUIRE(neutrophil != nullptr);
    CHECK(neutrophil->state == TreeNodeState::Maxed);
    const TreeNodeView* drain = t.find("cytotoxic.drain");
    REQUIRE(drain != nullptr);
    CHECK(drain->state == TreeNodeState::Locked);
    CHECK(drain->requirement == "Needs Cytotoxic T");
    const TreeNodeView* marrow = t.find("hub.bone_marrow_reserve");
    REQUIRE(marrow != nullptr);
    CHECK(marrow->state == TreeNodeState::Short);  // a new save has no Memory Cells
    CHECK(marrow->cost_memory > 0);
    CHECK(t.branch_points[0] == 0);
}

TEST_CASE("front-end screens: CPU cost per frame", "[ui][menu][perf]") {
    // Layout, input, animation and recording the draw list: the gui's whole
    // CPU side of a frame (the GL upload is the backend's, measured by the
    // render tests). The UI budget is 0.5 ms a frame on a release build;
    // the bound here is looser because the tests may run unoptimized and
    // alongside other work.
    Harness h;
    FrontModel tree;
    {
        const game::MetaConfig cfg;
        game::MetaProgression meta;
        meta.reset_to_new_game();
        tree.tree = app::make_tree_model(meta, cfg);
    }
    FrontModel results = campaign(1);
    results.run.valid = true;
    results.campaign_slot = 0;
    struct Case { FrontScreen screen; const FrontModel* model; };
    const FrontModel levels = campaign(3);
    const FrontModel none;
    const Case cases[] = {{FrontScreen::MainMenu, &none}, {FrontScreen::Tree, &tree},
                          {FrontScreen::LevelSelect, &levels}, {FrontScreen::Victory, &results}};
    for (const Case& c : cases) {
        CAPTURE(front_screen_name(c.screen));
        h.open(c.screen, *c.model);
        gui::DrawList dl;
        // Warm up: glyphs and icons bake on first use, once.
        for (int i = 0; i < 10; ++i) {
            h.frame(c.screen, *c.model);
            dl.reset(Vec2{1920, 1080}, 1.0f, 0.0f);
            h.gui.draw(dl);
        }
        constexpr int kFrames = 120;
        f64 frame_ms = 0.0, draw_ms = 0.0;
        for (int i = 0; i < kFrames; ++i) {
            WallClock a;
            h.frame(c.screen, *c.model);
            frame_ms += a.elapsed_ms();
            WallClock b;
            dl.reset(Vec2{1920, 1080}, 1.0f, static_cast<f32>(i) / 60.0f);
            h.gui.draw(dl);
            draw_ms += b.elapsed_ms();
        }
        const f64 ms = (frame_ms + draw_ms) / kFrames;
        std::fprintf(stderr, "[ui perf] %-8s %.3f ms/frame (update %.3f, draw %.3f; %zu vertices)\n",
                     front_screen_name(c.screen), ms, frame_ms / kFrames, draw_ms / kFrames, dl.vertices().size());
        CHECK(ms < 4.0);
    }
}
