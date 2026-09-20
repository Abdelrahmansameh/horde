// game/config/EnemyConfig.cpp — enemies.json.
//
// enemies.json is where "size", "health" and "speed" actually live. Size is
// deliberately a single number per family (`visual.silhouette`): the renderer
// draws it and ChaffFamilyParams::radius is derived from it, so what is drawn
// and what collides cannot drift apart.
#include "game/config/Schemas.h"

#include <algorithm>

namespace immune::game::detail {
namespace {

using config::Field;
using config::FieldKind;
using config::Json;
using config::Schema;

IMMUNE_CONFIG_SCHEMA_ASSERT(SpeedProfileParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(FamilyVisualParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(FamilyBehaviorParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(FamilyChaffParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(vfx::FamilyDeathVfx);
IMMUNE_CONFIG_SCHEMA_ASSERT(sim::HitFlashParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(sim::ReplicationSplitParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(render::LatchThrobParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(sim::HostileFamilyParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(BaseAttackParams);
IMMUNE_CONFIG_SCHEMA_ASSERT(EliteStatsParams);

constexpr Field kSpeedProfileFields[] = {
    IMMUNE_CONFIG_FIELD(SpeedProfileParams, max_speed, FieldKind::F32, "Top speed, world units/sec"),
    IMMUNE_CONFIG_FIELD(SpeedProfileParams, acceleration, FieldKind::F32, "How fast top speed is reached"),
    IMMUNE_CONFIG_FIELD(SpeedProfileParams, jitter, FieldKind::F32, "Random impulse magnitude; the 'alive' look"),
};
constexpr Schema kSpeedProfileSchema{"speed_profile", kSpeedProfileFields};

constexpr Field kVisualFields[] = {
    IMMUNE_CONFIG_FIELD(FamilyVisualParams, silhouette, FieldKind::F32, "Sprite diameter AND the source of collision radius"),
    IMMUNE_CONFIG_FIELD(FamilyVisualParams, tempo, FieldKind::F32, "Animation cycles per second"),
    IMMUNE_CONFIG_FIELD(FamilyVisualParams, wobble, FieldKind::F32, "SDF deformation amount"),
    IMMUNE_CONFIG_FIELD(FamilyVisualParams, color, FieldKind::Vec4, "RGBA; the family colour code"),
};
constexpr Schema kVisualSchema{"family_visual", kVisualFields};

constexpr Field kBehaviorFields[] = {
    IMMUNE_CONFIG_FIELD(FamilyBehaviorParams, base_density, FieldKind::F32, "Spawn density / HP contribution per agent"),
    IMMUNE_CONFIG_FIELD(FamilyBehaviorParams, replicates, FieldKind::Bool, "Virus: exponential pressure"),
};
constexpr Schema kBehaviorSchema{"family_behavior", kBehaviorFields};

constexpr Field kChaffFields[] = {
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, radius_from_silhouette, FieldKind::F32, "Collision radius = silhouette * this"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, separation_radius_mul, FieldKind::F32, "Separation radius = radius * this"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, separation_strength, FieldKind::F32, "How hard neighbours push apart"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, alignment_radius, FieldKind::F32, "Radius over which headings are averaged"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, alignment_strength, FieldKind::F32, "Makes a mass read as one moving body"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, pressure_threshold, FieldKind::F32, "Neighbour count above which a crowd is 'packed'"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, pressure_gain, FieldKind::F32, "Extra separation per neighbour past the threshold"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, pressure_max, FieldKind::F32, "Ceiling on the pressure multiplier"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, wall_restitution, FieldKind::F32, "Wall-normal bounce, 0..1"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, wall_splash, FieldKind::F32, "Blocked speed redirected along the wall, 0..1"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, contact_spacing, FieldKind::F32, "Min centre spacing as a multiple of radius"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, contact_stiffness, FieldKind::F32, "Overlap corrected per tick, 0..1"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, crowd_relief, FieldKind::F32, "Over-packed spread, radii per tick"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, drift_bias, FieldKind::F32, "How much ambient drift overrides flow"),
    IMMUNE_CONFIG_FIELD(FamilyChaffParams, replication_rate, FieldKind::F32, "Expected replications per agent per second"),
};
constexpr Schema kChaffSchema{"family_chaff", kChaffFields};

// The per-family death burst (vfx/DeathVfx.h). Sizes here are MULTIPLES OF THE
// AGENT'S BODY RADIUS, speeds are absolute world units/sec — see the header for
// why that split, and for what each knob buys.
constexpr Field kDeathVfxFields[] = {
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, enabled, FieldKind::Bool, "False draws no death burst at all for this family"),
    IMMUNE_CONFIG_ENUM_FIELD(vfx::FamilyDeathVfx, style, FieldKind::EnumU8, "Debris shape: 'burst' throws shards, 'lyse' throws globules", kDeathStyleEnum),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, color, FieldKind::Vec4, "RGBA; defaults to visual.color so the burst matches the body"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, scale, FieldKind::F32, "Master multiplier over every size in the burst"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, core_size, FieldKind::F32, "Core flash radius, x agent radius; 0 omits it"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, core_life, FieldKind::F32, "Core flash lifetime, seconds"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, core_white, FieldKind::F32, "How far the flash is pushed toward white, 0..1"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, ring_size, FieldKind::F32, "Shock ring FINAL radius, x agent radius"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, ring_life, FieldKind::F32, "Shock ring lifetime, seconds; 0 omits it"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, ring_alpha, FieldKind::F32, "Shock ring opacity"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_count, FieldKind::U32, "Debris pieces thrown"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_size_min, FieldKind::F32, "x agent radius"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_size_max, FieldKind::F32, "x agent radius"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_speed_min, FieldKind::F32, "World units/sec"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_speed_max, FieldKind::F32, "World units/sec"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_life_min, FieldKind::F32, "Seconds"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_life_max, FieldKind::F32, "Seconds"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_drag, FieldKind::F32, "Per-second velocity damping on the debris"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_buoyancy, FieldKind::F32, "+Y accel; negative makes debris fall"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_spin, FieldKind::F32, "Max |radians/sec|, signed per piece"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bit_scatter, FieldKind::F32, "0 = even radial spokes, 1 = uniform splatter"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bloom_count, FieldKind::U32, "Soft mist puffs left hanging"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bloom_size, FieldKind::F32, "x agent radius"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bloom_life, FieldKind::F32, "Seconds"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bloom_alpha, FieldKind::F32, "Mist opacity; keep low, a whole wave dies at once"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bloom_speed, FieldKind::F32, "World units/sec, radially outward"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, bloom_rise, FieldKind::F32, "+Y accel; positive drifts the mist up"),
    IMMUNE_CONFIG_FIELD(vfx::FamilyDeathVfx, inherit_velocity, FieldKind::F32, "Fraction of the agent's velocity the burst carries"),
};
constexpr Schema kDeathVfxSchema{"family_death_vfx", kDeathVfxFields};

// The per-family hit flash (sim/chaff/HitFlash.h). Every knob the effect has is
// here -- there is no second, hardcoded half of it anywhere -- so a family can
// go from a hard white pop to a slow crimson smoulder without a rebuild.
constexpr Field kHitFlashFields[] = {
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, enabled, FieldKind::Bool, "False shows no hit feedback at all for this family"),
    IMMUNE_CONFIG_ENUM_FIELD(sim::HitFlashParams, retrigger, FieldKind::EnumU8, "A second hit while flashing: 'highest', 'refresh' or 'accumulate'", kHitFlashRetriggerEnum),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, color, FieldKind::Vec4, "RGB the body flares toward; alpha is ignored (see strength)"),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, strength, FieldKind::F32, "Peak mix toward color, 0..1"),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, duration, FieldKind::F32, "Seconds to fade back; 0 disables the effect"),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, curve, FieldKind::F32, "Response exponent; >1 crushes weak continuous damage, <1 lifts it"),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, gain, FieldKind::F32, "Multiplier on the fraction of density one hit removed"),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, min_fraction, FieldKind::F32, "Hits removing less of the agent than this store nothing"),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, death_linger, FieldKind::F32, "Seconds a killed body keeps being drawn, white and fading; 0 is off"),
    IMMUNE_CONFIG_FIELD(sim::HitFlashParams, scale_punch, FieldKind::F32, "Extra sprite diameter at peak, as a fraction of silhouette; 0 is off"),
};
constexpr Schema kHitFlashSchema{"family_hit_flash", kHitFlashFields};

