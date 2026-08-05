// ui/Hud.cpp — real ImGui build menu + placement cursor. Owner: Wave 3B, 4E.
//
// Wave 4E adds: a world-space range/placement previsualizer ring, tower
// selection (click a placed tower) with an upgrade/sell/trigger-ability
// panel, a wave-preview panel, and a scaffolded active-ability bar. See the
// final report for two frozen-header gaps this wave found and could not
// close itself (ActiveAbilitySystem& and GameStateId not threaded into
// Hud::build()) -- flagged inline below at the exact spots they'd be needed.
#include "ui/Hud.h"

#include "core/Math.h"
#include "game/economy/Economy.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "ui/HudFormat.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

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

// ---------------------------------------------------------------------------
// Selection / cast-cursor state.
//
// Hud.h is a frozen contract this wave does not own (AGENT_BRIEF.md rule 2;
// the task brief is explicit: ".h is frozen"). Tower selection and the
// ability cast cursor are pure per-frame UI state with exactly one live Hud
// instance in the whole app, so file-scope statics here behave identically to
// instance members in practice, are fully encapsulated by this translation
// unit, and require no header change. Reported as a finding rather than
// silently worked around -- see the wave's final report if the orchestrator
// would rather these become real Hud members later.
EntityId g_selected_tower{};
bool g_cast_cursor_active = false;
game::AbilityId g_cast_cursor_ability = game::AbilityId::ComplementCascadeBurst;

/// Pick radius for click-to-select, in world units. Towers have no per-type
/// collision radius exposed to the UI (footprint_radius drives tissue/flow,
/// not picking), so this is a flat constant sized for the placement spacing
/// towers.validate() already enforces (Overlapping).
constexpr f32 kTowerPickRadius = 2.5f;

/// Draws a world-space circle by projecting sampled points through the
/// camera, so the tilted-topdown projection (world Y foreshortened by
/// cos(tilt), see render/Camera.h) renders it as the ellipse it actually is
/// instead of a screen-space circle that would be wrong under tilt.
void draw_range_ring(ImDrawList* draw_list, const render::Camera& camera, Vec2 center,
                     f32 range, ImU32 color, f32 thickness = 2.0f) {
    constexpr int kSegments = 48;
    ImVec2 points[kSegments];
    for (int i = 0; i < kSegments; ++i) {
        const f32 angle = (static_cast<f32>(i) / static_cast<f32>(kSegments)) * math::kTwoPi;
        const Vec2 world_pt = center + Vec2{std::cos(angle), std::sin(angle)} * range;
        const Vec2 screen_pt = camera.world_to_screen(world_pt);
        points[i] = ImVec2(screen_pt.x, screen_pt.y);
    }
    draw_list->AddPolyline(points, kSegments, color, ImDrawFlags_Closed, thickness);
}

constexpr ImU32 kRingValidColor = IM_COL32(90, 220, 120, 220);
constexpr ImU32 kRingInvalidColor = IM_COL32(230, 70, 70, 220);
constexpr ImU32 kRingSelectedColor = IM_COL32(230, 210, 90, 220);
} // namespace

