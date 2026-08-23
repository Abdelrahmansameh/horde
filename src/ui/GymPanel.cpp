// ui/GymPanel.cpp — ImGui front end for game/gym's command language.
//
// Every widget below ends in execute(), which runs a command string and echoes
// it into the log. Nothing here reaches the sim directly; see GymPanel.h.
#include "ui/GymPanel.h"

#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/gym/GymCommands.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"

#include <imgui.h>

#include <cstdio>

namespace immune::ui {
namespace {

const ImVec4 kColorEcho{0.55f, 0.68f, 0.66f, 1.00f};
const ImVec4 kColorOut{0.82f, 0.92f, 0.90f, 1.00f};
const ImVec4 kColorErr{0.95f, 0.45f, 0.40f, 1.00f};
const ImVec4 kColorDim{0.55f, 0.68f, 0.66f, 1.00f};

/// Splits a multi-line command result so each line is its own log row.
void append_lines(std::vector<std::string>& out, const std::string& text) {
    usize start = 0;
    while (start <= text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        out.emplace_back(text.substr(start, end - start));
        if (end == text.size()) break;
        start = end + 1;
    }
}

/// printf into a std::string, for building command lines.
template <typename... Args>
std::string text(const char* format, Args... args) {
    char buf[256];
    const int n = std::snprintf(buf, sizeof(buf), format, args...);
    return n > 0 ? std::string(buf, static_cast<usize>(n) < sizeof(buf)
                                        ? static_cast<usize>(n)
                                        : sizeof(buf) - 1)
                 : std::string{};
}

/// A button that runs `command` when clicked, with the command itself as its
/// tooltip -- the panel documents the text language as you use it.
bool command_button(const char* label, const char* command, const ImVec2& size = ImVec2(0, 0)) {
    const bool clicked = ImGui::Button(label, size);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", command);
    return clicked;
}

} // namespace

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

void GymPanel::print(const std::string& message, bool ok) {
    std::vector<std::string> split;
    append_lines(split, message);
    for (std::string& s : split) lines_.push_back(Line{std::move(s), ok ? u8{0} : u8{2}});
    scroll_to_bottom_ = true;
    // The log is a session record, not storage: cap it so a long session with a
    // button held down cannot grow it without bound.
    if (lines_.size() > 500) lines_.erase(lines_.begin(), lines_.begin() + 200);
}

void GymPanel::clear_log() { lines_.clear(); }

bool GymPanel::execute(game::GymContext& ctx, const std::string& line) {
    if (line.empty()) return true;
    lines_.push_back(Line{"> " + line, 1});

    // `clear` is the panel's own: it edits the log, which the executor cannot
    // see and must not know about.
    if (line == "clear" || line == "cls") {
        lines_.clear();
        return true;
    }

    const game::GymResult r = game::gym_execute(ctx, line);
    if (!r.message.empty()) print(r.message, r.ok);
    history_.push_back(line);
    if (history_.size() > 200) history_.erase(history_.begin());
    history_pos_ = -1;
    scroll_to_bottom_ = true;
    return r.ok;
}

void GymPanel::set_level(const std::string& level_name) {
    // Opens itself on the gym level and nowhere else. Any other level leaves
    // the current state alone, so a panel opened by hand elsewhere survives.
    if (level_name == "gym") visible_ = true;
}

std::string GymPanel::target_arg() const {
    switch (target_mode_) {
        case 1: return target_spawn_point_id_.empty() ? std::string("cursor") : target_spawn_point_id_;
        case 2: return "objective";
        case 3: return text("%.1f,%.1f", target_xy_[0], target_xy_[1]);
        default: return "cursor";
    }
}

std::string GymPanel::target_suffix() const { return " at " + target_arg(); }

// ---------------------------------------------------------------------------
// Target bar — one control that aims every command that takes a point
// ---------------------------------------------------------------------------

void GymPanel::draw_target_bar(game::GymContext& ctx) {
    ImGui::TextDisabled("TARGET");
    ImGui::SameLine();
    ImGui::RadioButton("cursor", &target_mode_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("spawn point", &target_mode_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("objective", &target_mode_, 2);
    ImGui::SameLine();
    ImGui::RadioButton("point", &target_mode_, 3);

    if (target_mode_ == 1) {
        std::vector<const char*> ids;
        if (ctx.world != nullptr) {
            for (const sim::SpawnPointRuntime& p : ctx.world->spawn_points()) ids.push_back(p.id.c_str());
        }
        if (ids.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kColorErr, "(this level has no spawn points)");
        } else {
            if (target_spawn_point_ >= static_cast<i32>(ids.size())) target_spawn_point_ = 0;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::Combo("##spawn_point", &target_spawn_point_, ids.data(), static_cast<int>(ids.size()));
            // Cached as a string because target_arg() has no context to look it
            // up from, and the spawn point list belongs to whichever level is loaded.
            target_spawn_point_id_ = ids[static_cast<usize>(target_spawn_point_)];
        }
    } else if (target_mode_ == 3) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat2("##xy", target_xy_, "%.1f");
    }
    ImGui::TextDisabled("aims spawn / cast / field / vfx / cam: '%s'", target_suffix().c_str());
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

void GymPanel::draw_horde_tab(game::GymContext& ctx) {
    const std::vector<const char*>& families = game::gym_family_names();
    const std::string at = target_suffix();

    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("family", &family_, families.data(), static_cast<int>(families.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("count", &spawn_count_, 50, 500);
    if (spawn_count_ < 1) spawn_count_ = 1;

    if (ImGui::Button("Spawn")) {
        execute(ctx, text("spawn %s %d%s", families[static_cast<usize>(family_)], spawn_count_,
                          at.c_str()));
    }
    ImGui::SameLine();
    if (ImGui::Button("Spawn every family")) {
        execute(ctx, text("spawn all %d%s", spawn_count_, at.c_str()));
    }
    ImGui::SameLine();
    if (ImGui::Button("Kill this family")) {
        execute(ctx, text("kill %s", families[static_cast<usize>(family_)]));
    }
    ImGui::SameLine();
    if (command_button("Kill all", "kill all")) execute(ctx, "kill all");

    ImGui::Separator();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("per spawn point", &flood_count_, 100, 500);
    if (flood_count_ < 1) flood_count_ = 1;
    ImGui::SameLine();
    if (ImGui::Button("Flood every spawn point")) execute(ctx, text("flood %d", flood_count_));

    ImGui::Separator();
    std::vector<const char*> elite_names;
    if (ctx.enemies != nullptr) {
        for (const game::EliteDef& e : ctx.enemies->elites()) elite_names.push_back(e.name);
    }
    if (elite_names.empty()) {
        ImGui::TextColored(kColorErr, "no elite roster in this context");
    } else {
        if (elite_ >= static_cast<i32>(elite_names.size())) elite_ = 0;
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("elite", &elite_, elite_names.data(), static_cast<int>(elite_names.size()));
        ImGui::SameLine();
        if (ImGui::Button("Spawn elite")) {
            execute(ctx, text("elite %s%s", elite_names[static_cast<usize>(elite_)], at.c_str()));
        }
        ImGui::SameLine();
        if (ImGui::Button("Spawn every elite")) execute(ctx, text("elite all%s", at.c_str()));
    }

    // Live readout: the numbers you are about to change, next to the buttons
    // that change them.
    if (ctx.world != nullptr) {
        const sim::SimSnapshot snap = ctx.world->snapshot();
        ImGui::Separator();
        ImGui::Text("chaff %llu / %zu   density %.0f   named %llu",
                    static_cast<unsigned long long>(snap.chaff_count),
                    ctx.world->chaff().capacity(), snap.total_density,
                    static_cast<unsigned long long>(snap.named_count));
        for (usize f = 0; f < families.size(); ++f) {
            if (f != 0) ImGui::SameLine();
            ImGui::TextColored(kColorDim, "%s %u", families[f], snap.chaff_by_family[f]);
        }
        if (ctx.spawns != nullptr && ctx.spawns->pending() > 0) {
            ImGui::TextColored(kColorEcho, "%u still streaming in", ctx.spawns->pending());
        }
    }
}

void GymPanel::draw_defense_tab(game::GymContext& ctx) {
    const std::string at = target_suffix();

    std::vector<const char*> tower_names;
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        tower_names.push_back(game::tower_type_name(static_cast<TowerType>(t)));
    }
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("tower", &tower_, tower_names.data(), static_cast<int>(tower_names.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("tier", &tower_tier_, 1, 3);

    if (ImGui::Button("Place")) {
        execute(ctx, text("tower %s%s tier %d", tower_names[static_cast<usize>(tower_)], at.c_str(),
                          tower_tier_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Place one of each")) {
        execute(ctx, text("tower all%s tier %d", at.c_str(), tower_tier_));
    }
    ImGui::SameLine();
    if (command_button("Upgrade all", "upgrade all")) execute(ctx, "upgrade all");
    ImGui::SameLine();
    if (command_button("Fire abilities", "fire")) execute(ctx, "fire");
    ImGui::SameLine();
    if (command_button("Sell all", "sell all")) execute(ctx, "sell all");

    if (ctx.towers != nullptr) {
        ImGui::TextDisabled("%zu tower(s) placed; the gym never charges for them",
                            ctx.towers->placed_towers().size());
    }

    ImGui::Separator();
    ImGui::TextDisabled("ABILITIES");
    if (ctx.abilities == nullptr) {
        ImGui::TextColored(kColorErr, "no ability system in this context");
    } else {
        static const char* const kCastArg[game::kAbilityCount] = {"complement", "histamine",
                                                                  "fever"};
        for (u32 i = 0; i < game::kAbilityCount; ++i) {
            const game::AbilityId id = static_cast<game::AbilityId>(i);
            const game::AbilityStatus st = ctx.abilities->status(id);
            if (i != 0) ImGui::SameLine();
            ImGui::BeginDisabled(!st.ready);
            if (ImGui::Button(game::ability_name(id))) {
                execute(ctx, text("cast %s%s", kCastArg[i], at.c_str()));
            }
            ImGui::EndDisabled();
            if (!st.ready) {
                ImGui::SameLine();
                ImGui::TextColored(kColorDim, "%.0fs", st.cooldown_remaining);
            }
        }
        if (command_button("Clear every cooldown", "ready")) execute(ctx, "ready");
    }

    ImGui::Separator();
    ImGui::TextDisabled("ECONOMY");
    if (ctx.economy == nullptr) {
        ImGui::TextColored(kColorErr, "no economy in this context");
    } else {
        ImGui::Text("ATP %u", ctx.economy->atp());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("##atp", &atp_amount_, 500, 5000);
        if (atp_amount_ < 0) atp_amount_ = 0;
        ImGui::SameLine();
        if (ImGui::Button("Set")) execute(ctx, text("atp %d", atp_amount_));
        ImGui::SameLine();
        if (ImGui::Button("Add")) execute(ctx, text("atp +%d", atp_amount_));
    }
}

void GymPanel::draw_waves_tab(game::GymContext& ctx) {
    if (ctx.waves == nullptr) {
        ImGui::TextColored(kColorErr, "no wave director in this context");
        return;
    }
    const game::WaveStatus st = ctx.waves->status();
    const std::vector<game::WaveDef>& table = ctx.waves->waves();

    ImGui::Text("wave %u of %zu", st.wave_index + 1, table.size());
    ImGui::SameLine();
    ImGui::TextColored(kColorDim, "%.1fs left, %u still to spawn%s", st.phase_time_remaining,
                       st.remaining_to_spawn, st.all_waves_complete ? "  [ALL COMPLETE]" : "");

    if (command_button("Start now (skip prep)", "wave start")) execute(ctx, "wave start");
    ImGui::SameLine();
    if (command_button("Next wave", "wave next")) execute(ctx, "wave next");
    ImGui::SameLine();
    if (command_button("Status to log", "wave status")) execute(ctx, "wave status");

    ImGui::Separator();
    if (table.empty()) {
        ImGui::TextDisabled("this level has no authored wave table");
        return;
    }
    // The wave list doubles as the jump control: clicking a row is "run this
    // one now", which is the whole reason to have a wave table in a gym.
    // Explicit per-column sizing, NOT SizingStretchProp: in that mode a
    // WidthFixed value is reinterpreted as a stretch *weight*, which squeezed
    // the name column (weight 1) down to a few pixels next to the numeric
    // columns (weights 24 and 60).
    if (ImGui::BeginTable("waves", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("agents", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("##jump", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < table.size(); ++i) {
            const game::WaveDef& w = table[i];
            u32 agents = 0;
            for (const game::SpawnEntry& e : w.spawns) agents += e.count;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(w.name.empty() ? "(unnamed)" : w.name.c_str());
            if (w.modifier != game::WaveModifier::None) {
                ImGui::SameLine();
                static const char* const kMod[] = {"", "fever", "swarm"};
                ImGui::TextColored(kColorEcho, "[%s]", kMod[static_cast<u32>(w.modifier)]);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%u", agents);
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Jump")) execute(ctx, text("wave %zu", i + 1));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void GymPanel::draw_world_tab(game::GymContext& ctx) {
    const std::string at = target_suffix();

    ImGui::TextDisabled("TIME");
    if (command_button("Pause", "time 0")) execute(ctx, "time 0");
    ImGui::SameLine();
    if (command_button("1x", "time 1")) execute(ctx, "time 1");
    ImGui::SameLine();
    if (command_button("2x", "time 2")) execute(ctx, "time 2");
    ImGui::SameLine();
    if (command_button("4x", "time 4")) execute(ctx, "time 4");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("##steps", &step_ticks_, 10, 60);
    if (step_ticks_ < 1) step_ticks_ = 1;
    ImGui::SameLine();
    if (ImGui::Button("Step")) execute(ctx, text("step %d", step_ticks_));

    ImGui::Separator();
    ImGui::TextDisabled("OBJECTIVE");
    if (ctx.toggles != nullptr) {
        bool invuln = ctx.toggles->objective_invulnerable;
        if (ImGui::Checkbox("infinite integrity", &invuln)) {
            execute(ctx, invuln ? "invuln on" : "invuln off");
        }
        ImGui::SameLine();
        const f32 integrity = ctx.world->snapshot().objective_integrity;
        if (ctx.toggles->objective_invulnerable) {
            ImGui::TextColored(kColorEcho, "held at %.0f  (leaks still count)",
                               static_cast<double>(integrity));
        } else {
            ImGui::TextColored(integrity < 100.0f ? kColorErr : kColorDim, "integrity %.1f",
                               static_cast<double>(integrity));
        }
    } else {
        ImGui::TextDisabled("no per-tick gym settings in this context");
    }

    ImGui::Separator();
    ImGui::TextDisabled("CAMERA / OVERLAYS");
    if (command_button("Fit level", "cam fit")) execute(ctx, "cam fit");
    ImGui::SameLine();
    if (ImGui::Button("Go to target")) {
        // `cam` takes the target directly, without the `at` keyword.
        execute(ctx, cam_height_ > 0.0f
                         ? text("cam %s %.0f", target_arg().c_str(), cam_height_)
                         : text("cam %s", target_arg().c_str()));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("view height", &cam_height_, 0.0f, 200.0f, "%.0f (0 = keep)");

    if (ImGui::Checkbox("debug overlay", &overlay_debug_)) {
        execute(ctx, overlay_debug_ ? "overlay debug on" : "overlay debug off");
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("threat overlay", &overlay_threat_)) {
        execute(ctx, overlay_threat_ ? "overlay threat on" : "overlay threat off");
    }

    ImGui::Separator();
    ImGui::TextDisabled("DAMAGE FIELD  (aggregate damage with no tower involved)");
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderFloat("radius", &field_radius_, 2.0f, 60.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderFloat("kill rate", &field_rate_, 1.0f, 400.0f, "%.0f/s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderFloat("seconds", &field_duration_, 0.1f, 10.0f, "%.1f");
    if (ImGui::Button("Submit field")) {
        execute(ctx, text("field %.0f %.0f %.1f%s", field_radius_, field_rate_, field_duration_,
                          at.c_str()));
    }

    ImGui::Separator();
    ImGui::TextDisabled("COMBAT EVENTS  (drives the particle layer directly)");
    const std::vector<const char*>& events = game::gym_vfx_event_names();
    if (vfx_ >= static_cast<i32>(events.size())) vfx_ = 0;
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("event", &vfx_, events.data(), static_cast<int>(events.size()));
    ImGui::SameLine();
    if (ImGui::Button("Raise")) {
        execute(ctx, text("vfx %s%s", events[static_cast<usize>(vfx_)], at.c_str()));
    }
    ImGui::SameLine();
    if (ImGui::Button("Raise every event")) execute(ctx, text("vfx all%s", at.c_str()));

    ImGui::Separator();
    if (command_button("Stats to log", "stats")) execute(ctx, "stats");
    ImGui::SameLine();
    if (command_button("Spawn points to log", "spawn_points")) execute(ctx, "spawn_points");
    ImGui::SameLine();
    if (command_button("Restart level", "restart")) execute(ctx, "restart");
}

// ---------------------------------------------------------------------------
// Log strip + input line
// ---------------------------------------------------------------------------

void GymPanel::draw_log(game::GymContext& ctx) {
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("gym_log", ImVec2(0.0f, -footer), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const Line& l : lines_) {
            const ImVec4 col = l.kind == 1 ? kColorEcho : (l.kind == 2 ? kColorErr : kColorOut);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
        if (scroll_to_bottom_) {
            ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom_ = false;
        }
    }
    ImGui::EndChild();

    // History recall. Explicit key checks rather than an ImGui history callback
    // because the callback only fires while the field owns focus, and every
    // button in the panel above steals it.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !history_.empty()) {
        const i32 last = static_cast<i32>(history_.size()) - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
            history_pos_ = history_pos_ < 0 ? last : (history_pos_ > 0 ? history_pos_ - 1 : 0);
            std::snprintf(input_, sizeof(input_), "%s",
                          history_[static_cast<usize>(history_pos_)].c_str());
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && history_pos_ >= 0) {
            if (history_pos_ >= last) {
                history_pos_ = -1;
                input_[0] = '\0';
            } else {
                ++history_pos_;
                std::snprintf(input_, sizeof(input_), "%s",
                              history_[static_cast<usize>(history_pos_)].c_str());
            }
        }
    }

    ImGui::SetNextItemWidth(-230.0f);
    const bool submitted = ImGui::InputText("##gym_input", input_, sizeof(input_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    if (submitted || focus_input_) {
        ImGui::SetKeyboardFocusHere(-1);
        focus_input_ = false;
    }
    ImGui::SameLine();
    const bool run_clicked = ImGui::Button("Run");
    ImGui::SameLine();
    if (ImGui::Button("help")) execute(ctx, "help");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) lines_.clear();

    if (submitted || run_clicked) {
        // The toggle key types itself into the field before the toggle check
        // sees it on some layouts; strip a leading backtick so the first command
        // after opening the panel is not silently mangled.
        char* line = input_;
        while (*line == '`' || *line == '~') ++line;
        const std::string command(line);
        input_[0] = '\0';
        execute(ctx, command);
        focus_input_ = true;
    }
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void GymPanel::build(game::GymContext& ctx) {
    // Owns its own toggle rather than going through platform::Action, so it
    // works identically in any game state and needs no change to the frozen
    // input binding table. Backtick, or F2 where backtick is a dead key. NOT F1:
    // platform::Action::ToggleDebugOverlay already owns that.
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        toggle();
    }
    if (!visible_) return;

    if (!greeted_) {
        greeted_ = true;
        print("Gym panel. Every button runs a command and echoes it here; hover one to see it.");
        print("Type 'help' for the full language. ` or F2 hides this window.");
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 20.0f, vp->WorkPos.y + 20.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(820.0f, 660.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Gym", &visible_)) {
        ImGui::End();
        return;
    }

    if (ctx.world == nullptr) {
        ImGui::TextColored(kColorErr, "No level loaded.");
        ImGui::TextDisabled("Load one to get the controls: 'level gym' in the box below.");
        draw_log(ctx);
        ImGui::End();
        return;
    }

    draw_target_bar(ctx);
    ImGui::Separator();

    // The tabs sit above a log strip that stays visible, so the result of a
    // click is on screen next to the button that caused it.
    const auto tab_flags = [this](i32 index) -> ImGuiTabItemFlags {
        return force_tab_ == index ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    // The tabs take everything the log strip does not: a fixed-height tab area
    // clipped the bottom of the longer tabs (the World tab's combat-event row
    // simply was not on screen), and a debug control you cannot see is a debug
    // control that does not exist.
    const float log_height = 150.0f;
    const float footer_height = ImGui::GetFrameHeightWithSpacing() +
                                ImGui::GetStyle().ItemSpacing.y * 3.0f;
    if (ImGui::BeginChild("gym_tabs", ImVec2(0.0f, -(log_height + footer_height)))) {
        if (ImGui::BeginTabBar("gym_tabbar")) {
            if (ImGui::BeginTabItem("Horde", nullptr, tab_flags(0))) {
                draw_horde_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Defense", nullptr, tab_flags(1))) {
                draw_defense_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Waves", nullptr, tab_flags(2))) {
                draw_waves_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World", nullptr, tab_flags(3))) {
                draw_world_tab(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    force_tab_ = -1;
    ImGui::Separator();
    draw_log(ctx);

    ImGui::End();
}

} // namespace immune::ui
