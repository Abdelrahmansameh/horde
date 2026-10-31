#include "ui/hud/HudParts.h"

#include "core/Math.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/widgets/Builders.h"
#include "render/Renderer.h"

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
        case game::AbilityId::FeverResponse: return "Fever";
        case game::AbilityId::FibrinClot: return "Clot";
        default: break;
    }
    return "?";
}

const char* ability_icon(game::AbilityId id) {
    switch (id) {
        case game::AbilityId::ComplementCascadeBurst: return "ability_complement_cascade";
        case game::AbilityId::FeverResponse: return "ability_fever_response";
        case game::AbilityId::FibrinClot: return "ability_fibrin_clot";
        default: break;
    }
    return "";
}

const char* ability_color(game::AbilityId id) {
    switch (id) {
        case game::AbilityId::ComplementCascadeBurst: return "ability_complement";
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
        case PathogenFamily::Spore: return plural ? "Spores" : "Spore";
        case PathogenFamily::Count: break;
    }
    return "?";
}

const char* family_icon(PathogenFamily f) {
    switch (f) {
        case PathogenFamily::Virus: return "pathogen_virus";
        case PathogenFamily::Bacteria: return "pathogen_bacteria";
        case PathogenFamily::Parasite: return "pathogen_parasite";
        case PathogenFamily::Spore: return "pathogen_spore";
        case PathogenFamily::Count: break;
    }
    return "";
}

const std::vector<TowerType>& dock_order() {
    static const std::vector<TowerType> kOrder = {TowerType::Neutrophil, TowerType::CytotoxicT,
                                                  TowerType::Macrophage, TowerType::GobletCell,
                                                  TowerType::Fibroblast};
    return kOrder;
}

u32 dock_slot(TowerType t) {
    const auto& order = dock_order();
    for (u32 i = 0; i < order.size(); ++i) {
        if (order[i] == t) return i;
    }
    return static_cast<u32>(order.size());
}

