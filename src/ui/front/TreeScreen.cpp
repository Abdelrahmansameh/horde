#include "ui/front/TreeScreen.h"

#include "gui/anim/Anim.h"
#include "gui/core/Gui.h"
#include "gui/draw/DrawList.h"
#include "gui/style/Theme.h"
#include "gui/widgets/Builders.h"
#include "gui/widgets/Widgets.h"
#include "ui/front/Backdrop.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

namespace immune::ui {

using namespace gui;

namespace {

/// Logical px the view keeps clear at the top of the screen (title, wallet,
/// Play) and round the other edges when it frames something.
constexpr f32 kTopInset = 120.0f;
constexpr f32 kEdgeInset = 28.0f;
/// Tree units of space kept round the revealed nodes when framing them.
constexpr f32 kFrameMargin = 70.0f;
/// One wheel notch, and one press of a zoom button.
constexpr f32 kWheelStep = 1.2f;
constexpr f32 kButtonStep = 1.45f;
/// How fast the view eases to its target (per second, exponential).
constexpr f32 kViewRate = 14.0f;
/// How long a newly revealed node takes to bud into view.
constexpr f32 kBudSeconds = 0.45f;
/// Root labels fade out as the view zooms out past these.
constexpr f32 kLabelFadeFrom = 0.62f;
constexpr f32 kLabelFadeTo = 0.5f;

// Tree units.
constexpr f32 kVesselWall = 14.0f;
constexpr f32 kVesselLumen = 7.0f;
/// Plasma flowing out along the lit vessels: dots this far apart, moving
/// this fast (the canvas's `flow` keyframe), shown only once big enough to see.
constexpr f32 kFlowDot = 1.9f;
constexpr f32 kFlowSpacing = 22.0f;
constexpr f32 kFlowSpeed = 44.0f / 1.8f;
constexpr f32 kFlowMinZoom = 0.45f;

void fill_parent(Widget& w) {
    anchor(w, Vec2{0, 0}, Vec2{0, 0});
    w.layout.width = Size::pct(1.0f);
    w.layout.height = Size::pct(1.0f);
}

std::string level_text(const TreeNodeView& n) {
    return n.max_level > 1 ? std::to_string(n.level) + "/" + std::to_string(n.max_level) : std::string();
}

} // namespace

// ---- Layout file ---------------------------------------------------------------------

Rect TreeLayout::bounds() const {
    Rect r{Vec2{1e9f, 1e9f}, Vec2{-1e9f, -1e9f}};
    auto grow = [&r](Vec2 p, f32 radius) {
        r.min.x = math::min(r.min.x, p.x - radius);
        r.min.y = math::min(r.min.y, p.y - radius);
        r.max.x = math::max(r.max.x, p.x + radius);
        r.max.y = math::max(r.max.y, p.y + radius);
    };
    for (const auto& [key, n] : nodes) grow(n.at, n.size * 0.5f);
    if (r.min.x > r.max.x) return Rect{Vec2{-1.0f, -1.0f}, Vec2{1.0f, 1.0f}};
    return r;
}

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

// ---- Nodes ---------------------------------------------------------------------------

/// One node: a wobbly cell (a spiky star for a capstone) whose fill and rim
/// say its state, its glyph, and level pips round its lower rim. Geometry in
/// the canvas's 100-unit node viewBox, so it scales with the view.
class TreeNodeButton : public Button {
public:
    TreeNodeButton(Gui& g, std::string id, std::string icon) : Button(std::move(id)), gui_(g), icon_(std::move(icon)) {
        shape.kind = ShapeKind::Ellipse;
        hover_scale = 1.08f;
        visible = false;
    }

    TreeNodeView view;
    /// 1 once on the map; eases up from 0 as a newly revealed node buds in.
    Tween bud{1.0f};
    const std::string& icon_name() const { return icon_; }

    void on_event(Event& e) override {
        // Every node hovers and springs; clicking one that cannot be bought
        // right now is the deny shake, not a purchase.
        if (e.type == EventType::Click && e.button == PointerButton::Left &&
            view.state != TreeNodeState::Available) {
            shake();
            gui_.play(UiSound::Deny);
            e.handled = true;
            return;
        }
        Button::on_event(e);
    }

