// game/config/GameConfig.cpp — sim/economy/abilities/meta schemas, the
// aggregate load/dump/bind, and the bootstrap defaults.
//
// default_game_config() deliberately reads most of its values back OUT of the
// live systems (TowerSystem::stats, EnemyRoster::load_defaults, render's
// family tables, ActiveAbilitySystem::load_defaults) rather than restating
// them. Those are the numbers the game actually runs on today, so the shipped
// JSON generated from this is correct by construction instead of by careful
// typing. Only values that live as file-static constants — the tower mechanism
// tables — are written out by hand here.
#include "game/config/Schemas.h"

#include "game/abilities/AbilityConfigApply.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/meta/MetaProgression.h"
#include "game/towers/TowerMechanics.h"
#include "render/ChaffBatcher.h"
#include "render/Renderer.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/chaff/HitFlash.h"

#include <array>

namespace immune::game::detail {
namespace {

using config::Field;
using config::FieldKind;
using config::Json;
using config::Schema;

IMMUNE_CONFIG_SCHEMA_ASSERT(SimCapacities);
IMMUNE_CONFIG_SCHEMA_ASSERT(SimGlobals);
IMMUNE_CONFIG_SCHEMA_ASSERT(SwarmerGlobals);
IMMUNE_CONFIG_SCHEMA_ASSERT(sim::FluidTuning);
IMMUNE_CONFIG_SCHEMA_ASSERT(EconomyConfig);
IMMUNE_CONFIG_SCHEMA_ASSERT(AbilityTuning);
IMMUNE_CONFIG_SCHEMA_ASSERT(MetaConfig);

constexpr Field kCapacityFields[] = {
    IMMUNE_CONFIG_FIELD(SimCapacities, max_chaff, FieldKind::U32, "Chaff agent ceiling; reserved once at level load"),
    IMMUNE_CONFIG_FIELD(SimCapacities, max_damage_fields, FieldKind::U32, ""),
    IMMUNE_CONFIG_FIELD(SimCapacities, max_projectiles, FieldKind::U32, "Live Gunner rounds"),
    IMMUNE_CONFIG_FIELD(SimCapacities, max_swarmers, FieldKind::U32, "Live Cytotoxic T granules"),
    IMMUNE_CONFIG_FIELD(SimCapacities, max_fluid_particles, FieldKind::U32, "Live Goblet Cell mucus particles"),
    IMMUNE_CONFIG_FIELD(SimCapacities, max_combat_events, FieldKind::U32, "Per-tick VFX event capacity"),
    IMMUNE_CONFIG_FIELD(SimCapacities, max_chaff_death_events, FieldKind::U32, "Chaff death bursts raised per tick, out of the budget above"),
};
constexpr Schema kCapacitySchema{"sim_capacities", kCapacityFields};

constexpr Field kSimGlobalsFields[] = {
    IMMUNE_CONFIG_FIELD(SimGlobals, spatial_cell_size, FieldKind::F32, "Spatial hash cell size; caps alignment_radius"),
    IMMUNE_CONFIG_FIELD(SimGlobals, flow_rebake_budget_ms, FieldKind::F64, "Milliseconds per frame for incremental rebakes"),
    IMMUNE_CONFIG_FIELD(SimGlobals, flow_rebake_margin_cells, FieldKind::U32, ""),
    IMMUNE_CONFIG_FIELD(SimGlobals, flow_smoothing_radius, FieldKind::F32, "World-space radius of flow-field direction smoothing (0 = exact shortest path)"),
    IMMUNE_CONFIG_FIELD(SimGlobals, flow_wall_cost, FieldKind::F32, "Extra traversal cost hard against a vessel wall (0 = tight corners)"),
    IMMUNE_CONFIG_FIELD(SimGlobals, flow_wall_falloff, FieldKind::F32, "Clearance in world units over which flow_wall_cost decays"),
    IMMUNE_CONFIG_FIELD(SimGlobals, flow_wall_exponent, FieldKind::F32, "Decay shape; higher keeps the wall cost in a thin layer at the lining"),
    IMMUNE_CONFIG_FIELD(SimGlobals, max_replications_per_tick, FieldKind::U32, "Global cap on replication spawns"),
    IMMUNE_CONFIG_FIELD(SimGlobals, max_neighbors_sampled, FieldKind::U32, "Neighbours one agent inspects per tick"),
    IMMUNE_CONFIG_FIELD(SimGlobals, slowed_speed_multiplier, FieldKind::F32, "Speed multiplier while slowed/frozen"),
    IMMUNE_CONFIG_FIELD(SimGlobals, ambient_drift, FieldKind::Vec2, "World 'wind' applied to drifting agents"),
    IMMUNE_CONFIG_FIELD(SimGlobals, damage_max_chain_links, FieldKind::U32, ""),
    IMMUNE_CONFIG_FIELD(SimGlobals, objective_damage_per_leak, FieldKind::F32, "Integrity lost per agent reaching the goal"),
};
constexpr Schema kSimGlobalsSchema{"sim_globals", kSimGlobalsFields};

constexpr Field kSwarmerFields[] = {
    IMMUNE_CONFIG_FIELD(SwarmerGlobals, damp, FieldKind::F32, "Velocity damping coefficient"),
    IMMUNE_CONFIG_FIELD(SwarmerGlobals, wander_gain, FieldKind::F32, "Wander strength while unattached"),
    IMMUNE_CONFIG_FIELD(SwarmerGlobals, ring_offset_fraction, FieldKind::F32, "Attach ring offset, as a fraction of attach_radius"),
    IMMUNE_CONFIG_FIELD(SwarmerGlobals, turn_gain, FieldKind::F32, "How sharply a granule turns toward its host"),
};
constexpr Schema kSwarmerSchema{"swarmers", kSwarmerFields};

// The fluid solver, one block for the whole world. These are physical
// constants, not balance numbers: `stiffness` and `near_stiffness` decide
// whether mucus behaves like water or like jelly, and the emission rate and
// drawn particle size both fall out of `rest_spacing`, so it is the one field
// here that cannot be moved without re-checking the look.
constexpr Field kFluidFields[] = {
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, smoothing_radius, FieldKind::F32, "SPH interaction radius h, world units"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, rest_spacing, FieldKind::F32, "Settled particle spacing; also sets emission rate and draw size"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, stiffness, FieldKind::F32, "Far-field pressure stiffness; signed, so it is also the surface tension"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, near_stiffness, FieldKind::F32, "Near-field stiffness; always repulsive, keeps the column from clumping"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, viscosity_linear, FieldKind::F32, "Linear viscosity impulse; water vs mucus"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, viscosity_quadratic, FieldKind::F32, "Quadratic viscosity impulse"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, drag, FieldKind::F32, "Ambient velocity damping, per second"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, wall_friction, FieldKind::F32, "Tangential velocity kept when running along tissue"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, chaff_drag, FieldKind::F32, "How hard a solid crowd brakes the jet, per second"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, chaff_full_occupancy, FieldKind::F32, "Agents in one spatial cell that count as a solid crowd"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, max_speed, FieldKind::F32, "Hard particle speed clamp; anti-tunnelling rail"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, substeps, FieldKind::U32, "Solver substeps per 60 Hz tick"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, coverage_cell_size, FieldKind::F32, "Damage/wetness grid cell size, world units"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, coverage_full, FieldKind::F32, "Particle mass in one cell that counts as fully soaked"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, splash_speed_threshold, FieldKind::F32, "Speed lost in one substep that counts as an impact"),
    IMMUNE_CONFIG_FIELD(sim::FluidTuning, max_splash_events, FieldKind::U32, "Cosmetic cap on FluidSplash events per tick"),
};
constexpr Schema kFluidSchema{"fluid", kFluidFields};

IMMUNE_CONFIG_SCHEMA_ASSERT(sim::SquadTuning);
constexpr Field kSquadFields[] = {
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, enabled, FieldKind::Bool, "Master switch; off reproduces the pre-squad horde exactly"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, target_squad_size, FieldKind::U32, "Agents per squad before the spawner opens a new one"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, max_squad_size, FieldKind::U32, "Membership ceiling; replicated daughters past it spawn independent (raised to target_squad_size if lower)"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, max_squads, FieldKind::U32, "Registry capacity; overflow spawns ungrouped rather than failing"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, follow_weight_max, FieldKind::F32, "Cap on the flow-vs-anchor blend; must stay below 1 so the flow field keeps a vote"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, follow_ramp, FieldKind::F32, "World units outside the squad radius over which the steer reaches its cap"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, lateral_push, FieldKind::F32, "Lateral impulse per tick at full weight; same units as separation_strength"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, squad_radius_scale, FieldKind::F32, "Squad radius = scale * sqrt(member count)"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, min_radius, FieldKind::F32, "Floor on the squad radius"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, anchor_lookahead, FieldKind::F32, "How far ahead of the centroid the anchor sits"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, anchor_max_speed, FieldKind::F32, "Leash rate limit on anchor advance, world units/second"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, project_window, FieldKind::F32, "Half-width of the windowed path projection search"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, foreign_radius_mult, FieldKind::F32, "Separation radius multiplier against other squads; bounded by the spatial cell size at init"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, foreign_strength_mult, FieldKind::F32, "Separation strength multiplier against other squads"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, path_lateral_jitter, FieldKind::F32, "Per-squad lateral offset as a fraction of path half-width"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, spawn_spacing, FieldKind::F32, "Minimum arc distance between two squads opening on the same path"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, retire_ticks, FieldKind::U32, "Consecutive empty ticks before a squad slot is recycled"),
    IMMUNE_CONFIG_FIELD(sim::SquadTuning, intake_distance, FieldKind::F32, "How far a squad may travel from its birthplace and still take new members"),
};
constexpr Schema kSquadSchema{"squads", kSquadFields};