constexpr Field kReplicationSplitFields[] = {
    IMMUNE_CONFIG_FIELD(sim::ReplicationSplitParams, enabled, FieldKind::Bool, "False skips the parent-to-daughters split morph"),
    IMMUNE_CONFIG_FIELD(sim::ReplicationSplitParams, duration, FieldKind::F32, "Seconds from one parent shell to two complete daughters"),
    IMMUNE_CONFIG_FIELD(sim::ReplicationSplitParams, separation_distance, FieldKind::F32, "Final centre gap, as a multiple of contact spacing"),
    IMMUNE_CONFIG_FIELD(sim::ReplicationSplitParams, pull_ease, FieldKind::F32, ">1 delays separation; <1 pulls outward early"),
    IMMUNE_CONFIG_FIELD(sim::ReplicationSplitParams, reveal_distance, FieldKind::F32, "Local capsid distance that reveals each missing half"),
    IMMUNE_CONFIG_FIELD(sim::ReplicationSplitParams, reveal_ease, FieldKind::F32, ">1 holds the seam longer; <1 completes daughters early"),
    IMMUNE_CONFIG_FIELD(sim::ReplicationSplitParams, seam_softness, FieldKind::F32, "Local-SDF feathering for the split seam; 0 is hard"),
};
constexpr Schema kReplicationSplitSchema{"family_replication_split", kReplicationSplitFields};

