// ui/Menu.cpp — main menu and level select. See Menu.h for why these screens
// are separate from Hud and why they report clicks rather than driving the
// state machine themselves.
#include "ui/Menu.h"

#include "game/config/GameConfig.h"
#include "game/meta/ImmunityTree.h"
#include "game/meta/MetaProgression.h"
#include "ui/Fonts.h"

#include <imgui.h>

#include <cstdio>
#include <vector>

namespace immune::ui {
namespace {

// Both screens are centred panels over the live background, so they share the
// same window flags: no chrome the player could drag, resize, or collapse into
// an unrecoverable state.
constexpr ImGuiWindowFlags kPanelFlags =
    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;

void center_next_window(i32 w, i32 h, f32 panel_w, f32 panel_h) {
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
}

/// Horizontally centres the next item of width `item_w` in the current window.
void center_next_item(f32 item_w) {
    const f32 avail = ImGui::GetContentRegionAvail().x;
    if (avail > item_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - item_w) * 0.5f);
}

/// A title drawn larger than body text. If a system font was found
/// (ui/Fonts.h), use the real larger-size instance baked for headings --
/// crisp at its native size, unlike a bitmap font stretched via Scale.
/// Otherwise fall back to scaling the stock atlas font, which keeps the menu
/// working even with zero fonts on disk (this project ships zero binary
/// assets, so a bundled .ttf is not an option).
void draw_title(const char* text, f32 scale) {
    ImFont* big = title_font();
    if (big) {
        ImGui::PushFont(big);
        center_next_item(ImGui::CalcTextSize(text).x);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        return;
    }
    const f32 old = ImGui::GetFont()->Scale;
    ImGui::GetFont()->Scale = scale;
    ImGui::PushFont(ImGui::GetFont());
    center_next_item(ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::GetFont()->Scale = old;
}

const ImVec4 kAccent(0.30f, 0.90f, 0.80f, 1.0f);
const ImVec4 kGold(1.0f, 0.82f, 0.35f, 1.0f);

/// A centred line of body text in `color`.
void centered_text(const char* text, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    center_next_item(ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

/// What the run paid (PROGRESSION.md §3): Memory Cells every time, the
/// Antibody only on a first clear -- said out loud either way, so a replay
/// that earned no Antibody reads as the rule rather than as a bug.
void draw_run_summary(const RunSummary& s, bool won) {
    if (!s.valid) return;
    char line[128];
    if (s.sandbox) {
        centered_text("Sandbox run: no Memory Cells or Antibodies.",
                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        return;
    }
    std::snprintf(line, sizeof(line), "+%u Memory Cells", s.memory_cells);
    centered_text(line, kAccent);
    if (s.antibodies > 0) {
        std::snprintf(line, sizeof(line), "+%u Antibod%s  (first clear)", s.antibodies,
                      s.antibodies == 1 ? "y" : "ies");
        centered_text(line, kGold);
    } else if (won) {
        centered_text("No Antibody: already cleared before.",
                      ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
}

/// The campaign's way out of a finished run: again, stronger, or elsewhere.
void draw_campaign_exits(MenuResult& result, const char* again_label) {
    const ImVec2 button{240.0f, 38.0f};
    center_next_item(button.x);
    if (ImGui::Button(again_label, button)) result.action = MenuAction::RestartLevel;
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    center_next_item(button.x);
    if (ImGui::Button("Strengthen Immunity", button)) result.action = MenuAction::OpenImmunityTree;
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    center_next_item(button.x);
    if (ImGui::Button("Level Select", button)) result.action = MenuAction::OpenLevelSelect;
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    center_next_item(button.x);
    if (ImGui::Button("Main Menu", button)) result.action = MenuAction::Back;
}

} // namespace

MenuResult Menu::build_main_menu(i32 screen_width, i32 screen_height) {
    MenuResult result;
    center_next_window(screen_width, screen_height, 560.0f, 380.0f);
    if (ImGui::Begin("##main_menu", nullptr, kPanelFlags)) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        draw_title("IMMUNE", 2.4f);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        // Wrapped rather than centred-on-measured-width. CalcTextSize outside
        // an open window under-reports against what actually gets rasterised
        // (font scaling is not applied the same way), which silently clipped
        // this line mid-word. Wrapping cannot clip at any font scale.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Hold the line inside the body.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, 28.0f));
        const ImVec2 button{240.0f, 42.0f};

        center_next_item(button.x);
        if (ImGui::Button("Play", button)) result.action = MenuAction::OpenLevelSelect;

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        center_next_item(button.x);
        if (ImGui::Button("Strengthen Immunity", button)) result.action = MenuAction::OpenImmunityTree;

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        center_next_item(button.x);
        if (ImGui::Button("Level Editor", button)) result.action = MenuAction::OpenEditor;

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        center_next_item(button.x);
        if (ImGui::Button("Quit", button)) result.action = MenuAction::Quit;
    }
    ImGui::End();
    return result;
}

MenuResult Menu::build_level_select(const std::vector<LevelEntry>& levels,
                                    i32 screen_width, i32 screen_height) {
    MenuResult result;
    center_next_window(screen_width, screen_height, 620.0f, 480.0f);
    if (ImGui::Begin("##level_select", nullptr, kPanelFlags)) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        draw_title("Select a Level", 1.6f);
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        ImGui::Separator();

        if (levels.empty()) {
            // An empty list almost always means the asset root did not resolve,
            // not that the campaign is genuinely empty -- say so, rather than
            // showing a blank panel that looks like a broken build.
            ImGui::Dummy(ImVec2(0.0f, 24.0f));
            ImGui::TextWrapped(
                "No levels found. Expected .json level files under the 'assets/levels' "
                "directory of the asset root. Set IMMUNE_ASSET_ROOT if the game is "
                "running from outside the repository.");
        } else {
            if (selected_ >= static_cast<int>(levels.size())) selected_ = 0;

            ImGui::BeginChild("##level_list", ImVec2(0.0f, 320.0f), true);
            for (int i = 0; i < static_cast<int>(levels.size()); ++i) {
                const LevelEntry& e = levels[static_cast<usize>(i)];
                ImGui::PushID(i);
                if (ImGui::Selectable(e.display_name.c_str(), selected_ == i,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected_ = i;
                    // Double-click is the conventional "open it now" gesture;
                    // single click only highlights, so the subtitle below can
                    // be read before committing.
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        result.action = MenuAction::StartLevel;
                        result.level_index = static_cast<usize>(i);
                    }
                }
                ImGui::SameLine();
                if (e.lane_count > 0) {
                    ImGui::TextDisabled("  %s - %u lane%s", e.region.c_str(), e.lane_count,
                                        e.lane_count == 1 ? "" : "s");
                } else {
                    ImGui::TextDisabled("  %s", e.region.c_str());
                }
                if (e.difficulty > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("- difficulty %d/10", e.difficulty);
                }
                // The first clear is what pays an Antibody (PROGRESSION.md
                // §3.2), so which levels still have one is worth a glance.
                ImGui::SameLine();
                if (e.cleared) {
                    ImGui::TextColored(kAccent, "  [cleared]");
                } else {
                    ImGui::TextColored(kGold, "  [+1 Antibody]");
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            const ImVec2 button{200.0f, 38.0f};
            if (ImGui::Button("Start", button)) {
                result.action = MenuAction::StartLevel;
                result.level_index = static_cast<usize>(selected_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Back", button)) result.action = MenuAction::Back;
        }
    }
    ImGui::End();
    return result;
}

MenuResult Menu::build_level_failed_screen(i32 screen_width, i32 screen_height, bool playtest,
                                           const RunSummary& summary) {
    MenuResult result;
    center_next_window(screen_width, screen_height, 480.0f, playtest ? 280.0f : 400.0f);
    if (ImGui::Begin("##level_failed", nullptr, kPanelFlags)) {
        ImGui::Dummy(ImVec2(0.0f, 20.0f));
        draw_title("Level Failed", 1.8f);
        ImGui::Dummy(ImVec2(0.0f, 16.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        center_next_item(ImGui::CalcTextSize("Objective destroyed.").x);
        ImGui::TextUnformatted("Objective destroyed.");
        ImGui::PopStyleColor();

        if (!playtest) {
            // Losing still pays (PROGRESSION.md §3.1): the point of this
            // screen in the campaign is "spend it and go again".
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            draw_run_summary(summary, false);
            ImGui::Dummy(ImVec2(0.0f, 16.0f));
            draw_campaign_exits(result, "Try Again");
            ImGui::End();
            return result;
        }

        ImGui::Dummy(ImVec2(0.0f, 24.0f));
        const ImVec2 button{180.0f, 42.0f};

        center_next_item(button.x);
        if (ImGui::Button("Restart", button)) result.action = MenuAction::RestartLevel;

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        center_next_item(button.x);
        if (playtest) {
            if (ImGui::Button("Back to Editor", button)) result.action = MenuAction::BackToEditor;
        } else if (ImGui::Button("Menu", button)) {
            result.action = MenuAction::Back;
        }
    }
    ImGui::End();
    return result;
}

MenuResult Menu::build_level_complete_screen(i32 screen_width, i32 screen_height, bool playtest,
                                             const RunSummary& summary) {
    MenuResult result;
    center_next_window(screen_width, screen_height, 480.0f, playtest ? 280.0f : 420.0f);
    if (ImGui::Begin("##level_complete", nullptr, kPanelFlags)) {
        ImGui::Dummy(ImVec2(0.0f, 20.0f));
        draw_title("Level Complete", 1.8f);
        ImGui::Dummy(ImVec2(0.0f, 16.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        center_next_item(ImGui::CalcTextSize("All waves cleared!").x);
        ImGui::TextUnformatted("All waves cleared!");
        ImGui::PopStyleColor();

        if (!playtest) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            draw_run_summary(summary, true);
            ImGui::Dummy(ImVec2(0.0f, 16.0f));
            // DESIGN.md §7.4: replaying for more Memory Cells is as legitimate
            // a next step as moving on, so it is offered first.
            draw_campaign_exits(result, "Replay");
            ImGui::End();
            return result;
        }

        ImGui::Dummy(ImVec2(0.0f, 24.0f));
        const ImVec2 button{180.0f, 42.0f};

        if (playtest) {
            center_next_item(button.x);
            if (ImGui::Button("Restart", button)) result.action = MenuAction::RestartLevel;
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
        }

        center_next_item(button.x);
        if (playtest) {
            if (ImGui::Button("Back to Editor", button)) result.action = MenuAction::BackToEditor;
        } else if (ImGui::Button("Menu", button)) {
            result.action = MenuAction::Back;
        }
    }
    ImGui::End();
    return result;
}

MenuResult Menu::build_pause_menu(i32 screen_width, i32 screen_height) {
    MenuResult result;
    center_next_window(screen_width, screen_height, 420.0f, 320.0f);
    if (ImGui::Begin("##pause_menu", nullptr, kPanelFlags)) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        draw_title("Paused", 1.8f);
        ImGui::Dummy(ImVec2(0.0f, 20.0f));
        const ImVec2 button{220.0f, 42.0f};

        center_next_item(button.x);
        if (ImGui::Button("Resume", button)) result.action = MenuAction::Resume;

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        center_next_item(button.x);
        if (ImGui::Button("Restart", button)) result.action = MenuAction::RestartLevel;

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        center_next_item(button.x);
        if (ImGui::Button("Main Menu", button)) result.action = MenuAction::Back;
    }
    ImGui::End();
    return result;
}

// ---------------------------------------------------------------------------
// Strengthen Immunity
// ---------------------------------------------------------------------------

namespace {

/// One node's card: name, level, and price, coloured by whether it can be
/// bought right now. Left-aligned multi-line button; a click only counts when
/// the purchase would succeed, and the tooltip says why when it would not.
/// Returns the card's rect for the vessel drawn down the column.
struct Card {
    ImVec2 min, max;
    bool owned = false;
};

Card node_card(const game::MetaProgression& meta, const game::MetaConfig& cfg, game::TreeNode n,
               MenuResult& result) {
    using game::MetaProgression;
    const game::TreeNodeDef& d = game::tree_node(n);
    const u8 lv = meta.level(n);
    const bool maxed = lv >= d.max_level;
    const MetaProgression::PurchaseResult check = meta.check_purchase(n, cfg);
    const bool buyable = check == MetaProgression::PurchaseResult::Ok;
    const bool locked = check == MetaProgression::PurchaseResult::Locked;
    const game::TreeCost cost = meta.next_cost(n, cfg);

    char status[96];
    if (maxed) {
        std::snprintf(status, sizeof(status), d.max_level == 1 ? "Owned" : "Lv %u/%u  MAX", static_cast<unsigned>(lv),
                      static_cast<unsigned>(d.max_level));
    } else {
        char price[48];
        if (cost.antibodies > 0 && cost.memory_cells > 0) {
            std::snprintf(price, sizeof(price), "%u AB + %u MC", cost.antibodies, cost.memory_cells);
        } else if (cost.antibodies > 0) {
            std::snprintf(price, sizeof(price), "%u AB", cost.antibodies);
        } else {
            std::snprintf(price, sizeof(price), "%u MC", cost.memory_cells);
        }
        if (d.max_level == 1) {
            std::snprintf(status, sizeof(status), "%s", price);
        } else {
            std::snprintf(status, sizeof(status), "Lv %u/%u   %s", static_cast<unsigned>(lv),
                          static_cast<unsigned>(d.max_level), price);
        }
    }
    char label[192];
    std::snprintf(label, sizeof(label), "%s\n%s", d.name, status);

    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImVec4 fg = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (maxed) {
        bg = ImVec4(0.12f, 0.38f, 0.34f, 0.85f);
    } else if (buyable) {
        bg = ImVec4(0.16f, 0.62f, 0.55f, 0.95f);
    } else if (locked) {
        bg = ImVec4(0.10f, 0.12f, 0.14f, 0.85f);
        fg = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    } else {
        bg = ImVec4(0.10f, 0.20f, 0.22f, 0.85f);
        fg = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    }
    if (d.kind == game::TreeNodeKind::Capstone || d.kind == game::TreeNodeKind::TowerRoot ||
        d.kind == game::TreeNodeKind::AbilityRoot) {
        // Milestones are Antibody purchases; a gold edge sets them apart.
        ImGui::PushStyleColor(ImGuiCol_Border, kGold);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Border));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, buyable ? kAccent : bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, buyable ? kAccent : bg);
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushID(static_cast<int>(n));
    const f32 h = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f + 4.0f;
    const bool clicked = ImGui::Button(label, ImVec2(-1.0f, h));
    ImGui::PopID();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);

    Card card{ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), lv > 0};
    if (clicked && buyable) {
        result.action = MenuAction::PurchaseNode;
        result.node = static_cast<u32>(n);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
        ImGui::TextUnformatted(d.name);
        ImGui::TextColored(kAccent, "%s%s", d.effect, d.max_level > 1 ? "  (per level)" : "");
        if (d.max_level > 1) ImGui::Text("Bought: %u of %u", static_cast<unsigned>(lv), static_cast<unsigned>(d.max_level));
        if (!maxed && !buyable) {
            if (check == MetaProgression::PurchaseResult::BelowThreshold) {
                ImGui::TextColored(kGold, "Needs %u points in this branch (has %u).",
                                   cfg.capstone_threshold, meta.branch_points(d.branch));
            } else {
                ImGui::TextColored(kGold, "Can't buy: %s.", game::purchase_result_text(check));
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return card;
}

/// The vessel a column's nodes hang off: one trunk down the left, a branch
/// out to every card, lit where the player has bought in.
void draw_vessel(const std::vector<Card>& cards) {
    if (cards.size() < 2) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 dim = ImGui::GetColorU32(ImVec4(0.20f, 0.45f, 0.42f, 0.55f));
    const ImU32 lit = ImGui::GetColorU32(ImVec4(0.30f, 0.90f, 0.80f, 0.95f));
    const f32 x = cards.front().min.x - 8.0f;
    for (usize i = 0; i < cards.size(); ++i) {
        const f32 y = (cards[i].min.y + cards[i].max.y) * 0.5f;
        dl->AddLine(ImVec2(x, y), ImVec2(cards[i].min.x, y), cards[i].owned ? lit : dim, 2.0f);
        if (i + 1 < cards.size()) {
            const f32 y2 = (cards[i + 1].min.y + cards[i + 1].max.y) * 0.5f;
            dl->AddLine(ImVec2(x, y), ImVec2(x, y2), cards[i + 1].owned ? lit : dim, 3.0f);
        }
    }
}

/// Every node in `branch` (and, for the hub, belonging to `ability`), in
/// catalog order, as one vessel.
void draw_node_group(const game::MetaProgression& meta, const game::MetaConfig& cfg,
                     game::TreeBranch branch, game::AbilityId ability, MenuResult& result) {
    std::vector<Card> cards;
    ImGui::Indent(14.0f);
    for (u32 i = 0; i < game::kTreeNodeCount; ++i) {
        const auto n = static_cast<game::TreeNode>(i);
        const game::TreeNodeDef& d = game::tree_node(n);
        if (d.branch != branch || d.ability != ability) continue;
        cards.push_back(node_card(meta, cfg, n, result));
    }
    ImGui::Unindent(14.0f);
    draw_vessel(cards);
}

} // namespace

MenuResult Menu::build_immunity_tree(const game::MetaProgression& meta, const game::MetaConfig& cfg,
                                     i32 screen_width, i32 screen_height) {
    MenuResult result;
    const f32 panel_w = static_cast<f32>(screen_width) - 40.0f;
    const f32 panel_h = static_cast<f32>(screen_height) - 40.0f;
    center_next_window(screen_width, screen_height, panel_w, panel_h);
    if (ImGui::Begin("##immunity_tree", nullptr, kPanelFlags)) {
        draw_title("Strengthen Immunity", 1.8f);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        // Wallet, and the two buttons that are not purchases.
        ImGui::TextColored(kAccent, "Memory Cells: %llu",
                           static_cast<unsigned long long>(meta.memory_cells()));
        ImGui::SameLine(0.0f, 28.0f);
        ImGui::TextColored(kGold, "Antibodies: %u", meta.antibodies());

        const ImVec2 small{150.0f, 30.0f};
        ImGui::SameLine();
        {
            const f32 room = ImGui::GetContentRegionAvail().x - (small.x * 2.0f + 8.0f);
            if (room > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
        }
        const bool can_respec = meta.can_respec(cfg);
        if (!can_respec) ImGui::BeginDisabled();
        if (ImGui::Button("Respec", small)) result.action = MenuAction::Respec;
        if (!can_respec) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Refund every purchase except the Neutrophil.\n"
                              "Costs %u Memory Cells.", cfg.respec_cost);
        }
        ImGui::SameLine();
        if (ImGui::Button("Back", small)) result.action = MenuAction::Back;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Every run earns Memory Cells (MC): they buy levels. A level's first "
                           "clear earns an Antibody (AB): it buys towers, abilities and capstones.");
        ImGui::PopStyleColor();
        ImGui::Separator();

        // The hub and the five tower branches, side by side (PROGRESSION.md §4).
        constexpr int kColumns = static_cast<int>(game::kTreeBranchCount);
        if (ImGui::BeginTable("##tree_columns", kColumns,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            for (int c = 0; c < kColumns; ++c) {
                ImGui::TableSetupColumn(game::branch_name(static_cast<game::TreeBranch>(c)),
                                        ImGuiTableColumnFlags_WidthStretch,
                                        c == 0 ? 1.25f : 1.0f);
            }
            ImGui::TableNextRow();
            for (int c = 0; c < kColumns; ++c) {
                ImGui::TableSetColumnIndex(c);
                const auto branch = static_cast<game::TreeBranch>(c);
                ImGui::PushID(c);
                ImGui::TextColored(kAccent, "%s", game::branch_name(branch));
                if (branch == game::TreeBranch::Hub) {
                    ImGui::TextDisabled("economy + abilities");
                } else if (!meta.tower_unlocked(game::branch_tower(branch))) {
                    ImGui::TextColored(kGold, "locked");
                } else {
                    ImGui::TextDisabled("%u/%u pts to capstone", meta.branch_points(branch),
                                        cfg.capstone_threshold);
                }
                ImGui::BeginChild("##col", ImVec2(0.0f, 0.0f), false);
                if (branch == game::TreeBranch::Hub) {
                    ImGui::TextDisabled("Economy");
                    draw_node_group(meta, cfg, branch, game::AbilityId::Count, result);
                    for (u32 a = 0; a < game::kAbilityCount; ++a) {
                        ImGui::Dummy(ImVec2(0.0f, 6.0f));
                        const auto id = static_cast<game::AbilityId>(a);
                        ImGui::TextDisabled("%s", game::ability_name(id));
                        draw_node_group(meta, cfg, branch, id, result);
                    }
                } else {
                    draw_node_group(meta, cfg, branch, game::AbilityId::Count, result);
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    return result;
}

} // namespace immune::ui