constexpr Field kEconomyFields[] = {
    IMMUNE_CONFIG_FIELD(EconomyConfig, starting_atp, FieldKind::U32, "ATP at level start"),
    IMMUNE_CONFIG_FIELD(EconomyConfig, passive_income_per_second, FieldKind::F32, ""),
    IMMUNE_CONFIG_FIELD(EconomyConfig, atp_per_density, FieldKind::F32, "ATP per unit of chaff density destroyed"),
    IMMUNE_CONFIG_FIELD(EconomyConfig, refund_fraction, FieldKind::F32, "Fraction of invested ATP returned on sell"),
};
constexpr Schema kEconomySchema{"economy", kEconomyFields};

constexpr Field kAbilityFields[] = {
    IMMUNE_CONFIG_FIELD(AbilityTuning, cooldown_seconds, FieldKind::F32, ""),
    IMMUNE_CONFIG_FIELD(AbilityTuning, radius, FieldKind::F32, "Field radius (per-link for the cascade)"),
    IMMUNE_CONFIG_FIELD(AbilityTuning, kill_rate, FieldKind::F32, ""),
    IMMUNE_CONFIG_FIELD(AbilityTuning, field_duration, FieldKind::F32, "Histamine: nova lifetime; Clot: seconds the bar stands"),
    IMMUNE_CONFIG_FIELD(AbilityTuning, fever_cooldown_relief, FieldKind::F32, "Fever only: seconds shaved off every tower"),
    IMMUNE_CONFIG_FIELD(AbilityTuning, barrier_half_length, FieldKind::F32, "Clot only: half the bar's length across the lane"),
    IMMUNE_CONFIG_FIELD(AbilityTuning, barrier_half_width, FieldKind::F32, "Clot only: half the bar's thickness"),
};
constexpr Schema kAbilitySchema{"ability", kAbilityFields};