// The feeding animation of a latched agent (render/LatchThrob.h). Amplitudes
// are in the sprite's local units, where the virus capsid has radius 0.36.
constexpr Field kLatchThrobFields[] = {
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, enabled, FieldKind::Bool, "False draws a latched agent exactly as a walking one"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, rate, FieldKind::F32, "Pump strokes per second (average; the rhythm is irregular)"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, throb, FieldKind::F32, "Whole-body breathing amplitude, local units"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, slosh, FieldKind::F32, "Volume shifted toward the host per stroke, local units"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, wave, FieldKind::F32, "Height of the peristaltic slugs rolling toward the host, local units"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, wave_count, FieldKind::F32, "Slugs per half-circumference; more is finer and faster"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, ripple, FieldKind::F32, "Fine skin shimmer between strokes, local units"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, squash, FieldKind::F32, "Bellows compression toward the host on the push, fraction of body length"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, probe, FieldKind::F32, "Length of the tube reaching into the host, local units; 0 draws none"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, stream, FieldKind::F32, "Brightness of the beaded cargo channel into the host, 0..1"),
    IMMUNE_CONFIG_FIELD(render::LatchThrobParams, glow, FieldKind::F32, "How much the push lights the whole body, fraction of its colour"),
};
constexpr Schema kLatchThrobSchema{"family_latch_throb", kLatchThrobFields};

// How the family hurts the player's cells (sim/hostile/HostileAttacks.h). A
// family may carry either attack or both; all zero is a harmless one.
constexpr Field kAttackFields[] = {
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_dps, FieldKind::F32, "Hit points/sec one latched agent takes off the tower or swarmer it rides; 0 = never latches"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_reach, FieldKind::F32, "Extra reach past body contact at which a latch happens, world units"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_cap_swarmer, FieldKind::U32, "Most agents of this family one swarmer carries at once"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_cap_tower, FieldKind::U32, "Most agents of this family one tower carries at once"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_cap_scar, FieldKind::U32, "Most agents of this family one collagen scar (Fibroblast wall) carries at once"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_speed, FieldKind::F32, "Lunge speed toward the spot on the host's membrane, world units/sec; full speed from the first tick"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_ease_distance, FieldKind::F32, "Distance from the spot inside which the lunge brakes, world units"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, latch_ease_power, FieldKind::F32, "Ease-out exponent: speed scales by (distance/ease_distance)^power; higher is a later, heavier brake"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, aura_dps, FieldKind::F32, "Hit points/sec dealt to every tower or swarmer whose body is inside the aura; 0 = no aura"),
    IMMUNE_CONFIG_FIELD(sim::HostileFamilyParams, aura_radius, FieldKind::F32, "Aura reach from the agent's centre to the victim's membrane, world units"),
};
constexpr Schema kAttackSchema{"family_attack", kAttackFields};