TowerType dock_tower(u32 slot) {
    const auto& order = dock_order();
    return slot < order.size() ? order[slot] : TowerType::Count;
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

// ---- Drag panel ------------------------------------------------------------------------

DragPanel::DragPanel(std::string id, const ShapeDesc& s) : Panel(std::move(id), s) { interactive = true; }

namespace {
f32 inward_motion(DockEdge edge, Vec2 movement) {
    switch (edge) {
        case DockEdge::Left: return movement.x;
        case DockEdge::Right: return -movement.x;
        case DockEdge::Top: return movement.y;
        case DockEdge::Bottom: return -movement.y;
        case DockEdge::None: return 0.0f;
    }
    return 0.0f;
}

constexpr f32 kDockReach = 28.0f;
// The on-screen margin, which is also the spacing rails keep from obstacles.
constexpr f32 kEdgeGap = 8.0f;

// A docked panel at rest, which another panel's drag may push along its rail.
DragPanel* pushable(Widget* w) {
    auto* panel = dynamic_cast<DragPanel*>(w);
    if (panel == nullptr || !panel->visible || panel->dragging() || panel->dock_edge() == DockEdge::None) return nullptr;
    return panel;
}

bool apart(const Rect& a, const Rect& b) {
    return a.max.x <= b.min.x || b.max.x <= a.min.x || a.max.y <= b.min.y || b.max.y <= a.min.y;
}
}

void DragPanel::on_event(Event& e) {
    if (gui() == nullptr) return;
    if (e.type == EventType::Down) {
        motion_.clear();
        record_motion();
        undocked_from_ = DockEdge::None;
        undock_pull_ = 0.0f;
        newly_docked_ = false;
        dragging_ = false;
        // Pause the slide at its grab point while the pointer owns it.
        press_reveal_target_ = reveal_.target();
        reveal_.snap(reveal());
        entry_.snap(entry_.value());
    } else if (e.type == EventType::DragStart) {
        dragging_ = true;
    } else if (e.type == EventType::Up && !dragging_) {
        reveal_.set_target(press_reveal_target_);
        entry_.set_target(0.0f);
    } else if (e.type == EventType::Drag) {
        if (edge_ != DockEdge::None) {
            const f32 inward = inward_motion(edge_, e.delta);
            const f32 distance = slide_distance();
            const f32 amount = reveal() + (undock_pull_ + inward) / distance;
            constexpr f32 kUndockPull = 6.0f;
            if (amount > 1.0f + kUndockPull / distance) {
                const Vec2 position = layout_rect().min;
                Vec2 movement = e.delta;
                // Include the small pull held at full exposure so the freed
                // panel catches up with the pointer without losing travel.
                switch (edge_) {
                    case DockEdge::Left: movement.x += undock_pull_; break;
                    case DockEdge::Right: movement.x -= undock_pull_; break;
                    case DockEdge::Top: movement.y += undock_pull_; break;
                    case DockEdge::Bottom: movement.y -= undock_pull_; break;
                    case DockEdge::None: break;
                }
                undocked_from_ = edge_;
                dock(DockEdge::None);
                move_to(position + movement);
                clamp_on_screen();
            } else {
                // Preserve excess inward travel across pointer updates. The
                // reveal spring is clamped, but a slow pull must still release.
                undock_pull_ = math::max(0.0f, (amount - 1.0f) * distance);
                const bool vertical = edge_ == DockEdge::Left || edge_ == DockEdge::Right;
                const Vec2 slide = limit_push(layout_rect(), vertical ? Vec2{0.0f, e.delta.y} : Vec2{e.delta.x, 0.0f});
                dock_along_ += vertical ? slide.y : slide.x;
                if (std::fabs(inward) > 0.0f) {
                    reveal_.snap(math::saturate(amount));
                    if (inward > 0.0f) newly_docked_ = false;
                }
                pin_to_edge();
            }
        } else {
            layout.offset += limit_push(layout_rect(), e.delta);
            clamp_on_screen();
            if (dockable) {
                const DockEdge edge = nearest_edge(motion_velocity());
                if (edge != DockEdge::None) dock(edge);
            }
        }
        record_motion();
        input_layout_changed_ = true;
        e.handled = true;
    } else if (e.type == EventType::DragEnd) {
        // A throw is how the panel was moving, not where the pointer went: a
        // panel held against an edge or another panel has no velocity.
        const Vec2 velocity = motion_velocity();
        if (dockable && edge_ == DockEdge::None) {
            const DockEdge edge = nearest_edge(velocity, 0.12f, true);
            if (edge != DockEdge::None) dock(edge);
        }
        if (edge_ != DockEdge::None) {
            const f32 speed = inward_motion(edge_, velocity);
            const bool opening = !newly_docked_ && (std::fabs(speed) > 450.0f ? speed > 0.0f : reveal() > 0.5f);
            reveal_.set_target(opening ? 1.0f : 0.0f);
            entry_.set_target(0.0f);
            reveal_.kick(math::clamp(speed / slide_distance(), -8.0f, 8.0f));
        }
        dragging_ = false;
        input_layout_changed_ = true;
        e.handled = true;
    }
}

void DragPanel::before_layout(f32 dt) {
    if (gui() == nullptr) return;
    if (edge_ == DockEdge::None) {
        reveal_.snap(1.0f);
        entry_.snap(0.0f);
        clamp_on_screen();
        return;
    }
    reveal_.update(dt);
    entry_.update(dt);
    if (reveal_.settled()) reveal_.snap(reveal_.target());
    if (entry_.settled()) entry_.snap(entry_.target());
    pin_to_edge();
}

void DragPanel::update(f32 dt) {
    Panel::update(dt);
    // A captured drag changes layout after picking; settle its geometry in the
    // same frame. Uncaptured animation is applied before layout and picking.
    if (input_layout_changed_ && gui() != nullptr) arrange(layout_rect());
    input_layout_changed_ = false;
}

f32 DragPanel::slide_distance() {
    const Vec2 s = measure(gui()->viewport());
    return math::max(1.0f, (edge_ == DockEdge::Left || edge_ == DockEdge::Right ? s.x : s.y) - kPeek + 8.0f);
}

DockEdge DragPanel::nearest_edge(Vec2 movement, f32 projection, bool release) {
    const Rect r = layout_rect();
    const Vec2 vp = gui()->viewport();
    const f32 distances[] = {r.min.x, vp.x - r.max.x, r.min.y, vp.y - r.max.y};
    const DockEdge edges[] = {DockEdge::Left, DockEdge::Right, DockEdge::Top, DockEdge::Bottom};
    f32 nearest = kDockReach;
    DockEdge edge = DockEdge::None;
    for (usize i = 0; i < 4; ++i) {
        const f32 toward = -inward_motion(edges[i], movement);
        if (edges[i] == undocked_from_) {
            // Carrying the panel clear of the edge it left re-arms that edge.
            if (distances[i] <= kDockReach) continue;
            undocked_from_ = DockEdge::None;
        }
        // A panel resting on an edge (held by the screen clamp) docks there
        // even while it slides along it, or is let go after a sideways move.
        const bool resting = distances[i] <= kEdgeGap + 0.5f;
        if (toward < 0.0f ? !(release && resting) : (toward == 0.0f && !resting)) continue;
        const f32 distance = distances[i] - (toward > 450.0f ? math::min(toward * projection, 160.0f) : 0.0f);
        if (distance < nearest) {
            nearest = distance;
            edge = edges[i];
        }
    }
    return edge;
}

Rect DragPanel::layout_rect() {
    const Vec2 s = measure(gui()->viewport());
    const Rect host{Vec2{0.0f, 0.0f}, gui()->viewport()};
    const Vec2 at = host.min + Vec2{layout.anchor.x * host.size().x, layout.anchor.y * host.size().y};
    const Vec2 pos = at - Vec2{layout.pivot.x * s.x, layout.pivot.y * s.y} + layout.offset;
    return Rect{pos, pos + s};
}

void DragPanel::move_to(Vec2 position) {
    layout.offset += position - layout_rect().min;
}

void DragPanel::clamp_on_screen() {
    if (gui() == nullptr) return;
    const Vec2 vp = gui()->viewport();
    const Rect r = layout_rect();
    constexpr f32 kMargin = 8.0f;
    move_to(Vec2{math::clamp(r.min.x, kMargin, math::max(kMargin, vp.x - r.size().x - kMargin)),
                 math::clamp(r.min.y, kMargin, math::max(kMargin, vp.y - r.size().y - kMargin))});
}

namespace {
constexpr f32 kMotionWindow = 0.08f;
}

void DragPanel::record_motion() {
    const f32 now = gui()->time();
    motion_.push_back(MotionSample{now, layout_rect().min});
    // Keep one sample from before the window as its baseline.
    while (motion_.size() > 2 && motion_[1].time <= now - kMotionWindow) motion_.erase(motion_.begin());
}

Vec2 DragPanel::motion_velocity() {
    if (motion_.empty()) return Vec2{0.0f, 0.0f};
    const f32 now = gui()->time();
    const MotionSample* base = &motion_.front();
    for (const MotionSample& s : motion_) {
        if (s.time <= now - kMotionWindow) base = &s;
    }
    return (layout_rect().min - base->pos) / math::max(now - base->time, 1.0f / 60.0f);
}

Vec2 DragPanel::limit_push(Rect from, Vec2 delta) {
    const Vec2 vp = gui()->viewport();
    const Vec2 gap{kEdgeGap, kEdgeGap};
    for (Widget* w : dock_obstacles) {
        DragPanel* other = pushable(w);
        if (other == nullptr) continue;
        const Rect to{from.min + delta, from.max + delta};
        const Rect body{other->rect().min - gap, other->rect().max + gap};
        if (apart(to, body)) continue;
        const DockEdge edge = other->dock_edge();
        const int along = edge == DockEdge::Left || edge == DockEdge::Right ? 1 : 0;
        const int across = 1 - along;
        if (from.max[along] > body.min[along] && from.min[along] < body.max[along]) {
            // Pressing onto it toward its edge: stop at its face, or hold
            // still if already over it, rather than squeezing it aside.
            if (edge == DockEdge::Left || edge == DockEdge::Top) {
                delta[across] = math::max(delta[across], math::min(0.0f, body.max[across] - from.min[across]));
            } else {
                delta[across] = math::min(delta[across], math::max(0.0f, body.min[across] - from.max[across]));
            }
            continue;
        }
        // Beside it: entering its rail pushes it along, until the far end of
        // the edge leaves it no room.
        Rect rail = body;
        if (edge == DockEdge::Left) rail.max.x = kPeek + kEdgeGap;
        if (edge == DockEdge::Right) rail.min.x = vp.x - kPeek - kEdgeGap;
        if (edge == DockEdge::Top) rail.max.y = kPeek + kEdgeGap;
        if (edge == DockEdge::Bottom) rail.min.y = vp.y - kPeek - kEdgeGap;
        if (apart(to, rail)) continue;
        const f32 length = other->rect().size()[along];
        const f32 last = math::max(kEdgeGap, vp[along] - length - kEdgeGap);
        if (delta[along] > 0.0f) {
            delta[along] = math::min(delta[along], math::max(0.0f, last - kEdgeGap - from.max[along]));
        } else if (delta[along] < 0.0f) {
            delta[along] = math::max(delta[along], math::min(0.0f, 2.0f * kEdgeGap + length - from.min[along]));
        }
    }
    return delta;
}

void DragPanel::pin_to_edge(bool settle) {
    clamp_on_screen();
    const Rect r = layout_rect();
    Vec2 pos = r.min;
    const f32 exposure = reveal();
    switch (edge_) {
        case DockEdge::Left: pos.x = kPeek - r.size().x + slide_distance() * exposure; break;
        case DockEdge::Right: pos.x = gui()->viewport().x - kPeek - slide_distance() * exposure; break;
        case DockEdge::Top: pos.y = kPeek - r.size().y + slide_distance() * exposure; break;
        case DockEdge::Bottom: pos.y = gui()->viewport().y - kPeek - slide_distance() * exposure; break;
        case DockEdge::None: return;
    }
    // Choose the nearest clear stretch of the edge. In a crowded viewport,
    // minimize covered area while keeping the whole rail within reach.
    const bool vertical = edge_ == DockEdge::Left || edge_ == DockEdge::Right;
    const f32 length = vertical ? r.size().y : r.size().x;
    const f32 limit = math::max(8.0f, (vertical ? gui()->viewport().y : gui()->viewport().x) - length - 8.0f);
    const f32 desired = dock_along_;
    std::vector<f32> candidates{desired, 8.0f, limit};
    std::vector<Rect> obstacles;
    for (Widget* w : dock_obstacles) {
        if (w == nullptr || !w->visible) continue;
        // A dragged rail moves through docked panels; they give way along
        // their own edges (limit_push keeps that from squeezing them).
        if (!settle && dragging_ && pushable(w) != nullptr) continue;
        Rect obstacle = w->rect();
        obstacle.min -= Vec2{8.0f, 8.0f};
        obstacle.max += Vec2{8.0f, 8.0f};
        obstacles.push_back(obstacle);
        candidates.push_back((vertical ? obstacle.min.y : obstacle.min.x) - length);
        candidates.push_back(vertical ? obstacle.max.y : obstacle.max.x);
    }
    f32 best_overlap = gui::kUnbounded;
    f32 best_distance = gui::kUnbounded;
    f32 best = desired;
    for (f32 candidate : candidates) {
        candidate = math::clamp(candidate, 8.0f, limit);
        const Vec2 start = vertical ? Vec2{pos.x, candidate} : Vec2{candidate, pos.y};
        Rect rail{start, start + r.size()};
        // Reserve the same edge strip throughout a slide, so opening does not
        // teleport the drawer sideways to chase changing collision bounds.
        if (edge_ == DockEdge::Left) { rail.min.x = 0.0f; rail.max.x = kPeek; }
        if (edge_ == DockEdge::Right) { rail.min.x = gui()->viewport().x - kPeek; rail.max.x = gui()->viewport().x; }
        if (edge_ == DockEdge::Top) { rail.min.y = 0.0f; rail.max.y = kPeek; }
        if (edge_ == DockEdge::Bottom) { rail.min.y = gui()->viewport().y - kPeek; rail.max.y = gui()->viewport().y; }
        f32 overlap = 0.0f;
        for (const Rect& obstacle : obstacles) {
            overlap += math::max(0.0f, math::min(rail.max.x, obstacle.max.x) - math::max(rail.min.x, obstacle.min.x)) *
                       math::max(0.0f, math::min(rail.max.y, obstacle.max.y) - math::max(rail.min.y, obstacle.min.y));
        }
        const f32 distance = std::fabs(candidate - desired);
        if (overlap < best_overlap || (overlap == best_overlap && distance < best_distance)) {
            best_overlap = overlap;
            best_distance = distance;
            best = candidate;
        }
    }
    dock_along_ = best;
    if (vertical) pos.y = best;
    else pos.x = best;
    pos += entry_offset_ * entry_.value();
    move_to(pos);
}

void DragPanel::dock(DockEdge edge) {
    const Vec2 position = layout_rect().min;
    edge_ = edge;
    dock_changed(edge);
    undock_pull_ = 0.0f;
    if (on_dock_changed) on_dock_changed(edge);
    reveal_.snap(1.0f);
    if (edge != DockEdge::None) reveal_.set_target(0.0f);
    entry_offset_ = Vec2{0.0f, 0.0f};
    entry_.snap(0.0f);
    dock_along_ = edge == DockEdge::Left || edge == DockEdge::Right ? position.y : position.x;
    newly_docked_ = edge != DockEdge::None;
    layout.anchor = layout.pivot = Vec2{edge == DockEdge::Right ? 1.0f : 0.0f,
                                       edge == DockEdge::Bottom ? 1.0f : 0.0f};
    move_to(position);
    if (edge != DockEdge::None) {
        pin_to_edge(true);
        // Retain both coordinates of the release frame, including any offset
        // needed to find a free stretch beside another HUD panel.
        entry_offset_ = position - layout_rect().min;
        entry_.snap(1.0f);
        entry_.set_target(0.0f);
        pin_to_edge(true);
    }
    input_layout_changed_ = true;
}

Vec2 BuildDock::content_size(Vec2 available) {
    Widget* atp = find("atp");
    const bool shown = atp != nullptr && atp->visible;
    if (atp != nullptr) atp->visible = true;
    const Vec2 result = Panel::content_size(available);
    if (atp != nullptr) atp->visible = shown;
    return result;
}

void BuildDock::arrange_children(Rect inner) {
    const f32 exposure = dock_edge() == DockEdge::None ? 1.0f : reveal();
    Widget* atp = find("atp");
    if (atp != nullptr) atp->visible = true;
    std::vector<BuildCard*> cards;
    for (const auto& child : children()) {
        if (auto* card = dynamic_cast<BuildCard*>(child.get()); card != nullptr && card->visible) {
            card->set_reveal(exposure);
            cards.push_back(card);
        }
    }
    Panel::arrange_children(inner);
    if (atp != nullptr) {
        atp->visible = exposure > 0.001f;
        atp->opacity = exposure;
    }
    if (dock_edge() == DockEdge::None) return;
    const bool vertical = dock_edge() == DockEdge::Left || dock_edge() == DockEdge::Right;
    const f32 length = cards.empty() ? 0.0f : static_cast<f32>(cards.size()) * 56.0f +
                                                static_cast<f32>(cards.size() - 1) * 4.0f;
    for (usize i = 0; i < cards.size(); ++i) {
        BuildCard& card = *cards[i];
        const Rect full = card.rect();
        Vec2 pos;
        if (vertical) {
            pos = Vec2{dock_edge() == DockEdge::Left ? rect().max.x - 64.0f : rect().min.x + 8.0f,
                       full.center().y - 28.0f};
        } else {
            pos = Vec2{rect().center().x - length * 0.5f + static_cast<f32>(i) * 60.0f,
                       dock_edge() == DockEdge::Top ? rect().max.y - 64.0f : rect().min.y + 8.0f};
        }
        card.arrange(Rect{math::lerp(pos, full.min, exposure),
                          math::lerp(pos + Vec2{56.0f, 56.0f}, full.max, exposure)});
    }
}

// ---- Ability bar -----------------------------------------------------------------------

AbilityBar::AbilityBar(std::string id, const ShapeDesc& s) : DragPanel(std::move(id), s) {
    layout.align = Align::Center;
    layout.gap = 14.0f;
    clip_children = true;
    dockable = true;
    dock_changed(DockEdge::None);
}

void AbilityBar::dock_changed(DockEdge edge) {
    // Standing on end along a side keeps the docked rail as long as the
    // cells need; across the top or bottom the row already is.
    const bool side = edge == DockEdge::Left || edge == DockEdge::Right;
    layout.axis = side ? Axis::Column : Axis::Row;
    layout.width = side ? Size::px(156.0f) : Size::fit();
    layout.height = side ? Size::fit() : Size::px(156.0f);
    layout.padding = side ? Insets{10.0f, 20.0f, 10.0f, 20.0f} : Insets{20.0f, 10.0f, 20.0f, 10.0f};
}

void AbilityBar::arrange_children(Rect inner) {
    const f32 exposure = dock_edge() == DockEdge::None ? 1.0f : reveal();
    std::vector<AbilityCell*> cells;
    for (const auto& child : children()) {
        if (auto* cell = dynamic_cast<AbilityCell*>(child.get()); cell != nullptr && cell->visible) {
            cell->set_reveal(exposure);
            cells.push_back(cell);
        }
    }
    Panel::arrange_children(inner);
    if (dock_edge() == DockEdge::None) return;
    const bool vertical = dock_edge() == DockEdge::Left || dock_edge() == DockEdge::Right;
    for (AbilityCell* cell : cells) {
        const Rect full = cell->rect();
        Vec2 pos;
        if (vertical) {
            pos = Vec2{dock_edge() == DockEdge::Left ? rect().max.x - 64.0f : rect().min.x + 8.0f,
                       full.center().y - 28.0f};
        } else {
            pos = Vec2{full.center().x - 28.0f,
                       dock_edge() == DockEdge::Top ? rect().max.y - 64.0f : rect().min.y + 8.0f};
        }
        cell->arrange(Rect{math::lerp(pos, full.min, exposure),
                           math::lerp(pos + Vec2{56.0f, 56.0f}, full.max, exposure)});
    }
}

// ---- Build card ------------------------------------------------------------------------

BuildCard::BuildCard(Gui& g, TowerType t, u32 slot)
    : Button(tower_id(t), g.theme().shape("card")), type(t), gui_(g) {
    const Theme& th = g.theme();
    // A horizontal strip: key badge, icon, name (and the shortfall under it), cost.
    fixed_size(*this, 276.0f, 84.0f);
    layout.padding = Insets{18.0f, 12.0f, 24.0f, 12.0f};
    layout.axis = Axis::Row;
    layout.align = Align::Center;
    layout.gap = 10.0f;

    badge_ = &emplace<Badge>(g, "key", std::to_string(slot + 1));
    icon_ = &icon(*this, "icon", tower_icon(t), 54.0f);
    Widget& names = column(*this, 1.0f, Align::Start);
    names_ = &names;
    names.layout.grow = 1.0f;
    name_ = &text(names, "name", tower_display_name(t), th.text("card_name"));
    short_ = &text(names, "short", "", th.text("card_short"));
    Widget& cost = row(*this, 2.0f, Align::Center);
    price_ = &cost;
    Icon& coin = icon(cost, "atp", "atp", 22.0f);
    coin.anim_transform = Affine2::rotate(-0.14f);
    cost_ = &text(cost, "cost", "0", th.text("card_cost"));

    has_disabled_shape = true;
    disabled_shape = th.shape("card.short");
}

void BuildCard::sync(const HudTowerCard& c, u32 atp, bool armed) {
    card_ = c;
    atp_ = atp;
    armed_ = armed;
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
        tooltip = "This level does not allow the " + std::string(tower_display_name(type));
    } else {
        short_->visible = false;
        tooltip.clear();
    }
    if (compact_) {
        tooltip = std::string(tower_display_name(type)) + " | " + std::to_string(c.cost) + " ATP";
        if (!c.allowed) tooltip += " | Not on this level";
        else if (!affordable) tooltip += " | Need " + std::to_string(c.cost - atp) + " more ATP";
    }
    nudge = armed && !compact_ ? Vec2{14.0f, 0.0f} : Vec2{0.0f, 0.0f};
    sync_availability_style();
}

