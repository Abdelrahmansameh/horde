#include "ui/hud/HudParts.h"

#include "core/Math.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/widgets/Builders.h"

#include <cmath>
#include <cstdio>

namespace immune::ui {

using namespace immune::gui;

// ---- Names --------------------------------------------------------------------------

const char* tower_display_name(TowerType t) {
    switch (t) {
        case TowerType::Neutrophil: return "Neutrophil";
        case TowerType::Macrophage: return "Macrophage";
        case TowerType::CytotoxicT: return "Cytotoxic T";
        case TowerType::GobletCell: return "Goblet Cell";
        case TowerType::Fibroblast: return "Fibroblast";
        case TowerType::Count: break;
    }
    return "?";
}

const char* tower_id(TowerType t) {
    switch (t) {
        case TowerType::Neutrophil: return "neutrophil";
        case TowerType::Macrophage: return "macrophage";
        case TowerType::CytotoxicT: return "cytotoxic_t";
        case TowerType::GobletCell: return "goblet_cell";
        case TowerType::Fibroblast: return "fibroblast";
        case TowerType::Count: break;
    }
    return "tower";
}

const char* ability_id_name(game::AbilityId id) {
    switch (id) {
        case game::AbilityId::ComplementCascadeBurst: return "cascade";
        case game::AbilityId::HistamineFlare: return "histamine";
        case game::AbilityId::FeverResponse: return "fever";
        case game::AbilityId::FibrinClot: return "clot";
        default: break;
    }
    return "ability";
}

const char* tower_icon(TowerType t) {
    switch (t) {
        case TowerType::Neutrophil: return "tower_neutrophil";
        case TowerType::Macrophage: return "tower_macrophage";
        case TowerType::CytotoxicT: return "tower_cytotoxic_t";
        case TowerType::GobletCell: return "tower_goblet_cell";
        case TowerType::Fibroblast: return "tower_fibroblast";
        case TowerType::Count: break;
    }
    return "";
}

const char* ability_short_name(game::AbilityId id) {
    switch (id) {
        case game::AbilityId::ComplementCascadeBurst: return "Cascade";
        case game::AbilityId::HistamineFlare: return "Histamine";
        case game::AbilityId::FeverResponse: return "Fever";
        case game::AbilityId::FibrinClot: return "Clot";
        default: break;
    }
    return "?";
}

const char* ability_icon(game::AbilityId id) {
    switch (id) {
        case game::AbilityId::ComplementCascadeBurst: return "ability_complement_cascade";
        case game::AbilityId::HistamineFlare: return "ability_histamine_flare";
        case game::AbilityId::FeverResponse: return "ability_fever_response";
        case game::AbilityId::FibrinClot: return "ability_fibrin_clot";
        default: break;
    }
    return "";
}

const char* ability_color(game::AbilityId id) {
    switch (id) {
        case game::AbilityId::ComplementCascadeBurst: return "ability_complement";
        case game::AbilityId::HistamineFlare: return "ability_histamine";
        case game::AbilityId::FeverResponse: return "ability_fever";
        case game::AbilityId::FibrinClot: return "ability_clot";
        default: break;
    }
    return "lavender";
}

const char* family_display_name(PathogenFamily f, bool plural) {
    switch (f) {
        case PathogenFamily::Virus: return plural ? "Viruses" : "Virus";
        case PathogenFamily::Bacteria: return "Bacteria";
        case PathogenFamily::Parasite: return plural ? "Parasites" : "Parasite";
        case PathogenFamily::Count: break;
    }
    return "?";
}

const char* family_icon(PathogenFamily f) {
    switch (f) {
        case PathogenFamily::Virus: return "pathogen_virus";
        case PathogenFamily::Bacteria: return "pathogen_bacteria";
        case PathogenFamily::Parasite: return "pathogen_parasite";
        case PathogenFamily::Count: break;
    }
    return "";
}

const std::vector<DockGroup>& dock_groups() {
    static const std::vector<DockGroup> kGroups = {
        {"Attack", {TowerType::Neutrophil, TowerType::CytotoxicT, TowerType::Macrophage}},
        {"Control", {TowerType::GobletCell, TowerType::Fibroblast}},
    };
    return kGroups;
}

u32 dock_slot(TowerType t) {
    u32 slot = 0;
    for (const DockGroup& g : dock_groups()) {
        for (TowerType x : g.towers) {
            if (x == t) return slot;
            ++slot;
        }
    }
    return slot;
}

TowerType dock_tower(u32 slot) {
    for (const DockGroup& g : dock_groups()) {
        if (slot < g.towers.size()) return g.towers[slot];
        slot -= static_cast<u32>(g.towers.size());
    }
    return TowerType::Count;
}

Color health_color(const Theme& theme, f32 frac) {
    if (frac > 0.5f) return theme.color("health_ok");
    if (frac > 0.25f) return theme.color("health");
    return theme.color("critical");
}

std::string format_clock(f32 seconds) {
    const int s = static_cast<int>(std::ceil(math::max(seconds, 0.0f)));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

// ---- Small pieces ----------------------------------------------------------------------

KeyCap::KeyCap(Gui& g, std::string id, std::string key) : Panel(std::move(id), g.theme().shape("keycap")) {
    layout.padding = Insets{7.0f, 4.0f, 7.0f, 5.0f};
    layout.min_size = Vec2{26.0f, 26.0f};
    layout.axis = Axis::Stack;
    layout.align = Align::Center;
    text(*this, "key", std::move(key), g.theme().text("keycap"), TextAlign::Center);
}

Badge::Badge(Gui& g, std::string id, std::string value) : Panel(std::move(id), g.theme().shape("badge")) {
    layout.padding = Insets{7.0f, 4.0f, 7.0f, 4.0f};
    layout.min_size = Vec2{24.0f, 24.0f};
    layout.axis = Axis::Stack;
    layout.align = Align::Center;
    label = &text(*this, "n", std::move(value), g.theme().text("badge"), TextAlign::Center);
}

Tag::Tag(Gui& g, std::string id, std::string value) : Panel(std::move(id), g.theme().shape("tag")) {
    layout.padding = Insets{10.0f, 3.0f, 10.0f, 3.0f};
    layout.axis = Axis::Stack;
    layout.align = Align::Center;
    blocks_pointer = false;
    text(*this, "t", std::move(value), g.theme().text("tag"), TextAlign::Center);
}

// ---- Build card ------------------------------------------------------------------------

BuildCard::BuildCard(Gui& g, TowerType t, u32 slot)
    : Button(tower_id(t), g.theme().shape("card")), type(t), gui_(g) {
    const Theme& th = g.theme();
    fixed_size(*this, 138.0f, 178.0f);
    layout.padding = Insets{14.0f, 18.0f, 14.0f, 12.0f};
    layout.axis = Axis::Column;
    layout.align = Align::Center;
    layout.gap = 1.0f;

    Widget& top = row(*this, 0.0f, Align::Center);
    top.layout.width = Size::fill();
    top.layout.justify = Justify::SpaceBetween;
    badge_ = &top.emplace<Badge>(g, "key", std::to_string(slot + 1));
    Widget& cost = row(top, 2.0f, Align::Center);
    Icon& coin = icon(cost, "atp", "atp", 22.0f);
    coin.anim_transform = Affine2::rotate(-0.14f);
    cost_ = &text(cost, "cost", "0", th.text("card_cost"));

    icon_ = &icon(*this, "icon", tower_icon(t), 76.0f);
    name_ = &text(*this, "name", tower_display_name(t), th.text("card_name"), TextAlign::Center);
    short_ = &text(*this, "short", "", th.text("card_short"), TextAlign::Center);

    progress_ = &emplace<Meter>("afford", th.shape("bar.atp"));
    anchor(*progress_, Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0.0f, -10.0f});
    fixed_size(*progress_, 114.0f, 12.0f);
    progress_->blocks_pointer = false;

    tag_ = &emplace<Tag>(g, "tag", "Placing");
    anchor(*tag_, Vec2{0.5f, 0.0f}, Vec2{0.5f, 0.5f}, Vec2{0.0f, -2.0f});
    tag_->visible = false;

    has_disabled_shape = true;
    disabled_shape = th.shape("card.short");
}

void BuildCard::sync(const HudTowerCard& c, u32 atp, bool armed) {
    const Theme& th = gui_.theme();
    visible = c.unlocked;
    const bool affordable = atp >= c.cost;
    enabled = c.allowed && affordable;
    cost_->set_text(std::to_string(c.cost));

    const bool dim = !enabled;
    badge_->shape = th.shape(dim ? "badge.muted" : "badge");
    cost_->style.color = th.color(dim ? "muted" : "ink");
    name_->style.color = th.color(dim ? "muted" : "ink");
    icon_->grayscale = dim;
    shape = th.shape(armed ? "card.armed" : "card");
    shape.wobble_seed = static_cast<f32>(static_cast<u32>(type) * 7 + 3);

    if (!c.allowed) {
        short_->set_text("Not on this level");
        short_->visible = true;
        progress_->visible = false;
        tooltip = "This level does not allow the " + std::string(tower_display_name(type));
    } else if (!affordable) {
        short_->set_text(std::to_string(c.cost - atp) + " ATP short");
        short_->visible = true;
        progress_->visible = true;
        progress_->set_level(c.cost > 0 ? static_cast<f32>(atp) / static_cast<f32>(c.cost) : 1.0f, 0.25f);
        tooltip.clear();
    } else {
        short_->visible = false;
        progress_->visible = false;
        tooltip.clear();
    }
    tag_->visible = armed;
    nudge = armed ? Vec2{0.0f, -14.0f} : Vec2{0.0f, 0.0f};
}

// ---- Ability cell ----------------------------------------------------------------------

AbilityCell::AbilityCell(Gui& g, game::AbilityId id, std::string key)
    : Widget(ability_id_name(id)), ability(id), gui_(g) {
    const Theme& th = g.theme();

    layout.axis = Axis::Column;
    layout.align = Align::Center;
    layout.gap = 4.0f;

    Widget& cell = stack(*this, Align::Center, "cell");
    fixed_size(cell, 96.0f, 96.0f);

    const Color accent = th.color(ability_color(id));
    halo_ = &cell.emplace<Ring>("halo", th.shape("ring.halo"));
    halo_->shape.stroke = with_alpha(accent, 0.55f);
    halo_->shape.dash_length = 0.0f;
    fixed_size(*halo_, 106.0f, 106.0f);
    anchor(*halo_, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f});