constexpr Field kBaseAttackFields[] = {
    IMMUNE_CONFIG_FIELD(BaseAttackParams, active, FieldKind::F32, "Seconds the strike is live"),
    IMMUNE_CONFIG_FIELD(BaseAttackParams, recovery, FieldKind::F32, "Seconds of recovery after a strike"),
    IMMUNE_CONFIG_FIELD(BaseAttackParams, range, FieldKind::F32, "Distance at which an attack starts"),
    IMMUNE_CONFIG_FIELD(BaseAttackParams, radius, FieldKind::F32, "Strike radius"),
    IMMUNE_CONFIG_FIELD(BaseAttackParams, damage, FieldKind::F32, "Damage per strike"),
    IMMUNE_CONFIG_FIELD(BaseAttackParams, death_fade, FieldKind::F32, "Seconds a corpse takes to fade"),
};
constexpr Schema kBaseAttackSchema{"base_attack", kBaseAttackFields};

constexpr Field kEliteStatsFields[] = {
    IMMUNE_CONFIG_FIELD(EliteStatsParams, max_health, FieldKind::F32, ""),
    IMMUNE_CONFIG_FIELD(EliteStatsParams, armor, FieldKind::F32, "Flat damage reduction per hit"),
    IMMUNE_CONFIG_FIELD(EliteStatsParams, speed, FieldKind::F32, "World units/sec; 0 for a stationary boss"),
    IMMUNE_CONFIG_FIELD(EliteStatsParams, sprite_size, FieldKind::F32, "Silhouette size; encodes threat tier"),
    IMMUNE_CONFIG_FIELD(EliteStatsParams, ability_cooldown, FieldKind::F32, ""),
    IMMUNE_CONFIG_FIELD(EliteStatsParams, telegraph_duration, FieldKind::F32, "Windup the player gets to react to"),
    IMMUNE_CONFIG_FIELD(EliteStatsParams, atp_bounty, FieldKind::U32, ""),
};
constexpr Schema kEliteStatsSchema{"elite_stats", kEliteStatsFields};

const char* family_key(PathogenFamily f) {
    switch (f) {
        case PathogenFamily::Virus: return "virus";
        case PathogenFamily::Bacteria: return "bacteria";
        case PathogenFamily::Count: break;
    }
    return "virus";
}

const char* speed_tier_key(SpeedTier t) {
    switch (t) {
        case SpeedTier::Slow: return "slow";
        case SpeedTier::Normal: return "normal";
        case SpeedTier::Fast: return "fast";
        case SpeedTier::Erratic: return "erratic";
    }
    return "normal";
}

constexpr std::string_view kFamilyEntryKeys[] = {"speed_tier", "visual", "behavior", "chaff",
                                                 "death_vfx", "hit_flash", "replication_split",
                                                 "latch_throb", "attack"};
constexpr std::string_view kEliteEntryKeys[] = {"id", "name", "family", "tier", "stats"};

} // namespace

// ---------------------------------------------------------------------------
// enemies.json
// ---------------------------------------------------------------------------