    void update(f32 dt) override {
        Button::update(dt);
        bud.update(dt);
        const f32 b = bud.value();
        opacity = math::saturate(b * 1.6f);
        if (b < 1.0f || !bud.done()) anim_transform = anim_transform * Affine2::scale(math::max(b, 0.05f));
    }

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

        if (locked) dl.push_alpha(0.5f);
        // A ~40-unit cell with a gentle wobble; capstones are an 8-lobed
        // star between 34 and 44 units.
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
        gui_.icons().draw_zoomable(dl, icon_, Rect{c - gh, c + gh}, kWhite, gui_.scale());

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

// ---- The view ------------------------------------------------------------------------

/// The nodes and the vessels between them, seen through a camera the player
/// pans and zooms. Children (nodes and their name pills) are placed here
/// every layout from their tree positions; the vessels are drawn in
/// draw_self, tessellated once per zoom and replayed while panning.
/// Anonymous, so node paths stay "tree/<key>".
class TreeCanvas : public Widget {
public:
    using View = TreeScreen::View;

    explicit TreeCanvas(const TreeLayout& layout) : bounds_(layout.bounds()) {
        interactive = true;
    }

    /// Runs after this frame's layout and input (the card follows the nodes).
    std::function<void(f32)> on_tick;

    TreeNodeButton& add_node(Gui& g, const std::string& key, const TreeLayout::Node& at) {
        TreeNodeButton& b = emplace<TreeNodeButton>(g, key, at.icon);
        nodes_.push_back(Slot{&b, at.at, at.size});
        index_[key] = nodes_.size() - 1;
        return b;
    }

    /// A name pill under a node; it shows with the node.
    void add_label(Panel& pill, const std::string& key) {
        if (index_.count(key) != 0) pills_.push_back(Pill{&pill, index_[key]});
    }

    /// The vessel from `parent` to `child`; `grandparent` ("" at the root)
    /// is where the vessel feeding the parent comes from.
    void add_edge(const std::string& grandparent, const std::string& parent, const std::string& child) {
        const auto p = index_.find(parent), c = index_.find(child);
        if (p == index_.end() || c == index_.end()) return;
        Edge e;
        e.parent = p->second;
        e.child = c->second;
        // A cubic that leaves the parent partly along the vessel feeding it
        // and arrives at the child head-on, so branches fork the way vessels do.
        const Vec2 a = nodes_[e.parent].at, b = nodes_[e.child].at;
        const f32 len = math::length(b - a);
        const Vec2 dir = math::normalize_safe(b - a);
        const auto g = index_.find(grandparent);
        const Vec2 trunk = g == index_.end() ? dir : math::normalize_safe(a - nodes_[g->second].at);
        const Vec2 leave = math::normalize_safe(trunk * 0.45f + dir);
        e.path.move_to(a);
        e.path.cubic_to(a + leave * (len * 0.38f), b - dir * (len * 0.30f), b);
        for (const Path::Contour& contour : e.path.contours()) {
            for (const Vec2& q : contour.points) {
                e.along.push_back(e.line.empty() ? 0.0f : e.along.back() + math::length(q - e.line.back()));
                e.line.push_back(q);
            }
        }
        edges_.push_back(std::move(e));
        strokes_dirty_ = true;
    }

    // ---- The camera ----------------------------------------------------------------

    View target() const { return target_; }
    bool dragging() const { return dragging_; }

    void set_view(View v) {
        // Before the first layout there is no screen to clamp to yet; the
        // layout clamps it.
        cam_ = target_ = rect().size().x >= 1.0f ? clamp(v) : v;
        pin_ = false;
        frame_pending_ = false;
    }
    void finish_animation() {
        cam_ = target_;
        pin_ = false;
    }

    /// Zooms by `factor` keeping the tree point under `screen` where it is.
    void zoom_at(Vec2 screen, f32 factor) {
        const Vec2 tree = target_.center + (screen - anchor()) / target_.zoom;
        View next = target_;
        next.zoom = clamp_zoom(target_.zoom * factor);
        next.center = tree - (screen - anchor()) / next.zoom;
        const View clamped = clamp(next);
        target_ = clamped;
        // Hold that point still while the zoom eases, unless the edge of the
        // tree stopped the view (then it just eases to where it must be).
        pin_ = math::length_sq(clamped.center - next.center) < 1e-4f;
        pin_screen_ = screen;
        pin_tree_ = tree;
    }