void BuildCard::sync_availability_style() {
    const Theme& th = gui_.theme();
    disabled_shape = th.shape("card.short");
    icon_->opacity = 1.0f;
    if (!compact_ || atp_ >= card_.cost) deny_tint_.snap(0.0f);
    if (!compact_ || enabled) return;

    // A quiet warm edge and a dimmer silhouette separate unavailable icons
    // from affordable cells without adding anything to the compact strip.
    const f32 feedback = deny_tint_.value();
    const Color warm = th.color("critical_cytoplasm");
    const Color warning = th.color("invalid");
    disabled_shape.fill = mix(disabled_shape.fill, warm, 0.24f + feedback * 0.24f);
    disabled_shape.fill2 = mix(disabled_shape.fill2, warm, 0.36f + feedback * 0.32f);
    disabled_shape.stroke = mix(disabled_shape.stroke, warning, 0.48f + feedback * 0.30f);
    disabled_shape.rim = with_alpha(warning, 0.32f + feedback * 0.22f);
    icon_->opacity = 0.64f + feedback * 0.14f;
}

void BuildCard::on_event(Event& e) {
    if (e.type == EventType::Deny && e.button == PointerButton::Left &&
        compact_ && atp_ < card_.cost) {
        deny_tint_.snap(1.0f);
        deny_tint_.start(0.0f, 0.28f, Ease::OutCubic);
        sync_availability_style();
    }
    Button::on_event(e);
}

