// ui/Menu.cpp — main menu and level select. See Menu.h for why these screens
// are separate from Hud and why they report clicks rather than driving the
// state machine themselves.
#include "ui/Menu.h"

#include <imgui.h>

#include <cstdio>

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

/// A title drawn larger than body text. ImGui's default font is a single size,
/// so scale rather than swap fonts — this keeps the menu working with the
/// stock atlas and avoids shipping a font file (this project has a zero
/// binary-asset rule).
void draw_title(const char* text, f32 scale) {
    const f32 old = ImGui::GetFont()->Scale;
    ImGui::GetFont()->Scale = scale;
    ImGui::PushFont(ImGui::GetFont());
    center_next_item(ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::GetFont()->Scale = old;
}

} // namespace

MenuResult Menu::build_main_menu(i32 screen_width, i32 screen_height) {
    MenuResult result;
    center_next_window(screen_width, screen_height, 560.0f, 320.0f);
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

} // namespace immune::ui
