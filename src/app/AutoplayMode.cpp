// app/AutoplayMode.cpp — assemble a level, hand it to the bot, record what
// happened. See AutoplayMode.h for the contract.
//
// WHY THE SETUP IS NOT build_world()
// Modes.cpp's build_world falls back to the built-in test level when --level
// is missing or unreadable, which is right for a benchmark (it just needs
// geometry) and wrong here: a balance report is *about* a named level, and one
// that silently describes a different level than its own header claims is
// worse than no report. So this path fails loudly instead, and keeps the
// LevelDef, which the bot's planner needs and build_world does not return.
#include "app/AutoplayMode.h"

#include "app/Modes.h"
#include "core/Log.h"
#include "game/abilities/AbilityConfigApply.h"
#include "game/autoplay/AutoPlayer.h"
#include "game/config/GameConfig.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/enemies/EnemyRoster.h"
#include "game/gym/GymCommands.h"
#include "game/level/Level.h"
#include "game/session/LevelSession.h"
#include "game/telemetry/RunTelemetry.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"

#include <cstdio>

namespace immune::app {
namespace {

/// Ticks a run is allowed before it is called at the tick limit. 30 simulated
/// minutes: comfortably longer than any authored level, short enough that a
/// bot which has stalled (nothing affordable, nothing dying) cannot hang a
/// sweep of a hundred runs.
constexpr u64 kDefaultMaxTicks = 108'000;

/// Everything a run owns, in one place so the caller is a straight line.
struct Run {
    std::unique_ptr<JobSystem> jobs;
    sim::SimWorld world;
    game::LevelDef level;
    game::LaneOwnershipMap lanes;
    game::TowerSystem towers;
    game::EnemyRoster enemies;
    game::WaveDirector waves;
    game::Economy economy;
    game::ActiveAbilitySystem abilities;
    game::GymSpawnQueue spawns;
    game::GymToggles toggles;
    game::AutoPlayer bot;
    game::RunTelemetry telemetry;
};

bool setup(Run& run, const Options& opt, const HeadlessConfig& tuning, std::string& error) {
    if (opt.level.empty()) {
        error = "--autoplay requires --level <path>";
        return false;
    }
    if (!platform::file_exists(opt.level)) {
        error = "level not found: " + opt.level;
        return false;
    }
    game::LevelLoader loader;
    const auto load = loader.load_file(opt.level, run.level);
    if (!load.ok) {
        error = "level load failed: " + load.error;
        return false;
    }

    game::apply_enemy_config(run.enemies, tuning.cfg.enemies);

    sim::SimDesc desc;
    desc.seed = opt.seed;
    // The horde cap has to clear the level's own peak or the report measures
    // the cap rather than the wave table.
    desc.max_chaff = 32768;
    desc.world_bounds = run.level.world_bounds;
    desc.max_damage_fields = tuning.cfg.sim.capacities.max_damage_fields;
    desc.max_projectiles = tuning.cfg.sim.capacities.max_projectiles;
    desc.max_swarmers = tuning.cfg.sim.capacities.max_swarmers;
    desc.max_fluid_particles = tuning.cfg.sim.capacities.max_fluid_particles;
    desc.max_combat_events = tuning.cfg.sim.capacities.max_combat_events;
    desc.max_chaff_death_events = tuning.cfg.sim.capacities.max_chaff_death_events;
    desc.fluid_tuning = tuning.cfg.sim.fluid;
    desc.spatial_cell_size = tuning.cfg.sim.globals.spatial_cell_size;
    desc.flow_rebake_budget_ms = tuning.cfg.sim.globals.flow_rebake_budget_ms;
    desc.flow_smoothing_radius = tuning.cfg.sim.globals.flow_smoothing_radius;
    desc.flow_wall_cost = tuning.cfg.sim.globals.flow_wall_cost;
    desc.flow_wall_falloff = tuning.cfg.sim.globals.flow_wall_falloff;
    desc.flow_wall_exponent = tuning.cfg.sim.globals.flow_wall_exponent;
    run.enemies.apply_to_tuning(desc.chaff_tuning);
    desc.chaff_tuning.max_replications_per_tick = tuning.cfg.sim.globals.max_replications_per_tick;
    desc.chaff_tuning.max_neighbors_sampled = tuning.cfg.sim.globals.max_neighbors_sampled;
    desc.chaff_tuning.ambient_drift = tuning.cfg.sim.globals.ambient_drift;
    if (run.level.ambient_drift != Vec2{0.0f, 0.0f}) {
        desc.chaff_tuning.ambient_drift = run.level.ambient_drift;
    }
    run.world.init(desc, run.jobs.get());

    const auto inst = loader.instantiate(run.level, run.world);
    if (!inst.ok) {
        error = "level instantiation failed: " + inst.error;
        return false;
    }
    run.lanes = loader.build_lane_ownership_map(run.level);

    run.towers.register_systems(run.world);
    run.enemies.register_systems(run.world);
    game::apply_tower_config(run.towers, tuning.cfg.towers);
    game::apply_ability_config(run.abilities, tuning.cfg.abilities);
    run.economy.configure(tuning.cfg.economy);
    run.economy.reset();
    run.waves.set_waves(run.level.waves);
    run.waves.start(run.world);
    // The real loss condition, always: a balance run that cannot lose is a
    // balance run that measures nothing.
    run.toggles = game::GymToggles{};
    run.toggles.hold_integrity = run.world.snapshot().objective_integrity;
    run.toggles.objective_invulnerable = false;
    return true;
}

} // namespace

int run_autoplay(const Options& opt) {
    const HeadlessConfig tuning = load_headless_config(opt);
    if (!tuning.ok) return 1;

    game::AutoPlayConfig bot_cfg;
    if (!game::parse_autoplay_profile(opt.profile, bot_cfg.profile, bot_cfg.single_type)) {
        IMMUNE_LOG_ERROR("unknown --profile '%s' (greedy-cheapest | spread-coverage | "
                         "save-for-tier3 | single-type:<tower>)",
                         opt.profile.c_str());
        return 1;
    }

    Run run;
    run.jobs = make_jobs(opt);
    std::string error;
    if (!setup(run, opt, tuning, error)) {
        IMMUNE_LOG_ERROR("autoplay setup failed: %s", error.c_str());
        return 1;
    }

    run.bot.configure(bot_cfg);
    run.bot.plan(run.level, run.lanes, run.world, run.towers, run.waves);
    run.telemetry.begin(run.world, run.level, run.lanes);

    game::LevelSystems systems;
    systems.world = &run.world;
    systems.towers = &run.towers;
    systems.enemies = &run.enemies;
    systems.waves = &run.waves;
    systems.economy = &run.economy;
    systems.abilities = &run.abilities;
    systems.spawns = &run.spawns;
    systems.toggles = &run.toggles;
    systems.survive_seconds = run.level.win.survive_seconds;

    const u64 max_ticks = opt.max_ticks == 0 ? kDefaultMaxTicks : opt.max_ticks;
    game::RunResult result = game::RunResult::TickLimit;

    for (u64 tick = 1; tick <= max_ticks; ++tick) {
        // The bot buys BETWEEN ticks, exactly where a player's intent is
        // applied in app/ -- never inside SimWorld::tick.
        const game::AutoPlayAction action = run.bot.tick(run.world, run.towers, run.economy, tick);
        switch (action.kind) {
            case game::AutoPlayAction::Kind::Placed:
                run.telemetry.on_tower_placed(action.tower, action.type, action.position,
                                              action.cost, tick);
                break;
            case game::AutoPlayAction::Kind::Upgraded:
                run.telemetry.on_tower_upgraded(action.tower, action.tier, action.cost, tick);
                break;
            case game::AutoPlayAction::Kind::None: break;
        }

        const game::SessionOutcome outcome = game::step_level(systems, nullptr);
        run.telemetry.sample(run.world, run.waves, run.economy);

        if (outcome == game::SessionOutcome::ObjectiveDestroyed) {
            result = game::RunResult::Lost;
            break;
        }
        if (outcome == game::SessionOutcome::Cleared) {
            result = game::RunResult::Cleared;
            break;
        }
    }
    run.telemetry.finish(result);
    run.telemetry.end(run.world);

    game::RunTelemetry::ReportHeader header;
    header.level_path = opt.level;
    header.level_name = run.level.name;
    header.profile = game::autoplay_profile_name(bot_cfg.profile, bot_cfg.single_type);
    header.seed = opt.seed;
    header.config_hash = std::to_string(tuning.hash);
    header.config_dir = resolve_config_dir(opt);

    const std::string report = run.telemetry.to_json(header);
    if (opt.report_path.empty()) {
        std::printf("%s\n", report.c_str());
        std::fflush(stdout);
    } else if (!platform::write_text_file(opt.report_path, report)) {
        IMMUNE_LOG_ERROR("could not write report to '%s'", opt.report_path.c_str());
        return 1;
    }

    // A one-line summary on stderr so a sweep's console is readable without
    // opening a hundred JSON files.
    const sim::SimSnapshot snap = run.world.snapshot();
    IMMUNE_LOG_INFO("autoplay %s: %s in %llu ticks, integrity %.1f, %u/%zu sites built",
                    header.profile.c_str(), game::run_result_name(result),
                    static_cast<unsigned long long>(snap.tick), snap.objective_integrity,
                    static_cast<u32>(run.telemetry.towers().size()), run.bot.sites().size());
    return 0;
}

} // namespace immune::app
