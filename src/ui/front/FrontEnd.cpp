#include "ui/front/FrontEnd.h"

#include "gui/anim/Anim.h"
#include "gui/core/Gui.h"
#include "gui/style/Theme.h"
#include "gui/widgets/Builders.h"
#include "gui/widgets/Widgets.h"
#include "core/Log.h"
#include "platform/FileIO.h"
#include "ui/front/Backdrop.h"
#include "ui/front/LevelCell.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace immune::ui {

using namespace gui;

namespace {

constexpr f32 kFadeIn = 0.24f;
constexpr f32 kFadeOut = 0.18f;

void fill_parent(Widget& w) {
    anchor(w, Vec2{0, 0}, Vec2{0, 0});
    w.layout.width = Size::pct(1.0f);
    w.layout.height = Size::pct(1.0f);
}

/// A canvas-sized (1920 x 1080) stage pinned to a point of the screen, so
/// art laid out in canvas pixels keeps its composition at any aspect ratio.
/// Anonymous, so it does not show in widget paths.
Widget& stage(Widget& parent, Vec2 at) {
    Widget& s = parent.emplace<Widget>();
    anchor(s, at, at);
    fixed_size(s, 1920.0f, 1080.0f);
    return s;
}

/// Places `w` with its `pivot` at canvas pixel `p` of a stage.
void place(Widget& w, Vec2 p, Vec2 pivot = Vec2{0.5f, 0.5f}) { anchor(w, Vec2{0, 0}, pivot, p); }

/// An icon with a looping motion: drifting pathogens, the mascot, its prey.
class Floater : public Icon {
public:
    Floater(std::string id, std::string name, Vec2 size, f32 rotation_deg) : Icon(std::move(id), std::move(name)) {
        fixed_size(*this, size.x, size.y);
        rotation_ = rotation_deg * math::kPi / 180.0f;
    }
    enum class Motion { Bob, Wobble, Throb };
    Motion motion = Motion::Bob;
    f32 phase = 0.0f;

    void update(f32 dt) override {
        t_ += dt;
        const f32 t = t_ + phase;
        switch (motion) {
            case Motion::Bob:
                anim_transform = Affine2::translate(Vec2{std::sin(t * 0.9f) * 4.0f, std::sin(t * 1.3f) * 6.0f}) *
                                 Affine2::rotate(rotation_ + std::sin(t * 0.7f) * 0.12f);
                break;
            case Motion::Wobble:
                anim_transform = Affine2::rotate(rotation_ + loop::wobble_rotation(t)) *
                                 Affine2::scale(loop::wobble_scale(t));
                break;
            case Motion::Throb:
                anim_transform = Affine2::scale(1.0f + (loop::throb_scale(t, 2.4f) - 1.0f));
                break;
        }
    }

private:
    f32 rotation_ = 0.0f;
    f32 t_ = 0.0f;
};

/// A button that also runs the canvas's `wobble` keyframe (Levels' Play).
class WobbleButton : public Button {
public:
    using Button::Button;
    void update(f32 dt) override {
        Button::update(dt);
        t_ += dt;
        anim_transform = anim_transform * Affine2::rotate(loop::wobble_rotation(t_)) *
                         Affine2::scale(loop::wobble_scale(t_));
    }

private:
    f32 t_ = 0.0f;
};

/// A membrane button with a centred label (and optionally a play glyph).
Button& labelled_button(Widget& parent, Theme& th, std::string id, const char* shape, Vec2 size,
                        const std::string& label, const char* style, bool glyph_after = false,
                        f32 glyph = 0.0f, bool wobble = false) {
    Button& b = wobble ? parent.emplace<WobbleButton>(std::move(id), th.shape(shape))
                       : parent.emplace<Button>(std::move(id), th.shape(shape));
    fixed_size(b, size.x, size.y);
    b.layout.axis = Axis::Row;
    b.layout.align = Align::Center;
    b.layout.justify = Justify::Center;
    b.layout.gap = 12.0f;
    if (glyph > 0.0f && !glyph_after) icon(b, "glyph", "glyph_play", glyph);
    text(b, "label", label, th.text(style), TextAlign::Center);
    if (glyph > 0.0f && glyph_after) icon(b, "glyph", "glyph_play", glyph);
    return b;
}

// Level map geometry (canvas Levels artboard): the serpentine vessel and the
// ten cell centres along it, first row left to right from the bottom.
constexpr const char* kLevelVessel =
    "M-40 800 L1590 800 C1850 800 1850 510 1590 510 L750 510 C490 510 490 220 750 220 L1960 220";
constexpr std::array<Vec2, 10> kCellCentres = {{{330, 800}, {750, 800}, {1170, 800}, {1590, 800}, {1590, 510},
                                                {1170, 510}, {750, 510}, {750, 220}, {1170, 220}, {1590, 220}}};

u64 mix(u64 h, u64 v) { return (h ^ v) * 0x100000001B3ULL; }
u64 mix_str(u64 h, const std::string& s) {
    for (char c : s) h = mix(h, static_cast<u8>(c));
    return mix(h, 0xFF);
}

} // namespace