void parse_enemies(const Json& doc, EnemyConfig& out, config::Ctx& ctx) {
    require_schema_version(doc, ctx);

    {
        config::Ctx::Scope scope(ctx, "speed_tiers");
        const Json& tiers = config::require_object(doc, "speed_tiers", ctx);
        std::string_view keys[4];
        for (u32 i = 0; i < 4; ++i) keys[i] = speed_tier_key(static_cast<SpeedTier>(i));
        config::reject_unknown_keys(tiers, keys, ctx);
        for (u32 i = 0; i < 4; ++i) {
            const char* key = speed_tier_key(static_cast<SpeedTier>(i));
            config::Ctx::Scope s(ctx, key);
            config::parse_struct(config::require_object(tiers, key, ctx), kSpeedProfileSchema,
                                 &out.speed_tiers[i], ctx);
        }
    }

    {
        config::Ctx::Scope scope(ctx, "families");
        const Json& families = config::require_object(doc, "families", ctx);
        std::string_view keys[kFamilyCount];
        for (u32 i = 0; i < kFamilyCount; ++i) keys[i] = family_key(static_cast<PathogenFamily>(i));
        config::reject_unknown_keys(families, keys, ctx);

        for (u32 i = 0; i < kFamilyCount; ++i) {
            const char* key = family_key(static_cast<PathogenFamily>(i));
            config::Ctx::Scope s(ctx, key);
            const Json& entry = config::require_object(families, key, ctx);
            config::reject_unknown_keys(entry, kFamilyEntryKeys, ctx);

            FamilyConfig& fc = out.families[i];
            fc.speed_tier = static_cast<SpeedTier>(
                config::require_enum(entry, "speed_tier", speed_tier_enum(), ctx));
            {
                config::Ctx::Scope v(ctx, "visual");
                config::parse_struct(config::require_object(entry, "visual", ctx), kVisualSchema,
                                     &fc.visual, ctx);
            }
            {
                config::Ctx::Scope b(ctx, "behavior");
                config::parse_struct(config::require_object(entry, "behavior", ctx),
                                     kBehaviorSchema, &fc.behavior, ctx);
            }
            {
                config::Ctx::Scope c(ctx, "chaff");
                config::parse_struct(config::require_object(entry, "chaff", ctx), kChaffSchema,
                                     &fc.chaff, ctx);
            }
            {
                config::Ctx::Scope d(ctx, "death_vfx");
                config::parse_struct(config::require_object(entry, "death_vfx", ctx),
                                     kDeathVfxSchema, &fc.death_vfx, ctx);
            }
            {
                config::Ctx::Scope h(ctx, "hit_flash");
                config::parse_struct(config::require_object(entry, "hit_flash", ctx),
                                     kHitFlashSchema, &fc.hit_flash, ctx);
            }
            {
                config::Ctx::Scope r(ctx, "replication_split");
                config::parse_struct(config::require_object(entry, "replication_split", ctx),
                                     kReplicationSplitSchema, &fc.replication_split, ctx);
            }
            {
                config::Ctx::Scope l(ctx, "latch_throb");
                config::parse_struct(config::require_object(entry, "latch_throb", ctx),
                                     kLatchThrobSchema, &fc.latch_throb, ctx);
            }
            {
                config::Ctx::Scope a(ctx, "attack");
                config::parse_struct(config::require_object(entry, "attack", ctx), kAttackSchema,
                                     &fc.attack, ctx);
            }
        }
    }

    {
        config::Ctx::Scope scope(ctx, "base_attack");
        config::parse_struct(config::require_object(doc, "base_attack", ctx), kBaseAttackSchema,
                             &out.base_attack, ctx);
    }
    {
        config::Ctx::Scope scope(ctx, "elites");
        const Json& elites = config::require_array(doc, "elites", ctx);
        out.elites.clear();
        out.elites.reserve(elites.size());
        for (usize i = 0; i < elites.size(); ++i) {
            config::Ctx::Scope s(ctx, i);
            const Json& entry = elites.at(i);
            if (!entry.is_object()) ctx.fail("elite entry must be an object");
            config::reject_unknown_keys(entry, kEliteEntryKeys, ctx);

            EliteConfig ec;
            ec.id = static_cast<u16>(config::require_u32(entry, "id", ctx));
            ec.name = config::require_string(entry, "name", ctx);
            ec.family = static_cast<PathogenFamily>(
                config::require_enum(entry, "family", family_enum(), ctx));
            ec.tier = static_cast<ThreatTier>(
                config::require_enum(entry, "tier", threat_tier_enum(), ctx));
            {
                config::Ctx::Scope st(ctx, "stats");
                config::parse_struct(config::require_object(entry, "stats", ctx), kEliteStatsSchema,
                                     &ec.stats, ctx);
            }
            out.elites.push_back(std::move(ec));
        }
    }
}