    ShapeDesc s = th.shape("cell.ability");
    s.rim = accent;
    s.liquid = with_alpha(accent, 0.38f);
    s.bubble = with_alpha(accent, 0.55f);
    button = &cell.emplace<Button>("button", s);
    fixed_size(*button, 92.0f, 92.0f);
    button->layout.axis = Axis::Stack;
    button->layout.align = Align::Center;
    button->hover_scale = 1.06f;
    glyph_ = &icon(*button, "glyph", ability_icon(id), 58.0f);
    timer_ = &text(*button, "timer", "", th.text("timer"), TextAlign::Center);

    tag_ = &cell.emplace<Tag>(g, "tag", "Aiming");
    anchor(*tag_, Vec2{0.5f, 0.0f}, Vec2{0.5f, 0.5f}, Vec2{0.0f, -2.0f});
    tag_->visible = false;

    Widget& keys = row(*this, 5.0f, Align::Center);
    keys.emplace<KeyCap>(g, "key", std::move(key));
    name_ = &text(keys, "name", ability_short_name(id), th.text("ability_name"));
}

void AbilityCell::sync(const HudAbility& a, bool armed) {
    const Theme& th = gui_.theme();
    visible = a.unlocked;
    ready_ = a.ready;
    button->enabled = a.ready;
    button->shape.level =
        a.ready ? 1.0f : math::saturate(1.0f - a.cooldown_remaining / math::max(a.cooldown_total, 1e-3f));
    glyph_->opacity = a.ready ? 1.0f : 0.45f;
    timer_->visible = !a.ready;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0fs", std::ceil(math::max(a.cooldown_remaining, 0.0f)));
    timer_->set_text(buf);
    name_->style.color = th.color(a.ready ? "ink" : "muted");
    halo_->visible = a.ready;
    tag_->visible = armed;
    button->tooltip = a.ready ? "" : std::string(game::ability_name(ability)) + " is recharging";
}