void BuildCard::update(f32 dt) {
    Button::update(dt);
    deny_tint_.update(dt);
    sync_availability_style();
}

void BuildCard::set_compact(bool compact) {
    set_reveal(compact ? 0.0f : 1.0f);
}

void BuildCard::set_reveal(f32 exposure) {
    const bool compact = exposure < 0.999f;
    const bool changed = compact_ != compact;
    compact_ = compact;
    reveal_ = exposure;
    const f32 details = math::saturate((exposure - 0.45f) / 0.55f);
    badge_->visible = names_->visible = price_->visible = details > 0.001f;
    badge_->opacity = names_->opacity = price_->opacity = details;
    clip_children = compact;
    hover_scale = compact ? 1.02f : 1.04f;
    if (changed) sync(card_, atp_, armed_);
}

void BuildCard::arrange_children(Rect inner) {
    Panel::arrange_children(inner);
    if (reveal_ >= 0.999f) return;
    const Rect full = icon_->rect();
    const Vec2 pos = rect().center() - Vec2{22.0f, 22.0f};
    icon_->arrange(Rect{math::lerp(pos, full.min, reveal_),
                        math::lerp(pos + Vec2{44.0f, 44.0f}, full.max, reveal_)});
}

// ---- Ability cell ----------------------------------------------------------------------