constexpr Field kMetaFields[] = {
    IMMUNE_CONFIG_FIELD(MetaConfig, base_run_reward, FieldKind::U32, "Flat, every run, even a wipe on wave 0"),
    IMMUNE_CONFIG_FIELD(MetaConfig, per_wave_reward, FieldKind::U32, "Per wave fully cleared"),
    IMMUNE_CONFIG_FIELD(MetaConfig, per_elite_reward, FieldKind::U32, ""),
    IMMUNE_CONFIG_FIELD(MetaConfig, per_boss_reward, FieldKind::U32, ""),
    IMMUNE_CONFIG_FIELD(MetaConfig, chaff_per_point, FieldKind::U32, "Chaff density killed per currency point"),
    IMMUNE_CONFIG_FIELD(MetaConfig, win_bonus, FieldKind::U32, "Flat bonus for a full clear"),
};
constexpr Schema kMetaSchema{"meta", kMetaFields};

constexpr std::string_view kSimKeys[] = {"schema",   "capacities", "globals",
                                        "swarmers", "fluid",      "squads"};
constexpr std::string_view kAbilitiesKeys[] = {"schema", "complement_cascade_burst",
                                               "histamine_flare", "fever_response",
                                               "fibrin_clot"};

const char* ability_key(AbilityId id) {
    switch (id) {
        case AbilityId::ComplementCascadeBurst: return "complement_cascade_burst";
        case AbilityId::HistamineFlare: return "histamine_flare";
        case AbilityId::FeverResponse: return "fever_response";
        case AbilityId::FibrinClot: return "fibrin_clot";
        case AbilityId::Count: break;
    }
    return "complement_cascade_burst";
}

} // namespace

