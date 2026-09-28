#include "app/UiBridge.h"

#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "gui/core/Gui.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "ui/HudFormat.h"
#include "ui/hud/HudScreen.h"

#include <cstdlib>

namespace immune::app {

namespace {

ui::HudWavePhase phase_of(game::WavePhase p) {
    switch (p) {
        case game::WavePhase::Prep: return ui::HudWavePhase::Prep;
        case game::WavePhase::Spawning: return ui::HudWavePhase::Spawning;
        case game::WavePhase::Clearing: return ui::HudWavePhase::Clearing;
        case game::WavePhase::Complete: return ui::HudWavePhase::Complete;
    }
    return ui::HudWavePhase::Complete;
}

ui::HudWavePreview preview_of(const game::WaveDef& w, u32 number) {
    ui::HudWavePreview p;
    p.valid = true;
    p.wave_number = number;
    for (const ui::fmt::WaveFamilyCount& f : ui::fmt::summarize_wave(w)) {
        p.families.push_back(ui::HudFamilyCount{f.family, f.count});
        p.total += f.count;
    }
    for (const game::SpawnEntry& s : w.spawns) {
        if (s.elite_id != 0) p.elites += 1;
    }
    return p;
}

} // namespace

ui::HudModel make_hud_model(const HudSources& src, const ui::HudScreen& screen, Vec2 world_cursor) {
    ui::HudModel m;
    const sim::SimWorld& world = *src.world;
    const sim::SimSnapshot snap = world.snapshot();
    // The sim's objective starts at 100 on every level (SimWorld::reset).
    m.integrity01 = math::saturate(snap.objective_integrity / 100.0f);

    const game::EconomySnapshot econ = src.economy->snapshot();
    m.atp = econ.atp;
    m.income_per_second = econ.income_per_second;
    m.time_scale = src.time_scale;

    // ---- Waves ----
    const game::WaveStatus ws = src.waves->status();
    const std::vector<game::WaveDef>& table = src.waves->waves();
    m.phase = phase_of(ws.phase);
    m.all_waves_complete = ws.all_waves_complete;
    m.wave_count = static_cast<u32>(table.size());
    m.wave_number = ws.wave_index + 1;
    m.phase_time_remaining = ws.phase_time_remaining;
    // During prep the upcoming wave is the one to prepare for; once it is on,
    // the next one is.
    if (ws.phase == game::WavePhase::Prep && ws.wave_index < table.size()) {
        m.prep_total = table[ws.wave_index].prep_time;
        m.preview = preview_of(table[ws.wave_index], ws.wave_index + 1);
    } else if (const game::WaveDef* next = src.waves->next_wave()) {
        m.preview = preview_of(*next, ws.wave_index + 2);
    }
    if (src.level != nullptr && !src.level->spawn_points.empty()) {
        m.has_spawn = true;
        m.spawn_world = src.level->spawn_points.front().position;
    }

    // ---- Cards ----
    const game::TowerSystem& towers = *src.towers;
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        const auto t = static_cast<TowerType>(i);
        ui::HudTowerCard& c = m.cards[i];
        c.unlocked = towers.tower_unlocked(t);
        c.allowed = towers.tower_allowed(t);
        c.cost = towers.stats(t, 1).build_cost;
        c.range = game::tower_mechanics(t, 1).swarm.search_radius;
    }

    // ---- Board ----
    const auto& registry = world.ecs().registry();
    const f32 refund_fraction = game::tower_globals().refund_fraction;
    for (EntityId id : towers.placed_towers()) {
        const entt::entity e = world.ecs().from_id(id);
        if (!registry.valid(e)) continue;
        const auto* tower = registry.try_get<sim::comp::Tower>(e);
        const auto* tf = registry.try_get<sim::comp::Transform>(e);
        if (tower == nullptr || tf == nullptr) continue;
        ui::HudTower t;
        t.id = id;
        t.type = tower->type;
        t.world = tf->position;
        t.range = tower->range;
        if (const auto* hp = registry.try_get<sim::comp::Health>(e)) {
            t.health = hp->current;
            t.health_max = hp->max;
        } else {
            t.health = t.health_max = 1.0f;
        }
        // Towers have no in-run upgrades, so what was invested is the build
        // cost; TowerSystem::sell applies the same fraction to it.
        t.refund = static_cast<u32>(static_cast<f32>(towers.stats(tower->type, tower->tier).build_cost) *
                                    refund_fraction);
        m.towers.push_back(t);
    }

    // ---- Abilities ----
    const game::ActiveAbilitySystem& ab = *src.abilities;
    for (u32 i = 0; i < game::kAbilityCount; ++i) {
        const auto id = static_cast<game::AbilityId>(i);
        const game::AbilityStatus st = ab.status(id);
        ui::HudAbility& a = m.abilities[i];
        a.unlocked = ab.unlocked(id);
        a.ready = st.ready;
        a.cooldown_remaining = st.cooldown_remaining;
        a.cooldown_total = st.cooldown_total;
        a.needs_target = id != game::AbilityId::FeverResponse;
        a.radius = id == game::AbilityId::FibrinClot ? ab.def(id).barrier_half_length : ab.def(id).radius;
    }

    // ---- Placement preview ----
    if (screen.has_build_cursor()) {
        const game::PlacementQuery q = towers.validate(world, screen.build_cursor(), world_cursor, m.atp);
        m.placement.active = true;
        m.placement.valid = q.valid();
        m.placement.world = q.snapped_position;
    }
    return m;
}

game::GymResult run_ui_command(const UiDriver& d, const std::vector<std::string>& tok) {
    auto fail = [](std::string m) { return game::GymResult{false, std::move(m)}; };
    auto okay = [](std::string m) { return game::GymResult{true, std::move(m)}; };
    if (d.gui == nullptr) return fail("no game UI in this context");
    const std::string sub = tok.size() > 1 ? tok[1] : std::string("dump");

    if (sub == "dump") return okay(d.gui->dump());
    if (sub == "click" || sub == "hover") {
        if (tok.size() < 3) return fail("usage: ui " + sub + " <path>");
        const bool ok = sub == "click" ? d.gui->click(tok[2]) : d.gui->hover(tok[2]);
        if (!ok) return fail("no visible widget at '" + tok[2] + "' (try: ui dump)");
        return okay(sub + " " + tok[2]);
    }
    if (sub == "pointer") {
        if (d.pointer == nullptr) return fail("ui pointer only works in --screenshot --ui (use the mouse)");
        if (tok.size() < 4) return fail("usage: ui pointer <x> <y>   (logical px, 1920x1080 frame)");
        *d.pointer = Vec2{std::strtof(tok[2].c_str(), nullptr), std::strtof(tok[3].c_str(), nullptr)};
        return okay("pointer at " + tok[2] + "," + tok[3]);
    }
    if (sub == "select") {
        if (d.hud == nullptr || d.model == nullptr) return fail("no HUD in this context");
        const long n = tok.size() > 2 ? std::strtol(tok[2].c_str(), nullptr, 10) : 0;
        if (n < 1 || static_cast<usize>(n) > d.model->towers.size()) {
            return fail("usage: ui select <n>, 1.." + std::to_string(d.model->towers.size()));
        }
        d.hud->select(d.model->towers[static_cast<usize>(n - 1)].id);
        return okay("selected tower " + std::to_string(n));
    }
    if (sub == "cancel") {
        if (d.hud != nullptr) d.hud->cancel();
        return okay("cancelled");
    }
    return fail("unknown ui subcommand '" + sub + "'");
}

} // namespace immune::app
