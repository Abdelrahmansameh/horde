#include "ui/hud/HudScreen.h"

#include "core/Math.h"
#include "gui/core/Gui.h"
#include "gui/widgets/Builders.h"
#include "platform/Input.h"
#include "render/Renderer.h"
#include "ui/HudFormat.h"
#include "ui/hud/HudParts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace immune::ui {

using namespace immune::gui;

namespace {

/// Pick radius for click-to-select, in world units: towers have no per-type
/// pick radius, and this matches the placement spacing validate() enforces.
constexpr f32 kTowerPickRadius = 2.5f;
/// Below this the organ is critical: the canvas's red panel and banner.
constexpr f32 kCriticalIntegrity = 0.25f;
/// How long "Wave N cleared" stays up.
constexpr f32 kToastSeconds = 3.0f;

Panel& panel(Widget& parent, std::string id, const ShapeDesc& shape) {
    return parent.emplace<Panel>(std::move(id), shape);
}

} // namespace

HudScreen::HudScreen(Gui& gui) : gui_(gui) {
    // Pathogen colours are owned by the renderer; never duplicate them.
    Theme& th = gui_.theme();
    th.set_color("family_virus", render::family_color(PathogenFamily::Virus));
    th.set_color("family_bacteria", render::family_color(PathogenFamily::Bacteria));
    th.set_color("family_parasite", render::family_color(PathogenFamily::Parasite));
    build();
}

HudScreen::~HudScreen() {
    if (hud_ != nullptr) gui_.layer(LayerId::Hud).remove(*hud_);
    if (world_ != nullptr) gui_.layer(LayerId::World).remove(*world_);
    if (popup_layer_root_ != nullptr) gui_.layer(LayerId::Popup).remove(*popup_layer_root_);
}

void HudScreen::set_visible(bool v) {
    visible_ = v;
    hud_->visible = v;
    world_->visible = v;
    popup_layer_root_->visible = v;
}

// ---- Build ----------------------------------------------------------------------------