void require_schema_version(const Json& doc, config::Ctx& ctx) {
    const u32 version = config::require_u32(doc, "schema", ctx);
    if (version != 1u) {
        ctx.fail("unsupported schema version " + std::to_string(version) + " (expected 1)");
    }
}

void write_schema_version(Json& doc) { doc["schema"] = 1; }

// --- sim.json ----------------------------------------------------------

void parse_sim(const Json& doc, SimConfig& out, config::Ctx& ctx) {
    require_schema_version(doc, ctx);
    config::reject_unknown_keys(doc, kSimKeys, ctx);
    {
        config::Ctx::Scope s(ctx, "capacities");
        config::parse_struct(config::require_object(doc, "capacities", ctx), kCapacitySchema,
                             &out.capacities, ctx);
    }
    {
        config::Ctx::Scope s(ctx, "globals");
        config::parse_struct(config::require_object(doc, "globals", ctx), kSimGlobalsSchema,
                             &out.globals, ctx);
    }
    {
        config::Ctx::Scope s(ctx, "swarmers");
        config::parse_struct(config::require_object(doc, "swarmers", ctx), kSwarmerSchema,
                             &out.swarmers, ctx);
    }
    {
        config::Ctx::Scope s(ctx, "fluid");
        config::parse_struct(config::require_object(doc, "fluid", ctx), kFluidSchema,
                             &out.fluid, ctx);
    }
    {
        config::Ctx::Scope s(ctx, "squads");
        config::parse_struct(config::require_object(doc, "squads", ctx), kSquadSchema,
                             &out.squads, ctx);
    }
}

