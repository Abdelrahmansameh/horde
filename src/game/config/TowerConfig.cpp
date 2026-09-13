// game/config/TowerConfig.cpp — towers.json.
//
// The file is keyed by tower name and indexed by tier, mirroring
// TowerSystem::stats(type, tier). Each tier row carries a `stats` object (the
// frozen TowerStats fields) and a `mechanics` object whose shape is chosen by
// the tower's ROLE — a hydro row cannot carry a stale burst_radius, because
// the parser demands exactly the keys that role uses and rejects the rest.
#include "game/config/Schemas.h"

#include <array>

namespace immune::game {

TowerRole tower_role(TowerType type) {
    switch (type) {
        case TowerType::Neutrophil: return TowerRole::Gunner;
        case TowerType::Macrophage: return TowerRole::Mortar;
        case TowerType::Interferon: return TowerRole::Cryo;
        case TowerType::CytotoxicT: return TowerRole::Tesla;
        case TowerType::GobletCell: return TowerRole::Hydro;
        case TowerType::NKCell:     return TowerRole::Blade;
    }
    return TowerRole::Gunner;
}

const char* tower_role_name(TowerRole role) {
    switch (role) {
        case TowerRole::Gunner: return "gunner";
        case TowerRole::Mortar: return "mortar";
        case TowerRole::Cryo:   return "cryo";
        case TowerRole::Tesla:  return "tesla";
        case TowerRole::Hydro:  return "hydro";
        case TowerRole::Blade:  return "blade";
    }
    return "gunner";
}

} // namespace immune::game

