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

constexpr std::string_view kFamilyEntryKeys[] = {"speed_tier", "visual", "behavior", "chaff"};
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
    }
    registry.bind("enemies.base_attack", kBaseAttackSchema, &cfg.base_attack);
    for (EliteConfig& ec : cfg.elites) {
        // Addressed by name, not index: "enemies.elites.<name>.stats.max_health"
        // survives reordering the array, an index would not.
        registry.bind("enemies.elites." + ec.name + ".stats", kEliteStatsSchema, &ec.stats);
    }
}

} // namespace immune::game::detail