void HudScreen::build() {
    Theme& th = gui_.theme();

    auto full = [](Widget& w) {
        w.layout.position = Position::Anchored;
        w.layout.width = Size::fill();
        w.layout.height = Size::fill();
    };
    hud_ = &gui_.layer(LayerId::Hud).emplace<Widget>("hud");
    full(*hud_);
    world_ = &gui_.layer(LayerId::World).emplace<Widget>("world");
    full(*world_);
    popup_layer_root_ = &gui_.layer(LayerId::Popup).emplace<Widget>("inspect");
    full(*popup_layer_root_);

    // ---- Critical vignette (under everything else in the HUD) ----
    // The canvas's radialGradient has r="72%" of the screen box, so its full
    // strength lies beyond the screen edge: 144% of the viewport, centred.
    vignette_ = &panel(*hud_, "vignette", th.shape("vignette.critical"));
    anchor(*vignette_, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f});
    vignette_->layout.width = Size::pct(1.44f);
    vignette_->layout.height = Size::pct(1.44f);
    vignette_->blocks_pointer = false;
    vignette_->visible = false;

    // ---- Organ integrity (top-left, 470x112) ----
    organ_ = &panel(*hud_, "organ", th.shape("membrane.host"));
    anchor(*organ_, Vec2{0, 0}, Vec2{0, 0}, Vec2{18, 16});
    fixed_size(*organ_, 470, 112);
    organ_->layout.padding = Insets{26, 16, 26, 16};
    organ_->layout.gap = 6;
    text(*organ_, "label", "Organ integrity", th.text("label"));
    Widget& organ_row = row(*organ_, 14, Align::Center);
    organ_pct_ = &text(organ_row, "value", "100%", th.text("value_big"));
    organ_pct_->layout.min_size = Vec2{92, 0};
    organ_bar_ = &organ_row.emplace<Meter>("bar", th.shape("bar.blood"));
    fixed_size(*organ_bar_, 300, 26);
    organ_bar_->blocks_pointer = false;
    organ_bar_->set_level(1.0f, 0.0f);

    // ---- "Organ failing" (top-centre) ----
    critical_banner_ = &panel(*hud_, "failing", th.shape("pill.critical"));
    anchor(*critical_banner_, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 26});
    critical_banner_->layout.padding = Insets{70, 16, 70, 18};
    critical_banner_->layout.axis = Axis::Stack;
    critical_banner_->layout.align = Align::Center;
    text(*critical_banner_, "text", "Organ failing", th.text("banner_critical"));
    critical_banner_->visible = false;

    // ---- Prep banner and the wave-cleared toast (top-centre) ----
    prep_banner_ = &panel(*hud_, "prep", th.shape("membrane.player"));
    anchor(*prep_banner_, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 22});
    prep_banner_->layout.axis = Axis::Row;
    prep_banner_->layout.align = Align::Center;
    prep_banner_->layout.gap = 18;
    prep_banner_->layout.padding = Insets{14, 12, 18, 14};
    Widget& clock = stack(*prep_banner_, Align::Center, "clock");
    fixed_size(clock, 96, 96);
    Panel& disc = panel(clock, "disc", th.shape("countdown.disc"));
    fixed_size(disc, 80, 80);
    anchor(disc, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f});
    prep_ring_ = &clock.emplace<Ring>("ring", th.shape("countdown.arc"));
    fixed_size(*prep_ring_, 96, 96);
    anchor(*prep_ring_, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f});
    prep_clock_ = &text(clock, "time", "0:00", th.text("countdown"), TextAlign::Center);
    anchor(*prep_clock_, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f});
    prep_wave_ = &text(*prep_banner_, "wave", "Wave 1", th.text("banner"));
    Button& send = prep_banner_->emplace<Button>("send", th.shape("button.primary"));
    send.layout.axis = Axis::Row;
    send.layout.align = Align::Center;
    send.layout.gap = 10;
    send.layout.padding = Insets{22, 12, 14, 12};
    text(send, "label", "Send now", th.text("button"));
    send.emplace<KeyCap>(gui_, "key", "Space");
    send.on_click = [this] { push(Intent{IntentKind::StartWaveEarly}); };
    prep_banner_->visible = false;

    toast_ = &panel(*hud_, "toast", th.shape("pill"));
    anchor(*toast_, Vec2{0.5f, 0}, Vec2{0.5f, 0}, Vec2{0, 150});
    toast_->layout.padding = Insets{60, 12, 60, 14};
    toast_->layout.axis = Axis::Stack;
    toast_->layout.align = Align::Center;
    toast_->blocks_pointer = false;
    toast_text_ = &text(*toast_, "text", "", th.text("toast"));
    toast_->visible = false;

    // ---- Clock controls (top-right) ----
    Widget& controls = row(*hud_, 4, Align::Center, "controls");
    anchor(controls, Vec2{1, 0}, Vec2{1, 0}, Vec2{-22, 18});
    static constexpr const char* kControlIds[4] = {"pause", "speed1", "speed2", "menu"};
    for (usize i = 0; i < 4; ++i) {
        Button& b = controls.emplace<Button>(kControlIds[i], th.shape("button.round"));
        fixed_size(b, 56, 56);
        b.layout.axis = Axis::Stack;
        b.layout.align = Align::Center;
        b.hover_scale = 1.08f;
        controls_[i] = &b;
    }
    icon(*controls_[0], "glyph", "glyph_pause", 44);
    text(*controls_[1], "label", "1\xC3\x97", th.text("speed"), TextAlign::Center);
    text(*controls_[2], "label", "2\xC3\x97", th.text("speed"), TextAlign::Center);
    icon(*controls_[3], "glyph", "glyph_menu", 44);
    controls_[0]->tooltip = "Pause (Space)";
    controls_[1]->tooltip = "Normal speed";
    controls_[2]->tooltip = "Double speed";
    controls_[3]->tooltip = "Menu (Esc)";
    controls_[0]->on_click = [this] {
        Intent i{IntentKind::SetTimeScale};
        if (model_.time_scale > 0.0f) {
            resume_scale_ = model_.time_scale;
            i.value = 0.0f;
        } else {
            i.value = resume_scale_;
        }
        push(i);
    };
    controls_[1]->on_click = [this] {
        Intent i{IntentKind::SetTimeScale};
        i.value = 1.0f;
        push(i);
    };
    controls_[2]->on_click = [this] {
        Intent i{IntentKind::SetTimeScale};
        i.value = 2.0f;
        push(i);
    };
    controls_[3]->on_click = [this] { push(Intent{IntentKind::OpenMenu}); };

    // ---- Next-wave panel (top-right, 380 wide) ----
    wave_panel_ = &panel(*hud_, "wave", th.shape("membrane.host"));
    anchor(*wave_panel_, Vec2{1, 0}, Vec2{1, 0}, Vec2{-18, 88});
    wave_panel_->layout.width = Size::px(380);
    wave_panel_->layout.padding = Insets{22, 18, 22, 22};
    wave_panel_->layout.gap = 9;
    wave_panel_->layout.align = Align::Stretch;
    Widget& header = row(*wave_panel_, 0, Align::Start);
    header.layout.justify = Justify::SpaceBetween;
    Widget& titles = column(header, 0);
    text(titles, "label", "Next", th.text("label"));
    wave_title_ = &text(titles, "title", "Wave 1", th.text("wave_title"));
    wave_total_ = &text(header, "total", "0", th.text("wave_total"));
    for (usize i = 0; i < kFamilyCount; ++i) {
        wave_rows_[i] = &wave_panel_->emplace<WaveRow>(gui_, "row" + std::to_string(i));
    }
    elite_row_ = &panel(*wave_panel_, "elite", th.shape("pill.elite"));
    elite_row_->layout.axis = Axis::Row;
    elite_row_->layout.align = Align::Center;
    elite_row_->layout.gap = 10;
    elite_row_->layout.padding = Insets{4, 4, 12, 4};
    Widget& ebox = stack(*elite_row_, Align::Center);
    fixed_size(ebox, 44, 40);
    icon(ebox, "icon", "marker_elite", 42);
    Label& elite_label = text(*elite_row_, "label", "Elite", th.text("elite"));
    elite_label.layout.grow = 1;
    elite_count_ = &text(*elite_row_, "count", "", th.text("elite_count"));

    // ---- Build dock (bottom) ----
    dock_ = &panel(*hud_, "dock", th.shape("membrane.player"));
    anchor(*dock_, Vec2{0.5f, 1}, Vec2{0.5f, 1}, Vec2{-100, -16});
    dock_->layout.height = Size::px(232);
    dock_->layout.axis = Axis::Row;
    dock_->layout.align = Align::End;
    dock_->layout.gap = 18;
    dock_->layout.padding = Insets{22, 16, 22, 18};
    Widget& atp = column(*dock_, 0, Align::Center, "atp");
    atp.layout.width = Size::px(160);
    atp.layout.height = Size::fill();
    atp.layout.justify = Justify::Center;
    atp_icon_ = &icon(atp, "icon", "atp", Vec2{110, 70});
    atp_value_ = &text(atp, "value", "0", th.text("atp_value"), TextAlign::Center);
    atp_rate_ = &text(atp, "rate", "ATP", th.text("atp_rate"), TextAlign::Center);
    u32 slot = 0;
    for (const DockGroup& g : dock_groups()) {
        Widget& col = column(*dock_, 6, Align::Start);
        Label& gl = text(col, "group", g.label, th.text("label"));
        gl.layout.padding.left = 8;
        Widget& cards = row(col, 8, Align::End);
        for (TowerType t : g.towers) {
            BuildCard& card = cards.emplace<BuildCard>(gui_, t, slot++);
            card.on_click = [this, t] { arm_tower(armed_tower_ == t ? TowerType::Count : t); };
            cards_[static_cast<usize>(t)] = &card;
        }
        dock_group_widgets_.push_back(&col);
    }

    // ---- Abilities (bottom-right, 464x156) ----
    abilities_ = &panel(*hud_, "abilities", th.shape("membrane.player"));
    anchor(*abilities_, Vec2{1, 1}, Vec2{1, 1}, Vec2{-18, -16});
    abilities_->layout.height = Size::px(156);
    abilities_->layout.axis = Axis::Row;
    abilities_->layout.align = Align::Center;
    abilities_->layout.gap = 14;
    abilities_->layout.padding = Insets{20, 10, 20, 10};
    static constexpr const char* kAbilityKeys[game::kAbilityCount] = {"Q", "W", "E", "R"};
    for (u32 i = 0; i < game::kAbilityCount; ++i) {
        const auto id = static_cast<game::AbilityId>(i);
        AbilityCell& cell = abilities_->emplace<AbilityCell>(gui_, id, kAbilityKeys[i]);
        cell.button->on_click = [this, id] { press_ability(id); };
        ability_cells_[i] = &cell;
    }

    // ---- Armed-cursor hint and the cost that follows the pointer ----
    hint_ = &hud_->emplace<HintPill>(gui_, "hint");
    anchor(*hint_, Vec2{0.5f, 1}, Vec2{0.5f, 1}, Vec2{-100, -262});
    hint_->visible = false;
    cursor_cost_ = &panel(*hud_, "cursor_cost", th.shape("pill"));
    cursor_cost_->layout.position = Position::Anchored;
    cursor_cost_->layout.axis = Axis::Row;
    cursor_cost_->layout.align = Align::Center;
    cursor_cost_->layout.gap = 6;
    cursor_cost_->layout.padding = Insets{14, 7, 16, 7};
    cursor_cost_->blocks_pointer = false;
    Icon& coin = icon(*cursor_cost_, "atp", "atp", 24);
    coin.anim_transform = Affine2::rotate(-0.2f);
    cursor_cost_value_ = &text(*cursor_cost_, "value", "", th.text("cursor_cost"));
    icon(*cursor_cost_, "arrow", "glyph_arrow", 18);
    cursor_cost_left_ = &text(*cursor_cost_, "left", "", th.text("cursor_left"));
    cursor_cost_->visible = false;

    // ---- World overlays ----
    ghost_ring_ = &world_->emplace<RangeRing>("ghost_ring");
    ghost_ring_->visible = false;
    ghost_icon_ = &icon(*world_, "ghost", "", 64);
    ghost_icon_->layout.position = Position::World;
    ghost_icon_->layout.pivot = Vec2{0.5f, 0.5f};
    ghost_icon_->opacity = 0.75f;
    ghost_icon_->visible = false;
    reticle_ = &world_->emplace<RangeRing>("reticle");
    reticle_->dash = 10;
    reticle_->gap = 8;
    reticle_->flow = 30;
    reticle_->dots = 8;
    reticle_->visible = false;
    selection_ring_ = &world_->emplace<Ring>("selection", th.shape("ring.halo"));
    selection_ring_->shape.stroke = th.color("lavender");
    selection_ring_->spin_speed = 0.8f;
    selection_ring_->layout.position = Position::World;
    selection_ring_->layout.pivot = Vec2{0.5f, 0.5f};
    fixed_size(*selection_ring_, 78, 78);
    selection_ring_->visible = false;

    // Positioned from the projected spawn each sync (not Position::World) so
    // it can be kept on screen: spawns often sit right at the world's edge.
    spawn_pill_ = &panel(*world_, "spawn", th.shape("pill.gold"));
    spawn_pill_->layout.position = Position::Anchored;
    spawn_pill_->layout.axis = Axis::Row;
    spawn_pill_->layout.align = Align::Center;
    spawn_pill_->layout.gap = 12;
    spawn_pill_->layout.padding = Insets{14, 6, 18, 6};
    spawn_pill_->blocks_pointer = false;
    for (usize i = 0; i < kFamilyCount; ++i) {
        Widget& e = row(*spawn_pill_, 2, Align::Center, "f" + std::to_string(i));
        icon(e, "icon", family_icon(static_cast<PathogenFamily>(i)), 30);
        spawn_counts_[i] = &text(e, "count", "", th.text("world_count"));
        spawn_entries_[i] = &e;
    }
    spawn_pill_->visible = false;

    popup_ = &popup_layer_root_->emplace<TowerPopup>(gui_, "popup");
    popup_->sell->on_click = [this] {
        if (!selected_.valid()) return;
        Intent i{IntentKind::SellTower};
        i.entity = selected_;
        push(i);
        selected_ = EntityId{};
    };
    popup_->visible = false;
}

