#include "ui/front/TreeScreen.h"

#include "gui/anim/Anim.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/style/Theme.h"
#include "gui/widgets/Builders.h"
#include "gui/widgets/Widgets.h"
#include "platform/FileIO.h"
#include "ui/front/Backdrop.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace immune::ui {

using namespace gui;

// ---- Layout file ---------------------------------------------------------------------

bool parse_tree_layout(const std::string& text, TreeLayout& out, std::string* error) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "tree layout is not a JSON object";
        return false;
    }
    TreeLayout l;
    try {
        for (const auto& [key, n] : j.at("nodes").items()) {
            l.nodes[key] = TreeLayout::Node{Vec2{n.at("x").get<f32>(), n.at("y").get<f32>()}, n.at("size").get<f32>(),
                                            n.at("icon").get<std::string>()};
        }
        for (const auto& v : j.at("vessels")) {
            TreeLayout::Vessel vessel;
            vessel.d = v.at("d").get<std::string>();
            vessel.wall = v.value("wall", 10.0f);
            vessel.lumen = v.value("lumen", 5.0f);
            vessel.node = v.value("node", std::string());
            vessel.flow = v.value("flow", 0.0f);
            l.vessels.push_back(std::move(vessel));
        }
        for (const auto& b : j.at("labels")) {
            l.labels.push_back(TreeLayout::Label{Vec2{b.at("x").get<f32>(), b.at("y").get<f32>()},
                                                 b.at("text").get<std::string>(), b.value("style", std::string("root")),
                                                 b.value("branch", std::string())});
        }
        const auto& hub = j.at("hub");
        l.hub = Vec2{hub.at("x").get<f32>(), hub.at("y").get<f32>()};
        l.hub_scale = hub.value("scale", 0.72f);
    } catch (const nlohmann::json::exception& e) {
        if (error) *error = std::string("tree layout: ") + e.what();
        return false;
    }
    out = std::move(l);
    return true;
}

bool load_tree_layout(const std::string& path, TreeLayout& out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_tree_layout(ss.str(), out, error);
}

// ---- Widgets -------------------------------------------------------------------------

/// Every vessel of the tree in one widget: walls, lumens lit by what the
/// player owns, and plasma flowing along the trunks.
class TreeVessels : public Widget {
public:
    struct Entry {
        Path path;
        f32 wall, lumen, flow;
        std::string node;
        bool lit = true;
    };
    std::vector<Entry> entries;
    /// Set when a vessel's lit state changes: the walls and lumens are
    /// tessellated once into a mesh and only re-recorded then.
    bool dirty = true;

    TreeVessels() : Widget("vessels") {}

    void draw_self(DrawList& dl) override {
        if (dirty || mesh_origin_ != rect().min || mesh_device_px_ != dl.device_px()) record(dl);
        dl.append_solid(mesh_.vertices, mesh_.indices);
        // Plasma flowing along the trunks, on top.
        StrokeStyle s;
        s.color = with_alpha(kWhite, 0.35f);
        s.dash_length = 4.0f;
        s.dash_gap = 18.0f;
        s.dash_offset = -loop::flow_dash_offset(dl.time());
        dl.push_transform(Affine2::translate(rect().min));
        for (const Entry& e : entries) {
            if (e.flow <= 0.0f) continue;
            s.width = e.flow;
            e.path.stroke(dl, s);
        }
        dl.pop_transform();
    }

private:
    void record(const DrawList& dl) {
        const Theme& th = gui()->theme();
        DrawList scratch;
        scratch.reset(dl.viewport(), dl.device_px(), 0.0f);
        scratch.push_transform(Affine2::translate(rect().min));
        for (const Entry& e : entries) {
            StrokeStyle s;
            s.width = e.wall;
            s.color = th.color("plum");
            e.path.stroke(scratch, s);
            s.width = e.lumen;
            s.color = th.color(e.lit ? "lavender" : "vein_off");
            e.path.stroke(scratch, s);
        }
        mesh_.capture(scratch);
        mesh_origin_ = rect().min;
        mesh_device_px_ = dl.device_px();
        dirty = false;
    }

    SolidMesh mesh_;
    Vec2 mesh_origin_{-1e9f, -1e9f};
    f32 mesh_device_px_ = 0.0f;
};

/// One node: a wobbly cell (a spiky star for a capstone) whose fill and rim
/// say its state, its glyph, and level pips round its lower rim. Geometry in
/// the canvas's 100-unit node viewBox.
class TreeNodeButton : public Button {
public:
    TreeNodeButton(Gui& g, std::string id, f32 size, std::string icon)
        : Button(std::move(id)), gui_(g), icon_(std::move(icon)) {
        fixed_size(*this, size, size);
        shape.kind = ShapeKind::Ellipse;
        hover_scale = 1.08f;
    }

