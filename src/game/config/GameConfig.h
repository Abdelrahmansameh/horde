// game/config/GameConfig.h — every gameplay-numeric tunable, as plain structs.
//
// RATIONALE
//  - This is the whole tuning surface of the game in one place: towers,
//    enemies, crowd physics, economy, abilities, meta rewards. Each
//    struct is a standard-layout aggregate of scalars so config/Field.h can
//    address it by offset, which is what makes `config set <path> <value>`
//    and a complete `config dump` fall out of the same declaration as the
//    parser.
//  - It lives in `game/` rather than `config/` because filling sim::ChaffTuning
//    and reading render::family_visual both require modules that `config/`
//    sits below. `config/` stays generic machinery; the schemas live next to
//    the systems they describe.
//  - Nothing here is a frozen contract. The frozen headers are reached through
//    seams they already expose (TowerSystem::set_stats, ChaffSystem::set_tuning,
//    EnemyRoster::load_defaults), so adopting config changes no frozen file.
#pragma once

#include "config/ConfigStore.h"
#include "config/Field.h"
#include "config/Registry.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/towers/TowerSystem.h"
#include "sim/chaff/HitFlash.h"
#include "sim/chaff/ReplicationSplit.h"
#include "sim/fluid/Fluid.h"
#include "sim/squad/Squads.h"
#include "sim/swarm/Swarmers.h"
#include "vfx/DeathVfx.h"

#include <string>
#include <vector>

