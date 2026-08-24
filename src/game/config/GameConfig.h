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
#include "sim/fluid/Fluid.h"
#include "sim/squad/Squads.h"

#include <string>
#include <vector>

namespace immune::game {

// ---------------------------------------------------------------------------
// towers.json
// ---------------------------------------------------------------------------

/// Which mechanism arm a tower type uses. Derived from TowerType, never
/// authored: the role IS the type, so letting a file claim otherwise would let
/// a config describe a tower the code cannot build.
enum class TowerRole : u8 { Gunner, Mortar, Cryo, Tesla, Hydro, Blade };

TowerRole tower_role(TowerType type);
const char* tower_role_name(TowerRole role);

struct GunnerParams {
    f32 round_speed = 45.0f;
    f32 hit_radius = 0.45f;
    f32 spread = 0.045f;
};

struct MortarParams {
    f32 burst_seconds = 0.30f;
    f32 burst_radius = 5.0f;
    f32 burst_falloff = 0.4f;
    f32 marked_multiplier = 1.5f;
};

struct CryoParams {
    f32 arc_radians = 0.60f;
    f32 inner_fraction = 0.55f;
    f32 cone_falloff = 0.5f;
    u32 max_freeze_events = 6;
};

struct TeslaParams {
    u32 release_per_shot = 36;
    f32 swarmer_lifetime = 6.8f;
    f32 swarmer_speed = 26.0f;
    f32 swarmer_dps = 4.2f;
    f32 attach_radius = 0.55f;
    f32 search_radius = 9.0f;
    f32 launch_spread = 0.85f;
};

/// The Goblet Cell's nozzle. Note what is NOT here: no damage radius, no
/// falloff curve, no beam length. Where the mucus goes and what it touches is
/// decided by the fluid solver (sim/fluid/Fluid.h), and these are only the
/// terms of its release. The solver's own constants live in sim.json, because
/// there is one solver for the whole world and every nozzle shares it.
struct HydroParams {
    /// How long one trigger pull keeps spraying. This is what makes the attack
    /// read as BURSTS: the tower sprays for this long, then reloads for
    /// TowerStats::fire_interval, and the gap is where the player watches the
    /// slug of fluid travel, land, and spread.
    f32 burst_seconds = 0.34f;
    /// Muzzle velocity of the jet, world units/sec. Together with the tower's
    /// range this decides whether the beam still has pressure when it arrives.
    f32 jet_speed = 34.0f;
    /// Half-width of the nozzle mouth. Sets beam thickness — and, through the
    /// swept-area emission model, the flow rate.
    f32 nozzle_radius = 1.05f;
    /// Launch cone half-angle, radians. Deliberately tiny.
    f32 spread = 0.05f;
    /// Seconds a droplet survives once it has left the cell. THE burst-lifetime
    /// knob: it is what stops a board full of Goblet Cells from silting up into
    /// one permanent lake, and what makes a splash a moment rather than terrain.
    f32 droplet_lifetime = 1.9f;
    /// Multiplier on the geometrically-derived emission rate. 1 emits at
    /// exactly rest density; lower gives a thinner, gappier, cheaper stream.
    f32 flow_scale = 1.0f;
    /// Seconds a NAMED agent stays weakened (chaff_flags::kMarked's counterpart
    /// for the ECS half of the sim, comp::Marked) after this tower's own strike
    /// hits it. Refreshed on every hit, so a Goblet Cell with a target locked in
    /// range keeps it permanently marked; one that loses its target lets the
    /// mark run out. Chaff, by contrast, stays marked forever once soaked (see
    /// sim/fluid/Fluid.cpp) — a named agent is a much bigger prize, so its
    /// version of the debuff is not a free permanent buff to the whole roster.
    f32 mark_seconds = 2.6f;
};

struct BladeParams {
    f32 spin_rad_per_sec = 9.0f;
    f32 rotor_falloff = 0.0f;
    u32 max_slash_events = 5;
};

/// All six arms held flat. Only the arm matching the tower's role is read or
/// written; a flat aggregate keeps offsetof trivial and costs a few hundred
/// bytes for the whole table.
struct TowerMechanics {
    GunnerParams gunner{};
    MortarParams mortar{};
    CryoParams cryo{};
    TeslaParams tesla{};
    HydroParams hydro{};
    BladeParams blade{};
};

/// Neutrophil's NET ability.
struct NetAbilityParams {
    u32 micro_units = 3;
    f32 spread_jitter = 0.5f;
    f32 micro_lifetime = 1.5f;
    f32 micro_sprite_size = 0.3f;
    f32 net_duration = 3.0f;
    Vec4 micro_tint{0.3f, 0.6f, 1.0f, 1.0f};
    Vec4 net_tint{0.2f, 0.8f, 0.9f, 0.5f};
};

struct TowerGlobals {
    /// Fraction of invested ATP returned on sell. Mirrors
    /// EconomyConfig::refund_fraction; economy.json is the source and this is
    /// filled from it at load so the two cannot drift.
    f32 refund_fraction = 0.7f;
    /// Base of the tower shape-id space, kept clear of overlay ids.
    u32 shape_base = 16;
    /// Half-width, in cells, of the window would_block_all_paths() searches.
    f32 block_check_pad_cells = 14.0f;
};

struct TowerConfig {
    TowerGlobals globals{};
    TowerStats stats[kTowerTypeCount][3]{};
    TowerMechanics mechanics[kTowerTypeCount][3]{};
    NetAbilityParams net{};
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
    f32 contact_stiffness = 0.7f;
    f32 drift_bias = 0.0f;
    f32 replication_rate = 0.0f;
};

struct FamilyConfig {
    SpeedTier speed_tier = SpeedTier::Normal;
    FamilyVisualParams visual{};
    FamilyBehaviorParams behavior{};
    FamilyChaffParams chaff{};
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
    u32 max_swarmers = 24576;
    u32 max_fluid_particles = 8192;
    u32 max_combat_events = 8192;
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