    TreeNodeView view;
    bool selected = false;
    const std::string& icon_name() const { return icon_; }

    void draw_self(DrawList& dl) override {
        const Theme& th = gui_.theme();
        const Vec2 c = center();
        const f32 u = size().x / 100.0f;
        const f32 t = dl.time();
        const bool locked = view.state == TreeNodeState::Locked;
        const bool capstone = view.role == TreeNodeRole::Capstone;
        const bool owned = view.level > 0;

        if (view.state == TreeNodeState::Available) {
            ShapeDesc halo;
            halo.kind = ShapeKind::Ellipse;
            halo.center = c;
            halo.half_size = Vec2{56.0f, 56.0f} * u;
            const f32 a = loop::halo_opacity(t);
            halo.fill = with_alpha(kWhite, 0.22f * a);
            halo.stroke = with_alpha(kWhite, a);
            halo.stroke_width = 4.0f * u;
            dl.shape(halo);
        }
        if (selected) {
            ShapeDesc ring;
            ring.kind = ShapeKind::Arc;
            ring.center = c;
            ring.half_size = Vec2{62.0f, 62.0f} * u;
            ring.stroke = kWhite;
            ring.stroke_width = 6.0f * u;
            ring.dash_length = 14.0f * u;
            ring.dash_gap = 10.0f * u;
            ring.dash_offset = -loop::spin_rotation(t) * ring.half_size.x;
            dl.shape(ring);
        }

        if (locked) dl.push_alpha(0.5f);
        // Canvas: a ~40-unit cell with a gentle wobble; capstones are an
        // 8-lobed star between 34 and 44 units.
        ShapeDesc body;
        body.kind = ShapeKind::Ellipse;
        body.center = c;
        body.half_size = Vec2{capstone ? 39.0f : 40.0f, capstone ? 39.0f : 40.0f} * u;
        body.wobble_amp = (capstone ? 5.0f : 1.3f) * u;
        body.wobble_wavelength = capstone ? body.half_size.x * math::kTwoPi / 8.0f : 60.0f * u;
        body.wobble_seed = static_cast<f32>(view.node) * 3.7f;
        body.wobble_speed = capstone ? 0.0f : 0.8f;

        ShapeDesc shadow = body;
        shadow.center = c + Vec2{0.0f, 6.0f * u};
        shadow.fill = with_alpha(th.color("dim"), 0.35f);
        dl.shape(shadow);
        if (capstone) {
            ShapeDesc gold = body;
            gold.half_size = body.half_size * 1.12f;
            gold.wobble_amp = body.wobble_amp * 1.12f;
            gold.wobble_wavelength = body.wobble_wavelength * 1.12f;
            gold.stroke = th.color("gold");
            gold.stroke_width = 5.0f * u;
            dl.shape(gold);
        }
        body.fill = th.color(view.state == TreeNodeState::Maxed ? "tree_fill_maxed"
                             : owned                             ? "tree_fill_owned"
                             : locked                            ? "locked"
                                                                 : "tree_fill_open");
        body.stroke = th.color("plum");
        body.stroke_width = 5.0f * u;
        dl.shape(body);
        ShapeDesc rim = body;
        rim.half_size = body.half_size * 0.83f;
        rim.wobble_amp = body.wobble_amp * 0.83f;
        rim.wobble_wavelength = body.wobble_wavelength * 0.83f;
        rim.fill = kTransparent;
        rim.stroke = th.color(owned                                       ? "lavender_deep"
                              : view.state == TreeNodeState::Available ? "lavender"
                              : view.state == TreeNodeState::Short     ? "tree_rim_short"
                                                                       : "level_ring_locked");
        dl.shape(rim);

        const f32 glyph = view.role == TreeNodeRole::TowerRoot     ? 62.0f
                          : view.role == TreeNodeRole::AbilityRoot ? 49.5f
                                                                   : 60.8f;
        const Vec2 gh = Vec2{glyph, glyph} * (0.5f * u);
        gui_.icons().draw(dl, icon_, Rect{c - gh, c + gh}, kWhite, gui_.scale());

        // Level pips: 23 degrees apart round the bottom, filled from the right.
        if (view.role == TreeNodeRole::Stat) {
            const i32 n = view.max_level;
            for (i32 i = 0; i < n; ++i) {
                const f32 deg = 90.0f + 23.0f * (static_cast<f32>(i) - static_cast<f32>(n - 1) * 0.5f);
                const f32 a = deg * math::kPi / 180.0f;
                ShapeDesc pip;
                pip.kind = ShapeKind::Ellipse;
                pip.center = c + Vec2{-std::cos(a), std::sin(a)} * (47.0f * u);
                pip.half_size = Vec2{8.5f, 8.5f} * u;
                pip.fill = i < view.level ? th.color("lavender_ink") : kWhite;
                pip.stroke = th.color("plum");
                pip.stroke_width = 3.5f * u;
                dl.shape(pip);
            }
        }
        if (locked) dl.pop_alpha();
    }

private:
    Gui& gui_;
    std::string icon_;
};

