// ui/Fonts.cpp — see Fonts.h. Loads a system-installed TTF instead of
// shipping one, keeping the repo's zero-binary-asset rule intact.
#include "ui/Fonts.h"

#include "core/Types.h"

#include <imgui.h>

#include <cstdio>

namespace immune::ui {
namespace {

ImFont* g_title_font = nullptr;

constexpr f32 kBodySize = 18.0f;
constexpr f32 kTitleSize = 34.0f;

/// Courier New (or the closest monospace equivalent on non-Windows
/// platforms), most-preferred first. First one found on disk wins.
constexpr const char* kCandidates[] = {
    // Windows
    "C:\\Windows\\Fonts\\cour.ttf",
    // macOS
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/Library/Fonts/Courier New.ttf",
    // Linux (common distro paths for a Courier-alike monospace)
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
};

bool file_exists(const char* path) {
    if (FILE* f = std::fopen(path, "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

const char* find_system_font() {
    for (const char* path : kCandidates) {
        if (file_exists(path)) return path;
    }
    return nullptr;
}

} // namespace

void load_system_fonts() {
    const char* path = find_system_font();
    if (!path) return; // no-op: keep ImGui's built-in font

    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 3;

    // First font added becomes ImGui's default; bake a second, larger
    // instance of the same face for headings (draw_title() in Menu.cpp).
    ImFont* body = io.Fonts->AddFontFromFileTTF(path, kBodySize, &cfg);
    if (!body) return;
    g_title_font = io.Fonts->AddFontFromFileTTF(path, kTitleSize, &cfg);
}

ImFont* title_font() { return g_title_font; }

} // namespace immune::ui