Json dump_sim(const SimConfig& cfg) {
    Json doc = Json::object();
    write_schema_version(doc);
    Json capacities = Json::object();
    config::dump_struct(capacities, kCapacitySchema, &cfg.capacities);
    doc["capacities"] = std::move(capacities);
    Json globals = Json::object();
    config::dump_struct(globals, kSimGlobalsSchema, &cfg.globals);
    doc["globals"] = std::move(globals);
    Json swarmers = Json::object();
    config::dump_struct(swarmers, kSwarmerSchema, &cfg.swarmers);
    doc["swarmers"] = std::move(swarmers);
    Json fluid = Json::object();
    config::dump_struct(fluid, kFluidSchema, &cfg.fluid);
    doc["fluid"] = std::move(fluid);
    Json squads = Json::object();
    config::dump_struct(squads, kSquadSchema, &cfg.squads);
    doc["squads"] = std::move(squads);
    return doc;
}

void bind_sim(config::Registry& registry, SimConfig& cfg) {
    registry.bind("sim.capacities", kCapacitySchema, &cfg.capacities);
    registry.bind("sim.globals", kSimGlobalsSchema, &cfg.globals);
    registry.bind("sim.swarmers", kSwarmerSchema, &cfg.swarmers);
    registry.bind("sim.fluid", kFluidSchema, &cfg.fluid);
    registry.bind("sim.squads", kSquadSchema, &cfg.squads);
}

// --- economy.json ------------------------------------------------------

void parse_economy(const Json& doc, EconomyConfig& out, config::Ctx& ctx) {
    require_schema_version(doc, ctx);
    // The economy file is flat, so "schema" has to be allowed through
    // alongside the struct's own fields.
    std::vector<std::string_view> allowed{"schema"};
    for (const Field& f : kEconomySchema.fields) allowed.emplace_back(f.name);
    config::reject_unknown_keys(doc, allowed, ctx);
    for (const Field& f : kEconomySchema.fields) {
        auto* bytes = reinterpret_cast<u8*>(&out) + f.offset;
        if (f.kind == FieldKind::U32) {
            *reinterpret_cast<u32*>(bytes) = config::require_u32(doc, f.name, ctx);
        } else {
            *reinterpret_cast<f32*>(bytes) = config::require_f32(doc, f.name, ctx);
        }
    }
}

Json dump_economy(const EconomyConfig& cfg) {
    Json doc = Json::object();
    write_schema_version(doc);
    config::dump_struct(doc, kEconomySchema, &cfg);
    return doc;
}

void bind_economy(config::Registry& registry, EconomyConfig& cfg) {
    registry.bind("economy", kEconomySchema, &cfg);
}

// --- abilities.json ----------------------------------------------------

void parse_abilities(const Json& doc, AbilityConfig& out, config::Ctx& ctx) {
    require_schema_version(doc, ctx);
    config::reject_unknown_keys(doc, kAbilitiesKeys, ctx);
    for (u32 i = 0; i < kAbilityCount; ++i) {
        const char* key = ability_key(static_cast<AbilityId>(i));
        config::Ctx::Scope s(ctx, key);
        config::parse_struct(config::require_object(doc, key, ctx), kAbilitySchema, &out.ability[i],
                             ctx);
    }
}

Json dump_abilities(const AbilityConfig& cfg) {
    Json doc = Json::object();
    write_schema_version(doc);
    for (u32 i = 0; i < kAbilityCount; ++i) {
        Json entry = Json::object();
        config::dump_struct(entry, kAbilitySchema, &cfg.ability[i]);
        doc[ability_key(static_cast<AbilityId>(i))] = std::move(entry);
    }
    return doc;
}

void bind_abilities(config::Registry& registry, AbilityConfig& cfg) {
    for (u32 i = 0; i < kAbilityCount; ++i) {
        registry.bind(std::string("abilities.") + ability_key(static_cast<AbilityId>(i)),
                      kAbilitySchema, &cfg.ability[i]);
    }
}

// --- meta.json ---------------------------------------------------------

void parse_meta(const Json& doc, MetaConfig& out, config::Ctx& ctx) {
    require_schema_version(doc, ctx);
    std::vector<std::string_view> allowed{"schema"};
    for (const Field& f : kMetaSchema.fields) allowed.emplace_back(f.name);
    config::reject_unknown_keys(doc, allowed, ctx);
    for (const Field& f : kMetaSchema.fields) {
        *reinterpret_cast<u32*>(reinterpret_cast<u8*>(&out) + f.offset) =
            config::require_u32(doc, f.name, ctx);
    }
}

