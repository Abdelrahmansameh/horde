// ui/front/TreeScreen.h — the Strengthen Immunity tree, on the gui framework.
//
// The canvas's Tree artboard lays the 75 nodes out as a lymphatic system:
// five tower columns grow up from a lymph-node hub (root at the bottom, stat
// pairs up the trunk, the capstone crowning it), the core upgrades fan out
// around the hub, and the four abilities sit in the corners on vessels that
// loop round from it. tools/extract_tree_layout.py writes that placement to
// assets/ui/tree_layout.json; this screen draws from it, so a node's place,
// its vessel and its label are exactly as designed.
//
// A node shows its state the way the canvas does: dimmed when locked, a white
// halo when it can be bought now, a lavender body once owned (deeper when
// maxed), level pips round its lower rim, a gold star ring on capstones. A
// vessel lights up when the node it feeds is owned. Clicking a node selects
// it into the top bar (name, level, effect, what it needs, its price, Grow).
//
// Buying is app/'s job: Grow reports MenuAction::PurchaseNode, exactly like
// the ImGui screen this replaces, so the screen can never disagree with the
// rules it displays.
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

/// assets/ui/tree_layout.json, in canvas pixels of a 1920x1080 frame.
struct TreeLayout {
    struct Node {
        Vec2 at;
        f32 size = 48.0f;
        std::string icon;
    };
    struct Vessel {
        std::string d;         ///< SVG path data.
        f32 wall = 10.0f;      ///< Plum outer stroke width.
        f32 lumen = 5.0f;      ///< Inner stroke width.
        std::string node;      ///< Lit when this node is owned; empty = always lit.
        f32 flow = 0.0f;       ///< Width of the flowing dash; 0 = none.
    };
    struct Label {
        Vec2 at;               ///< Top centre.
        std::string text;
        std::string style;     ///< "root", "ability" or "capstone".
        std::string branch;    ///< Capstone: whose points it shows.
    };
    std::map<std::string, Node> nodes;
    std::vector<Vessel> vessels;
    std::vector<Label> labels;
    Vec2 hub{960.0f, 945.0f};
    f32 hub_scale = 0.72f;
};

bool parse_tree_layout(const std::string& json, TreeLayout& out, std::string* error = nullptr);
bool load_tree_layout(const std::string& path, TreeLayout& out, std::string* error = nullptr);

class TreeNodeButton;
class TreeVessels;

class TreeScreen {
public:
    /// Builds into `root` (the screen's container). `emit` receives clicks.
    TreeScreen(gui::Gui& gui, gui::Widget& root, const TreeLayout& layout, std::function<void(MenuResult)> emit);

    /// Brings every node, vessel, label and the top bar up to date.
    void sync(const TreeModel& m);

    /// The node shown in the top bar ("" before the first sync).
    const std::string& selected() const { return selected_; }
    bool select(const std::string& key);

private:
    void sync_info();

    gui::Gui& gui_;
    std::function<void(MenuResult)> emit_;
    TreeModel model_;
    std::string selected_;
    std::map<std::string, TreeNodeButton*> nodes_;
    TreeVessels* vessels_ = nullptr;
    std::vector<std::pair<gui::Label*, usize>> capstone_labels_;  ///< Label, branch index.
    std::vector<std::string> capstone_names_;

    // Top bar.
    gui::Icon* info_icon_ = nullptr;
    gui::Label* info_name_ = nullptr;
    gui::Label* info_level_ = nullptr;
    gui::Label* info_desc_ = nullptr;
    gui::Label* info_req_ = nullptr;
    gui::Widget* info_mem_ = nullptr;
    gui::Label* info_mem_value_ = nullptr;
    gui::Widget* info_ab_ = nullptr;
    gui::Label* info_ab_value_ = nullptr;
    gui::Button* grow_ = nullptr;
    gui::Label* grow_label_ = nullptr;
    gui::Label* memory_value_ = nullptr;
    gui::Label* antibody_value_ = nullptr;
    gui::Button* respec_ = nullptr;
};

} // namespace immune::ui