// ---- Interaction state ------------------------------------------------------------------

void HudScreen::arm_tower(TowerType t) {
    armed_tower_ = t;
    if (t != TowerType::Count) {
        armed_ability_ = game::AbilityId::Count;
        selected_ = EntityId{};
    }
}

void HudScreen::arm_ability(game::AbilityId a) {
    armed_ability_ = a;
    if (a != game::AbilityId::Count) {
        armed_tower_ = TowerType::Count;
        selected_ = EntityId{};
    }
}

void HudScreen::select(EntityId id) {
    selected_ = id;
    if (id.valid()) {
        armed_tower_ = TowerType::Count;
        armed_ability_ = game::AbilityId::Count;
    }
}

bool HudScreen::cancel() {
    if (has_build_cursor() || has_cast_cursor() || selected_.valid()) {
        arm_tower(TowerType::Count);
        arm_ability(game::AbilityId::Count);
        selected_ = EntityId{};
        return true;
    }
    return false;
}

void HudScreen::press_ability(game::AbilityId a) {
    const HudAbility& st = model_.abilities[static_cast<usize>(a)];
    if (!st.unlocked || !st.ready) return;
    if (!st.needs_target) {
        // Fever Response ignores the target point (ActiveAbilities.h); no
        // reason to make the player click the world for it.
        Intent i{IntentKind::CastAbility};
        i.ability_id = a;
        push(i);
        arm_ability(game::AbilityId::Count);
        return;
    }
    arm_ability(armed_ability_ == a ? game::AbilityId::Count : a);
}