namespace immune::game {

// ---------------------------------------------------------------------------
// towers.json
// ---------------------------------------------------------------------------

/// Which swarmer KIND a tower type releases. Derived from TowerType, never
/// authored: the kind IS the type, so letting a file claim otherwise would let
/// a config describe a tower the code cannot build.
sim::SwarmerKind tower_kind(TowerType type);
const char* tower_kind_name(sim::SwarmerKind kind);

/// The swarmer chassis, shared by every tower: how many a volley releases and
/// how each one flies, aggros and reaches. What a swarmer does on contact is
/// the per-kind payload below.
struct SwarmParams {
    u32 release_per_shot = 24;
    /// Seconds before a swarmer retires. Against fire_interval and
    /// release_per_shot this sets the standing cloud size.
    f32 lifetime = 4.0f;
    f32 speed = 20.0f;
    /// Aggro radius: how far a targetless swarmer looks for something to go at.
    f32 search_radius = 9.0f;
    /// Contact radius. Latch: latches inside it; bombers: detonate inside it;
    /// Shooter: its standoff -- stops and fires inside, chases outside.
    f32 attach_radius = 0.55f;
    /// Launch cone half-angle, radians. Volleys are scattered across it.
    f32 launch_spread = 0.85f;
    /// Body radius, world units: drawn at this size, and kept this far (x
    /// sim::kWallContactFraction) off the vessel wall.
    f32 size = 1.5f;
};

struct LatchParams {
    /// Density drained per second by one attached swarmer.
    f32 dps = 4.2f;
};

struct ShooterParams {
    f32 fire_interval = 0.25f;
    f32 round_damage = 2.0f;
    f32 round_speed = 40.0f;
    f32 round_hit_radius = 0.45f;
    /// Aim jitter half-angle, radians.
    f32 round_spread = 0.10f;
    /// Distance between squad-mates along the rank a volley holds.
    f32 formation_spacing = 3.5f;
};

struct BomberParams {
    /// Seconds a bomber chases one target before detonating where it is.
    f32 chase_seconds = 1.0f;
    f32 burst_radius = 4.0f;
    /// Total density an agent at the centre loses over the burst.
    f32 burst_damage = 20.0f;
    /// How long the burst field lingers. The rate is damage / seconds.
    f32 burst_seconds = 0.3f;
    f32 burst_falloff = 0.4f;
    /// Hit points a named agent at the centre takes, before armor.
    f32 named_damage = 20.0f;
};

struct SlowBomberParams {
    /// Seconds a bomber chases one target before detonating where it is.
    f32 chase_seconds = 1.0f;
    f32 zone_radius = 3.0f;
    /// Seconds the circle stays on the ground.
    f32 zone_duration = 3.0f;
    /// Seconds an agent stays slowed after its last tick inside the circle.
    f32 slow_duration = 1.5f;
    /// Max-speed multiplier while slowed; 0.4 = 40% speed.
    f32 slow_factor = 0.4f;
};

struct MucusBomberParams {
    /// Seconds a bomber chases one target before detonating where it is.
    f32 chase_seconds = 1.0f;
    /// Fluid particles one splash puts down.
    u32 droplets = 24;
    f32 splash_radius = 1.2f;
    /// Outward launch speed of the splash's rim droplets.
    f32 splash_speed = 6.0f;
    /// Seconds each droplet survives.
    f32 droplet_lifetime = 2.0f;
    /// Density removed per second from a FULLY soaked coverage cell.
    f32 splash_dps = 10.0f;
    /// Seconds a named agent inside the splash stays weakened (comp::Marked).
    f32 mark_seconds = 2.5f;
};

/// The chassis plus every payload arm held flat. Only the arm matching the
/// tower's kind is read or written; a flat aggregate keeps offsetof trivial
/// and costs a few hundred bytes for the whole table.
struct TowerMechanics {
    SwarmParams swarm{};
    LatchParams latch{};
    ShooterParams shooter{};
    BomberParams bomber{};
    SlowBomberParams slow_bomber{};
    MucusBomberParams mucus_bomber{};
};

struct TowerGlobals {
    /// Fraction of invested ATP returned on sell. Mirrors
    /// EconomyConfig::refund_fraction; economy.json is the source and this is
    /// filled from it at load so the two cannot drift.
    f32 refund_fraction = 0.7f;
    /// Base of the tower shape-id space, kept clear of overlay ids.
    u32 shape_base = 16;
};

struct TowerConfig {
    TowerGlobals globals{};
    TowerStats stats[kTowerTypeCount][3]{};
    TowerMechanics mechanics[kTowerTypeCount][3]{};
};

// ---------------------------------------------------------------------------
// enemies.json
// ---------------------------------------------------------------------------

struct SpeedProfileParams {
    f32 max_speed = 6.0f;
    f32 acceleration = 24.0f;
    f32 jitter = 0.4f;
};

/// The renderer's per-family look. `silhouette` is also the physical size:
/// ChaffFamilyParams::radius is derived from it, so what is drawn and what
/// collides can never disagree.
struct FamilyVisualParams {
    f32 silhouette = 1.8f;
    f32 tempo = 1.0f;
    f32 wobble = 0.5f;
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct FamilyBehaviorParams {
    f32 base_density = 1.0f;
    bool replicates = false;
};

/// Everything sim::ChaffFamilyParams holds, authorable per family. Thirteen of
/// these were previously unreachable from any data path at all.
struct FamilyChaffParams {
    f32 radius_from_silhouette = 0.5f;
    f32 separation_radius_mul = 2.4f;
    f32 separation_strength = 8.0f;
    f32 alignment_radius = 2.4f;
    f32 alignment_strength = 3.0f;
    f32 pressure_threshold = 6.0f;
    f32 pressure_gain = 0.12f;
    f32 pressure_max = 4.0f;
    f32 wall_restitution = 0.15f;
    f32 wall_splash = 0.75f;
    f32 contact_spacing = 2.0f;
    f32 contact_stiffness = 1.0f;
    f32 crowd_relief = 0.7f;
    f32 drift_bias = 0.0f;
    f32 replication_rate = 0.0f;
};

struct FamilyConfig {
    SpeedTier speed_tier = SpeedTier::Normal;
    FamilyVisualParams visual{};
    FamilyBehaviorParams behavior{};
    FamilyChaffParams chaff{};
    /// The burst this family throws when it dies. Reused verbatim from vfx/
    /// rather than mirrored here, for the same reason SimConfig reuses
    /// sim::FluidTuning: a mirror is a second place for the numbers to live,
    /// and the two would eventually disagree about what the game actually
    /// draws. See vfx/DeathVfx.h for what each knob does.
    vfx::FamilyDeathVfx death_vfx{};
    /// The white flare this family shows when something hits it. Reused
    /// verbatim from sim/ for the same no-mirror reason as `death_vfx` above;
    /// see sim/chaff/HitFlash.h for what each knob does and for why that struct
    /// lives one layer lower than the death burst's does.
    sim::HitFlashParams hit_flash{};
    /// The continuous parent-shell-to-two-daughters replication morph.
    sim::ReplicationSplitParams replication_split{};
};

/// Shared melee shape every elite starts from.
struct BaseAttackParams {
    f32 active = 0.2f;
    f32 recovery = 0.4f;
    f32 range = 10.0f;
    f32 radius = 3.0f;
    f32 damage = 10.0f;
    f32 death_fade = 0.6f;
};

/// Core stats an elite shares with every other elite. Mirrors EliteDef minus
/// its identity fields (id/name/family/tier), which are structure, not tuning.
struct EliteStatsParams {
    f32 max_health = 500.0f;
    f32 armor = 0.0f;
    f32 speed = 3.0f;
    f32 sprite_size = 2.0f;
    f32 ability_cooldown = 6.0f;
    f32 telegraph_duration = 0.8f;
    u32 atp_bounty = 50;
};

/// An elite's authorable surface is its identity plus the shared stat block.
/// There is no per-elite behaviour arm today: the roster ships no elites, and
/// a redesigned one adds its own params struct back alongside this.
struct EliteConfig {
    u16 id = 0;
    std::string name;
    PathogenFamily family = PathogenFamily::Virus;
    ThreatTier tier = ThreatTier::Elite;
    EliteStatsParams stats{};
};

struct EnemyConfig {
    SpeedProfileParams speed_tiers[4]{};   ///< Indexed by SpeedTier.
    FamilyConfig families[kFamilyCount]{};
    BaseAttackParams base_attack{};
    std::vector<EliteConfig> elites;
};

// ---------------------------------------------------------------------------
// sim.json
// ---------------------------------------------------------------------------

struct SimCapacities {
    u32 max_chaff = 16384;
    u32 max_damage_fields = 512;
    u32 max_projectiles = 8192;
    u32 max_swarmers = 32768;
    u32 max_slow_zones = 256;
    u32 max_fluid_particles = 8192;
    u32 max_combat_events = 8192;
    /// Chaff death bursts raised per tick, out of the budget above. See
    /// sim::SimDesc::max_chaff_death_events for why deaths get their own cap.
    u32 max_chaff_death_events = 512;
};

struct SimGlobals {
    f32 spatial_cell_size = 4.0f;
    f64 flow_rebake_budget_ms = 0.5;
    u32 flow_rebake_margin_cells = 16;
    /// World-space radius of the flow field's direction-smoothing pass. Raise
    /// it for a lazier field that follows the lane's overall shape; drop it to
    /// 0 for the exact shortest-path directions. See
    /// sim::FlowFieldBakeDesc::smoothing_radius.
    f32 flow_smoothing_radius = 2.0f;
    /// Extra traversal cost for a cell hard against a vessel wall, and the
    /// clearance over which it decays. Together these set how wide a turn the
    /// horde takes; 0 cost restores exact shortest-path cornering. See
    /// sim::SimDesc::flow_wall_cost.
    f32 flow_wall_cost = 0.0f;
    f32 flow_wall_falloff = 34.0f;
    f32 flow_wall_exponent = 12.0f;
    u32 max_replications_per_tick = 128;
    u32 max_neighbors_sampled = 24;
    f32 slowed_speed_multiplier = 0.4f;
    Vec2 ambient_drift{0.5f, 0.28f};
    u32 damage_max_chain_links = 8;
    /// Objective integrity lost per agent that reaches the goal.
    f32 objective_damage_per_leak = 1.0f;
};

struct SwarmerGlobals {
    f32 damp = 1.5f;
    f32 wander_gain = 0.45f;
    f32 ring_offset_fraction = 0.6f;
    f32 turn_gain = 9.0f;
};

struct SimConfig {
    SimCapacities capacities{};
    SimGlobals globals{};
    SwarmerGlobals swarmers{};
    /// Squad grouping, reused verbatim from sim/squad for the same reason the
    /// fluid block below is: the config cannot drift from the struct the sim
    /// actually reads. See sim/squad/Squads.h for what each knob does and why
    /// the follow weight must stay below 1.
    sim::SquadTuning squads{};
    /// The fluid solver's own constants, reused verbatim from sim/fluid so the
    /// config cannot drift from the struct the solver actually reads. It sits
    /// in sim.json rather than towers.json because there is exactly ONE solver
    /// per world and every Goblet Cell on the board shares it — putting it on
    /// a tower row would imply per-tower physics that cannot exist.
    sim::FluidTuning fluid{};
};

// ---------------------------------------------------------------------------
// abilities.json / meta.json
// ---------------------------------------------------------------------------

/// AbilityDef minus its `name`, which is display identity rather than tuning.
struct AbilityTuning {
    f32 cooldown_seconds = 60.0f;
    f32 radius = 20.0f;
    f32 kill_rate = 40.0f;
    f32 field_duration = 1.5f;
    f32 fever_cooldown_relief = 3.0f;
    f32 barrier_half_length = 7.0f;
    f32 barrier_half_width = 1.5f;
};

struct AbilityConfig {
    AbilityTuning ability[kAbilityCount]{};
};

struct MetaConfig {
    u32 base_run_reward = 10;
    u32 per_wave_reward = 15;
    u32 per_elite_reward = 8;
    u32 per_boss_reward = 50;
    u32 chaff_per_point = 200;
    u32 win_bonus = 40;
};

// ---------------------------------------------------------------------------
// Aggregate
// ---------------------------------------------------------------------------

struct GameConfig {
    TowerConfig towers{};
    EnemyConfig enemies{};
    SimConfig sim{};
    EconomyConfig economy{};
    AbilityConfig abilities{};
    MetaConfig meta{};
};

/// The six files, in load order. Used by ConfigStore::expect_file and by the
/// dump, so neither can forget one.
std::vector<std::string> config_file_names();

/// Declares the six files on `store`, then loads and parses `dir`.
/// On failure `err` carries a message naming the file and field path, and
/// `out` is left untouched.
bool load_game_config(config::ConfigStore& store, const std::string& dir, GameConfig& out,
                      std::string& err);

/// Parses an already-loaded store into `out`. Split out so tests can drive the
/// parser without touching the filesystem.
bool parse_game_config(const config::ConfigStore& store, GameConfig& out, std::string& err);

/// Serializes `cfg` to one Json document per file name, in config_file_names()
/// order. Round-trips: parsing the output reproduces `cfg` exactly.
std::vector<config::Json> dump_game_config(const GameConfig& cfg);

/// Writes dump_game_config() to `dir`. This is how the shipped files are
/// generated from the code's own values rather than transcribed by hand.
bool write_game_config(const GameConfig& cfg, const std::string& dir, std::string& err);

/// Binds every struct in `cfg` into `registry` so the gym console can address
/// it. Call after every load; a reload re-binds.
void bind_game_config(config::Registry& registry, GameConfig& cfg);

/// The current hardcoded values, as a GameConfig. This is the bootstrap source
/// for the shipped files and the regression baseline that proves the migration
/// moved no numbers.
GameConfig default_game_config();

} // namespace immune::game
