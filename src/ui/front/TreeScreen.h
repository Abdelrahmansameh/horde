// ui/front/TreeScreen.h — the Strengthen Immunity tree, on the gui framework.
//
// A classic radial skill tree. tools/gen_tree_layout.py lays out every node
// of game/meta/ImmunityTree from its parent links: the Neutrophil in the
// middle, three core economy lines round it, and each subject (the attack
// towers, the abilities, the systemic lines and the control towers) growing
// outward in its own wedge; it writes that to assets/ui/tree_layout.json.
// The vessels between nodes follow the game's parent links
// (TreeNodeView::parent), so the drawing cannot disagree with the rules.
//
// The tree is uncovered as it grows (TreeModel::revealed): a node shows once
// its parent is owned, so a new campaign sees the Neutrophil and its three
// cores, zoomed in, and each purchase buds the next nodes into view.
//
// The view pans (drag anywhere), zooms about the pointer (wheel) and has
// zoom-in, zoom-out and recenter buttons; zooming out stops at the whole
// tree. Hovering a node opens its card beside it (name, level, effect, what
// it needs, its price); clicking a node buys its next level. Buying is app/'s
// job: a click reports MenuAction::PurchaseNode and app/ buys through
// MetaProgression, exactly as before.
#pragma once

#include "core/Types.h"
#include "ui/Menu.h"
#include "ui/front/TreeModel.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace immune::gui { class Gui; class Widget; class Label; class Icon; class Button; class Panel; }

namespace immune::ui {

/// assets/ui/tree_layout.json, in tree units: one logical pixel at zoom 1,
/// y down, the centre (the Neutrophil) at the origin.
struct TreeLayout {
    struct Node {
        Vec2 at;
        f32 size = 62.0f;  ///< Diameter.
        std::string icon;
    };
    std::map<std::string, Node> nodes;

    /// Every node, rims included.
    Rect bounds() const;
};

bool parse_tree_layout(const std::string& json, TreeLayout& out, std::string* error = nullptr);
bool load_tree_layout(const std::string& path, TreeLayout& out, std::string* error = nullptr);

class TreeNodeButton;
class TreeCanvas;

class TreeScreen {
public:
    /// Where the view looks: the tree point at the middle of the screen, and
    /// logical px per tree unit.
    struct View {
        Vec2 center{0.0f, 0.0f};
        f32 zoom = 1.0f;
    };
    /// Furthest the player can zoom in; zooming out stops at the whole tree.
    static constexpr f32 kMaxZoom = 1.8f;
    /// Opening (and recentering) frames the revealed nodes, never closer than this.
    static constexpr f32 kFrameZoom = 1.35f;

    /// Builds into `root` (the screen's container) from the layout and the
    /// first model; `emit` receives clicks. With `view`, opens where a
    /// previous visit left off instead of framing the revealed nodes.
    TreeScreen(gui::Gui& gui, gui::Widget& root, const TreeLayout& layout, const TreeModel& model,
               std::function<void(MenuResult)> emit, const View* view = nullptr);
    ~TreeScreen();
    TreeScreen(const TreeScreen&) = delete;
    TreeScreen& operator=(const TreeScreen&) = delete;

    /// Brings every node, vessel, label, the card and the wallet up to date.
    /// Nodes a purchase reveals bud into view.
    void sync(const TreeModel& m);

    /// The node whose card is open ("" when none).
    const std::string& hovered() const { return hovered_; }

    /// Where the view will settle (the target of any running animation).
    View view() const;
    /// Jumps the view (no animation), clamped to the tree.
    void set_view(View v);
    /// Zooms about the middle of the screen, animated.
    void zoom_by(f32 factor);
    /// Frames the revealed nodes, animated.
    void recenter();
    /// Ends any view animation at its target (tests, screenshots).
    void finish_animation();

private:
    void tick(f32 dt);
    void sync_card();
    void buy(const std::string& key);

    gui::Gui& gui_;
    std::function<void(MenuResult)> emit_;
    TreeModel model_;
    TreeCanvas* canvas_ = nullptr;
    std::map<std::string, TreeNodeButton*> nodes_;
    std::string hovered_;
    usize revealed_count_ = 0;

    // The card of the hovered node.
    gui::Panel* card_ = nullptr;
    gui::Icon* card_icon_ = nullptr;
    gui::Label* card_name_ = nullptr;
    gui::Label* card_level_ = nullptr;
    gui::Label* card_desc_ = nullptr;
    gui::Label* card_req_ = nullptr;
    gui::Widget* card_mem_ = nullptr;
    gui::Label* card_mem_value_ = nullptr;
    gui::Widget* card_ab_ = nullptr;
    gui::Label* card_ab_value_ = nullptr;
    gui::Label* card_hint_ = nullptr;

    gui::Label* memory_value_ = nullptr;
    gui::Label* antibody_value_ = nullptr;
    gui::Button* respec_ = nullptr;
};

} // namespace immune::ui