    /// Frames every revealed node (or snaps there before the first layout).
    void frame_revealed(bool animate) {
        if (rect().size().x < 1.0f) {
            frame_pending_ = true;
            return;
        }
        Rect box{Vec2{1e9f, 1e9f}, Vec2{-1e9f, -1e9f}};
        for (const Slot& s : nodes_) {
            if (!s.button->visible) continue;
            const f32 r = s.size * 0.5f + kFrameMargin;
            box.min = Vec2{math::min(box.min.x, s.at.x - r), math::min(box.min.y, s.at.y - r)};
            box.max = Vec2{math::max(box.max.x, s.at.x + r), math::max(box.max.y, s.at.y + r)};
        }
        if (box.min.x > box.max.x) box = bounds_;
        const Rect safe = safe_area();
        View v;
        v.zoom = math::clamp(math::min(safe.size().x / box.size().x, safe.size().y / box.size().y), min_zoom(),
                             TreeScreen::kFrameZoom);
        v.center = box.center() - (safe.center() - anchor()) / v.zoom;
        target_ = clamp(v);
        pin_ = false;
        if (!animate) cam_ = target_;
    }

    // ---- Widget ----------------------------------------------------------------------

    void on_event(Event& e) override {
        switch (e.type) {
            case EventType::DragStart:
                dragging_ = true;
                pin_ = false;
                break;
            case EventType::Drag:
                cam_.center = cam_.center - e.delta / cam_.zoom;
                cam_ = clamp(cam_);
                target_.center = cam_.center;
                target_ = clamp(target_);
                e.handled = true;
                break;
            case EventType::DragEnd:
                dragging_ = false;
                break;
            case EventType::Wheel:
                zoom_at(e.pos, std::pow(kWheelStep, e.wheel));
                e.handled = true;
                break;
            default:
                break;
        }
    }

    void update(f32 dt) override {
        // Ease the view: zoom geometrically, the centre either pinned to the
        // point being zoomed about or straight to its target.
        const f32 a = 1.0f - std::exp(-kViewRate * dt);
        cam_.zoom = std::exp(math::lerp(std::log(cam_.zoom), std::log(target_.zoom), a));
        cam_.center = pin_ ? pin_tree_ - (pin_screen_ - anchor()) / cam_.zoom
                           : math::lerp(cam_.center, target_.center, a);
        if (std::fabs(cam_.zoom / target_.zoom - 1.0f) < 1e-3f &&
            math::length(cam_.center - target_.center) * cam_.zoom < 0.25f) {
            cam_ = target_;
            pin_ = false;
        }
        // A node that finished budding joins the cached vessels; one whose
        // owned state changed relights its vessel.
        for (Edge& e : edges_) {
            const TreeNodeButton& child = *nodes_[e.child].button;
            const bool settled = child.visible && child.bud.done();
            const bool lit = child.view.level > 0;
            if (settled != e.settled || lit != e.lit) strokes_dirty_ = true;
            e.settled = settled;
            e.lit = lit;
        }
        if (on_tick) on_tick(dt);
    }

    void draw_self(DrawList& dl) override {
        const f32 z = drawn_.zoom;
        const Vec2 origin = anchor() - drawn_.center * z;

        // The settled vessels, tessellated at the zoom they are seen at;
        // re-tessellated once the view settles at a new zoom.
        const f32 ratio = z / strokes_zoom_;
        const bool settled = cam_.zoom == target_.zoom;
        if (strokes_dirty_ || strokes_device_px_ != dl.device_px() || ratio > 1.3f || ratio < 1.0f / 1.3f ||
            (settled && std::fabs(ratio - 1.0f) > 0.002f)) {
            record_strokes(dl, z);
        }
        dl.push_transform(Affine2::translate(origin) * Affine2::scale(z / strokes_zoom_));
        dl.append_solid(strokes_.vertices, strokes_.indices);
        dl.pop_transform();

        // Vessels still budding in, and plasma flowing out along the lit ones.
        dl.push_transform(Affine2::translate(origin) * Affine2::scale(z));
        for (const Edge& e : edges_) {
            const TreeNodeButton& child = *nodes_[e.child].button;
            if (!child.visible || e.settled) continue;
            dl.push_alpha(math::saturate(child.bud.value()));
            stroke_vessel(dl, e, true);
            stroke_vessel(dl, e, false);
            dl.pop_alpha();
        }
        if (z >= kFlowMinZoom) {
            // One SDF dot per drop of plasma: a quad each, where a dashed
            // stroke would tessellate two round caps per dash every frame.
            ShapeDesc dot;
            dot.kind = ShapeKind::Ellipse;
            dot.half_size = Vec2{kFlowDot, kFlowDot};
            dot.fill = with_alpha(kWhite, 0.35f);
            const f32 phase = std::fmod(dl.time() * kFlowSpeed, kFlowSpacing);
            const Rect screen = rect();
            for (const Edge& e : edges_) {
                if (!e.settled || !e.lit || e.line.size() < 2) continue;
                usize seg = 0;
                for (f32 at = phase; at < e.along.back(); at += kFlowSpacing) {
                    while (seg + 2 < e.line.size() && e.along[seg + 1] < at) ++seg;
                    const f32 span = math::max(e.along[seg + 1] - e.along[seg], 1e-4f);
                    dot.center = math::lerp(e.line[seg], e.line[seg + 1], math::saturate((at - e.along[seg]) / span));
                    if (screen.contains(origin + dot.center * z)) dl.shape(dot);
                }
            }
        }
        dl.pop_transform();
    }

