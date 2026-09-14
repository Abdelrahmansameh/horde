// game/config/TowerConfig.cpp — towers.json.
//
// The file is keyed by tower name and indexed by tier, mirroring
// TowerSystem::stats(type, tier). Each tier row carries a `stats` object (the
// TowerStats fields), a `swarm` object (the swarmer chassis every tower
// shares: volley size, lifetime, speed, aggro and contact radii) and a
// `payload` object whose shape is chosen by the tower's KIND — a bomber row
// cannot carry a stale dps, because the parser demands exactly the keys that
// kind uses and rejects the rest.
#include "game/config/Schemas.h"

#include <array>

namespace immune::game {

sim::SwarmerKind tower_kind(TowerType type) {
    switch (type) {
        case TowerType::Neutrophil: return sim::SwarmerKind::Shooter;
        case TowerType::Macrophage: return sim::SwarmerKind::Bomber;
        case TowerType::Interferon: return sim::SwarmerKind::SlowBomber;
        case TowerType::CytotoxicT: return sim::SwarmerKind::Latch;
        case TowerType::GobletCell: return sim::SwarmerKind::MucusBomber;
        case TowerType::Count:      break;
    }
    return sim::SwarmerKind::Latch;
}

const char* tower_kind_name(sim::SwarmerKind kind) { return sim::swarmer_kind_name(kind); }

} // namespace immune::game