void AbilityCell::update(f32) {
    const f32 t = gui_.time();
    halo_->opacity = loop::halo_opacity(t);
    // The canvas wobbles a ready ability to invite the click.
    if (ready_) {
        halo_->anim_transform = Affine2::scale(loop::wobble_scale(t));
        glyph_->anim_transform = Affine2::rotate(loop::wobble_rotation(t) * 2.0f);
    } else {
        halo_->anim_transform = Affine2{};
        glyph_->anim_transform = Affine2{};
    }
}

// ---- Wave row -----------------------------------------------------------------------------

WaveRow::WaveRow(Gui& g, std::string id) : Widget(std::move(id)), gui_(g) {
    const Theme& th = g.theme();
    layout.axis = Axis::Row;
    layout.align = Align::Center;
    layout.gap = 10.0f;
    layout.width = Size::fill();
    Widget& box = stack(*this, Align::Center);
    fixed_size(box, 44.0f, 38.0f);
    icon_ = &icon(box, "icon", "", 40.0f);
    Widget& mid = column(*this, 4.0f, Align::Stretch);
    mid.layout.grow = 1.0f;
    name_ = &text(mid, "name", "", th.text("row_name"));
    bar_ = &mid.emplace<Meter>("bar", th.shape("minibar"));
    bar_->layout.height = Size::px(11.0f);
    bar_->blocks_pointer = false;
    count_ = &text(*this, "count", "", th.text("row_count"), TextAlign::Right);
    count_->layout.min_size = Vec2{54.0f, 0.0f};
}