AbilityCell::AbilityCell(Gui& g, game::AbilityId id, std::string key)
    : Widget(ability_id_name(id)), ability(id), gui_(g) {
    const Theme& th = g.theme();

    layout.axis = Axis::Column;
    layout.align = Align::Center;
    layout.gap = 4.0f;

    Widget& cell = stack(*this, Align::Center, "cell");
    cell_ = &cell;
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
    keys_ = &keys;
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

void AbilityCell::set_reveal(f32 exposure) {
    reveal_ = exposure;
    const f32 details = math::saturate((exposure - 0.45f) / 0.55f);
    keys_->visible = details > 0.001f;
    keys_->opacity = details;
}

void AbilityCell::arrange_children(Rect inner) {
    Widget::arrange_children(inner);
    if (reveal_ >= 0.999f) return;
    // Docked: the fluid cell, its halo and glyph shrink about this widget's
    // centre, which the bar has already moved into the exposed strip.
    const Vec2 c = rect().center();
    const auto shrink = [&](Widget& w, f32 compact) {
        const Rect full = w.rect();
        const Vec2 half{compact * 0.5f, compact * 0.5f};
        w.arrange(Rect{math::lerp(c - half, full.min, reveal_), math::lerp(c + half, full.max, reveal_)});
    };
    shrink(*cell_, 56.0f);
    shrink(*halo_, 62.0f);
    shrink(*button, 54.0f);
    shrink(*glyph_, 34.0f);
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
    icon_->name = family_icon(f.family);
    name_->set_text(family_display_name(f.family, true));
    count_->set_text("\xC3\x97" + std::to_string(f.count));
    // Read the current palette so a tuning reload also updates the wave bars.
    bar_->shape.liquid = render::family_color(f.family);
    bar_->set_level(total > 0 ? static_cast<f32>(f.count) / static_cast<f32>(total) : 0.0f);
}

// ---- Enemy entrance -------------------------------------------------------------------------

SpawnMarker::SpawnMarker(Gui& g, std::string id)
    : Panel(std::move(id), g.theme().shape("spawn.entry")), gui_(g) {
    layout.position = Position::Anchored;
    layout.pivot = Vec2{0.5f, 0.5f};
    fixed_size(*this, 68.0f, 68.0f);
    shape_inset = Insets::all(7.0f);
    blocks_pointer = false;
    accepts_pointer = false;
}

void SpawnMarker::draw_self(DrawList& dl) {
    const Theme& th = gui_.theme();
    // A soft breathing rim uses the same warm accent as the wave UI. The
    // badge stays still, keeping the path endpoint easy to locate.
    ShapeDesc halo;
    halo.kind = ShapeKind::Ellipse;
    halo.center = center();
    const f32 pulse = preparing ? 0.5f + 0.5f * std::sin(gui_.time() * 2.8f) : 0.0f;
    halo.half_size = Vec2{31.0f, 31.0f} + Vec2{2.0f, 2.0f} * pulse;
    halo.stroke = with_alpha(th.color("gold"), preparing ? 0.28f + pulse * 0.22f : 0.22f);
    halo.stroke_width = 1.5f;
    dl.shape(halo);
    Panel::draw_self(dl);

    const Vec2 d = math::length_sq(direction) > math::kEpsilon
                       ? math::normalize_safe(direction) : Vec2{1.0f, 0.0f};
    const Vec2 side{-d.y, d.x};
    const auto at = [&](f32 x, f32 y) { return center() + d * x + side * y; };
    // Small dots feed into two rounded chevrons along the path direction,
    // describing its flow without promising a pathogen or count.
    for (f32 y : {-5.0f, 5.0f}) {
        ShapeDesc dot;
        dot.kind = ShapeKind::Ellipse;
        dot.center = at(-15.0f, y);
        dot.half_size = Vec2{2.2f, 2.2f};
        dot.fill = th.color("gold");
        dl.shape(dot);
    }
    for (f32 x : {-7.0f, 6.0f}) {
        const Vec2 chevron[] = {at(x, -8.0f), at(x + 7.0f, 0.0f), at(x, 8.0f)};
        dl.stroke_polyline(chevron, false, StrokeStyle{4.5f, th.color("plum")});
    }
}

// ---- Organ destination ----------------------------------------------------------------------

OrganMarker::OrganMarker(Gui& g, std::string id)
    : SpawnMarker(g, std::move(id)) {}

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
