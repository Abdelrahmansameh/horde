// game/config/Schemas.h — internal split of the six file parsers.
//
// One translation unit per group keeps each file readable; GameConfig.cpp is
// the only thing that calls these, and it drives them in config_file_names()
// order so parse, dump and bind can never disagree about which file is which.
#pragma once

#include "config/Json.h"
#include "config/Registry.h"
#include "game/config/GameConfig.h"

namespace immune::game::detail {

// Each parse_* throws config's std::runtime_error with a field path; each
// dump_* produces a document that parse_* reads back to identical values.

void parse_towers(const config::Json& doc, TowerConfig& out, config::Ctx& ctx);
config::Json dump_towers(const TowerConfig& cfg);
void bind_towers(config::Registry& registry, TowerConfig& cfg);

void parse_enemies(const config::Json& doc, EnemyConfig& out, config::Ctx& ctx);
config::Json dump_enemies(const EnemyConfig& cfg);
void bind_enemies(config::Registry& registry, EnemyConfig& cfg);

void parse_sim(const config::Json& doc, SimConfig& out, config::Ctx& ctx);
config::Json dump_sim(const SimConfig& cfg);
void bind_sim(config::Registry& registry, SimConfig& cfg);

void parse_economy(const config::Json& doc, EconomyConfig& out, config::Ctx& ctx);
config::Json dump_economy(const EconomyConfig& cfg);
void bind_economy(config::Registry& registry, EconomyConfig& cfg);

void parse_abilities(const config::Json& doc, AbilityConfig& out, config::Ctx& ctx);
config::Json dump_abilities(const AbilityConfig& cfg);
void bind_abilities(config::Registry& registry, AbilityConfig& cfg);

void parse_meta(const config::Json& doc, MetaConfig& out, config::Ctx& ctx);
config::Json dump_meta(const MetaConfig& cfg);
void bind_meta(config::Registry& registry, MetaConfig& cfg);

/// Shared: every file carries "schema": 1 and is rejected otherwise, matching
/// the level loader's policy.
void require_schema_version(const config::Json& doc, config::Ctx& ctx);
void write_schema_version(config::Json& doc);

// Shared enum tables, so every config file spells a family the same way the
// level loader and the --sim-test scripts already do. Sentinel-terminated, and
// constexpr so they can be referenced from constexpr Field tables.

inline constexpr config::EnumEntry kFamilyEnum[] = {
    {"virus", static_cast<i64>(PathogenFamily::Virus)},
    {"bacteria", static_cast<i64>(PathogenFamily::Bacteria)},
    {nullptr, 0},
};

inline constexpr config::EnumEntry kSpeedTierEnum[] = {
    {"slow", static_cast<i64>(SpeedTier::Slow)},
    {"normal", static_cast<i64>(SpeedTier::Normal)},
    {"fast", static_cast<i64>(SpeedTier::Fast)},
    {"erratic", static_cast<i64>(SpeedTier::Erratic)},
    {nullptr, 0},
};

/// vfx::DeathStyle — which shape a family's body comes apart into when it dies.
inline constexpr config::EnumEntry kDeathStyleEnum[] = {
    {"burst", static_cast<i64>(vfx::DeathStyle::Burst)},
    {"lyse", static_cast<i64>(vfx::DeathStyle::Lyse)},
    {nullptr, 0},
};

inline constexpr config::EnumEntry kThreatTierEnum[] = {
    {"chaff", static_cast<i64>(ThreatTier::Chaff)},
    {"elite", static_cast<i64>(ThreatTier::Elite)},
    {"boss", static_cast<i64>(ThreatTier::Boss)},
    {nullptr, 0},
};

inline std::span<const config::EnumEntry> family_enum() { return {kFamilyEnum, 6}; }
inline std::span<const config::EnumEntry> speed_tier_enum() { return {kSpeedTierEnum, 4}; }
inline std::span<const config::EnumEntry> threat_tier_enum() { return {kThreatTierEnum, 3}; }

} // namespace immune::game::detail