void WaveRow::sync(const HudFamilyCount& f, u32 total) {
    const Theme& th = gui_.theme();
    icon_->name = family_icon(f.family);
    name_->set_text(family_display_name(f.family, true));
    count_->set_text("\xC3\x97" + std::to_string(f.count));
    static constexpr const char* kFamilyColor[kFamilyCount] = {"family_virus", "family_bacteria",
                                                               "family_parasite"};
    bar_->shape.liquid = th.color(kFamilyColor[static_cast<u32>(f.family)]);
    bar_->set_level(total > 0 ? static_cast<f32>(f.count) / static_cast<f32>(total) : 0.0f);
}

// ---- Range ring -----------------------------------------------------------------------------

RangeRing::RangeRing(std::string id) : Widget(std::move(id)) {}

void RangeRing::draw_self(DrawList& dl) {
    Gui* g = gui();
    if (g == nullptr || radius <= 0.0f) return;
    constexpr int kSegments = 72;
    Vec2 pts[kSegments];
    for (int i = 0; i < kSegments; ++i) {
        const f32 a = static_cast<f32>(i) / static_cast<f32>(kSegments) * math::kTwoPi;
        pts[i] = g->project(world + Vec2{std::cos(a), std::sin(a)} * radius);
    }
    if (fill.a > 0.0f) dl.fill_convex(pts, fill);
    StrokeStyle st{stroke_width, stroke};
    if (dash > 0.0f) {
        st.dash_length = dash;
        st.dash_gap = gap;
        st.dash_offset = -g->time() * flow;
    }
    dl.stroke_polyline(pts, true, st);
    if (dots > 0) {
        const f32 spin = g->time() * 0.6f;
        for (u32 i = 0; i < dots; ++i) {
            const f32 a = spin + static_cast<f32>(i) / static_cast<f32>(dots) * math::kTwoPi;
            ShapeDesc d;
            d.kind = ShapeKind::Ellipse;
            d.center = g->project(world + Vec2{std::cos(a), std::sin(a)} * radius);
            d.half_size = Vec2{6.0f, 6.0f};
            d.fill = kWhite;
            d.stroke = stroke;
            d.stroke_width = 2.5f;
            dl.shape(d);
        }
    }
}

// ---- Tower popup ------------------------------------------------------------------------------