Json dump_meta(const MetaConfig& cfg) {
    Json doc = Json::object();
    write_schema_version(doc);
    config::dump_struct(doc, kMetaSchema, &cfg);
    return doc;
}

void bind_meta(config::Registry& registry, MetaConfig& cfg) {
    registry.bind("meta", kMetaSchema, &cfg);
}

} // namespace immune::game::detail

// ===========================================================================
// Aggregate
// ===========================================================================

namespace immune::game {
namespace {

/// Order is load order, dump order and report order. One list so they cannot
/// disagree about which file is which.
constexpr const char* kFileNames[] = {
    "towers.json", "enemies.json", "sim.json",
    "economy.json", "abilities.json", "meta.json",
};
constexpr usize kFileCount = sizeof(kFileNames) / sizeof(kFileNames[0]);

} // namespace

std::vector<std::string> config_file_names() {
    return std::vector<std::string>(kFileNames, kFileNames + kFileCount);
}

bool parse_game_config(const config::ConfigStore& store, GameConfig& out, std::string& err) {
    GameConfig staged;
    try {
        {
            config::Ctx ctx("towers.json");
            detail::parse_towers(store.file("towers.json"), staged.towers, ctx);
        }
        {
            config::Ctx ctx("enemies.json");
            detail::parse_enemies(store.file("enemies.json"), staged.enemies, ctx);
        }
        {
            config::Ctx ctx("sim.json");
            detail::parse_sim(store.file("sim.json"), staged.sim, ctx);
        }
        {
            config::Ctx ctx("economy.json");
            detail::parse_economy(store.file("economy.json"), staged.economy, ctx);
        }
        {
            config::Ctx ctx("abilities.json");
            detail::parse_abilities(store.file("abilities.json"), staged.abilities, ctx);
        }
        {
            config::Ctx ctx("meta.json");
            detail::parse_meta(store.file("meta.json"), staged.meta, ctx);
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }

    // refund_fraction exists in both economy.json (where it belongs) and the
    // tower globals (where the sell path reads it). economy.json wins, so the
    // two can never be edited into disagreement.
    staged.towers.globals.refund_fraction = staged.economy.refund_fraction;

    out = std::move(staged);
    return true;
}

bool load_game_config(config::ConfigStore& store, const std::string& dir, GameConfig& out,
                      std::string& err) {
    for (const std::string& name : config_file_names()) store.expect_file(name);
    const config::LoadResult result = store.load_dir(dir);
    if (!result.ok) {
        err = result.error;
        return false;
    }
    return parse_game_config(store, out, err);
}

std::vector<config::Json> dump_game_config(const GameConfig& cfg) {
    return {
        detail::dump_towers(cfg.towers),   detail::dump_enemies(cfg.enemies),
        detail::dump_sim(cfg.sim),         detail::dump_economy(cfg.economy),
        detail::dump_abilities(cfg.abilities), detail::dump_meta(cfg.meta),
    };
}

bool write_game_config(const GameConfig& cfg, const std::string& dir, std::string& err) {
    const std::vector<std::string> names = config_file_names();
    const std::vector<config::Json> docs = dump_game_config(cfg);
    for (usize i = 0; i < names.size(); ++i) {
        if (!config::ConfigStore::write_file(dir, names[i], docs[i], err)) return false;
    }
    return true;
}

void bind_game_config(config::Registry& registry, GameConfig& cfg) {
    registry.clear();
    detail::bind_towers(registry, cfg.towers);
    detail::bind_enemies(registry, cfg.enemies);
    detail::bind_sim(registry, cfg.sim);
    detail::bind_economy(registry, cfg.economy);
    detail::bind_abilities(registry, cfg.abilities);
    detail::bind_meta(registry, cfg.meta);
}

// ---------------------------------------------------------------------------
// Bootstrap defaults
// ---------------------------------------------------------------------------

namespace {

/// The mechanism table now lives in TowerSystem.cpp behind
/// game/towers/TowerMechanics.h, so the defaults are read back out of it
/// rather than restated here. Restating them was the one place in this file
/// where the bootstrap could have disagreed with the code it is bootstrapping.
void fill_default_mechanics(TowerConfig& towers) {
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        for (u32 tier = 0; tier < 3; ++tier) {
            towers.mechanics[t][tier] =
                tower_mechanics(static_cast<TowerType>(t), static_cast<u8>(tier + 1));
        }
    }
}

} // namespace

GameConfig default_game_config() {
    GameConfig cfg;

    // --- towers: read straight out of the live table -----------------------
    {
        TowerSystem towers;
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            for (u32 tier = 0; tier < 3; ++tier) {
                cfg.towers.stats[t][tier] =
                    towers.stats(static_cast<TowerType>(t), static_cast<u8>(tier + 1));
            }
        }
    }
    fill_default_mechanics(cfg.towers);
    cfg.towers.globals = tower_globals();
    cfg.towers.net = NetAbilityParams{3u,   0.5f, 1.5f, 0.3f, 3.0f,
                                      Vec4{0.3f, 0.6f, 1.0f, 1.0f},
                                      Vec4{0.2f, 0.8f, 0.9f, 0.5f}};