// ---- Screen roots -------------------------------------------------------------------

/// One screen's widget tree: fades in (budding up from 97%) or out as a group.
class FrontEnd::ScreenRoot : public Widget {
public:
    ScreenRoot(std::string id, FrontScreen s) : Widget(std::move(id)), screen(s) {
        fill_parent(*this);
        layout.axis = Axis::Stack;
        group_opacity = true;
        fade_.snap(0.0f);
        fade_.start(1.0f, kFadeIn, Ease::OutCubic);
        opacity = 0.0f;
    }
    FrontScreen screen;

    void leave() {
        leaving_ = true;
        accepts_pointer = false;
        fade_.start(0.0f, kFadeOut, Ease::InOutCubic);
    }
    bool leaving() const { return leaving_; }
    bool gone() const { return leaving_ && fade_.done(); }
    void finish() { fade_.snap(fade_.target()); apply(); }

    void update(f32 dt) override {
        fade_.update(dt);
        apply();
    }

private:
    void apply() {
        opacity = fade_.value();
        const f32 s = leaving_ ? 1.0f : 0.97f + 0.03f * fade_.value();
        anim_transform = Affine2::scale(s);
    }
    Tween fade_{0.0f};
    bool leaving_ = false;
};

// ---- FrontEnd -----------------------------------------------------------------------

const char* front_screen_name(FrontScreen s) {
    switch (s) {
        case FrontScreen::None: return "none";
        case FrontScreen::MainMenu: return "menu";
        case FrontScreen::Tree: return "tree";
        case FrontScreen::LevelSelect: return "levels";
        case FrontScreen::Pause: return "pause";
        case FrontScreen::Victory: return "victory";
        case FrontScreen::Defeat: return "defeat";
    }
    return "none";
}

FrontEnd::FrontEnd(Gui& gui, const std::string& tree_layout_path) : gui_(gui) {
    const std::string path = tree_layout_path.empty() ? platform::asset_path("ui/tree_layout.json") : tree_layout_path;
    std::string err;
    if (!load_tree_layout(path, tree_layout_, &err)) IMMUNE_LOG_ERROR("%s", err.c_str());
    // Anonymous: screen paths start at the screen ("levels/cell4").
    layer_root_ = &gui_.layer(LayerId::Modal).emplace<Widget>();
    fill_parent(*layer_root_);
    layer_root_->layout.axis = Axis::Stack;
}

FrontEnd::~FrontEnd() {
    if (layer_root_ != nullptr) gui_.layer(LayerId::Modal).remove(*layer_root_);
}

