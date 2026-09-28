#include "app/UiBridge.h"

#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/config/GameConfig.h"
#include "game/level/Level.h"
#include "game/meta/ImmunityTree.h"
#include "game/meta/MetaProgression.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "gui/core/Gui.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "ui/HudFormat.h"
#include "ui/front/FrontEnd.h"
#include "ui/hud/HudScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

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

/// Uniform Catmull-Rom through `pts` (endpoints clamped), `steps` samples per
/// segment -- the same curve the tissue rasterizer lays the lumen along.
std::vector<Vec2> sample_spline(const std::vector<Vec2>& pts, i32 steps) {
    std::vector<Vec2> out;
    const i32 n = static_cast<i32>(pts.size());
    if (n < 3) return pts;
    auto at = [&](i32 i) { return pts[static_cast<usize>(std::clamp(i, 0, n - 1))]; };
    for (i32 i = 0; i + 1 < n; ++i) {
        const Vec2 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        for (i32 k = 0; k < steps; ++k) {
            const f32 t = static_cast<f32>(k) / static_cast<f32>(steps);
            const f32 t2 = t * t, t3 = t2 * t;
            out.push_back((p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                           (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
                          0.5f);
        }
    }
    out.push_back(pts.back());
    return out;
}

/// Outline of a thick polyline: one side out, the other back.
std::vector<Vec2> ribbon(const std::vector<Vec2>& line, const std::vector<f32>& half_widths) {
    std::vector<Vec2> left, right;
    const usize n = line.size();
    for (usize i = 0; i < n; ++i) {
        const Vec2 d = math::normalize_safe(line[math::min(i + 1, n - 1)] - line[i > 0 ? i - 1 : 0]);
        const Vec2 nrm{-d.y, d.x};
        const f32 hw = half_widths[math::min(i, half_widths.size() - 1)];
        left.push_back(line[i] + nrm * hw);
        right.push_back(line[i] - nrm * hw);
    }
    std::reverse(right.begin(), right.end());
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

std::vector<Vec2> circle(Vec2 c, f32 r, i32 segments = 20) {
    std::vector<Vec2> out;
    for (i32 i = 0; i < segments; ++i) {
        const f32 a = math::kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
        out.push_back(c + Vec2{std::cos(a), std::sin(a)} * r);
    }
    return out;
}

} // namespace

ui::LevelThumb make_level_thumb(const game::LevelDef& def) {
    ui::LevelThumb thumb;
    Rect b = def.world_bounds;
    if (b.size().x <= 0.0f || b.size().y <= 0.0f) {
        b = Rect{Vec2{1e9f, 1e9f}, Vec2{-1e9f, -1e9f}};
        for (const game::Vessel& v : def.vessels) {
            for (const game::VesselPoint& p : v.points) {
                b.min = Vec2{math::min(b.min.x, p.position.x), math::min(b.min.y, p.position.y)};
                b.max = Vec2{math::max(b.max.x, p.position.x), math::max(b.max.y, p.position.y)};
            }
        }
        if (b.size().x <= 0.0f || b.size().y <= 0.0f) return thumb;
    }
    // Contain-fit the play area into the square, centred.
    const f32 span = math::max(b.size().x, b.size().y);
    const Vec2 c = b.center();
    auto norm = [&](Vec2 p) { return Vec2{0.5f + (p.x - c.x) / span, 0.5f - (p.y - c.y) / span}; };

    for (const game::Vessel& v : def.vessels) {
        if (v.points.empty()) continue;
        std::vector<Vec2> ctrl;
        f32 width = 0.0f;
        for (const game::VesselPoint& p : v.points) {
            ctrl.push_back(p.position);
            width += p.width;
        }
        ui::LevelThumb::Lane lane;
        for (Vec2 p : sample_spline(ctrl, 6)) lane.points.push_back(norm(p));
        // The world lumen includes the wall; the thumbnail draws the wall as
        // its own band around the lumen, so the lumen is drawn a little thinner.
        lane.width = width / static_cast<f32>(v.points.size()) / span * 0.83f;
        thumb.lanes.push_back(std::move(lane));
    }
    for (const game::ObstacleDef& o : def.obstacles) {
        ui::LevelThumb::Obstacle ob;
        std::vector<Vec2> world;
        switch (o.shape) {
            case game::ObstacleShape::Disc: world = circle(o.position, o.radius); break;
            case game::ObstacleShape::Box: {
                const Vec2 ax{std::cos(o.rotation), std::sin(o.rotation)};
                const Vec2 ay{-ax.y, ax.x};
                const Vec2 hx = ax * o.half_extents.x, hy = ay * o.half_extents.y;
                world = {o.position - hx - hy, o.position + hx - hy, o.position + hx + hy, o.position - hx + hy};
                break;
            }
            case game::ObstacleShape::Polygon:
                for (const game::VesselPoint& p : o.points) world.push_back(p.position);
                break;
            case game::ObstacleShape::Capsule:
                if (o.points.size() >= 2) {
                    world = ribbon({o.points[0].position, o.points[1].position}, {o.radius});
                }
                break;
            case game::ObstacleShape::Ridge: {
                std::vector<Vec2> ctrl;
                std::vector<f32> hw;
                for (const game::VesselPoint& p : o.points) ctrl.push_back(p.position);
                const std::vector<Vec2> line = sample_spline(ctrl, 6);
                for (usize i = 0; i < line.size(); ++i) {
                    const usize k = math::min(i / 6, o.points.size() - 1);
                    hw.push_back(o.points[k].width * 0.5f);
                }
                if (!line.empty()) world = ribbon(line, hw);
                break;
            }
        }
        if (world.size() < 3) continue;
        for (Vec2 p : world) ob.outline.push_back(norm(p));
        thumb.obstacles.push_back(std::move(ob));
    }
    for (const game::SpawnPoint& s : def.spawn_points) thumb.spawns.push_back(norm(s.position));
    return thumb;
}

bool is_campaign_level(const std::string& path) {
    return std::filesystem::path(path).filename().string().rfind("campaign_", 0) == 0;
}

std::vector<ui::CampaignLevel> make_campaign(const std::vector<ui::LevelEntry>& levels,
                                             const std::vector<game::LevelDef>& defs) {
    std::vector<usize> order;
    for (usize i = 0; i < levels.size(); ++i) {
        if (is_campaign_level(levels[i].path)) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](usize a, usize b) {
        return std::filesystem::path(levels[a].path).filename() < std::filesystem::path(levels[b].path).filename();
    });
    std::vector<ui::CampaignLevel> out;
    for (usize i : order) {
        ui::CampaignLevel c;
        c.number = static_cast<u32>(out.size() + 1);
        c.name = levels[i].display_name;
        c.level_index = i;
        if (i < defs.size()) c.thumb = make_level_thumb(defs[i]);
        out.push_back(std::move(c));
    }
    refresh_campaign(out, levels);
    return out;
}

void refresh_campaign(std::vector<ui::CampaignLevel>& campaign, const std::vector<ui::LevelEntry>& levels) {
    for (ui::CampaignLevel& c : campaign) {
        c.cleared = c.level_index < levels.size() && levels[c.level_index].cleared;
    }
    for (usize i = 0; i < campaign.size(); ++i) campaign[i].locked = !ui::campaign_unlocked(campaign, i);
}

ui::FrontModel make_screenshot_front_model(const game::LevelDef& level, const std::string& current_level_path) {
    std::vector<ui::LevelEntry> entries;
    std::vector<game::LevelDef> defs;
    for (const std::string& path : platform::list_files(platform::asset_path("levels"), ".json")) {
        if (!is_campaign_level(path)) continue;
        game::LevelLoader loader;
        game::LevelDef def;
        if (!loader.load_file(path, def).ok) continue;
        ui::LevelEntry e;
        e.path = path;
        e.level_id = def.name;
        e.display_name = !def.display_name.empty() ? def.display_name : def.name;
        entries.push_back(std::move(e));
        defs.push_back(std::move(def));
    }
    ui::FrontModel m;
    m.campaign = make_campaign(entries, defs);
    const std::string current = std::filesystem::path(current_level_path).filename().string();
    for (usize i = 0; i < m.campaign.size(); ++i) {
        if (std::filesystem::path(entries[m.campaign[i].level_index].path).filename().string() == current) {
            m.campaign_slot = static_cast<i32>(i);
        }
    }
    // Everything before the current level is cleared, so the map shows the
    // player partway through the campaign.
    for (i32 i = 0; i < m.campaign_slot; ++i) entries[m.campaign[static_cast<usize>(i)].level_index].cleared = true;
    refresh_campaign(m.campaign, entries);
    m.level_name = !level.display_name.empty() ? level.display_name : level.name;
    // The tree as the canvas's Tree artboard shows it: a few levels bought,
    // 486 Memory Cells and an Antibody to spend.
    {
        const game::MetaConfig cfg;
        using T = game::TreeNode;
        const T buys[] = {T::NeutrophilRoundDamage, T::NeutrophilRoundDamage, T::NeutrophilRoundDamage,
                          T::NeutrophilVolleyCadence, T::NeutrophilVolleyCadence, T::NeutrophilAggroRange,
                          T::NeutrophilHealth, T::NeutrophilHealth, T::CytotoxicRoot, T::CytotoxicDrain,
                          T::BoneMarrowReserve, T::BoneMarrowReserve, T::RapidMetabolism, T::CellularResilience,
                          T::HistamineUnlock, T::HistamineCooldown};
        game::MetaProgression rich;
        rich.reset_to_new_game();
        rich.credit(100000, 100);
        for (T b : buys) rich.purchase(b, cfg);
        game::MetaProgression meta;
        meta.reset_to_new_game();
        meta.credit(486 + rich.spent_memory_cells(), 1 + rich.spent_antibodies());
        for (T b : buys) meta.purchase(b, cfg);
        m.tree = make_tree_model(meta, cfg);
    }
    m.run.valid = true;
    m.run.memory_cells = 120;
    m.run.antibodies = 1;
    m.run.first_clear = true;
    m.unlocked_next = true;
    return m;
}

ui::TreeModel make_tree_model(const game::MetaProgression& meta, const game::MetaConfig& cfg) {
    using PR = game::MetaProgression::PurchaseResult;
    ui::TreeModel m;
    m.memory_cells = meta.memory_cells();
    m.antibodies = meta.antibodies();
    m.capstone_threshold = cfg.capstone_threshold;
    m.can_respec = meta.can_respec(cfg);
    m.respec_cost = cfg.respec_cost;
    for (usize b = 0; b < m.branch_points.size(); ++b) {
        m.branch_points[b] = meta.branch_points(static_cast<game::TreeBranch>(b + 1));
    }
    for (u32 i = 0; i < game::kTreeNodeCount; ++i) {
        const auto n = static_cast<game::TreeNode>(i);
        const game::TreeNodeDef& d = game::tree_node(n);
        ui::TreeNodeView v;
        v.key = d.key;
        v.node = i;
        v.name = d.name;
        v.effect = d.effect;
        v.level = meta.level(n);
        v.max_level = d.max_level;
        switch (d.kind) {
            case game::TreeNodeKind::TowerRoot: v.role = ui::TreeNodeRole::TowerRoot; break;
            case game::TreeNodeKind::AbilityRoot: v.role = ui::TreeNodeRole::AbilityRoot; break;
            case game::TreeNodeKind::Capstone: v.role = ui::TreeNodeRole::Capstone; break;
            default: v.role = ui::TreeNodeRole::Stat; break;
        }
        const PR r = meta.check_purchase(n, cfg);
        switch (r) {
            case PR::Ok: v.state = ui::TreeNodeState::Available; break;
            case PR::Maxed: v.state = ui::TreeNodeState::Maxed; break;
            case PR::Locked:
            case PR::BelowThreshold: v.state = ui::TreeNodeState::Locked; break;
            case PR::NeedMemoryCells:
            case PR::NeedAntibodies: v.state = ui::TreeNodeState::Short; break;
        }
        if (r == PR::BelowThreshold) {
            v.requirement = "Needs " + std::to_string(cfg.capstone_threshold) + " points in " +
                            game::branch_name(d.branch);
        } else if (r == PR::Locked) {
            const game::TreeNode root = d.ability != game::AbilityId::Count ? game::ability_root(d.ability)
                                                                             : game::tower_root(game::branch_tower(d.branch));
            v.requirement = std::string("Needs ") + game::tree_node(root).name;
        }
        if (v.state != ui::TreeNodeState::Maxed) {
            const game::TreeCost c = meta.next_cost(n, cfg);
            v.cost_memory = c.memory_cells;
            v.cost_antibodies = c.antibodies;
        }
        m.nodes.push_back(std::move(v));
    }
    return m;
}

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
    if (sub == "level") {
        if (d.front == nullptr) return fail("no level select in this context");
        const long n = tok.size() > 2 ? std::strtol(tok[2].c_str(), nullptr, 10) : 0;
        if (n < 1 || !d.front->select_level(static_cast<usize>(n - 1))) {
            return fail("usage: ui level <n>: an unlocked campaign level, 1-based");
        }
        return okay("selected level " + tok[2]);
    }
    if (sub == "screen") {
        if (d.screen == nullptr) return fail("ui screen only works in --screenshot --ui (the game's state decides)");
        const std::string name = tok.size() > 2 ? tok[2] : std::string();
        for (ui::FrontScreen s : {ui::FrontScreen::None, ui::FrontScreen::MainMenu, ui::FrontScreen::Tree,
                                  ui::FrontScreen::LevelSelect,
                                  ui::FrontScreen::Pause, ui::FrontScreen::Victory, ui::FrontScreen::Defeat}) {
            if (name == ui::front_screen_name(s)) {
                *d.screen = s;
                return okay("showing " + name);
            }
        }
        return fail("usage: ui screen none|menu|tree|levels|pause|victory|defeat");
    }
    return fail("unknown ui subcommand '" + sub + "'");
}

} // namespace immune::app