namespace immune::game::detail {
namespace {

using config::Field;
using config::FieldKind;
using config::Json;
using config::Schema;

IMMUNE_CONFIG_SCHEMA_ASSERT(TowerStats);
IMMUNE_CONFIG_SCHEMA_ASSERT(GunnerParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(MortarParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(CryoParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(TeslaParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(HydroParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(BladeParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(NetAbilityParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(TowerGlobals);

constexpr Field kStatsFields[] = {
    IMMUNE_CONFIG_FIELD(TowerStats, range, FieldKind::F32, "Targeting radius, world units"),
    IMMUNE_CONFIG_FIELD(TowerStats, fire_interval, FieldKind::F32, "Seconds between shots"),
    IMMUNE_CONFIG_FIELD(TowerStats, damage, FieldKind::F32, "Per-shot damage to named agents"),
    IMMUNE_CONFIG_FIELD(TowerStats, kill_rate, FieldKind::F32, "Chaff density removed per second in-field"),
    IMMUNE_CONFIG_FIELD(TowerStats, footprint_radius, FieldKind::F32, "Body radius: tower spacing and sprite size; not an obstacle"),
    IMMUNE_CONFIG_FIELD(TowerStats, build_cost, FieldKind::U32, "ATP to place"),
    IMMUNE_CONFIG_FIELD(TowerStats, upgrade_cost, FieldKind::U32, "ATP to reach the next tier; 0 at max tier"),
    IMMUNE_CONFIG_FIELD(TowerStats, ability_cooldown, FieldKind::F32, "Seconds; 0 means no active ability"),
    IMMUNE_CONFIG_FIELD(TowerStats, family_mask, FieldKind::U8, "Bitmask of affectable pathogen families; 255 = all"),
};
constexpr Schema kStatsSchema{"tower_stats", kStatsFields};

constexpr Field kGunnerFields[] = {
    IMMUNE_CONFIG_FIELD(GunnerParams, round_speed, FieldKind::F32, "Muzzle velocity, units/sec"),
    IMMUNE_CONFIG_FIELD(GunnerParams, hit_radius, FieldKind::F32, "Projectile hit radius"),
    IMMUNE_CONFIG_FIELD(GunnerParams, spread, FieldKind::F32, "Muzzle spread half-angle, radians"),
    IMMUNE_CONFIG_FIELD(GunnerParams, muzzle_arc_radians, FieldKind::F32,
                        "Half-angle of the arc the spawn point slides along, radians"),
    IMMUNE_CONFIG_FIELD(GunnerParams, muzzle_radial_jitter, FieldKind::F32,
                        "Random in/out spawn offset along the standoff, world units"),
};
constexpr Schema kGunnerSchema{"gunner", kGunnerFields};

constexpr Field kMortarFields[] = {
    IMMUNE_CONFIG_FIELD(MortarParams, burst_seconds, FieldKind::F32, "Lifetime of the burst field"),
    IMMUNE_CONFIG_FIELD(MortarParams, burst_radius, FieldKind::F32, "Burst circle radius"),
    IMMUNE_CONFIG_FIELD(MortarParams, burst_falloff, FieldKind::F32, "Damage falloff toward the rim, 0..1"),
    IMMUNE_CONFIG_FIELD(MortarParams, marked_multiplier, FieldKind::F32, "Damage multiplier against marked agents"),
};
constexpr Schema kMortarSchema{"mortar", kMortarFields};

constexpr Field kCryoFields[] = {
    IMMUNE_CONFIG_FIELD(CryoParams, arc_radians, FieldKind::F32, "Cone half-angle"),
    IMMUNE_CONFIG_FIELD(CryoParams, inner_fraction, FieldKind::F32, "Fraction of reach counted as fully encased"),
    IMMUNE_CONFIG_FIELD(CryoParams, cone_falloff, FieldKind::F32, "Damage falloff across the cone"),
    IMMUNE_CONFIG_FIELD(CryoParams, max_freeze_events, FieldKind::U32, "Cosmetic cap on Freeze events per pulse"),
};
constexpr Schema kCryoSchema{"cryo", kCryoFields};

constexpr Field kTeslaFields[] = {
    IMMUNE_CONFIG_FIELD(TeslaParams, release_per_shot, FieldKind::U32, "Swarmers released per shot"),
    IMMUNE_CONFIG_FIELD(TeslaParams, swarmer_lifetime, FieldKind::F32, "Seconds before a granule dissolves"),
    IMMUNE_CONFIG_FIELD(TeslaParams, swarmer_speed, FieldKind::F32, "Granule travel speed"),
    IMMUNE_CONFIG_FIELD(TeslaParams, swarmer_dps, FieldKind::F32, "Density drained per second by one attached granule"),
    IMMUNE_CONFIG_FIELD(TeslaParams, attach_radius, FieldKind::F32, "How close a granule latches"),
    IMMUNE_CONFIG_FIELD(TeslaParams, search_radius, FieldKind::F32, "How far a loose granule looks for a host"),
    IMMUNE_CONFIG_FIELD(TeslaParams, launch_spread, FieldKind::F32, "Launch cone half-angle, radians"),
};
constexpr Schema kTeslaSchema{"tesla", kTeslaFields};

constexpr Field kHydroFields[] = {
    IMMUNE_CONFIG_FIELD(HydroParams, burst_seconds, FieldKind::F32, "How long one trigger pull keeps spraying"),
    IMMUNE_CONFIG_FIELD(HydroParams, jet_speed, FieldKind::F32, "Muzzle velocity of the jet, units/sec"),
    IMMUNE_CONFIG_FIELD(HydroParams, nozzle_radius, FieldKind::F32, "Half-width of the nozzle mouth; sets beam thickness and flow rate"),
    IMMUNE_CONFIG_FIELD(HydroParams, spread, FieldKind::F32, "Launch cone half-angle, radians"),
    IMMUNE_CONFIG_FIELD(HydroParams, droplet_lifetime, FieldKind::F32, "Seconds a droplet survives after leaving the cell"),
    IMMUNE_CONFIG_FIELD(HydroParams, flow_scale, FieldKind::F32, "Multiplier on the derived emission rate; 1 = rest density"),
    IMMUNE_CONFIG_FIELD(HydroParams, mark_seconds, FieldKind::F32, "Seconds a named agent stays weakened after this tower strikes it"),
};
constexpr Schema kHydroSchema{"hydro", kHydroFields};

constexpr Field kBladeFields[] = {
    IMMUNE_CONFIG_FIELD(BladeParams, spin_rad_per_sec, FieldKind::F32, "Rotor angular velocity"),
    IMMUNE_CONFIG_FIELD(BladeParams, rotor_falloff, FieldKind::F32, "Damage falloff across the rotor"),
    IMMUNE_CONFIG_FIELD(BladeParams, max_slash_events, FieldKind::U32, "Cosmetic cap on BladeSlash events per pulse"),
};
constexpr Schema kBladeSchema{"blade", kBladeFields};

constexpr Field kNetFields[] = {
    IMMUNE_CONFIG_FIELD(NetAbilityParams, micro_units, FieldKind::U32, "Micro-units spawned"),
    IMMUNE_CONFIG_FIELD(NetAbilityParams, spread_jitter, FieldKind::F32, "Launch angle jitter, radians"),
    IMMUNE_CONFIG_FIELD(NetAbilityParams, micro_lifetime, FieldKind::F32, "Micro-unit lifetime, seconds"),
    IMMUNE_CONFIG_FIELD(NetAbilityParams, micro_sprite_size, FieldKind::F32, ""),
    IMMUNE_CONFIG_FIELD(NetAbilityParams, net_duration, FieldKind::F32, "Seconds the slow zone persists"),
    IMMUNE_CONFIG_FIELD(NetAbilityParams, micro_tint, FieldKind::Vec4, "RGBA"),
    IMMUNE_CONFIG_FIELD(NetAbilityParams, net_tint, FieldKind::Vec4, "RGBA"),
};
constexpr Schema kNetSchema{"net_ability", kNetFields};

constexpr Field kGlobalsFields[] = {
    IMMUNE_CONFIG_FIELD(TowerGlobals, refund_fraction, FieldKind::F32, "Filled from economy.json; kept here for addressing"),
    IMMUNE_CONFIG_FIELD(TowerGlobals, shape_base, FieldKind::U32, "Base of the tower shape-id space"),
};
constexpr Schema kGlobalsSchema{"tower_globals", kGlobalsFields};

/// The mechanics schema and the sub-struct offset for one role. Selecting both
/// from the role is what keeps a tier row's mechanics object exactly the shape
/// its tower actually reads.
struct RoleBinding {
    const Schema* schema;
    usize offset;
};

RoleBinding role_binding(TowerRole role) {
    switch (role) {
        case TowerRole::Gunner: return {&kGunnerSchema, offsetof(TowerMechanics, gunner)};
        case TowerRole::Mortar: return {&kMortarSchema, offsetof(TowerMechanics, mortar)};
        case TowerRole::Cryo:   return {&kCryoSchema,   offsetof(TowerMechanics, cryo)};
        case TowerRole::Tesla:  return {&kTeslaSchema,  offsetof(TowerMechanics, tesla)};
        case TowerRole::Hydro:  return {&kHydroSchema,  offsetof(TowerMechanics, hydro)};
        case TowerRole::Blade:  return {&kBladeSchema,  offsetof(TowerMechanics, blade)};
    }
    return {&kGunnerSchema, offsetof(TowerMechanics, gunner)};
}

void* mechanics_arm(TowerMechanics& m, TowerRole role) {
    return reinterpret_cast<u8*>(&m) + role_binding(role).offset;
}

const void* mechanics_arm(const TowerMechanics& m, TowerRole role) {
    return reinterpret_cast<const u8*>(&m) + role_binding(role).offset;
}

constexpr std::string_view kTierRowKeys[] = {"stats", "mechanics"};
constexpr std::string_view kTowerEntryKeys[] = {"role", "tiers"};

} // namespace

void parse_towers(const Json& doc, TowerConfig& out, config::Ctx& ctx) {
    require_schema_version(doc, ctx);

    {
        config::Ctx::Scope scope(ctx, "globals");
        config::parse_struct(config::require_object(doc, "globals", ctx), kGlobalsSchema,
                             &out.globals, ctx);
    }
    {
        config::Ctx::Scope scope(ctx, "net_ability");
        config::parse_struct(config::require_object(doc, "net_ability", ctx), kNetSchema,
                             &out.net, ctx);
    }

    const Json& towers = config::require_object(doc, "towers", ctx);
    {
        config::Ctx::Scope scope(ctx, "towers");
        // Every tower must be present: an absent entry would silently keep
        // whatever happened to be in memory, which is exactly the failure mode
        // the authoritative-config decision exists to prevent.
        std::string_view names[kTowerTypeCount];
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            names[t] = tower_type_name(static_cast<TowerType>(t));
        }
        config::reject_unknown_keys(towers, names, ctx);

        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            const auto type = static_cast<TowerType>(t);
            const char* name = tower_type_name(type);
            const TowerRole role = tower_role(type);
            const RoleBinding binding = role_binding(role);

            config::Ctx::Scope tower_scope(ctx, name);
            const Json& entry = config::require_object(towers, name, ctx);
            config::reject_unknown_keys(entry, kTowerEntryKeys, ctx);

            // `role` is derived from the type, so it is validated rather than
            // read: it documents the file for a human without letting one
            // claim a tower is something the code cannot build.
            const std::string declared_role = config::require_string(entry, "role", ctx);
            if (declared_role != tower_role_name(role)) {
                ctx.fail("role is '" + declared_role + "' but " + name + " is a " +
                         tower_role_name(role) + " (role is fixed by the tower type)");
            }

            const Json& tiers = config::require_array(entry, "tiers", ctx);
            if (tiers.size() != 3) ctx.fail("'tiers' must have exactly 3 entries");

            for (u32 tier = 0; tier < 3; ++tier) {
                config::Ctx::Scope tier_scope(ctx, static_cast<usize>(tier));
                const Json& row = tiers.at(tier);
                if (!row.is_object()) ctx.fail("tier entry must be an object");
                config::reject_unknown_keys(row, kTierRowKeys, ctx);
                {
                    config::Ctx::Scope s(ctx, "stats");
                    config::parse_struct(config::require_object(row, "stats", ctx), kStatsSchema,
                                         &out.stats[t][tier], ctx);
                }
                {
                    config::Ctx::Scope s(ctx, "mechanics");
                    config::parse_struct(config::require_object(row, "mechanics", ctx),
                                         *binding.schema,
                                         mechanics_arm(out.mechanics[t][tier], role), ctx);
                }
            }
        }
    }
}

Json dump_towers(const TowerConfig& cfg) {
    Json doc = Json::object();
    write_schema_version(doc);

    Json globals = Json::object();
    config::dump_struct(globals, kGlobalsSchema, &cfg.globals);
    doc["globals"] = std::move(globals);

    Json towers = Json::object();
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        const TowerRole role = tower_role(type);
        const RoleBinding binding = role_binding(role);

        Json tiers = Json::array();
        for (u32 tier = 0; tier < 3; ++tier) {
            Json stats = Json::object();
            config::dump_struct(stats, kStatsSchema, &cfg.stats[t][tier]);
            Json mechanics = Json::object();
            config::dump_struct(mechanics, *binding.schema,
                                mechanics_arm(cfg.mechanics[t][tier], role));

            Json row = Json::object();
            row["stats"] = std::move(stats);
            row["mechanics"] = std::move(mechanics);
            tiers.push_back(std::move(row));
        }

        Json entry = Json::object();
        entry["role"] = tower_role_name(role);
        entry["tiers"] = std::move(tiers);
        towers[tower_type_name(type)] = std::move(entry);
    }
    doc["towers"] = std::move(towers);

    Json net = Json::object();
    config::dump_struct(net, kNetSchema, &cfg.net);
    doc["net_ability"] = std::move(net);

    return doc;
}

void bind_towers(config::Registry& registry, TowerConfig& cfg) {
    registry.bind("towers.globals", kGlobalsSchema, &cfg.globals);
    registry.bind("towers.net_ability", kNetSchema, &cfg.net);

    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        const TowerRole role = tower_role(type);
        const RoleBinding binding = role_binding(role);
        const std::string base = std::string("towers.") + tower_type_name(type) + ".";
        for (u32 tier = 0; tier < 3; ++tier) {
            // Tier is spelled 1..3 in a path, matching what the player and the
            // gym command see, not the 0-based array index.
            const std::string row = base + std::to_string(tier + 1) + ".";
            registry.bind(row + "stats", kStatsSchema, &cfg.stats[t][tier]);
            registry.bind(row + "mechanics", *binding.schema,
                          mechanics_arm(cfg.mechanics[t][tier], role));
        }
    }
}

} // namespace immune::game::detail