    /// Only what is on screen records anything: zoomed in, most of the tree
    /// is off it.
    void draw(DrawList& dl) override {
        if (!visible) return;
        draw_self(dl);
        const f32 margin = 40.0f;
        const Rect view{rect().min - Vec2{margin, margin}, rect().max + Vec2{margin, margin}};
        for (const auto& c : children()) {
            if (c->visible && c->rect().overlaps(view)) c->draw(dl);
        }
    }

protected:
    void arrange_children(Rect) override {
        if (frame_pending_ && rect().size().x >= 1.0f) {
            frame_pending_ = false;
            frame_revealed(false);
        }
        // The viewport may have changed under a settled view.
        cam_ = clamp(cam_);
        target_ = clamp(target_);
        drawn_ = cam_;
        const f32 z = drawn_.zoom;
        for (const Slot& s : nodes_) {
            if (!s.button->visible) continue;
            const Vec2 c = to_screen(s.at);
            const Vec2 h{s.size * z * 0.5f, s.size * z * 0.5f};
            s.button->arrange(Rect{c - h, c + h});
        }
        // Name pills hang under their node and shrink with the view, fading
        // out once they would be too small to read.
        const f32 fade = math::saturate((z - kLabelFadeTo) / (kLabelFadeFrom - kLabelFadeTo));
        for (const Pill& l : pills_) {
            const Slot& s = nodes_[l.node];
            l.pill->visible = s.button->visible && fade > 0.0f;
            if (!l.pill->visible) continue;
            l.pill->opacity = fade * s.button->opacity;
            const Vec2 size = l.pill->measure(Vec2{kUnbounded, kUnbounded});
            const Vec2 c = to_screen(s.at) + Vec2{0.0f, (s.size * 0.5f + 10.0f) * z + size.y * 0.5f * z};
            l.pill->arrange(Rect{c - size * 0.5f, c + size * 0.5f});
            l.pill->anim_transform = Affine2::scale(z);
        }
    }

private:
    struct Slot {
        TreeNodeButton* button;
        Vec2 at;
        f32 size;
    };
    struct Pill {
        Panel* pill;
        usize node;
    };
    struct Edge {
        usize parent = 0, child = 0;
        Path path;
        /// The path flattened, and the distance along it at each point.
        std::vector<Vec2> line;
        std::vector<f32> along;
        bool settled = false;  ///< In the cached strokes.
        bool lit = false;
    };

    Vec2 anchor() const { return rect().center(); }
    Vec2 to_screen(Vec2 tree) const { return anchor() + (tree - drawn_.center) * drawn_.zoom; }

