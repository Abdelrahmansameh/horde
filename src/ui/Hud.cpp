// ui/Hud.cpp — real ImGui build menu + placement cursor. Owner: Wave 3B.
//
// Minimal-but-real implementation: a build menu that arms the placement
// cursor, click-to-place, and a status readout (ATP, wave phase, objective
// integrity). Deliberately does not yet cover tower selection/upgrade/sell
// panels or the per-lane threat overlay (no lane geometry exists to summarize
// -- only vessel splines) or in-panel tower tooltips; those are real Wave 3B
// scope left for later, not placeholders standing in for missing plumbing.
#include "ui/Hud.h"

#include "core/Math.h"
#include "game/economy/Economy.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "sim/SimWorld.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <SDL.h>

#include <cstdio>

namespace immune::ui {

bool Hud::init(platform::Window& window, platform::InputState& input) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

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

void Hud::shutdown() {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void Hud::begin_frame(platform::InputState& input) {
    if (!initialized_) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const ImGuiIO& io = ImGui::GetIO();
    input.set_ui_capture(io.WantCaptureMouse, io.WantCaptureKeyboard);
}

namespace {
const char* wave_phase_name(game::WavePhase p) {
    switch (p) {
        case game::WavePhase::Prep:     return "Prep";
        case game::WavePhase::Spawning: return "Spawning";
        case game::WavePhase::Clearing: return "Clearing";
        case game::WavePhase::Complete: return "Complete";
    }
    return "?";
}
} // namespace

void Hud::build(const sim::SimWorld& world, const game::Economy& economy,
                const game::WaveDirector& waves, const game::TowerSystem& towers,
                const render::Camera& camera, platform::InputState& input,
                std::vector<Intent>& out_intents) {
    if (!initialized_) return;

    const sim::SimSnapshot snap = world.snapshot();
    const game::EconomySnapshot econ = economy.snapshot();
    const game::WaveStatus wave_status = waves.status();

    // ---- Status bar --------------------------------------------------------
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("Status", nullptr, flags)) {
        ImGui::Text("ATP: %u  (+%.1f/s)", econ.atp, econ.income_per_second);
        ImGui::Text("Objective integrity: %.0f%%", snap.objective_integrity);
        if (wave_status.all_waves_complete) {
            ImGui::Text("All waves cleared");
        } else {
            ImGui::Text("Wave %u: %s", wave_status.wave_index + 1,
                       wave_phase_name(wave_status.phase));
            if (wave_status.phase == game::WavePhase::Prep) {
                ImGui::Text("Next wave in %.0fs", math::max(wave_status.phase_time_remaining, 0.0f));
                if (ImGui::Button("Start now")) out_intents.push_back(Intent{IntentKind::StartWaveEarly});
            } else if (wave_status.phase == game::WavePhase::Spawning) {
                ImGui::Text("%u still incoming", wave_status.remaining_to_spawn);
            }
        }
        ImGui::Text("Chaff: %llu  Named: %llu", static_cast<unsigned long long>(snap.chaff_count),
                   static_cast<unsigned long long>(snap.named_count));
    }
    ImGui::End();

    // ---- Build menu ----------------------------------------------------------
    ImGui::SetNextWindowPos(ImVec2(12, static_cast<f32>(ImGui::GetIO().DisplaySize.y) - 60.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.7f);
    if (ImGui::Begin("Build", nullptr, flags)) {
        for (u32 i = 0; i < kTowerTypeCount; ++i) {
            const TowerType type = static_cast<TowerType>(i);
            const game::TowerStats& stats = towers.stats(type, 1);
            char label[64];
            std::snprintf(label, sizeof(label), "%s (%u)", game::tower_type_name(type),
                         stats.build_cost);
            const bool affordable = economy.can_afford(stats.build_cost);
            if (!affordable) ImGui::BeginDisabled();
            const bool armed = build_cursor_active_ && build_cursor_type_ == type;
            if (armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
            if (ImGui::Button(label)) set_build_cursor(type);
            if (armed) ImGui::PopStyleColor();
            if (!affordable) ImGui::EndDisabled();
            if (i + 1 < kTowerTypeCount) ImGui::SameLine();
        }
        if (build_cursor_active_) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel (Esc)")) clear_build_cursor();
        }
    }
    ImGui::End();

    // ---- Number-key shortcuts arm the same build cursor as clicking a button.
    using platform::Action;
    static constexpr Action kSelectActions[kTowerTypeCount] = {
        Action::SelectTower1, Action::SelectTower2, Action::SelectTower3, Action::SelectTower4,
        Action::SelectTower5, Action::SelectTower6, Action::SelectTower7, Action::SelectTower8,
    };
    if (!input.ui_capture_keyboard()) {
        for (u32 i = 0; i < kTowerTypeCount; ++i) {
            if (input.action_pressed(kSelectActions[i])) set_build_cursor(static_cast<TowerType>(i));
        }
        if (input.action_pressed(Action::CancelPlacement)) clear_build_cursor();
    }

    // ---- Click-to-place: only when the cursor is armed and the click wasn't
    // consumed by an ImGui widget (build menu buttons already fired above).
    if (build_cursor_active_ && !input.ui_capture_mouse() &&
        input.mouse_pressed(platform::MouseButton::Left)) {
        const Vec2 world_pos = camera.screen_to_world(input.mouse_pos());
        Intent intent;
        intent.kind = IntentKind::PlaceTower;
        intent.tower_type = build_cursor_type_;
        intent.world_position = world_pos;
        out_intents.push_back(intent);
        clear_build_cursor();
    }
    if (build_cursor_active_ && !input.ui_capture_mouse() &&
        input.mouse_pressed(platform::MouseButton::Right)) {
        clear_build_cursor();
    }
}

void Hud::render() {
    if (!initialized_) return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Hud::update_threat_overlay(const sim::SimWorld&) {
    // No lane geometry exists yet to summarize into a per-lane indicator
    // (levels author vessel splines, not lanes) -- real Wave 3B scope once
    // that data model exists, not a stand-in for missing plumbing today.
    threats_.clear();
}

} // namespace immune::ui
