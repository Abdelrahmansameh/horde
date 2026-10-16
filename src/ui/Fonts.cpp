// ui/Fonts.cpp — see Fonts.h. Loads a system-installed TTF for the ImGui
// developer tools.
#include "ui/Fonts.h"

#include "core/Types.h"
#include "platform/FileIO.h"

#include <imgui.h>

#include <cstring>
#include <limits>

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

const char* find_system_font() {
    for (const char* path : kCandidates) {
        if (platform::file_exists(path)) return path;
    }
    return nullptr;
}

} // namespace

void load_system_fonts() {
    const char* path = find_system_font();
    if (!path) return; // no-op: keep ImGui's built-in font
    const auto data = platform::read_binary_file(path);
    if (!data || data->empty() || data->size() > static_cast<usize>(std::numeric_limits<int>::max())) return;

    // ImGui keeps font source bytes until the atlas is destroyed. Allocate
    // with its allocator and transfer ownership rather than retaining a
    // pointer into the temporary file buffer.
    void* font_data = ImGui::MemAlloc(data->size());
    if (font_data == nullptr) return;
    std::memcpy(font_data, data->data(), data->size());

    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 3;
    cfg.FontDataOwnedByAtlas = true;

    // The first font added becomes ImGui's default.
    io.Fonts->AddFontFromMemoryTTF(font_data, static_cast<int>(data->size()), kBodySize, &cfg);
}

} // namespace immune::ui