// ---- Sync ----------------------------------------------------------------------------------

void HudScreen::sync(const HudModel& m, Vec2 world_cursor) {
    model_ = m;
    if (!visible_) return;
    const Theme& th = gui_.theme();

    // Drop a selection whose tower is gone (sold, or destroyed by the horde).
    if (selected_.valid() &&
        std::none_of(m.towers.begin(), m.towers.end(), [&](const HudTower& t) { return t.id == selected_; })) {
        selected_ = EntityId{};
    }
    // Disarm what the player no longer owns.
    if (has_build_cursor() && !m.cards[static_cast<usize>(armed_tower_)].unlocked) arm_tower(TowerType::Count);
    if (has_cast_cursor() && !m.abilities[static_cast<usize>(armed_ability_)].ready) {
        arm_ability(game::AbilityId::Count);
    }

    // ---- Organ ----
    const bool critical = m.integrity01 < kCriticalIntegrity;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f%%", std::ceil(m.integrity01 * 100.0f - 0.5f));
    organ_pct_->set_text(buf);
    organ_pct_->style.color = th.color(critical ? "critical" : "ink");
    organ_bar_->set_level(m.integrity01, 0.4f);
    organ_bar_->shape.liquid = th.color(critical ? "critical" : "blood");
    const u32 seed = static_cast<u32>(organ_->shape.wobble_seed);
    organ_->shape = th.shape(critical ? "membrane.critical" : "membrane.host");
    organ_->shape.wobble_seed = static_cast<f32>(seed);
    critical_banner_->visible = critical;
    vignette_->visible = critical;

    // ---- Waves ----
    const bool prep = m.phase == HudWavePhase::Prep && !m.all_waves_complete;
    prep_banner_->visible = prep;
    critical_banner_->visible = critical && !prep;
    if (prep) {
        prep_clock_->set_text(format_clock(m.phase_time_remaining));
        prep_ring_->progress = m.prep_total > 0.0f ? math::saturate(m.phase_time_remaining / m.prep_total) : 0.0f;
        prep_wave_->set_text("Wave " + std::to_string(m.wave_number));
    }
    // "Wave N cleared" when a wave ends and prep for the next begins.
    if (m.phase == HudWavePhase::Prep && last_phase_ != HudWavePhase::Prep && last_wave_number_ > 0) {
        toast_text_->set_text("Wave " + std::to_string(last_wave_number_) + " cleared");
        toast_until_ = gui_.time() + kToastSeconds;
    }
    last_phase_ = m.phase;
    last_wave_number_ = m.wave_number;
    toast_->visible = gui_.time() < toast_until_;

    const HudWavePreview& pv = m.preview;
    wave_panel_->visible = pv.valid;
    if (pv.valid) {
        wave_title_->set_text("Wave " + std::to_string(pv.wave_number));
        wave_total_->set_text(std::to_string(pv.total));
        std::vector<HudFamilyCount> fams = pv.families;
        std::stable_sort(fams.begin(), fams.end(),
                         [](const HudFamilyCount& a, const HudFamilyCount& b) { return a.count > b.count; });
        for (usize i = 0; i < wave_rows_.size(); ++i) {
            wave_rows_[i]->visible = i < fams.size() && fams[i].count > 0;
            if (wave_rows_[i]->visible) wave_rows_[i]->sync(fams[i], pv.total);
        }
        elite_row_->visible = pv.elites > 0;
        elite_count_->set_text("\xC3\x97" + std::to_string(pv.elites));
    }

    // ---- Clock controls ----
    const bool paused = m.time_scale <= 0.0f;
    controls_[0]->shape = th.shape(paused ? "button.round.active" : "button.round");
    controls_[1]->shape = th.shape(!paused && m.time_scale < 1.5f ? "button.round.active" : "button.round");
    controls_[2]->shape = th.shape(m.time_scale >= 1.5f ? "button.round.active" : "button.round");
    for (usize i = 1; i < 3; ++i) {
        const bool active = i == 1 ? (!paused && m.time_scale < 1.5f) : m.time_scale >= 1.5f;
        static_cast<Label*>(controls_[i]->find("label"))->style.color = th.color(active ? "white" : "ink");
    }

    // ---- Dock ----
    atp_value_->set_text(std::to_string(m.atp));
    std::snprintf(buf, sizeof(buf), "ATP \xC2\xB7 +%.1f/s", static_cast<double>(m.income_per_second));
    atp_rate_->set_text(buf);
    for (usize i = 0; i < kTowerTypeCount; ++i) {
        cards_[i]->sync(m.cards[i], m.atp, armed_tower_ == static_cast<TowerType>(i));
    }
    usize gi = 0;
    for (const DockGroup& g : dock_groups()) {
        bool any = false;
        for (TowerType t : g.towers) any = any || m.cards[static_cast<usize>(t)].unlocked;
        dock_group_widgets_[gi++]->visible = any;
    }

    bool any_ability = false;
    for (usize i = 0; i < game::kAbilityCount; ++i) {
        ability_cells_[i]->sync(m.abilities[i], armed_ability_ == static_cast<game::AbilityId>(i));
        any_ability = any_ability || m.abilities[i].unlocked;
    }
    abilities_->visible = any_ability;
    // The dock sits left of centre when the ability panel shares the bottom.
    dock_->layout.offset.x = any_ability ? -100.0f : 0.0f;
    hint_->layout.offset.x = dock_->layout.offset.x;

    // ---- Armed cursor ----
    if (has_build_cursor()) {
        hint_->visible = true;
        hint_->sync("Place", th.color("lavender"));
    } else if (has_cast_cursor()) {
        hint_->visible = true;
        hint_->sync("Cast", th.color("aim"));
    } else {
        hint_->visible = false;
    }

    sync_world(m, world_cursor);
}

