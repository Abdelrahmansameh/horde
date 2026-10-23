#include "game/session/RunSetup.h"
#include "game/session/LevelSession.h"
#include "game/abilities/AbilityConfigApply.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/enemies/EnemyRoster.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/wave/WaveDirector.h"

namespace immune::game {
RunTuning make_run_tuning(const GameConfig& base, const MetaProgression& meta, bool sandbox) {
    RunTuning run;
    run.config = base;
    if (!sandbox) run.tree = meta.apply_to(run.config);
    else run.tree.immunity.leak_damage = base.sim.globals.objective_damage_per_leak;
    return run;
}

sim::SimDesc make_level_sim_desc(const GameConfig& base, const EnemyRoster& enemies,
                                 const LevelDef& level, u64 seed) {
    sim::SimDesc desc;
    desc.seed = seed;
    desc.world_bounds = level.world_bounds;
    desc.sim_bounds = level_sim_bounds(level);
    desc.max_chaff = base.sim.capacities.max_chaff;
    desc.max_damage_fields = base.sim.capacities.max_damage_fields;
    desc.max_projectiles = base.sim.capacities.max_projectiles;
    desc.max_swarmers = base.sim.capacities.max_swarmers;
    desc.max_fluid_particles = base.sim.capacities.max_fluid_particles;
    desc.max_combat_events = base.sim.capacities.max_combat_events;
    desc.max_chaff_death_events = base.sim.capacities.max_chaff_death_events;
    desc.fluid_tuning = base.sim.fluid;
    desc.squad_tuning = base.sim.squads;
    desc.swarmer_collision = base.sim.swarmer_collision;
    desc.hostile_tuning = hostile_tuning(base.sim.hostile);
    desc.burrow_tuning = burrow_tuning();
    desc.spatial_cell_size = base.sim.globals.spatial_cell_size;
    desc.flow_rebake_budget_ms = base.sim.globals.flow_rebake_budget_ms;
    desc.flow_smoothing_radius = base.sim.globals.flow_smoothing_radius;
    desc.flow_wall_cost = base.sim.globals.flow_wall_cost;
    desc.flow_wall_falloff = base.sim.globals.flow_wall_falloff;
    desc.flow_wall_exponent = base.sim.globals.flow_wall_exponent;
    enemies.apply_to_tuning(desc.chaff_tuning);
    desc.chaff_tuning.max_replications_per_tick = base.sim.globals.max_replications_per_tick;
    desc.chaff_tuning.max_neighbors_sampled = base.sim.globals.max_neighbors_sampled;
    desc.chaff_tuning.ambient_drift = level.ambient_drift != Vec2{0.0f, 0.0f}
        ? level.ambient_drift : base.sim.globals.ambient_drift;
    return desc;
}

void apply_run_tuning(const GameConfig& base, const RunTuning& run,
                      const LevelDef& level, sim::SimWorld& world,
                      TowerSystem& towers, EnemyRoster& enemies, Economy& economy,
                      ActiveAbilitySystem& abilities, bool sandbox) {
    apply_tower_config(towers, run.config.towers);
    apply_enemy_config(enemies, base.enemies);
    apply_ability_config(abilities, run.config.abilities);
    towers.set_unlocked_towers(sandbox ? TowerSystem::kAllTowersMask : run.tree.tower_mask);
    abilities.set_unlocked(sandbox ? ActiveAbilitySystem::kAllAbilitiesMask : run.tree.ability_mask);
    auto hostile = hostile_tuning(base.sim.hostile);
    hostile.damage_taken_mult = run.tree.hostile_damage_taken_mult;
    world.hostile().set_tuning(hostile);
    world.burrow().set_tuning(burrow_tuning());
    world.set_immunity(run.tree.immunity);
    const u32 bonus = run.config.economy.starting_atp > base.economy.starting_atp
        ? run.config.economy.starting_atp - base.economy.starting_atp : 0;
    apply_level_rules(level, run.config.economy, economy, towers, bonus);
}

RunResult collect_run_result(const sim::SimWorld& world, const WaveDirector& waves, bool won) {
    RunResult result;
    const auto status = waves.status();
    result.waves_cleared = status.all_waves_complete
        ? static_cast<u32>(waves.waves().size()) : status.wave_index;
    const auto snapshot = world.snapshot();
    for (u32 f = 0; f < kFamilyCount; ++f)
        result.chaff_killed_total += snapshot.chaff_killed_by_family[f];
    result.won = won;
    return result;
}
} // namespace immune::game