Json dump_enemies(const EnemyConfig& cfg) {
    Json doc = Json::object();
    write_schema_version(doc);

    Json tiers = Json::object();
    for (u32 i = 0; i < 4; ++i) {
        Json profile = Json::object();
        config::dump_struct(profile, kSpeedProfileSchema, &cfg.speed_tiers[i]);
        tiers[speed_tier_key(static_cast<SpeedTier>(i))] = std::move(profile);
    }
    doc["speed_tiers"] = std::move(tiers);

    Json families = Json::object();
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const FamilyConfig& fc = cfg.families[i];
        Json entry = Json::object();
        entry["speed_tier"] = speed_tier_key(fc.speed_tier);
        Json visual = Json::object();
        config::dump_struct(visual, kVisualSchema, &fc.visual);
        entry["visual"] = std::move(visual);
        Json behavior = Json::object();
        config::dump_struct(behavior, kBehaviorSchema, &fc.behavior);
        entry["behavior"] = std::move(behavior);
        Json chaff = Json::object();
        config::dump_struct(chaff, kChaffSchema, &fc.chaff);
        entry["chaff"] = std::move(chaff);
        Json death_vfx = Json::object();
        config::dump_struct(death_vfx, kDeathVfxSchema, &fc.death_vfx);
        entry["death_vfx"] = std::move(death_vfx);
        Json hit_flash = Json::object();
        config::dump_struct(hit_flash, kHitFlashSchema, &fc.hit_flash);
        entry["hit_flash"] = std::move(hit_flash);
        Json replication_split = Json::object();
        config::dump_struct(replication_split, kReplicationSplitSchema, &fc.replication_split);
        entry["replication_split"] = std::move(replication_split);
        Json latch_throb = Json::object();
        config::dump_struct(latch_throb, kLatchThrobSchema, &fc.latch_throb);
        entry["latch_throb"] = std::move(latch_throb);
        Json attack = Json::object();
        config::dump_struct(attack, kAttackSchema, &fc.attack);
        entry["attack"] = std::move(attack);
        families[family_key(static_cast<PathogenFamily>(i))] = std::move(entry);
    }
    doc["families"] = std::move(families);

    Json base_attack = Json::object();
    config::dump_struct(base_attack, kBaseAttackSchema, &cfg.base_attack);
    doc["base_attack"] = std::move(base_attack);

    Json elites = Json::array();
    for (const EliteConfig& ec : cfg.elites) {
        Json entry = Json::object();
        entry["id"] = ec.id;
        entry["name"] = ec.name;
        entry["family"] = family_key(ec.family);
        entry["tier"] = std::string(config::enum_name(
            Field{"tier", FieldKind::EnumU8, 0, "", kThreatTierEnum}, &ec.tier));
        Json stats = Json::object();
        config::dump_struct(stats, kEliteStatsSchema, &ec.stats);
        entry["stats"] = std::move(stats);
        elites.push_back(std::move(entry));
    }
    doc["elites"] = std::move(elites);

    return doc;
}

void bind_enemies(config::Registry& registry, EnemyConfig& cfg) {
    for (u32 i = 0; i < 4; ++i) {
        registry.bind(std::string("enemies.speed_tiers.") + speed_tier_key(static_cast<SpeedTier>(i)),
                      kSpeedProfileSchema, &cfg.speed_tiers[i]);
    }
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const std::string base =
            std::string("enemies.families.") + family_key(static_cast<PathogenFamily>(i)) + ".";
        registry.bind(base + "visual", kVisualSchema, &cfg.families[i].visual);
        registry.bind(base + "behavior", kBehaviorSchema, &cfg.families[i].behavior);
        registry.bind(base + "chaff", kChaffSchema, &cfg.families[i].chaff);
        registry.bind(base + "death_vfx", kDeathVfxSchema, &cfg.families[i].death_vfx);
        registry.bind(base + "hit_flash", kHitFlashSchema, &cfg.families[i].hit_flash);
        registry.bind(base + "replication_split", kReplicationSplitSchema,
                      &cfg.families[i].replication_split);
        registry.bind(base + "latch_throb", kLatchThrobSchema, &cfg.families[i].latch_throb);
        registry.bind(base + "attack", kAttackSchema, &cfg.families[i].attack);
    }
    registry.bind("enemies.base_attack", kBaseAttackSchema, &cfg.base_attack);
    for (EliteConfig& ec : cfg.elites) {
        // Addressed by name, not index: "enemies.elites.<name>.stats.max_health"
        // survives reordering the array, an index would not.
        registry.bind("enemies.elites." + ec.name + ".stats", kEliteStatsSchema, &ec.stats);
    }
}

} // namespace immune::game::detail