void HudScreen::sync_world(const HudModel& m, Vec2 world_cursor) {
    const Theme& th = gui_.theme();
    const bool prep = m.phase == HudWavePhase::Prep && !m.all_waves_complete;
    const bool critical = m.integrity01 < kCriticalIntegrity;
    const bool show_all_rings = has_build_cursor() || prep;

    // Per-tower overlays: create for new towers, drop for gone ones.
    for (auto it = tower_overlays_.begin(); it != tower_overlays_.end();) {
        const bool alive = std::any_of(m.towers.begin(), m.towers.end(),
                                       [&](const HudTower& t) { return t.id.value == it->first; });
        if (alive) {
            ++it;
            continue;
        }
        world_->remove(*it->second.ring);
        world_->remove(*it->second.pip);
        it = tower_overlays_.erase(it);
    }
    for (const HudTower& t : m.towers) {
        TowerOverlay& o = tower_overlays_[t.id.value];
        if (o.ring == nullptr) {
            o.ring = &world_->emplace<RangeRing>("ring" + std::to_string(t.id.value));
            o.pip = &world_->emplace<Meter>("pip" + std::to_string(t.id.value), th.shape("bar.health"));
            o.pip->layout.position = Position::World;
            o.pip->layout.pivot = Vec2{0.5f, 0.0f};
            o.pip->layout.offset = Vec2{0.0f, 24.0f};
            o.pip->blocks_pointer = false;
            fixed_size(*o.pip, 54, 11);
        }
        const bool selected = t.id == selected_;
        o.ring->world = t.world;
        o.ring->radius = t.range;
        o.ring->visible = show_all_rings || selected;
        if (selected) {
            o.ring->fill = with_alpha(th.color("lavender"), 0.22f);
            o.ring->stroke = th.color("lavender");
            o.ring->dash = 10;
            o.ring->gap = 7;
            o.ring->flow = 12;
        } else {
            o.ring->fill = kTransparent;
            o.ring->stroke = with_alpha(kWhite, 0.55f);
            o.ring->dash = 2;
            o.ring->gap = 8;
            o.ring->flow = 0;
        }
        const f32 frac = t.health_max > 0.0f ? t.health / t.health_max : 1.0f;
        o.pip->layout.world = t.world;
        o.pip->visible = critical || frac < 0.999f || selected;
        o.pip->shape.liquid = health_color(th, frac);
        o.pip->set_level(frac, 0.2f);
    }

    // Placement ghost.
    const bool placing = has_build_cursor() && m.placement.active;
    ghost_ring_->visible = placing;
    ghost_icon_->visible = placing;
    cursor_cost_->visible = false;
    if (placing) {
        const HudTowerCard& card = m.cards[static_cast<usize>(armed_tower_)];
        ghost_ring_->world = m.placement.world;
        ghost_ring_->radius = card.range;
        ghost_ring_->stroke = m.placement.valid ? kWhite : th.color("invalid");
        ghost_ring_->fill = with_alpha(m.placement.valid ? kWhite : th.color("invalid"), 0.10f);
        ghost_ring_->stroke_width = 3.5f;
        ghost_ring_->dash = 2;
        ghost_ring_->gap = 7;
        ghost_icon_->name = tower_icon(armed_tower_);
        ghost_icon_->layout.world = m.placement.world;
        ghost_icon_->grayscale = !m.placement.valid;
        // The price tag that follows the pointer (off the UI only).
        if (!gui_.wants_pointer()) {
            cursor_cost_->visible = true;
            cursor_cost_value_->set_text("\xE2\x88\x92" + std::to_string(card.cost));
            const i64 left = static_cast<i64>(m.atp) - static_cast<i64>(card.cost);
            cursor_cost_left_->set_text(left >= 0 ? std::to_string(left) + " left"
                                                  : std::to_string(-left) + " short");
            const Vec2 p = gui_.pointer();
            const Vec2 size = cursor_cost_->measure(gui_.viewport());
            cursor_cost_->layout.offset = Vec2{p.x - size.x - 34.0f, p.y - size.y * 0.5f - 44.0f};
        }
    }

    // Aim reticle.
    reticle_->visible = has_cast_cursor();
    if (has_cast_cursor()) {
        const HudAbility& a = m.abilities[static_cast<usize>(armed_ability_)];
        reticle_->world = world_cursor;
        reticle_->radius = a.radius;
        reticle_->stroke = th.color("aim");
        reticle_->fill = with_alpha(th.color("aim"), 0.12f);
    }

    // Selection: spinning ring, popup.
    const HudTower* sel = nullptr;
    for (const HudTower& t : m.towers) {
        if (t.id == selected_) sel = &t;
    }
    selection_ring_->visible = sel != nullptr;
    popup_->visible = sel != nullptr;
    if (sel != nullptr) {
        selection_ring_->layout.world = sel->world;
        popup_->sync(*sel);
    }

    // Incoming composition at the lane entrance during prep.
    spawn_pill_->visible = prep && m.has_spawn && m.preview.valid;
    if (spawn_pill_->visible) {
        const Vec2 at = gui_.project(m.spawn_world) + Vec2{30.0f, 70.0f};
        const Vec2 size = spawn_pill_->measure(gui_.viewport());
        const Vec2 vp = gui_.viewport();
        spawn_pill_->layout.offset = Vec2{math::clamp(at.x, 16.0f, math::max(vp.x - size.x - 16.0f, 16.0f)),
                                          math::clamp(at.y, 16.0f, math::max(vp.y - size.y - 16.0f, 16.0f))};
        for (usize i = 0; i < kFamilyCount; ++i) {
            u32 count = 0;
            for (const HudFamilyCount& f : m.preview.families) {
                if (static_cast<usize>(f.family) == i) count = f.count;
            }
            spawn_entries_[i]->visible = count > 0;
            spawn_counts_[i]->set_text("\xC3\x97" + std::to_string(count));
        }
    }
}