    /// Where framed things go: the screen minus the top bar and a margin.
    Rect safe_area() const {
        const Rect r = rect();
        Rect s{Vec2{r.min.x + kEdgeInset, r.min.y + kTopInset}, Vec2{r.max.x - kEdgeInset, r.max.y - kEdgeInset}};
        if (s.max.x <= s.min.x || s.max.y <= s.min.y) s = r;
        return s;
    }
    /// Zoomed all the way out, the whole tree fills the safe area.
    f32 min_zoom() const {
        const Rect safe = safe_area();
        const Vec2 b = bounds_.size() + Vec2{kFrameMargin, kFrameMargin} * 2.0f;
        if (safe.size().x <= 0.0f || b.x <= 0.0f || b.y <= 0.0f) return TreeScreen::kMaxZoom;
        return math::min(math::min(safe.size().x / b.x, safe.size().y / b.y), TreeScreen::kMaxZoom);
    }
    f32 clamp_zoom(f32 z) const { return math::clamp(z, min_zoom(), TreeScreen::kMaxZoom); }
    /// Keeps the zoom in range and the middle of the screen over the tree.
    View clamp(View v) const {
        v.zoom = clamp_zoom(v.zoom);
        v.center.x = math::clamp(v.center.x, bounds_.min.x, bounds_.max.x);
        v.center.y = math::clamp(v.center.y, bounds_.min.y, bounds_.max.y);
        return v;
    }

    void stroke_vessel(DrawList& dl, const Edge& e, bool wall) const {
        const Theme& th = gui()->theme();
        StrokeStyle s;
        s.width = wall ? kVesselWall : kVesselLumen;
        s.color = wall ? th.color("plum") : th.color(nodes_[e.child].button->view.level > 0 ? "lavender" : "vein_off");
        e.path.stroke(dl, s);
    }

    void record_strokes(const DrawList& dl, f32 zoom) {
        DrawList scratch;
        scratch.reset(dl.viewport(), dl.device_px(), 0.0f);
        scratch.push_transform(Affine2::scale(zoom));
        // Every wall, then every lumen, so a fork's lumens join cleanly.
        for (const bool wall : {true, false}) {
            for (const Edge& e : edges_) {
                if (e.settled) stroke_vessel(scratch, e, wall);
            }
        }
        strokes_.capture(scratch);
        strokes_zoom_ = zoom;
        strokes_device_px_ = dl.device_px();
        strokes_dirty_ = false;
    }

    Rect bounds_;
    std::vector<Slot> nodes_;
    std::map<std::string, usize> index_;
    std::vector<Pill> pills_;
    std::vector<Edge> edges_;

    View cam_, target_, drawn_;
    bool frame_pending_ = true;
    bool dragging_ = false;
    bool pin_ = false;
    Vec2 pin_screen_{0.0f, 0.0f}, pin_tree_{0.0f, 0.0f};

