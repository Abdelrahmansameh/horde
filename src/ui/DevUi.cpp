// ui/DevUi.cpp — the ImGui context for developer tools (see DevUi.h).
#include "ui/DevUi.h"

#include "platform/Input.h"
#include "platform/Window.h"
#include "ui/Fonts.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <SDL.h>

namespace immune::ui {

namespace {

/// Bio/immune-themed reskin over ImGui's dark defaults: deep clotted-navy
/// panels, a bioluminescent teal accent, and rounded "cell membrane" shapes
/// in place of the stock hard-edged debug-tool look. Applied once at init --
/// every tool window shares this ImGui context, so one style pass covers
/// them all.
void apply_immune_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    // Rounded, breathing-room-heavy shapes read as "designed UI" rather than
    // "engine debug overlay".
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.IndentSpacing = 18.0f;

    // Deep navy/clot base with a bioluminescent teal accent -- reads as an
    // organism's control panel rather than a grey ImGui demo window.
    const ImVec4 kBase       (0.05f, 0.08f, 0.10f, 1.00f);
    const ImVec4 kBasePanel  (0.07f, 0.11f, 0.14f, 0.92f);
    const ImVec4 kBaseDeep   (0.04f, 0.06f, 0.08f, 0.95f);
    const ImVec4 kTeal       (0.20f, 0.75f, 0.68f, 1.00f);
    const ImVec4 kTealDim    (0.14f, 0.45f, 0.42f, 1.00f);
    const ImVec4 kTealHot    (0.30f, 0.90f, 0.80f, 1.00f);
    const ImVec4 kTextSoft   (0.82f, 0.92f, 0.90f, 1.00f);
    const ImVec4 kTextDim    (0.55f, 0.68f, 0.66f, 1.00f);

    c[ImGuiCol_Text]                 = kTextSoft;
    c[ImGuiCol_TextDisabled]         = kTextDim;
    c[ImGuiCol_WindowBg]             = kBasePanel;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = kBaseDeep;
    c[ImGuiCol_Border]               = ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.35f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = ImVec4(kBase.x, kBase.y, kBase.z, 0.85f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(kTealDim.x, kTealDim.y, kTealDim.z, 0.55f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(kTealDim.x, kTealDim.y, kTealDim.z, 0.75f);
    c[ImGuiCol_TitleBg]              = kBaseDeep;
    c[ImGuiCol_TitleBgActive]        = kBaseDeep;
    c[ImGuiCol_TitleBgCollapsed]     = kBaseDeep;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0.25f);
    c[ImGuiCol_ScrollbarGrab]        = kTealDim;
    c[ImGuiCol_ScrollbarGrabHovered] = kTeal;
    c[ImGuiCol_ScrollbarGrabActive]  = kTealHot;
    c[ImGuiCol_CheckMark]            = kTealHot;
    c[ImGuiCol_SliderGrab]           = kTeal;
    c[ImGuiCol_SliderGrabActive]     = kTealHot;
    c[ImGuiCol_Button]               = ImVec4(kTealDim.x, kTealDim.y, kTealDim.z, 0.55f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.75f);
    c[ImGuiCol_ButtonActive]         = ImVec4(kTealHot.x, kTealHot.y, kTealHot.z, 0.85f);
    c[ImGuiCol_Header]               = ImVec4(kTealDim.x, kTealDim.y, kTealDim.z, 0.55f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.75f);
    c[ImGuiCol_HeaderActive]         = ImVec4(kTealHot.x, kTealHot.y, kTealHot.z, 0.85f);
    c[ImGuiCol_Separator]            = ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.30f);
    c[ImGuiCol_SeparatorHovered]     = kTeal;
    c[ImGuiCol_SeparatorActive]      = kTealHot;
    c[ImGuiCol_ResizeGrip]           = ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.25f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.55f);
    c[ImGuiCol_ResizeGripActive]     = kTeal;
    c[ImGuiCol_PlotLines]            = kTeal;
    c[ImGuiCol_PlotLinesHovered]     = kTealHot;
    c[ImGuiCol_PlotHistogram]        = kTeal;
    c[ImGuiCol_PlotHistogramHovered] = kTealHot;
    c[ImGuiCol_TextSelectedBg]       = ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.35f);
    c[ImGuiCol_NavHighlight]         = kTealHot;
}

} // namespace

bool DevUi::init(platform::Window& window, platform::InputState& input) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Docking, for the level editor's panel layout (docs/LEVEL_EDITOR.md). It
    // is opt-in per config flag and changes nothing for a window that does not
    // ask for it, so the HUD, the menus and the gym panel are unaffected --
    // they keep the free-floating placement imgui.ini already persists.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    apply_immune_theme();
    load_system_fonts(); // must run before backend Init() builds the atlas texture

    if (!ImGui_ImplSDL2_InitForOpenGL(window.sdl_window(), window.gl_context())) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 450")) {
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // InputState::poll() owns the one SDL_PollEvent loop; this is how ImGui
    // gets to see raw events (clicks, wheel, text) without a second competing
    // drain of the queue.
    input.set_raw_event_sink(
        [](const SDL_Event& ev) { ImGui_ImplSDL2_ProcessEvent(&ev); });

    initialized_ = true;
    return true;
}

void DevUi::shutdown() {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void DevUi::begin_frame(platform::InputState& input) {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const ImGuiIO& io = ImGui::GetIO();
    input.set_ui_capture(io.WantCaptureMouse, io.WantCaptureKeyboard);
}

void DevUi::render() {
    if (!initialized_) return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace immune::ui
