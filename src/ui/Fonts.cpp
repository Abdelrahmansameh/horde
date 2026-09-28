// ui/Fonts.cpp — see Fonts.h. Loads a system-installed TTF for the ImGui
// developer tools.
#include "ui/Fonts.h"

#include "core/Types.h"

#include <imgui.h>

#include <cstdio>

namespace immune::ui {
namespace {

constexpr f32 kBodySize = 18.0f;

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

    // The first font added becomes ImGui's default.
    io.Fonts->AddFontFromFileTTF(path, kBodySize, &cfg);
}

} // namespace immune::ui