u64 FrontEnd::signature(FrontScreen s, const FrontModel& m) {
    u64 h = mix(0xCBF29CE484222325ULL, static_cast<u64>(s));
    switch (s) {
        case FrontScreen::LevelSelect:
            for (const CampaignLevel& l : m.campaign) {
                h = mix(h, (l.cleared ? 1u : 0u) | (l.locked ? 2u : 0u) | (static_cast<u64>(l.level_index) << 2));
                h = mix_str(h, l.name);
            }
            break;
        case FrontScreen::Pause:
            h = mix_str(h, m.level_name);
            break;
        case FrontScreen::Victory:
        case FrontScreen::Defeat:
            h = mix(h, m.run.valid ? 1 : 0);
            h = mix(h, m.run.sandbox ? 1 : 0);
            h = mix(h, m.run.memory_cells);
            h = mix(h, m.run.antibodies);
            h = mix(h, m.playtest ? 1 : 0);
            h = mix(h, static_cast<u64>(m.campaign_slot + 1));
            h = mix(h, m.unlocked_next ? 1 : 0);
            h = mix(h, m.campaign.size());
            h = mix_str(h, m.level_name);
            break;
        default:
            break;
    }
    return h;
}

void FrontEnd::show(FrontScreen s, const FrontModel& m) {
    // Screens that finished fading out are dropped.
    for (usize i = 0; i < leaving_.size();) {
        if (leaving_[i]->gone()) {
            layer_root_->remove(*leaving_[i]);
            leaving_.erase(leaving_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    const u64 sig = signature(s, m);
    if (s == current_ && sig == signature_) {
        // The tree updates in place: a purchase changes one node, and a
        // rebuild would drop hover and selection.
        if (tree_ != nullptr) tree_->sync(m.tree);
        return;
    }
    const bool same_screen = s == current_;
    current_ = s;
    signature_ = sig;
    if (tree_ != nullptr) {
        tree_view_ = tree_->view();
        tree_.reset();
    }

    if (live_ != nullptr) {
        if (same_screen) {
            // The data changed under an open screen: rebuild it in place.
            layer_root_->remove(*live_);
        } else {
            live_->leave();
            leaving_.push_back(live_);
        }
        live_ = nullptr;
    }
    cells_.clear();
    play_button_ = nullptr;
    if (s == FrontScreen::None) return;

    // A screen coming back while its previous self is still fading out
    // replaces it outright (two trees must not share one path).
    const std::string id = front_screen_name(s);
    for (usize i = 0; i < leaving_.size(); ++i) {
        if (leaving_[i]->id() == id) {
            layer_root_->remove(*leaving_[i]);
            leaving_.erase(leaving_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    live_ = &layer_root_->emplace<ScreenRoot>(id, s);
    if (same_screen) live_->finish();
    build(*live_, s, m);
}

void FrontEnd::finish_transitions() {
    if (live_ != nullptr) live_->finish();
    for (ScreenRoot* r : leaving_) layer_root_->remove(*r);
    leaving_.clear();
}

MenuResult FrontEnd::take_result() {
    const MenuResult r = pending_;
    pending_ = MenuResult{};
    return r;
}

void FrontEnd::emit(MenuResult r) {
    if (pending_.action == MenuAction::None) pending_ = r;
}

void FrontEnd::build(ScreenRoot& root, FrontScreen s, const FrontModel& m) {
    switch (s) {
        case FrontScreen::MainMenu: build_main(root); break;
        case FrontScreen::Tree: build_tree(root, m); break;
        case FrontScreen::LevelSelect: build_levels(root, m); break;
        case FrontScreen::Pause: build_pause(root, m); break;
        case FrontScreen::Victory: build_results(root, m, true); break;
        case FrontScreen::Defeat: build_results(root, m, false); break;
        case FrontScreen::None: break;
    }
}

// ---- Main menu ----------------------------------------------------------------------

void FrontEnd::build_main(Widget& root) {
    Theme& th = gui_.theme();
    fill_parent(root.emplace<TissueBackdrop>("backdrop"));

    // The vessel, the drifting pathogens and the macrophage keep to the right
    // of the screen; the title and buttons to the left.
    Widget& art = stage(root, Vec2{1.0f, 0.5f});
    VesselStroke& vessel = art.emplace<VesselStroke>("vessel");
    fill_parent(vessel);
    vessel.path.svg("M1050 1120C1120 820 1500 760 1560 520S1760 180 2000 120");
    auto layer = [&](f32 width, const char* color, f32 alpha = 1.0f) {
        VesselStroke::Layer l;
        l.style.width = width;
        l.style.color = with_alpha(th.color(color), alpha);
        l.style.cap = LineCap::Butt;
        vessel.layers.push_back(l);
    };
    layer(210.0f, "vessel_wall");
    layer(190.0f, "vessel_rim");
    layer(160.0f, "vessel_lumen");
    layer(70.0f, "vessel_core", 0.8f);

    // Canvas positions, scales and headings of the pathogens in the vessel.
    struct Drift { const char* icon; Vec2 at; f32 scale; f32 rot; };
    static constexpr Drift kDrift[] = {
        {"pathogen_bacteria", {1207.7f, 159.0f}, 0.69f, 16.2f}, {"pathogen_virus", {1017.4f, 587.4f}, 0.74f, 0.0f},
        {"pathogen_parasite", {1673.3f, 273.1f}, 0.80f, 0.0f}, {"pathogen_bacteria", {1317.0f, 808.6f}, 0.60f, 169.7f},
        {"pathogen_virus", {1022.6f, 675.1f}, 0.74f, 0.0f},    {"pathogen_virus", {1170.2f, 357.5f}, 0.53f, 0.0f},
        {"pathogen_parasite", {1533.1f, 704.8f}, 0.80f, 0.0f}, {"pathogen_virus", {1219.0f, 297.4f}, 0.53f, 0.0f},
        {"pathogen_parasite", {1822.4f, 890.9f}, 0.80f, 0.0f}, {"pathogen_virus", {1739.6f, 66.9f}, 0.67f, 0.0f},
        {"pathogen_parasite", {1776.4f, 874.1f}, 0.80f, 0.0f}, {"pathogen_virus", {1171.7f, 466.8f}, 0.63f, 0.0f},
        {"pathogen_bacteria", {1142.0f, 352.9f}, 0.53f, 83.3f}, {"pathogen_bacteria", {1877.8f, 558.1f}, 0.78f, 26.1f},
        {"pathogen_virus", {1592.9f, 123.8f}, 0.69f, 0.0f},    {"pathogen_bacteria", {1388.2f, 236.4f}, 0.80f, 57.7f},
    };
    i32 n = 0;
    for (const Drift& d : kDrift) {
        // The HUD icons draw each pathogen at ~1.05-1.15x in a 64 box, turned
        // -25 degrees for the bacterium; undo that to match the canvas.
        const bool bact = d.icon[9] == 'b';
        const f32 px = 64.0f * d.scale / 1.05f;
        Floater& f = art.emplace<Floater>("drift" + std::to_string(n), d.icon, Vec2{px, px}, d.rot + (bact ? 25.0f : 0.0f));
        f.phase = static_cast<f32>(n) * 1.7f;
        place(f, d.at);
        ++n;
    }
    // The macrophage (canvas: translate(1080 360) scale(1.25) of a 420 x 320
    // drawing) reaching for a bacterium that wobbles in its grip.
    Floater& mascot = art.emplace<Floater>("mascot", "mascot_macrophage", Vec2{525.0f, 400.0f}, 0.0f);
    mascot.motion = Floater::Motion::Throb;
    place(mascot, Vec2{1080.0f, 360.0f}, Vec2{0, 0});
    const f32 prey = 64.0f * 1.9f * 1.25f / 1.05f;
    Floater& caught = art.emplace<Floater>("prey", "pathogen_bacteria", Vec2{prey, prey}, -35.0f + 25.0f);
    caught.motion = Floater::Motion::Wobble;
    place(caught, Vec2{1080.0f + 430.0f * 1.25f, 360.0f + 176.0f * 1.25f});

    Widget& left = stage(root, Vec2{0.0f, 0.5f});
    Label& title = text(left, "title", "IMMUNE", th.text("menu_title"));
    place(title, Vec2{150.0f, 200.0f}, Vec2{0, 0});
    Button& play = labelled_button(left, th, "play", "button.cta", Vec2{460, 120}, "Play Game", "menu_cta", false, 40.0f);
    place(play, Vec2{160.0f, 520.0f}, Vec2{0, 0});
    play.tooltip = "Strengthen your immunity, then pick a level";
    play.on_click = [this] { emit(MenuResult{MenuAction::OpenImmunityTree}); };
    Button& quit = labelled_button(left, th, "quit", "button.flesh", Vec2{400, 86}, "Quit to Desktop", "menu_button");
    place(quit, Vec2{190.0f, 672.0f}, Vec2{0, 0});
    quit.on_click = [this] { emit(MenuResult{MenuAction::Quit}); };
}

// ---- Strengthen Immunity ------------------------------------------------------------

void FrontEnd::build_tree(Widget& root, const FrontModel& m) {
    tree_ = std::make_unique<TreeScreen>(gui_, root, tree_layout_, m.tree, [this](MenuResult r) { emit(r); },
                                         tree_view_ ? &*tree_view_ : nullptr);
}

// ---- Level select -------------------------------------------------------------------

void FrontEnd::build_levels(Widget& root, const FrontModel& m) {
    Theme& th = gui_.theme();
    campaign_ = m.campaign;
    fill_parent(root.emplace<TissueBackdrop>("backdrop"));

    Widget& map = stage(root, Vec2{0.5f, 0.5f});
    const usize count = math::min(campaign_.size(), kCellCentres.size());
    const usize frontier = campaign_frontier(campaign_);

    // The whole vessel, pale; then the stretch the player has opened (up to
    // the next level to play) in blood colours with plasma flowing along it.
    VesselStroke& pale = map.emplace<VesselStroke>("vessel");
    fill_parent(pale);
    pale.path.svg(kLevelVessel);
    auto layer = [&](VesselStroke& v, f32 width, const char* color) {
        VesselStroke::Layer l;
        l.style.width = width;
        l.style.color = th.color(color);
        v.layers.push_back(l);
        return &v.layers.back();
    };
    layer(pale, 64.0f, "vessel_wall");
    layer(pale, 50.0f, "vessel_pale");
    layer(pale, 26.0f, "vessel_pale_core");

    if (count > 0) {
        const std::vector<Vec2>& full = pale.path.contours().front().points;
        const Vec2 target = kCellCentres[frontier < count ? frontier : count - 1];
        const bool all_open = frontier + 1 >= count && campaign_[count - 1].cleared;
        // The open stretch ends where the next level's cell sits on the
        // vessel: the nearest point on any segment (a straight run is a
        // single segment, so vertices alone would not do).
        usize cut = full.size();
        Vec2 end = full.back();
        if (!all_open) {
            f32 best = 1e30f;
            for (usize i = 0; i + 1 < full.size(); ++i) {
                const Vec2 a = full[i], ab = full[i + 1] - full[i];
                const f32 len2 = math::max(math::length_sq(ab), 1e-6f);
                const f32 t = math::saturate(((target.x - a.x) * ab.x + (target.y - a.y) * ab.y) / len2);
                const Vec2 p = a + ab * t;
                const f32 d = math::length_sq(p - target);
                if (d < best) {
                    best = d;
                    cut = i + 1;
                    end = p;
                }
            }
        }
        VesselStroke& open = map.emplace<VesselStroke>("open");
        fill_parent(open);
        open.path.move_to(full.front());
        for (usize i = 1; i < cut; ++i) open.path.line_to(full[i]);
        open.path.line_to(end);
        layer(open, 50.0f, "vessel_rim");
        layer(open, 36.0f, "vessel_lumen");
        VesselStroke::Layer* flow = layer(open, 8.0f, "lavender");
        flow->style.dash_length = 6.0f;
        flow->style.dash_gap = 16.0f;
        flow->flow_speed = 44.0f / 1.8f;
    }

    for (usize i = 0; i < count; ++i) {
        const CampaignLevel& lvl = campaign_[i];
        LevelCell& cell = map.emplace<LevelCell>(gui_, "cell" + std::to_string(lvl.number), lvl);
        place(cell, kCellCentres[i]);
        cell.frontier = i == frontier && !lvl.cleared;
        cell.tooltip = lvl.locked ? "Clear " + (i > 0 ? campaign_[i - 1].name : std::string("the level before")) + " first"
                     : lvl.cleared ? lvl.name + " (cleared)"
                                   : lvl.name + ": +1 Antibody on first clear";
        cell.on_click = [this, i] {
            if (selected_ == static_cast<i32>(i)) {
                emit(MenuResult{MenuAction::StartLevel, campaign_[i].level_index});
            } else {
                select_level(i);
            }
        };
        cells_.push_back(&cell);

        Panel& pill = map.emplace<Panel>("name" + std::to_string(lvl.number), th.shape("level.pill"));
        if (lvl.locked) pill.shape.fill = th.color("level_pill_locked");
        pill.blocks_pointer = false;
        pill.layout.axis = Axis::Stack;
        pill.layout.padding = Insets{12, 5, 12, 5};
        text(pill, "text", lvl.name, th.text("level_pill"), TextAlign::Center);
        place(pill, kCellCentres[i] + Vec2{0.0f, LevelCell::kSize * 0.5f + 6.0f}, Vec2{0.5f, 0.0f});
    }

    Button& back = root.emplace<Button>("back", th.shape("button.back"));
    fixed_size(back, 56, 56);
    anchor(back, Vec2{0, 0}, Vec2{0, 0}, Vec2{36, 32});
    back.layout.axis = Axis::Stack;
    back.layout.align = Align::Center;
    icon(back, "glyph", "glyph_back", 50.0f);
    back.tooltip = "Strengthen Immunity (Esc)";
    back.on_click = [this] { emit(MenuResult{MenuAction::OpenImmunityTree}); };
    Label& title = text(root, "title", "Campaign", th.text("screen_title"));
    anchor(title, Vec2{0, 0}, Vec2{0, 0}, Vec2{110, 30});

    if (count == 0) {
        // No campaign on disk almost always means the asset root did not
        // resolve; say so rather than showing an empty map.
        Label& none = text(root, "empty",
                           "No campaign levels found under assets/levels (campaign_NN_*.json). "
                           "Set IMMUNE_ASSET_ROOT if the game runs from outside the repository.",
                           th.text("screen_title"), TextAlign::Center);
        none.wrap = true;
        none.layout.width = Size::px(1100.0f);
        anchor(none, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f});
    }

    Button& play = labelled_button(root, th, "play", "button.cta", Vec2{320, 100}, "Play", "menu_cta", true, 34.0f,
                                   true);
    anchor(play, Vec2{1, 1}, Vec2{1, 1}, Vec2{-40, -30});
    play.tooltip = "Play the selected level";
    play.on_click = [this] {
        if (selected_ >= 0 && static_cast<usize>(selected_) < campaign_.size()) {
            emit(MenuResult{MenuAction::StartLevel, campaign_[static_cast<usize>(selected_)].level_index});
        }
    };
    play_button_ = &play;

    // Returning players land on where they left off.
    if (selected_ < 0 || static_cast<usize>(selected_) >= count || campaign_[static_cast<usize>(selected_)].locked) {
        selected_ = count > 0 ? static_cast<i32>(frontier) : -1;
    }
    sync_selection();
}

bool FrontEnd::select_level(usize pos) {
    if (pos >= campaign_.size() || pos >= kCellCentres.size() || campaign_[pos].locked) return false;
    selected_ = static_cast<i32>(pos);
    sync_selection();
    return true;
}

void FrontEnd::sync_selection() {
    for (usize i = 0; i < cells_.size(); ++i) cells_[i]->selected = static_cast<i32>(i) == selected_;
    if (play_button_ != nullptr) play_button_->visible = selected_ >= 0;
}

// ---- Pause and results --------------------------------------------------------------

namespace {

/// The dimmed game frame behind a modal panel; eats clicks.
void veil(Widget& root, Theme& th, f32 alpha) {
    Panel& v = root.emplace<Panel>("veil", th.shape("veil"));
    v.shape.fill = with_alpha(th.color("dim"), alpha);
    fill_parent(v);
}

Panel& modal_panel(Widget& root, Theme& th, const char* shape) {
    Panel& p = root.emplace<Panel>("panel", th.shape(shape));
    anchor(p, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f});
    p.layout.width = Size::px(780.0f);
    p.layout.axis = Axis::Column;
    p.layout.align = Align::Center;
    p.layout.gap = 14.0f;
    p.layout.padding = Insets{44, 40, 44, 36};
    return p;
}

/// A white reward row: icon (or number badge), name, value.
Widget& reward_row(Widget& parent, Gui& g, std::string id, const char* icon_name, const std::string& name,
                   const std::string& value, const char* value_style, const std::string& badge_text = {}) {
    Theme& th = g.theme();
    Panel& r = parent.emplace<Panel>(std::move(id), th.shape("row.reward"));
    r.layout.width = Size::fill();
    r.layout.height = Size::px(64.0f);
    r.layout.axis = Axis::Row;
    r.layout.align = Align::Center;
    r.layout.gap = 14.0f;
    r.layout.padding = Insets{18, 0, 18, 0};
    if (!badge_text.empty()) {
        Panel& b = r.emplace<Panel>("badge", th.shape("badge"));
        fixed_size(b, 36, 36);
        b.layout.axis = Axis::Stack;
        b.layout.align = Align::Center;
        text(b, "n", badge_text, th.text("badge"), TextAlign::Center);
    } else if (icon_name != nullptr) {
        icon(r, "icon", icon_name, 40.0f);
    }
    Label& n = text(r, "name", name, th.text("reward_name"));
    n.layout.grow = 1.0f;
    if (!value.empty()) text(r, "value", value, th.text(value_style));
    return r;
}

} // namespace

void FrontEnd::build_pause(Widget& root, const FrontModel& m) {
    Theme& th = gui_.theme();
    veil(root, th, 0.55f);
    Panel& p = modal_panel(root, th, "panel.result");
    text(p, "title", "Paused", th.text("result_title"), TextAlign::Center);
    if (!m.level_name.empty()) text(p, "level", m.level_name, th.text("result_sub"), TextAlign::Center);
    p.emplace<Spacer>(0.0f).layout.height = Size::px(30.0f);
    Widget& buttons = row(p, 14.0f, Align::Center, "buttons");
    Button& resume = labelled_button(buttons, th, "resume", "button.cta", Vec2{260, 84}, "Resume", "result_button_cta");
    resume.tooltip = "Back to the fight (Esc)";
    resume.on_click = [this] { emit(MenuResult{MenuAction::Resume}); };
    Button& restart = labelled_button(buttons, th, "restart", "button.soft", Vec2{190, 84}, "Restart", "result_button");
    restart.on_click = [this] { emit(MenuResult{MenuAction::RestartLevel}); };
    Button& menu = labelled_button(buttons, th, "menu", "button.flesh", Vec2{210, 84}, "Main menu", "result_button");
    menu.tooltip = "Abandon this run";
    menu.on_click = [this] { emit(MenuResult{MenuAction::Back}); };
}

void FrontEnd::build_results(Widget& root, const FrontModel& m, bool victory) {
    Theme& th = gui_.theme();
    campaign_ = m.campaign;
    veil(root, th, victory ? 0.55f : 0.6f);
    Panel& p = modal_panel(root, th, victory ? "panel.result" : "panel.result_fail");
    text(p, "title", victory ? "Level cleared" : "Level failed",
         th.text(victory ? "result_title" : "result_title_fail"), TextAlign::Center);
    if (!m.level_name.empty()) text(p, "level", m.level_name, th.text("result_sub"), TextAlign::Center);

    const bool has_next = m.campaign_slot >= 0 && static_cast<usize>(m.campaign_slot) + 1 < m.campaign.size();
    const CampaignLevel* next = has_next ? &m.campaign[static_cast<usize>(m.campaign_slot) + 1] : nullptr;

    Widget& rows = column(p, 12.0f, Align::Stretch, "rewards");
    rows.layout.width = Size::fill();
    if (!m.playtest && m.run.valid) {
        char buf[32];
        if (m.run.sandbox) {
            reward_row(rows, gui_, "sandbox", "memory_cell", "Sandbox run: no Memory Cells or Antibodies", "",
                       "reward_memory");
        } else {
            std::snprintf(buf, sizeof(buf), "+%u", m.run.memory_cells);
            reward_row(rows, gui_, "memory", "memory_cell", "Memory cells", buf, "reward_memory");
            if (m.run.antibodies > 0) {
                std::snprintf(buf, sizeof(buf), "+%u", m.run.antibodies);
                reward_row(rows, gui_, "antibody", "antibody", m.run.antibodies == 1 ? "Antibody" : "Antibodies", buf,
                           "reward_antibody");
            }
        }
        if (victory && m.unlocked_next && next != nullptr) {
            reward_row(rows, gui_, "unlocked", nullptr, next->name + " unlocked", "", "reward_memory",
                       std::to_string(next->number));
        }
    }
    rows.visible = !rows.children().empty();
    p.emplace<Spacer>(0.0f).layout.height = Size::px(rows.visible ? 40.0f : 24.0f);

    Widget& buttons = row(p, 14.0f, Align::Center, "buttons");
    if (m.playtest) {
        // An editor playtest's way out is back to the document being tested.
        Button& restart = labelled_button(buttons, th, "restart", "button.cta", Vec2{260, 84}, "Restart",
                                          "result_button_cta");
        restart.on_click = [this] { emit(MenuResult{MenuAction::RestartLevel}); };
        Button& editor = labelled_button(buttons, th, "editor", "button.soft", Vec2{260, 84}, "Back to Editor",
                                         "result_button");
        editor.on_click = [this] { emit(MenuResult{MenuAction::BackToEditor}); };
        return;
    }
    Button& tree = labelled_button(buttons, th, "tree", "button.cta", Vec2{330, 84}, "Strengthen Immunity",
                                   "result_button_cta");
    tree.tooltip = "Spend Memory Cells and Antibodies";
    tree.on_click = [this] { emit(MenuResult{MenuAction::OpenImmunityTree}); };
    if (victory) {
        if (next != nullptr && !next->locked) {
            Button& nb = labelled_button(buttons, th, "next", "button.soft", Vec2{220, 84}, "Next level",
                                         "result_button");
            nb.tooltip = next->name;
            const usize index = next->level_index;
            nb.on_click = [this, index] { emit(MenuResult{MenuAction::StartLevel, index}); };
        } else {
            Button& lv = labelled_button(buttons, th, "levels", "button.soft", Vec2{220, 84}, "Levels", "result_button");
            lv.on_click = [this] { emit(MenuResult{MenuAction::OpenLevelSelect}); };
        }
        // DESIGN.md §7.4: replaying for more Memory Cells is a fair next step.
        Button& replay = labelled_button(buttons, th, "replay", "button.flesh", Vec2{170, 84}, "Replay",
                                         "result_button");
        replay.on_click = [this] { emit(MenuResult{MenuAction::RestartLevel}); };
    } else {
        Button& retry = labelled_button(buttons, th, "retry", "button.flesh", Vec2{170, 84}, "Retry", "result_button");
        retry.on_click = [this] { emit(MenuResult{MenuAction::RestartLevel}); };
        Button& lv = labelled_button(buttons, th, "levels", "button.flesh", Vec2{170, 84}, "Levels", "result_button");
        lv.on_click = [this] { emit(MenuResult{MenuAction::OpenLevelSelect}); };
    }
}

} // namespace immune::ui