// ---- Screen ------------------------------------------------------------------------

namespace {

void fill_parent(Widget& w) {
    anchor(w, Vec2{0, 0}, Vec2{0, 0});
    w.layout.width = Size::pct(1.0f);
    w.layout.height = Size::pct(1.0f);
}

Panel& pill(Widget& parent, Theme& th, std::string id, const char* shape, const std::string& label, const char* style) {
    Panel& p = parent.emplace<Panel>(std::move(id), th.shape(shape));
    p.blocks_pointer = false;
    p.layout.axis = Axis::Stack;
    p.layout.align = Align::Center;
    p.layout.padding = Insets{12, 5, 12, 5};
    text(p, "text", label, th.text(style), TextAlign::Center);
    return p;
}

std::string with_dot_points(const std::string& name, u32 points, u32 threshold) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " \xC2\xB7 %u/%u", math::min(points, threshold), threshold);
    return name + buf;
}

} // namespace

TreeScreen::TreeScreen(Gui& gui, Widget& root, const TreeLayout& layout, std::function<void(MenuResult)> emit)
    : gui_(gui), emit_(std::move(emit)) {
    Theme& th = gui_.theme();
    fill_parent(root.emplace<TissueBackdrop>("backdrop"));
    Panel& veil = root.emplace<Panel>("veil", th.shape("veil"));
    veil.shape.fill = with_alpha(th.color("dim"), 0.5f);
    fill_parent(veil);

    // The tree itself, in canvas pixels, centred across and pinned to the
    // top, so on a taller-than-16:9 screen its top bar stays level with the
    // title and the wallet.
    Widget& stage = root.emplace<Widget>();
    anchor(stage, Vec2{0.5f, 0.0f}, Vec2{0.5f, 0.0f});
    fixed_size(stage, 1920.0f, 1080.0f);

    vessels_ = &stage.emplace<TreeVessels>();
    fill_parent(*vessels_);
    for (const TreeLayout::Vessel& v : layout.vessels) {
        TreeVessels::Entry e;
        e.path.svg(v.d);
        e.wall = v.wall;
        e.lumen = v.lumen;
        e.flow = v.flow;
        e.node = v.node;
        vessels_->entries.push_back(std::move(e));
    }
    Icon& hub = stage.emplace<Icon>("hub", "tree_hub");
    const f32 hub_px = 228.0f * layout.hub_scale;
    fixed_size(hub, hub_px, hub_px);
    anchor(hub, Vec2{0, 0}, Vec2{0.5f, 0.5f}, layout.hub);

    for (const TreeLayout::Label& l : layout.labels) {
        const bool cap = l.style == "capstone";
        Panel& p = pill(stage, th, {}, cap ? "pill.capstone" : "level.pill", l.text,
                        cap ? "tree_capstone" : l.style == "ability" ? "tree_pill_small" : "level_pill");
        anchor(p, Vec2{0, 0}, Vec2{0.5f, 0.0f}, l.at);
        if (cap) {
            for (usize b = 0; b < kTreeBranchIds.size(); ++b) {
                if (l.branch == kTreeBranchIds[b]) {
                    capstone_labels_.push_back({static_cast<Label*>(p.children().front().get()), b});
                    capstone_names_.push_back(l.text);
                }
            }
        }
    }

    for (const auto& [key, n] : layout.nodes) {
        TreeNodeButton& b = stage.emplace<TreeNodeButton>(gui_, key, n.size, n.icon);
        anchor(b, Vec2{0, 0}, Vec2{0.5f, 0.5f}, n.at);
        const std::string k = key;
        b.on_click = [this, k] { select(k); };
        nodes_[key] = &b;
    }

    // ---- Top-left: back and title ----
    Button& back = root.emplace<Button>("back", th.shape("button.back"));
    fixed_size(back, 56, 56);
    anchor(back, Vec2{0, 0}, Vec2{0, 0}, Vec2{36, 32});
    back.layout.axis = Axis::Stack;
    back.layout.align = Align::Center;
    icon(back, "glyph", "glyph_back", 50.0f);
    back.tooltip = "Main menu (Esc)";
    back.on_click = [this] { emit_(MenuResult{MenuAction::Back}); };
    Label& title = text(root, "title", "Strengthen Immunity", th.text("screen_title"));
    anchor(title, Vec2{0, 0}, Vec2{0, 0}, Vec2{110, 30});

    // ---- Top bar: the selected node ----
    Panel& info = stage.emplace<Panel>("info", th.shape("panel.info"));
    anchor(info, Vec2{0, 0}, Vec2{0, 0}, Vec2{640, 14});
    fixed_size(info, 780, 96);
    info.layout.axis = Axis::Row;
    info.layout.align = Align::Center;
    info.layout.gap = 14.0f;
    info.layout.padding = Insets{18, 0, 22, 0};
    Panel& disc = info.emplace<Panel>("glyph", th.shape("tree.glyph"));
    fixed_size(disc, 64, 64);
    disc.layout.axis = Axis::Stack;
    disc.layout.align = Align::Center;
    disc.blocks_pointer = false;
    info_icon_ = &icon(disc, "icon", "stat_damage", 50.0f);
    Widget& words = column(info, 2.0f, Align::Start, "words");
    // Takes what the glyph, price and Grow leave (the description ellipsizes).
    words.layout.width = Size::fill();
    Widget& head = row(words, 8.0f, Align::End);
    info_name_ = &text(head, "name", "", th.text("tree_name"));
    info_level_ = &text(head, "level", "", th.text("tree_level"));
    info_desc_ = &text(words, "desc", "", th.text("tree_desc"));
    info_desc_->ellipsize = true;
    info_desc_->layout.width = Size::fill();
    info_req_ = &text(words, "req", "", th.text("tree_req"));
    info_mem_ = &row(info, 4.0f, Align::Center, "cost_mc");
    icon(*info_mem_, "icon", "memory_cell", 24.0f);
    info_mem_value_ = &text(*info_mem_, "value", "", th.text("tree_cost"));
    info_ab_ = &row(info, 4.0f, Align::Center, "cost_ab");
    icon(*info_ab_, "icon", "antibody", 24.0f);
    info_ab_value_ = &text(*info_ab_, "value", "", th.text("tree_cost"));
    grow_ = &info.emplace<Button>("grow", th.shape("button.grow"));
    grow_->has_disabled_shape = true;
    grow_->disabled_shape = th.shape("button.grow.off");
    grow_->layout.height = Size::px(54.0f);
    grow_->layout.min_size = Vec2{112.0f, 0.0f};
    grow_->layout.axis = Axis::Stack;
    grow_->layout.align = Align::Center;
    grow_->layout.padding = Insets{18, 0, 18, 0};
    grow_label_ = &text(*grow_, "label", "Grow", th.text("tree_grow"), TextAlign::Center);
    grow_->on_click = [this] {
        const TreeNodeView* n = model_.find(selected_);
        if (n != nullptr && n->state == TreeNodeState::Available) {
            MenuResult r{MenuAction::PurchaseNode};
            r.node = n->node;
            emit_(r);
        }
    };

    // ---- Top-right: wallet, respec, Play ----
    Widget& right = row(root, 14.0f, Align::Center);
    anchor(right, Vec2{1, 0}, Vec2{1, 0}, Vec2{-30, 18});
    Widget& wallet = column(right, 8.0f, Align::End, "wallet");
    Widget& purses = row(wallet, 10.0f, Align::Center);
    Panel& mc = purses.emplace<Panel>("memory", th.shape("pill.wallet"));
    mc.layout.axis = Axis::Row;
    mc.layout.align = Align::Center;
    mc.layout.gap = 6.0f;
    mc.layout.height = Size::px(52.0f);
    mc.layout.padding = Insets{8, 0, 16, 0};
    mc.tooltip = "Memory Cells: every run earns them; they buy levels";
    icon(mc, "icon", "memory_cell", 32.0f);
    memory_value_ = &text(mc, "value", "0", th.text("wallet"));
    Panel& ab = purses.emplace<Panel>("antibodies", th.shape("pill.wallet.ab"));
    ab.layout = mc.layout;
    ab.tooltip = "Antibodies: a level's first clear earns one; they buy towers, abilities and capstones";
    icon(ab, "icon", "antibody", 32.0f);
    antibody_value_ = &text(ab, "value", "0", th.text("wallet"));
    respec_ = &wallet.emplace<Button>("respec", th.shape("button.soft"));
    respec_->has_disabled_shape = true;
    respec_->disabled_shape = th.shape("button.grow.off");
    respec_->layout.axis = Axis::Stack;
    respec_->layout.align = Align::Center;
    respec_->layout.padding = Insets{16, 6, 16, 7};
    text(*respec_, "label", "Respec", th.text("tree_respec"), TextAlign::Center);
    respec_->on_click = [this] { emit_(MenuResult{MenuAction::Respec}); };

    Button& play = right.emplace<Button>("play", th.shape("button.cta"));
    fixed_size(play, 200, 80);
    play.layout.axis = Axis::Row;
    play.layout.align = Align::Center;
    play.layout.justify = Justify::Center;
    play.layout.gap = 12.0f;
    text(play, "label", "Play", th.text("tree_play"), TextAlign::Center);
    icon(play, "glyph", "glyph_play", 28.0f);
    play.layout.has_align_self = true;
    play.layout.align_self = Align::Start;
    play.tooltip = "Choose a level";
    play.on_click = [this] { emit_(MenuResult{MenuAction::OpenLevelSelect}); };
}

