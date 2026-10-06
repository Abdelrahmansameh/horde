// game/config/TowerConfig.cpp — towers.json.
//
// The file is keyed by tower name. Each tower carries a `stats` object (the
// TowerStats fields), a `swarm` object (the swarmer chassis every tower
// shares: speed, aggro and contact radii) and a
// `payload` object whose shape is chosen by the tower's KIND — a bomber
// cannot carry a stale dps, because the parser demands exactly the keys that
// kind uses and rejects the rest.
#include "game/config/Schemas.h"

#include <array>

namespace immune::game {

sim::SwarmerKind tower_kind(TowerType type) {
    switch (type) {
        case TowerType::Neutrophil: return sim::SwarmerKind::Shooter;
        case TowerType::Macrophage: return sim::SwarmerKind::ArborGrabber;
        case TowerType::CytotoxicT: return sim::SwarmerKind::Latch;
        case TowerType::GobletCell: return sim::SwarmerKind::MucusBomber;
        case TowerType::Fibroblast: return sim::SwarmerKind::Builder;
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
IMMUNE_CONFIG_SCHEMA_ASSERT(ArborGrabberParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(MucusBomberParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(BuilderParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(TowerGlobals);

constexpr Field kStatsFields[] = {
    IMMUNE_CONFIG_FIELD(TowerStats, fire_interval, FieldKind::F32, "Seconds between volleys; volleys never pause inside a round"),
    IMMUNE_CONFIG_FIELD(TowerStats, footprint_radius, FieldKind::F32, "Body radius: tower spacing and sprite size; not an obstacle"),
    IMMUNE_CONFIG_FIELD(TowerStats, build_cost, FieldKind::U32, "ATP to place"),
    IMMUNE_CONFIG_FIELD(TowerStats, family_mask, FieldKind::U8, "Bitmask of affectable pathogen families; 255 = all"),
    IMMUNE_CONFIG_FIELD(TowerStats, max_health, FieldKind::F32, "Integrity the horde has to chew through (viruses latch, bacteria burn)"),
};
constexpr Schema kStatsSchema{"tower_stats", kStatsFields};

constexpr Field kSwarmFields[] = {
    IMMUNE_CONFIG_FIELD(SwarmParams, release_per_shot, FieldKind::U32, "Swarmers released per volley"),
    IMMUNE_CONFIG_FIELD(SwarmParams, speed, FieldKind::F32, "Swarmer travel speed"),
    IMMUNE_CONFIG_FIELD(SwarmParams, search_radius, FieldKind::F32, "Aggro radius: how far a loose swarmer looks for a target; also how far the tower looks to face its volley"),
    IMMUNE_CONFIG_FIELD(SwarmParams, attach_radius, FieldKind::F32, "Contact radius; a shooter's standoff"),
    IMMUNE_CONFIG_FIELD(SwarmParams, launch_spread, FieldKind::F32, "Launch cone half-angle, radians"),
    IMMUNE_CONFIG_FIELD(SwarmParams, size, FieldKind::F32, "Body radius, world units: drawn size and wall clearance"),
    IMMUNE_CONFIG_FIELD(SwarmParams, max_health, FieldKind::F32, "Hit points a swarmer is released with; the horde drains them and the unit dissolves at zero"),
};
constexpr Schema kSwarmSchema{"swarm", kSwarmFields};

constexpr Field kLatchFields[] = {
    IMMUNE_CONFIG_FIELD(LatchParams, dps, FieldKind::F32, "Density drained per second by one attached swarmer"),
    IMMUNE_CONFIG_FIELD(LatchParams, attach_seconds, FieldKind::F32, "Seconds the entry into the host takes; the drain starts when it ends, 0 = instant"),
    IMMUNE_CONFIG_FIELD(LatchParams, attach_steps, FieldKind::U32, "Discrete lurches the entry movement is chopped into"),
};
constexpr Schema kLatchSchema{"latch", kLatchFields};

constexpr Field kShooterFields[] = {
    IMMUNE_CONFIG_FIELD(ShooterParams, fire_interval, FieldKind::F32, "Seconds between two rounds of one volley; 0 fires the whole magazine at once"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_damage, FieldKind::F32, "Density removed by one round; hit points off a named agent"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_speed, FieldKind::F32, "Muzzle velocity, units/sec"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_hit_radius, FieldKind::F32, "Projectile hit radius"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_spread, FieldKind::F32, "Per-round aim jitter half-angle, radians, on top of the volley cone"),
    IMMUNE_CONFIG_FIELD(ShooterParams, magazine_size, FieldKind::U32, "Rounds per volley, drawn as granules inside the cell"),
    IMMUNE_CONFIG_FIELD(ShooterParams, gather_seconds, FieldKind::F32, "Seconds the loaded rounds take to gather at the membrane facing the target"),
    IMMUNE_CONFIG_FIELD(ShooterParams, reload_seconds, FieldKind::F32, "Seconds from a volley's last round to a full magazine; rounds appear one by one across it"),
    IMMUNE_CONFIG_FIELD(ShooterParams, volley_cone, FieldKind::F32, "Half-angle, radians, of the cone a volley fans its rounds across"),
    IMMUNE_CONFIG_FIELD(ShooterParams, granule_size, FieldKind::F32, "Look: a round's radius inside the cell, as a fraction of the swarmer's size"),
    IMMUNE_CONFIG_FIELD(ShooterParams, magazine_spread, FieldKind::F32, "Look: radius of the clump the loaded rounds pack into around the cell's middle, as a fraction of the swarmer's size"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_size, FieldKind::F32, "Look: a round's radius in flight, as a fraction of the swarmer's size; it leaves at granule_size and grows to this"),
    IMMUNE_CONFIG_FIELD(ShooterParams, round_grow_seconds, FieldKind::F32, "Look: seconds a fired round takes to grow from granule_size to round_size; 0 = at once"),
    IMMUNE_CONFIG_FIELD(ShooterParams, spawn_seconds, FieldKind::F32, "Look: seconds a reloaded round's spawn pop lasts"),
    IMMUNE_CONFIG_FIELD(ShooterParams, spawn_start_scale, FieldKind::F32, "Look: scale a reloaded round pops in from, of its resting size"),
    IMMUNE_CONFIG_FIELD(ShooterParams, spawn_overshoot, FieldKind::F32, "Look: scale the spawn pop peaks at before settling to 1"),
    IMMUNE_CONFIG_FIELD(ShooterParams, spawn_peak, FieldKind::F32, "Look: fraction of spawn_seconds at which the pop peaks"),
    IMMUNE_CONFIG_FIELD(ShooterParams, spawn_ease_power, FieldKind::F32, "Look: ease-in exponent up to the peak; 1 = linear"),
    IMMUNE_CONFIG_FIELD(ShooterParams, formation_spacing, FieldKind::F32, "Distance between squad-mates along the rank"),
    IMMUNE_CONFIG_FIELD(ShooterParams, kite_fraction, FieldKind::F32, "Kite radius as a fraction of the standoff; an enemy inside it makes the shooter back away while firing (0 disables)"),
    IMMUNE_CONFIG_FIELD(ShooterParams, kite_flow_weight, FieldKind::F32, "How much the flow field bends the retreat toward where the horde is heading (0 = straight away from the threat)"),
    IMMUNE_CONFIG_FIELD(ShooterParams, kite_speed_mult, FieldKind::F32, "Retreat speed as a multiple of the swarm speed"),
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

constexpr Field kArborGrabberFields[] = {
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, arm_count, FieldKind::U32, "Independent pseudopod trees per unit; clamped to the renderer's three arm channels"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, max_captives, FieldKind::U32, "Maximum ordinary enemies one pseudopod swallows in a single pull, including its initial target"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, cluster_radius, FieldKind::F32, "Enemies this close to the initial target join its pull; 0 disables group grabs"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, extend_seconds, FieldKind::F32, "Seconds a new pseudopod tree takes to reach its target"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, latch_seconds, FieldKind::F32, "Seconds the terminal fingers spend closing around a target"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, pull_seconds, FieldKind::F32, "Seconds a latched target takes to reach the body"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, recover_seconds, FieldKind::F32, "Seconds the empty branch takes to melt back into the silhouette"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, fake_mass, FieldKind::F32, "Visual arm-volume compensation, 0 strict redistribution to 1 stable core"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, kite_fraction, FieldKind::F32, "Kite radius as a fraction of arm reach; 0 disables backing off"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, kite_flow_weight, FieldKind::F32, "How much the flow field bends the retreat; 0 = straight away"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, kite_speed_mult, FieldKind::F32, "Retreat speed as a multiple of swarm speed"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, wall_spacing, FieldKind::F32, "Most room between squad-mates along the lane wall; shrinks to fit the lane"),
    IMMUNE_CONFIG_FIELD(ArborGrabberParams, body_block, FieldKind::F32, "Share of a pathogen overlap the pathogen takes: 0 the unit yields, 1 an immovable wall"),
};
constexpr Schema kArborGrabberSchema{"arbor_grabber", kArborGrabberFields};

constexpr Field kMucusBomberFields[] = {
    IMMUNE_CONFIG_FIELD(MucusBomberParams, chase_seconds, FieldKind::F32, "Seconds a bomber chases one target before detonating where it is"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, droplets, FieldKind::U32, "Fluid particles one splash puts down"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, splash_radius, FieldKind::F32, "Radius the droplets fill at the instant of the splash"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, splash_speed, FieldKind::F32, "Outward launch speed of the rim droplets"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, droplet_lifetime, FieldKind::F32, "Seconds a droplet survives"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, slow_duration, FieldKind::F32, "Seconds an enemy stays slowed after leaving mucus"),
    IMMUNE_CONFIG_FIELD(MucusBomberParams, slow_factor, FieldKind::F32, "Max-speed multiplier while slowed; 0.1 = 10% speed"),
};
constexpr Schema kMucusBomberSchema{"mucus_bomber", kMucusBomberFields};

constexpr Field kBuilderFields[] = {
    IMMUNE_CONFIG_FIELD(BuilderParams, build_radius, FieldKind::F32, "How far from the tower a builder may be sent to lay a scar"),
    IMMUNE_CONFIG_FIELD(BuilderParams, build_min_radius, FieldKind::F32, "No scar site closer to the tower than this"),
    IMMUNE_CONFIG_FIELD(BuilderParams, build_min_clearance, FieldKind::F32, "SDF clearance a scar site needs, world units"),
    IMMUNE_CONFIG_FIELD(BuilderParams, build_candidates, FieldKind::U32, "Random sites tried per builder before the tower gives up on it"),
    IMMUNE_CONFIG_FIELD(BuilderParams, scar_half_length, FieldKind::F32, "Half-length of the scar along the wall (laid across the flow)"),
    IMMUNE_CONFIG_FIELD(BuilderParams, scar_half_width, FieldKind::F32, "Half-width of the scar: its thickness"),
    IMMUNE_CONFIG_FIELD(BuilderParams, scar_tilt, FieldKind::F32, "Max random tilt off square-to-the-flow, radians either way; 0 = every wall exactly across the arrows"),
    IMMUNE_CONFIG_FIELD(BuilderParams, scar_health, FieldKind::F32, "Integrity a fresh scar stands with; viruses latch on it, bacteria burn it"),
    IMMUNE_CONFIG_FIELD(BuilderParams, scar_reinforce, FieldKind::F32, "Hit points a builder adds to a scar in its way instead of starting a new one; 0 = it just dissolves"),
    IMMUNE_CONFIG_FIELD(BuilderParams, scar_spacing, FieldKind::F32, "No two scar centres closer than this; a builder arriving inside it reinforces"),
    IMMUNE_CONFIG_FIELD(BuilderParams, max_scars, FieldKind::U32, "Live scars one tower may own; 0 = unlimited; at the cap builders reinforce"),
    IMMUNE_CONFIG_FIELD(BuilderParams, scar_lifetime, FieldKind::F32, "Seconds a scar stands before dissolving on its own; 0 = permanent"),
    IMMUNE_CONFIG_FIELD(BuilderParams, crowd_push, FieldKind::F32, "Share of the crowd's shove a walking builder takes, 0..1; 0 crawls through the horde untouched"),
};
constexpr Schema kBuilderSchema{"builder", kBuilderFields};

constexpr Field kGlobalsFields[] = {
    IMMUNE_CONFIG_FIELD(TowerGlobals, refund_fraction, FieldKind::F32, "Filled from economy.json; kept here for addressing"),
    IMMUNE_CONFIG_FIELD(TowerGlobals, shape_base, FieldKind::U32, "Base of the tower shape-id space"),
    IMMUNE_CONFIG_FIELD(TowerGlobals, placement_interval, FieldKind::F32, "Seconds between cells placed while holding the mouse button"),
};
constexpr Schema kGlobalsSchema{"tower_globals", kGlobalsFields};

/// The payload schema and the sub-struct offset for one kind. Selecting both
/// from the kind is what keeps a tower's payload object exactly the shape
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
        case sim::SwarmerKind::MucusBomber: return {&kMucusBomberSchema, offsetof(TowerMechanics, mucus_bomber)};
        case sim::SwarmerKind::Builder:     return {&kBuilderSchema,     offsetof(TowerMechanics, builder)};
        case sim::SwarmerKind::ArborGrabber:return {&kArborGrabberSchema,offsetof(TowerMechanics, arbor_grabber)};
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

constexpr std::string_view kTowerEntryKeys[] = {"kind", "stats", "swarm", "payload"};

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

            {
                config::Ctx::Scope s(ctx, "stats");
                config::parse_struct(config::require_object(entry, "stats", ctx), kStatsSchema,
                                     &out.stats[t], ctx);
            }
            {
                config::Ctx::Scope s(ctx, "swarm");
                config::parse_struct(config::require_object(entry, "swarm", ctx), kSwarmSchema,
                                     &out.mechanics[t].swarm, ctx);
            }
            {
                config::Ctx::Scope s(ctx, "payload");
                config::parse_struct(config::require_object(entry, "payload", ctx),
                                     *binding.schema, payload_arm(out.mechanics[t], kind), ctx);
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

        Json stats = Json::object();
        config::dump_struct(stats, kStatsSchema, &cfg.stats[t]);
        Json swarm = Json::object();
        config::dump_struct(swarm, kSwarmSchema, &cfg.mechanics[t].swarm);
        Json payload = Json::object();
        config::dump_struct(payload, *binding.schema, payload_arm(cfg.mechanics[t], kind));

        Json entry = Json::object();
        entry["kind"] = tower_kind_name(kind);
        entry["stats"] = std::move(stats);
        entry["swarm"] = std::move(swarm);
        entry["payload"] = std::move(payload);
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
        registry.bind(base + "stats", kStatsSchema, &cfg.stats[t]);
        registry.bind(base + "swarm", kSwarmSchema, &cfg.mechanics[t].swarm);
        registry.bind(base + "payload", *binding.schema, payload_arm(cfg.mechanics[t], kind));
    }
}

} // namespace immune::game::detail