    SolidMesh strokes_;
    f32 strokes_device_px_ = 0.0f;
    f32 strokes_zoom_ = 1.0f;
    bool strokes_dirty_ = true;
};

// ---- Screen ------------------------------------------------------------------------

TreeScreen::TreeScreen(Gui& gui, Widget& root, const TreeLayout& layout, const TreeModel& model,
                       std::function<void(MenuResult)> emit, const View* view)
    : gui_(gui), emit_(std::move(emit)) {
    Theme& th = gui_.theme();
    fill_parent(root.emplace<TissueBackdrop>("backdrop"));
    Panel& veil = root.emplace<Panel>("veil", th.shape("veil"));
    veil.shape.fill = with_alpha(th.color("dim"), 0.5f);
    fill_parent(veil);

    // ---- The tree ----
    canvas_ = &root.emplace<TreeCanvas>(layout);
    fill_parent(*canvas_);
    // Name pills first, so the nodes draw (and take the pointer) over them.
    std::vector<std::pair<Panel*, std::string>> pills;
    for (const TreeNodeView& n : model.nodes) {
        if (layout.nodes.count(n.key) == 0) continue;
        if (n.role != TreeNodeRole::TowerRoot && n.role != TreeNodeRole::AbilityRoot) continue;
        Panel& p = canvas_->emplace<Panel>(std::string(), th.shape("level.pill"));
        p.blocks_pointer = false;
        p.visible = false;
        p.layout.axis = Axis::Stack;
        p.layout.align = Align::Center;
        p.layout.padding = Insets{12, 5, 12, 5};
        text(p, "text", n.name, th.text(n.role == TreeNodeRole::TowerRoot ? "level_pill" : "tree_pill_small"),
             TextAlign::Center);
        pills.emplace_back(&p, n.key);
    }
    for (const TreeNodeView& n : model.nodes) {
        const auto at = layout.nodes.find(n.key);
        if (at == layout.nodes.end()) continue;
        TreeNodeButton& b = canvas_->add_node(gui_, n.key, at->second);
        const std::string key = n.key;
        b.on_click = [this, key] { buy(key); };
        nodes_[key] = &b;
    }
    for (const auto& [pill, key] : pills) canvas_->add_label(*pill, key);
    for (const TreeNodeView& n : model.nodes) {
        if (n.parent.empty()) continue;
        const TreeNodeView* parent = model.find(n.parent);
        canvas_->add_edge(parent != nullptr ? parent->parent : std::string(), n.parent, n.key);
    }
    canvas_->on_tick = [this](f32 dt) { tick(dt); };

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

    // ---- Bottom-right: the view ----
    Widget& controls = column(root, 12.0f, Align::Center, "view");
    anchor(controls, Vec2{1, 1}, Vec2{1, 1}, Vec2{-34, -34});
    auto view_button = [&](const char* id, const char* glyph, const char* tip, std::function<void()> fn) {
        Button& b = controls.emplace<Button>(id, th.shape("button.back"));
        fixed_size(b, 56, 56);
        b.layout.axis = Axis::Stack;
        b.layout.align = Align::Center;
        icon(b, "glyph", glyph, 46.0f);
        b.tooltip = tip;
        b.on_click = std::move(fn);
    };
    view_button("zoom_in", "glyph_plus", "Zoom in (or scroll)", [this] { zoom_by(kButtonStep); });
    view_button("zoom_out", "glyph_minus", "Zoom out (or scroll); drag to look around",
                [this] { zoom_by(1.0f / kButtonStep); });
    view_button("recenter", "glyph_recenter", "Show everything grown so far", [this] { recenter(); });

    // ---- The card of the hovered node, over everything ----
    card_ = &root.emplace<Panel>("card", th.shape("tree.card"));
    card_->blocks_pointer = false;
    card_->accepts_pointer = false;
    card_->visible = false;
    anchor(*card_, Vec2{0, 0}, Vec2{0, 0});
    card_->layout.axis = Axis::Column;
    card_->layout.gap = 6.0f;
    card_->layout.padding = Insets{20, 16, 22, 18};
    card_->layout.max_size = Vec2{420.0f, kUnbounded};
    Widget& head = row(*card_, 12.0f, Align::Center, "head");
    Panel& disc = head.emplace<Panel>("glyph", th.shape("tree.glyph"));
    fixed_size(disc, 56, 56);
    disc.layout.axis = Axis::Stack;
    disc.layout.align = Align::Center;
    disc.blocks_pointer = false;
    card_icon_ = &icon(disc, "icon", "stat_damage", 44.0f);
    Widget& title_row = row(head, 8.0f, Align::End, "title");
    card_name_ = &text(title_row, "name", "", th.text("tree_name"));
    card_level_ = &text(title_row, "level", "", th.text("tree_level"));
    card_desc_ = &text(*card_, "desc", "", th.text("tree_desc"));
    card_desc_->wrap = true;
    card_req_ = &text(*card_, "req", "", th.text("tree_req"));
    card_req_->wrap = true;
    Widget& foot = row(*card_, 14.0f, Align::Center, "foot");
    card_mem_ = &row(foot, 4.0f, Align::Center, "cost_mc");
    icon(*card_mem_, "icon", "memory_cell", 24.0f);
    card_mem_value_ = &text(*card_mem_, "value", "", th.text("tree_cost"));
    card_ab_ = &row(foot, 4.0f, Align::Center, "cost_ab");
    icon(*card_ab_, "icon", "antibody", 24.0f);
    card_ab_value_ = &text(*card_ab_, "value", "", th.text("tree_cost"));
    card_hint_ = &text(foot, "hint", "", th.text("tree_hint"));

    sync(model);
    if (view != nullptr) canvas_->set_view(*view);
}

TreeScreen::~TreeScreen() {
    // The widgets outlive this object while the screen fades out; they must
    // not call back into it.
    canvas_->on_tick = nullptr;
}

TreeScreen::View TreeScreen::view() const { return canvas_->target(); }
void TreeScreen::set_view(View v) { canvas_->set_view(v); }
void TreeScreen::zoom_by(f32 factor) { canvas_->zoom_at(canvas_->rect().center(), factor); }
void TreeScreen::recenter() { canvas_->frame_revealed(true); }
void TreeScreen::finish_animation() { canvas_->finish_animation(); }

void TreeScreen::buy(const std::string& key) {
    const TreeNodeView* n = model_.find(key);
    if (n == nullptr || n->state != TreeNodeState::Available) return;
    MenuResult r{MenuAction::PurchaseNode};
    r.node = n->node;
    emit_(r);
}

void TreeScreen::sync(const TreeModel& m) {
    const bool first = model_.nodes.empty();
    model_ = m;
    const std::vector<bool> shown = m.revealed();
    usize revealed = 0;
    for (usize i = 0; i < m.nodes.size(); ++i) {
        const auto it = nodes_.find(m.nodes[i].key);
        if (it == nodes_.end()) continue;
        TreeNodeButton& b = *it->second;
        if (shown[i] && !b.visible && !first) {
            // Revealed by a purchase: bud into view.
            b.bud.snap(0.0f);
            b.bud.start(1.0f, kBudSeconds, Ease::OutBack);
        }
        b.visible = shown[i];
        b.view = m.nodes[i];
        revealed += shown[i] ? 1u : 0u;
    }
    // A respec took nodes off the map: look at what is left.
    if (!first && revealed < revealed_count_) canvas_->frame_revealed(true);
    revealed_count_ = revealed;

    memory_value_->set_text(std::to_string(m.memory_cells));
    antibody_value_->set_text(std::to_string(m.antibodies));
    respec_->enabled = m.can_respec;
    respec_->tooltip = "Refund every purchase except the Neutrophil. Costs " + std::to_string(m.respec_cost) +
                       " Memory Cells.";
    sync_card();
}

void TreeScreen::tick(f32) {
    std::string key;
    if (!canvas_->dragging()) {
        for (const auto& [k, b] : nodes_) {
            if (b->visible && b->hovered()) {
                key = k;
                break;
            }
        }
    }
    if (key != hovered_) {
        hovered_ = key;
        sync_card();
    }
    if (hovered_.empty()) return;

    // Beside the node, on whichever side has room, kept on screen.
    const Rect node = nodes_[hovered_]->rect();
    const Vec2 vp = gui_.viewport();
    const Vec2 size = card_->measure(vp);
    Vec2 pos{node.max.x + 18.0f, node.center().y - size.y * 0.5f};
    if (pos.x + size.x > vp.x - 12.0f) pos.x = node.min.x - 18.0f - size.x;
    pos.x = math::clamp(pos.x, 12.0f, math::max(12.0f, vp.x - size.x - 12.0f));
    pos.y = math::clamp(pos.y, 12.0f, math::max(12.0f, vp.y - size.y - 12.0f));
    card_->layout.offset = pos;
    card_->arrange(Rect{pos, pos + size});
}

void TreeScreen::sync_card() {
    const TreeNodeView* n = hovered_.empty() ? nullptr : model_.find(hovered_);
    card_->visible = n != nullptr;
    if (n == nullptr) return;
    const Theme& th = gui_.theme();
    card_icon_->name = nodes_[hovered_]->icon_name();
    card_name_->set_text(n->name);
    card_level_->set_text(level_text(*n));
    card_desc_->set_text(n->effect);
    card_req_->set_text(n->requirement);
    card_req_->visible = !n->requirement.empty();

    const bool maxed = n->state == TreeNodeState::Maxed;
    const bool short_mc = n->cost_memory > model_.memory_cells;
    const bool short_ab = n->cost_antibodies > model_.antibodies;
    card_mem_->visible = !maxed && n->cost_memory > 0;
    card_mem_value_->set_text(std::to_string(n->cost_memory));
    card_mem_value_->style = th.text(short_mc ? "tree_cost_short" : "tree_cost");
    card_ab_->visible = !maxed && n->cost_antibodies > 0;
    card_ab_value_->set_text(std::to_string(n->cost_antibodies));
    card_ab_value_->style = th.text(short_ab ? "tree_cost_short" : "tree_cost");

    std::string hint;
    switch (n->state) {
        case TreeNodeState::Available: hint = "Click to grow"; break;
        case TreeNodeState::Short: hint = short_ab ? "Not enough Antibodies" : "Not enough Memory Cells"; break;
        case TreeNodeState::Maxed: hint = n->max_level > 1 ? "Fully grown" : "Owned"; break;
        case TreeNodeState::Locked: break;
    }
    card_hint_->set_text(hint);
    card_hint_->visible = !hint.empty();
    card_hint_->style = th.text(n->state == TreeNodeState::Short ? "tree_req" : "tree_hint");
}

} // namespace immune::ui