TowerPopup::TowerPopup(Gui& g, std::string id) : Panel(std::move(id), g.theme().shape("membrane.player")), gui_(g) {
    const Theme& th = g.theme();
    shape.radius = 30.0f;
    shape.decor_count = 2;
    layout.position = Position::World;
    layout.pivot = Vec2{0.0f, 1.0f};
    layout.offset = Vec2{70.0f, -64.0f};
    layout.width = Size::px(320.0f);
    layout.padding = Insets{22.0f, 18.0f, 22.0f, 20.0f};
    layout.axis = Axis::Column;
    layout.gap = 10.0f;
    layout.align = Align::Stretch;

    name_ = &text(*this, "name", "", th.text("popup_title"));
    Widget& hp = row(*this, 10.0f, Align::Center);
    health_ = &hp.emplace<Meter>("health", th.shape("bar.health"));
    health_->layout.height = Size::px(18.0f);
    health_->layout.grow = 1.0f;
    health_->blocks_pointer = false;
    health_text_ = &text(hp, "value", "", th.text("popup_value"));

    sell = &emplace<Button>("sell", th.shape("pill.sell"));
    sell->layout.height = Size::px(46.0f);
    sell->layout.axis = Axis::Row;
    sell->layout.justify = Justify::Center;
    sell->layout.align = Align::Center;
    sell->layout.gap = 6.0f;
    sell->hover_scale = 1.03f;
    text(*sell, "label", "Sell", th.text("sell"));
    text(*sell, "plus", "+", th.text("sell"));
    Icon& coin = icon(*sell, "atp", "atp", 26.0f);
    coin.anim_transform = Affine2::rotate(-0.2f);
    refund_ = &text(*sell, "refund", "0", th.text("sell"));
}

void TowerPopup::sync(const HudTower& t) {
    layout.world = t.world;
    name_->set_text(tower_display_name(t.type));
    const f32 frac = t.health_max > 0.0f ? t.health / t.health_max : 1.0f;
    health_->shape.liquid = health_color(gui_.theme(), frac);
    health_->set_level(frac, 0.2f);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f / %.0f", std::ceil(t.health), t.health_max);
    health_text_->set_text(buf);
    refund_->set_text(std::to_string(t.refund));
}

void TowerPopup::draw_self(DrawList& dl) {
    // The tail: a lavender vessel from the popup's lower-left corner curving
    // down to the tower it belongs to. Drawn first so the panel sits on it.
    const Vec2 tower = gui_.project(layout.world);
    const Rect r = rect();
    const Vec2 from{r.min.x + 44.0f, r.max.y - 8.0f};
    Path tail(0.5f);
    tail.move_to(from).cubic_to(from + Vec2{0.0f, 40.0f}, tower + Vec2{40.0f, -30.0f}, tower + Vec2{12.0f, -12.0f});
    tail.stroke(dl, StrokeStyle{11.0f, gui_.theme().color("plum")});
    tail.stroke(dl, StrokeStyle{6.5f, gui_.theme().color("lavender")});
    Panel::draw_self(dl);
}

// ---- Hint pill ---------------------------------------------------------------------------------

HintPill::HintPill(Gui& g, std::string id) : Panel(std::move(id), g.theme().shape("pill")) {
    const Theme& th = g.theme();
    layout.axis = Axis::Row;
    layout.align = Align::Center;
    layout.gap = 8.0f;
    layout.padding = Insets{22.0f, 10.0f, 22.0f, 10.0f};
    emplace<KeyCap>(g, "lmb", "LMB");
    verb_ = &text(*this, "verb", "Place", th.text("hint"));
    ShapeDesc dot;
    dot.kind = ShapeKind::Ellipse;
    dot_ = &emplace<Panel>("dot", dot);
    fixed_size(*dot_, 8.0f, 8.0f);
    dot_->blocks_pointer = false;
    emplace<KeyCap>(g, "rmb", "RMB");
    text(*this, "slash", "/", th.text("hint"));
    emplace<KeyCap>(g, "esc", "Esc");
    text(*this, "cancel", "Cancel", th.text("hint"));
}

void HintPill::sync(const std::string& verb, Color accent) {
    verb_->set_text(verb);
    dot_->shape.fill = accent;
    shape.stroke = accent;
}

} // namespace immune::ui