bool TreeScreen::select(const std::string& key) {
    if (nodes_.find(key) == nodes_.end()) return false;
    selected_ = key;
    for (auto& [k, b] : nodes_) b->selected = k == key;
    sync_info();
    return true;
}

void TreeScreen::sync(const TreeModel& m) {
    model_ = m;
    for (const TreeNodeView& n : m.nodes) {
        const auto it = nodes_.find(n.key);
        if (it == nodes_.end()) continue;
        it->second->view = n;
        it->second->tooltip = n.name;
    }
    for (TreeVessels::Entry& e : vessels_->entries) {
        const TreeNodeView* n = e.node.empty() ? nullptr : m.find(e.node);
        const bool lit = e.node.empty() || (n != nullptr && n->level > 0);
        if (lit != e.lit) vessels_->dirty = true;
        e.lit = lit;
    }
    for (usize i = 0; i < capstone_labels_.size(); ++i) {
        capstone_labels_[i].first->set_text(with_dot_points(capstone_names_[i],
                                                            m.branch_points[capstone_labels_[i].second],
                                                            m.capstone_threshold));
    }
    memory_value_->set_text(std::to_string(m.memory_cells));
    antibody_value_->set_text(std::to_string(m.antibodies));
    respec_->enabled = m.can_respec;
    respec_->tooltip = "Refund every purchase except the Neutrophil. Costs " + std::to_string(m.respec_cost) +
                       " Memory Cells.";

    // First visit: the first thing the player can buy, else the Neutrophil.
    if (selected_.empty() || m.find(selected_) == nullptr) {
        std::string pick = nodes_.count("neutrophil.unlock") != 0 ? "neutrophil.unlock" : std::string();
        for (const TreeNodeView& n : m.nodes) {
            if (n.state == TreeNodeState::Available && nodes_.count(n.key) != 0) {
                pick = n.key;
                break;
            }
        }
        if (pick.empty() && !nodes_.empty()) pick = nodes_.begin()->first;
        select(pick);
    } else {
        sync_info();
    }
}

void TreeScreen::sync_info() {
    const TreeNodeView* n = model_.find(selected_);
    const auto it = nodes_.find(selected_);
    if (n == nullptr || it == nodes_.end()) return;
    info_icon_->name = it->second->icon_name();
    info_name_->set_text(n->name);
    info_level_->set_text(n->max_level > 1 ? std::to_string(n->level) + "/" + std::to_string(n->max_level) : "");
    info_desc_->set_text(n->effect);
    info_req_->set_text(n->requirement);
    info_req_->visible = !n->requirement.empty();
    const bool maxed = n->state == TreeNodeState::Maxed;
    info_mem_->visible = !maxed && n->cost_memory > 0;
    info_mem_value_->set_text(std::to_string(n->cost_memory));
    info_ab_->visible = !maxed && n->cost_antibodies > 0;
    info_ab_value_->set_text(std::to_string(n->cost_antibodies));
    grow_->enabled = n->state == TreeNodeState::Available;
    grow_label_->set_text(maxed ? "Maxed" : n->state == TreeNodeState::Locked ? "Locked" : "Grow");
    grow_label_->style = gui_.theme().text(grow_->enabled ? "tree_grow" : "tree_grow_off");
}

} // namespace immune::ui