    // --- enemies: read out of the roster and the renderer's tables ---------
    {
        EnemyRoster roster;
        roster.load_defaults();
        sim::ChaffTuning tuning;
        roster.apply_to_tuning(tuning);

        for (u32 i = 0; i < kFamilyCount; ++i) {
            const auto family = static_cast<PathogenFamily>(i);
            const FamilyDef& def = roster.family(family);
            const render::FamilyVisual visual = render::family_visual(family);
            const sim::ChaffFamilyParams& p = tuning.family[i];
            FamilyConfig& fc = cfg.enemies.families[i];

            fc.speed_tier = def.speed_tier;
            fc.visual = FamilyVisualParams{visual.silhouette, visual.tempo, visual.wobble,
                                           render::family_color(family)};
            fc.behavior = FamilyBehaviorParams{def.base_density, def.replicates};

            // The death burst, read out of vfx/'s own table the same way the
            // silhouette is read out of render/'s. `color` is then overwritten
            // from the family colour: the two tables ship the same values, but
            // only one of them can be the source, and the body's colour is
            // obviously it. That makes "the death matches the enemy" a property
            // of the generator rather than of somebody remembering.
            fc.death_vfx = vfx::family_death_vfx(family);
            fc.death_vfx.color = render::family_color(family);

            // The hit flash, read out of sim/'s table the same way. Its colour
            // is NOT overwritten from the family colour the way the death
            // burst's is: a burst is made of the body coming apart, so it has
            // to be the body's colour, but a flash is a light landing ON the
            // body and reads as an event precisely because it is not.
            fc.hit_flash = sim::family_hit_flash(family);
            fc.replication_split = sim::family_replication_split(family);

            // The two size derivations come from the live enemy config so the
            // bootstrap cannot disagree with what apply_to_tuning() actually
            // applies; everything else is read back off the derived params.
            fc.chaff = enemy_config().families[i].chaff;
            fc.chaff.separation_strength = p.separation_strength;
            fc.chaff.alignment_radius = p.alignment_radius;
            fc.chaff.alignment_strength = p.alignment_strength;
            fc.chaff.pressure_threshold = p.pressure_threshold;
            fc.chaff.pressure_gain = p.pressure_gain;
            fc.chaff.pressure_max = p.pressure_max;
            fc.chaff.wall_restitution = p.wall_restitution;
            fc.chaff.wall_splash = p.wall_splash;
            fc.chaff.contact_spacing = p.contact_spacing;
            fc.chaff.contact_stiffness = p.contact_stiffness;
            fc.chaff.crowd_relief = p.crowd_relief;
            fc.chaff.drift_bias = p.drift_bias;
            fc.chaff.replication_rate = p.replication_rate;

            // Every speed tier is used by at least one family, so recovering
            // the four profiles from the per-family values costs nothing and
            // guarantees they match what the sim runs on today.
            SpeedProfileParams& profile = cfg.enemies.speed_tiers[static_cast<u32>(def.speed_tier)];
            profile = SpeedProfileParams{p.max_speed, p.acceleration, p.jitter};
        }

        cfg.enemies.base_attack = BaseAttackParams{0.2f, 0.4f, 10.0f, 3.0f, 10.0f, 0.6f};

        // Empty today: the roster ships no elites. The loop stays so a
        // redesigned elite reaches the shipped config the moment it is added.
        cfg.enemies.elites.clear();
        for (const EliteDef& def : roster.elites()) {
            EliteConfig ec;
            ec.id = def.id;
            ec.name = def.name;
            ec.family = def.family;
            ec.tier = def.tier;
            ec.stats = EliteStatsParams{def.max_health,      def.armor,
                                        def.speed,           def.sprite_size,
                                        def.ability_cooldown, def.telegraph_duration,
                                        def.atp_bounty};
            cfg.enemies.elites.push_back(std::move(ec));
        }
    }