// ---- Input -----------------------------------------------------------------------------------

void HudScreen::handle_input(const platform::InputState& in, Vec2 world_cursor, bool pointer_over_ui,
                             std::vector<Intent>& out) {
    using platform::Action;
    using platform::MouseButton;

    // Pulses and timers that run whether or not anything was pressed.
    const f32 t = gui_.time();
    critical_banner_->opacity = loop::pulse_opacity(t);
    vignette_->opacity = loop::pulse_opacity(t);
    atp_icon_->anim_transform = Affine2::scale(loop::beat_scale(t));

    if (visible_ && !in.ui_capture_keyboard()) {
        // Number keys follow the dock order (the canvas's key badges).
        static constexpr Action kSelect[kTowerTypeCount] = {Action::SelectTower1, Action::SelectTower2,
                                                            Action::SelectTower3, Action::SelectTower4,
                                                            Action::SelectTower5};
        for (u32 slot = 0; slot < kTowerTypeCount; ++slot) {
            if (!in.action_pressed(kSelect[slot])) continue;
            const TowerType ty = dock_tower(slot);
            if (ty == TowerType::Count) continue;
            BuildCard* card = cards_[static_cast<usize>(ty)];
            if (!card->visible) continue;
            if (card->enabled) arm_tower(armed_tower_ == ty ? TowerType::Count : ty);
            else {
                card->shake();
                gui_.play(gui::UiSound::Deny);
            }
        }
        static constexpr Action kCast[game::kAbilityCount] = {Action::CastAbility1, Action::CastAbility2,
                                                               Action::CastAbility3, Action::CastAbility4};
        for (u32 i = 0; i < game::kAbilityCount; ++i) {
            if (!in.action_pressed(kCast[i])) continue;
            AbilityCell* cell = ability_cells_[i];
            if (!cell->visible) continue;
            if (cell->button->enabled) press_ability(static_cast<game::AbilityId>(i));
            else {
                cell->button->shake();
                gui_.play(gui::UiSound::Deny);
            }
        }
    }

    // World clicks: only when the pointer is not on the UI.
    if (visible_ && !pointer_over_ui) {
        const bool left = in.mouse_pressed(MouseButton::Left);
        const bool right = in.mouse_pressed(MouseButton::Right);
        if (has_build_cursor()) {
            // The cursor stays armed after a placement so the same tower can be
            // dropped repeatedly; right-click or Escape disarms it.
            if (left) {
                Intent i{IntentKind::PlaceTower};
                i.tower_type = armed_tower_;
                i.world_position = world_cursor;
                out.push_back(i);
            } else if (right) {
                arm_tower(TowerType::Count);
            }
        } else if (has_cast_cursor()) {
            if (left) {
                Intent i{IntentKind::CastAbility};
                i.ability_id = armed_ability_;
                i.world_position = world_cursor;
                out.push_back(i);
                arm_ability(game::AbilityId::Count);
            } else if (right) {
                arm_ability(game::AbilityId::Count);
            }
        } else if (left) {
            std::vector<std::pair<EntityId, Vec2>> candidates;
            candidates.reserve(model_.towers.size());
            for (const HudTower& tw : model_.towers) candidates.emplace_back(tw.id, tw.world);
            select(fmt::pick_nearest(candidates, world_cursor, kTowerPickRadius));
        } else if (right) {
            selected_ = EntityId{};
        }
    }

    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

} // namespace immune::ui