void Hud::build(const sim::SimWorld& world, const game::Economy& economy,
                const game::WaveDirector& waves, const game::TowerSystem& towers,
                const game::ActiveAbilitySystem& abilities, const render::Camera& camera,
                platform::InputState& input, std::vector<Intent>& out_intents) {
    if (!initialized_) return;

    const sim::SimSnapshot snap = world.snapshot();
    const game::EconomySnapshot econ = economy.snapshot();
    const game::WaveStatus wave_status = waves.status();
    const ImGuiIO& io = ImGui::GetIO();

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

    // ---- Wave preview panel ---------------------------------------------
    // During Prep, waves_[wave_index] (reachable via waves.waves() +
    // status().wave_index -- both public) is the wave about to spawn, and
    // phase_time_remaining is exactly the time until it starts. Once that
    // wave is Spawning/Clearing, waves.next_wave() (wave_index + 1) is the
    // one worth previewing instead, but it has no fixed start countdown yet
    // (its prep_time only begins once the current wave finishes Clearing).
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    if (ImGui::Begin("Wave Preview", nullptr, flags)) {
        const std::vector<game::WaveDef>& all_waves = waves.waves();
        const game::WaveDef* preview = nullptr;
        bool has_hard_countdown = false;
        if (wave_status.phase == game::WavePhase::Prep &&
            wave_status.wave_index < all_waves.size()) {
            preview = &all_waves[wave_status.wave_index];
            has_hard_countdown = true;
            ImGui::TextUnformatted("Up next:");
        } else {
            preview = waves.next_wave();
            if (preview) ImGui::TextUnformatted("After this wave:");
        }

        if (!preview) {
            ImGui::TextUnformatted("No further waves.");
        } else {
            if (!preview->name.empty()) ImGui::Text("%s", preview->name.c_str());
            for (const fmt::WaveFamilyCount& fam : fmt::summarize_wave(*preview)) {
                if (fam.has_elite) {
                    ImGui::Text("%s x%u", fmt::pathogen_family_name(fam.family), fam.count);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "[ELITE]");
                } else {
                    ImGui::Text("%s x%u", fmt::pathogen_family_name(fam.family), fam.count);
                }
            }
            if (has_hard_countdown) {
                ImGui::Text("Starts in %s",
                           fmt::format_countdown_seconds(wave_status.phase_time_remaining).c_str());
            } else {
                ImGui::Text("Prep %s once this wave clears",
                           fmt::format_countdown_seconds(preview->prep_time).c_str());
            }
        }
    }
    ImGui::End();

    // ---- Active ability bar ----------------------------------------------
    // BLOCKED (partial): Hud::build() has no ActiveAbilitySystem& (App owns
    // one -- App.cpp's abilities_ -- but its read-only AbilityStatus is not
    // threaded through here), so buttons cannot show cooldown/ready state or
    // disable themselves while on cooldown and show remaining seconds, now
    // that `abilities` is threaded through (orchestrator follow-up to this
    // wave's own flagged gap).
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, io.DisplaySize.y - 60.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    if (ImGui::Begin("Abilities", nullptr, flags)) {
        static constexpr game::AbilityId kAbilityIds[3] = {
            game::AbilityId::ComplementCascadeBurst, game::AbilityId::HistamineFlare,
            game::AbilityId::FeverResponse};
        for (u32 i = 0; i < 3; ++i) {
            const game::AbilityId id = kAbilityIds[i];
            const game::AbilityStatus st = abilities.status(id);
            const bool armed = g_cast_cursor_active && g_cast_cursor_ability == id;
            char label[64];
            if (st.ready) {
                std::snprintf(label, sizeof(label), "%s", game::ability_name(id));
            } else {
                std::snprintf(label, sizeof(label), "%s (%.0fs)", game::ability_name(id),
                             st.cooldown_remaining);
            }
            if (armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.45f, 0.1f, 1.0f));
            if (!st.ready) ImGui::BeginDisabled();
            if (ImGui::Button(label)) {
                if (id == game::AbilityId::FeverResponse) {
                    // Fever Response ignores target_point (ActiveAbilities.h) --
                    // no reason to make the player click the world for it.
                    Intent intent;
                    intent.kind = IntentKind::CastAbility;
                    intent.ability_id = id;
                    intent.world_position = camera.center();
                    out_intents.push_back(intent);
                    g_cast_cursor_active = false;
                } else {
                    g_cast_cursor_active = true;
                    g_cast_cursor_ability = id;
                    clear_build_cursor(); // mutually exclusive armed cursor
                }
            }
            if (!st.ready) ImGui::EndDisabled();
            if (armed) ImGui::PopStyleColor();
            if (i + 1 < 3) ImGui::SameLine();
        }
        if (g_cast_cursor_active) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel##ability")) g_cast_cursor_active = false;
        }
    }
    ImGui::End();

    // ---- Selected tower panel ---------------------------------------------
    if (g_selected_tower.valid()) {
        const entt::entity e = world.ecs().from_id(g_selected_tower);
        const auto& registry = world.ecs().registry();
        if (!registry.valid(e) || !registry.all_of<sim::comp::Tower>(e)) {
            g_selected_tower = EntityId{}; // sold/destroyed since last frame
        } else {
            const sim::comp::Tower& tower = registry.get<sim::comp::Tower>(e);
            const auto* transform = registry.try_get<sim::comp::Transform>(e);
            const Vec2 tower_pos = transform ? transform->position : Vec2{0.0f, 0.0f};
            const game::TowerStats& tstats = towers.stats(tower.type, tower.tier);

            draw_range_ring(ImGui::GetBackgroundDrawList(), camera, tower_pos, tstats.range,
                            kRingSelectedColor);

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 160.0f), ImGuiCond_Always,
                                    ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.7f);
            if (ImGui::Begin("Selected Tower", nullptr, flags)) {
                ImGui::Text("%s -- tier %u", game::tower_type_name(tower.type), tower.tier);
                ImGui::Text("Range %.1f  Dmg %.1f  Fire %.2fs", tstats.range, tstats.damage,
                           tstats.fire_interval);

                if (tower.tier < 3) {
                    const bool affordable = economy.can_afford(tstats.upgrade_cost);
                    char label[64];
                    std::snprintf(label, sizeof(label), "Upgrade (%u)", tstats.upgrade_cost);
                    if (!affordable) ImGui::BeginDisabled();
                    if (ImGui::Button(label)) {
                        Intent intent;
                        intent.kind = IntentKind::UpgradeTower;
                        intent.entity = g_selected_tower;
                        out_intents.push_back(intent);
                    }
                    if (!affordable) ImGui::EndDisabled();
                    ImGui::SameLine();
                }

                if (ImGui::Button("Sell")) {
                    Intent intent;
                    intent.kind = IntentKind::SellTower;
                    intent.entity = g_selected_tower;
                    out_intents.push_back(intent);
                    g_selected_tower = EntityId{};
                }

                if (tstats.ability_cooldown > 0.0f) {
                    ImGui::SameLine();
                    const bool ready = tower.ability_cooldown <= 0.0f;
                    if (!ready) ImGui::BeginDisabled();
                    char ability_label[32];
                    if (ready) {
                        std::snprintf(ability_label, sizeof(ability_label), "Ability");
                    } else {
                        std::snprintf(ability_label, sizeof(ability_label), "Ability (%s)",
                                     fmt::format_countdown_seconds(tower.ability_cooldown).c_str());
                    }
                    if (ImGui::Button(ability_label)) {
                        Intent intent;
                        intent.kind = IntentKind::TriggerAbility;
                        intent.entity = g_selected_tower;
                        out_intents.push_back(intent);
                    }
                    if (!ready) ImGui::EndDisabled();
                }

                if (ImGui::Button("Deselect")) g_selected_tower = EntityId{};
            }
            ImGui::End();
        }
    }

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
            if (ImGui::Button(label)) {
                set_build_cursor(type);
                g_cast_cursor_active = false; // mutually exclusive armed cursor
            }
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
            if (input.action_pressed(kSelectActions[i])) {
                set_build_cursor(static_cast<TowerType>(i));
                g_cast_cursor_active = false;
            }
        }
        if (input.action_pressed(Action::CancelPlacement)) {
            clear_build_cursor();
            g_cast_cursor_active = false;
        }
    }

    // ---- Placement previsualizer: a world-space ring under the build
    // cursor, sized to the armed tower's range and tinted by whether the
    // hovered spot would validate.
    if (build_cursor_active_) {
        const Vec2 hovered_world = camera.screen_to_world(input.mouse_pos());
        const game::PlacementQuery query =
            towers.validate(world, build_cursor_type_, hovered_world, economy.atp());
        const game::TowerStats& armed_stats = towers.stats(build_cursor_type_, 1);
        draw_range_ring(ImGui::GetBackgroundDrawList(), camera, query.snapped_position,
                        armed_stats.range, query.valid() ? kRingValidColor : kRingInvalidColor);
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

    // ---- Cast-cursor click-to-target: Complement Cascade Burst and
    // Histamine Flare both resolve at a world point (Fever Response fires
    // instantly from the ability bar above and never reaches here).
    if (g_cast_cursor_active && !input.ui_capture_mouse() &&
        input.mouse_pressed(platform::MouseButton::Left)) {
        const Vec2 world_pos = camera.screen_to_world(input.mouse_pos());
        Intent intent;
        intent.kind = IntentKind::CastAbility;
        intent.ability_id = g_cast_cursor_ability;
        intent.world_position = world_pos;
        out_intents.push_back(intent);
        g_cast_cursor_active = false;
    }
    if (g_cast_cursor_active && !input.ui_capture_mouse() &&
        input.mouse_pressed(platform::MouseButton::Right)) {
        g_cast_cursor_active = false;
    }

    // ---- Tower selection: only when neither cursor is armed, so a build or
    // ability click can never be swallowed by a selection pick underneath it.
    if (!build_cursor_active_ && !g_cast_cursor_active && !input.ui_capture_mouse() &&
        input.mouse_pressed(platform::MouseButton::Left)) {
        const Vec2 world_pos = camera.screen_to_world(input.mouse_pos());
        std::vector<std::pair<EntityId, Vec2>> candidates;
        candidates.reserve(towers.placed_towers().size());
        for (EntityId id : towers.placed_towers()) {
            const entt::entity e = world.ecs().from_id(id);
            if (!world.ecs().registry().valid(e)) continue;
            if (const auto* tf = world.ecs().registry().try_get<sim::comp::Transform>(e)) {
                candidates.emplace_back(id, tf->position);
            }
        }
        g_selected_tower = fmt::pick_nearest(candidates, world_pos, kTowerPickRadius);
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