    // --- sim: SimDesc/ChaffTuning defaults plus the loose literals ---------
    {
        const sim::SimDesc desc;
        cfg.sim.capacities = SimCapacities{
            static_cast<u32>(desc.max_chaff),           static_cast<u32>(desc.max_damage_fields),
            static_cast<u32>(desc.max_projectiles),     static_cast<u32>(desc.max_swarmers),
            static_cast<u32>(desc.max_fluid_particles), static_cast<u32>(desc.max_combat_events),
            static_cast<u32>(desc.max_chaff_death_events)};
        cfg.sim.globals.spatial_cell_size = desc.spatial_cell_size;
        cfg.sim.globals.flow_rebake_budget_ms = desc.flow_rebake_budget_ms;
        cfg.sim.globals.flow_rebake_margin_cells = 16u;
        cfg.sim.globals.flow_smoothing_radius = 2.0f;
        cfg.sim.globals.flow_wall_cost = desc.flow_wall_cost;
        cfg.sim.globals.flow_wall_falloff = desc.flow_wall_falloff;
        cfg.sim.globals.flow_wall_exponent = desc.flow_wall_exponent;
        cfg.sim.globals.max_replications_per_tick = desc.chaff_tuning.max_replications_per_tick;
        cfg.sim.globals.max_neighbors_sampled = desc.chaff_tuning.max_neighbors_sampled;
        cfg.sim.globals.slowed_speed_multiplier = 0.4f;
        cfg.sim.globals.ambient_drift = Vec2{0.5f, 0.28f};
        cfg.sim.globals.damage_max_chain_links = 8u;
        cfg.sim.globals.objective_damage_per_leak = 1.0f;
        cfg.sim.swarmers = SwarmerGlobals{1.5f, 0.45f, 0.6f, 9.0f};
        cfg.sim.fluid = desc.fluid_tuning;
        cfg.sim.squads = desc.squad_tuning;
    }

    cfg.economy = EconomyConfig{};

    cfg.abilities = ability_config();

    cfg.meta = MetaConfig{MetaProgression::kBaseRunReward, MetaProgression::kPerWaveReward,
                          MetaProgression::kPerEliteReward, MetaProgression::kPerBossReward,
                          MetaProgression::kChaffPerPoint, MetaProgression::kWinBonus};

    return cfg;
}

} // namespace immune::game