namespace immune::game::detail {
namespace {

using config::Field;
using config::FieldKind;
using config::Json;
using config::Schema;

IMMUNE_CONFIG_SCHEMA_ASSERT(TowerStats);
IMMUNE_CONFIG_SCHEMA_ASSERT(SwarmParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(LatchParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(ShooterParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(BomberParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(SlowBomberParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(MucusBomberParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(TowerGlobals);

constexpr Field kStatsFields[] = {
    IMMUNE_CONFIG_FIELD(TowerStats, fire_interval, FieldKind::F32, "Seconds between volleys; volleys never pause inside a round"),
    IMMUNE_CONFIG_FIELD(TowerStats, footprint_radius, FieldKind::F32, "Body radius: tower spacing and sprite size; not an obstacle"),
    IMMUNE_CONFIG_FIELD(TowerStats, build_cost, FieldKind::U32, "ATP to place"),
    IMMUNE_CONFIG_FIELD(TowerStats, upgrade_cost, FieldKind::U32, "ATP to reach the next tier; 0 at max tier"),
    IMMUNE_CONFIG_FIELD(TowerStats, family_mask, FieldKind::U8, "Bitmask of affectable pathogen families; 255 = all"),
};
constexpr Schema kStatsSchema{"tower_stats", kStatsFields};

constexpr Field kSwarmFields[] = {
    IMMUNE_CONFIG_FIELD(SwarmParams, release_per_shot, FieldKind::U32, "Swarmers released per volley"),
    IMMUNE_CONFIG_FIELD(SwarmParams, lifetime, FieldKind::F32, "Seconds before a swarmer retires (bombers detonate in place)"),
    IMMUNE_CONFIG_FIELD(SwarmParams, speed, FieldKind::F32, "Swarmer travel speed"),
    IMMUNE_CONFIG_FIELD(SwarmParams, search_radius, FieldKind::F32, "Aggro radius: how far a loose swarmer looks for a target; also how far the tower looks to face its volley"),
    IMMUNE_CONFIG_FIELD(SwarmParams, attach_radius, FieldKind::F32, "Contact radius; a shooter's standoff"),
    IMMUNE_CONFIG_FIELD(SwarmParams, launch_spread, FieldKind::F32, "Launch cone half-angle, radians"),
    IMMUNE_CONFIG_FIELD(SwarmParams, size, FieldKind::F32, "Body radius, world units: drawn size and wall clearance"),
};
constexpr Schema kSwarmSchema{"swarm", kSwarmFields};

constexpr Field kLatchFields[] = {
    IMMUNE_CONFIG_FIELD(LatchParams, dps, FieldKind::F32, "Density drained per second by one attached swarmer"),
};
constexpr Schema kLatchSchema{"latch", kLatchFields};

constexpr Field kShooterFields[] = {
    IMMUNE_CONFIG_FIELD(ShooterParams, fire_interval, FieldKind::F32, "Seconds between one swarmer's rounds"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_damage, FieldKind::F32, "Density removed by one round; hit points off a named agent"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_speed, FieldKind::F32, "Muzzle velocity, units/sec"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_hit_radius, FieldKind::F32, "Projectile hit radius"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_spread, FieldKind::F32, "Aim jitter half-angle, radians"),
    IMMUNE_CONFIG_FIELD(ShooterParams, formation_spacing, FieldKind::F32, "Distance between squad-mates along the rank"),
};
constexpr Schema kShooterSchema{"shooter", kShooterFields};

constexpr Field kBomberFields[] = {
    IMMUNE_CONFIG_FIELD(BomberParams, chase_seconds, FieldKind::F32, "Seconds a bomber chases one target before detonating where it is"),
    IMMUNE_CONFIG_FIELD(BomberParams, burst_radius, FieldKind::F32, "Burst circle radius"),
    IMMUNE_CONFIG_FIELD(BomberParams, burst_damage, FieldKind::F32, "Density an agent at the centre loses over the burst"),
    IMMUNE_CONFIG_FIELD(BomberParams, burst_seconds, FieldKind::F32, "Lifetime of the burst field"),
    IMMUNE_CONFIG_FIELD(BomberParams, burst_falloff, FieldKind::F32, "Damage falloff toward the rim, 0..1"),
    IMMUNE_CONFIG_FIELD(BomberParams, named_damage, FieldKind::F32, "Hit points off a named agent at the centre, before armor"),
};
constexpr Schema kBomberSchema{"bomber", kBomberFields};

constexpr Field kSlowBomberFields[] = {
    IMMUNE_CONFIG_FIELD(SlowBomberParams, chase_seconds, FieldKind::F32, "Seconds a bomber chases one target before detonating where it is"),
    IMMUNE_CONFIG_FIELD(SlowBomberParams, zone_radius, FieldKind::F32, "Slow circle radius"),
    IMMUNE_CONFIG_FIELD(SlowBomberParams, zone_duration, FieldKind::F32, "Seconds the circle stays on the ground"),
    IMMUNE_CONFIG_FIELD(SlowBomberParams, slow_duration, FieldKind::F32, "Seconds an agent stays slowed after leaving the circle"),
    IMMUNE_CONFIG_FIELD(SlowBomberParams, slow_factor, FieldKind::F32, "Max-speed multiplier while slowed; 0.4 = 40% speed"),
};
constexpr Schema kSlowBomberSchema{"slow_bomber", kSlowBomberFields};

constexpr Field kMucusBomberFields[] = {
    IMMUNE_CONFIG_FIELD(MucusBomberParams, chase_seconds, FieldKind::F32, "Seconds a bomber chases one target before detonating where it is"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, droplets, FieldKind::U32, "Fluid particles one splash puts down"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, splash_radius, FieldKind::F32, "Radius the droplets fill at the instant of the splash"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, splash_speed, FieldKind::F32, "Outward launch speed of the rim droplets"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, droplet_lifetime, FieldKind::F32, "Seconds a droplet survives"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, splash_dps, FieldKind::F32, "Density removed per second from a fully soaked coverage cell"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, mark_seconds, FieldKind::F32, "Seconds a named agent inside the splash stays weakened"),
};
constexpr Schema kMucusBomberSchema{"mucus_bomber", kMucusBomberFields};

constexpr Field kGlobalsFields[] = {
    IMMUNE_CONFIG_FIELD(TowerGlobals, refund_fraction, FieldKind::F32, "Filled from economy.json; kept here for addressing"),
    IMMUNE_CONFIG_FIELD(TowerGlobals, shape_base, FieldKind::U32, "Base of the tower shape-id space"),
};
constexpr Schema kGlobalsSchema{"tower_globals", kGlobalsFields};

/// The payload schema and the sub-struct offset for one kind. Selecting both
/// from the kind is what keeps a tier row's payload object exactly the shape
/// its tower actually reads.
struct KindBinding {
    const Schema* schema;
    usize offset;
};

KindBinding kind_binding(sim::SwarmerKind kind) {
    switch (kind) {
        case sim::SwarmerKind::Latch:       return {&kLatchSchema,      offsetof(TowerMechanics, latch)};
        case sim::SwarmerKind::Shooter:     return {&kShooterSchema,    offsetof(TowerMechanics, shooter)};
        case sim::SwarmerKind::Bomber:      return {&kBomberSchema,     offsetof(TowerMechanics, bomber)};
        case sim::SwarmerKind::SlowBomber:  return {&kSlowBomberSchema, offsetof(TowerMechanics, slow_bomber)};
        case sim::SwarmerKind::MucusBomber: return {&kMucusBomberSchema, offsetof(TowerMechanics, mucus_bomber)};
        case sim::SwarmerKind::Count:       break;
    }
    return {&kLatchSchema, offsetof(TowerMechanics, latch)};
}

void* payload_arm(TowerMechanics& m, sim::SwarmerKind kind) {
    return reinterpret_cast<u8*>(&m) + kind_binding(kind).offset;
}

const void* payload_arm(const TowerMechanics& m, sim::SwarmerKind kind) {
    return reinterpret_cast<const u8*>(&m) + kind_binding(kind).offset;
}

constexpr std::string_view kTierRowKeys[] = {"stats", "swarm", "payload"};
constexpr std::string_view kTowerEntryKeys[] = {"kind", "tiers"};

} // namespace

void parse_towers(const Json& doc, TowerConfig& out, config::Ctx& ctx) {
    require_schema_version(doc, ctx);

    {
        config::Ctx::Scope scope(ctx, "globals");
        config::parse_struct(config::require_object(doc, "globals", ctx), kGlobalsSchema,
                             &out.globals, ctx);
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
            const sim::SwarmerKind kind = tower_kind(type);
            const KindBinding binding = kind_binding(kind);

            config::Ctx::Scope tower_scope(ctx, name);
            const Json& entry = config::require_object(towers, name, ctx);
            config::reject_unknown_keys(entry, kTowerEntryKeys, ctx);

            // `kind` is derived from the type, so it is validated rather than
            // read: it documents the file for a human without letting one
            // claim a tower is something the code cannot build.
            const std::string declared_kind = config::require_string(entry, "kind", ctx);
            if (declared_kind != tower_kind_name(kind)) {
                ctx.fail("kind is '" + declared_kind + "' but " + name + " is a " +
                         tower_kind_name(kind) + " (kind is fixed by the tower type)");
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
                    config::Ctx::Scope s(ctx, "swarm");
                    config::parse_struct(config::require_object(row, "swarm", ctx), kSwarmSchema,
                                         &out.mechanics[t][tier].swarm, ctx);
                }
                {
                    config::Ctx::Scope s(ctx, "payload");
                    config::parse_struct(config::require_object(row, "payload", ctx),
                                         *binding.schema,
                                         payload_arm(out.mechanics[t][tier], kind), ctx);
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
        const sim::SwarmerKind kind = tower_kind(type);
        const KindBinding binding = kind_binding(kind);

        Json tiers = Json::array();
        for (u32 tier = 0; tier < 3; ++tier) {
            Json stats = Json::object();
            config::dump_struct(stats, kStatsSchema, &cfg.stats[t][tier]);
            Json swarm = Json::object();
            config::dump_struct(swarm, kSwarmSchema, &cfg.mechanics[t][tier].swarm);
            Json payload = Json::object();
            config::dump_struct(payload, *binding.schema,
                                payload_arm(cfg.mechanics[t][tier], kind));

            Json row = Json::object();
            row["stats"] = std::move(stats);
            row["swarm"] = std::move(swarm);
            row["payload"] = std::move(payload);
            tiers.push_back(std::move(row));
        }

        Json entry = Json::object();
        entry["kind"] = tower_kind_name(kind);
        entry["tiers"] = std::move(tiers);
        towers[tower_type_name(type)] = std::move(entry);
    }
    doc["towers"] = std::move(towers);

    return doc;
}

void bind_towers(config::Registry& registry, TowerConfig& cfg) {
    registry.bind("towers.globals", kGlobalsSchema, &cfg.globals);

    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        const sim::SwarmerKind kind = tower_kind(type);
        const KindBinding binding = kind_binding(kind);
        const std::string base = std::string("towers.") + tower_type_name(type) + ".";
        for (u32 tier = 0; tier < 3; ++tier) {
            // Tier is spelled 1..3 in a path, matching what the player and the
            // gym command see, not the 0-based array index.
            const std::string row = base + std::to_string(tier + 1) + ".";
            registry.bind(row + "stats", kStatsSchema, &cfg.stats[t][tier]);
            registry.bind(row + "swarm", kSwarmSchema, &cfg.mechanics[t][tier].swarm);
            registry.bind(row + "payload", *binding.schema,
                          payload_arm(cfg.mechanics[t][tier], kind));
        }
    }
}

} // namespace immune::game::detail
