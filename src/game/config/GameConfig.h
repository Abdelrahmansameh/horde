// game/config/GameConfig.h — every gameplay-numeric tunable, as plain structs.
//
// RATIONALE
//  - This is the whole tuning surface of the game in one place: towers,
//    enemies, waves, crowd physics, economy, abilities, meta rewards. Each
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
#include "game/wave/WaveDirector.h"
#include "sim/fluid/Fluid.h"

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
    bool clumps = false;
    bool drifts = false;
    bool can_hide = false;
    bool leaves_hazard = false;
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

struct BurrowerParams {
    f32 burrow_interval = 3.5f;
    f32 resurface_delay = 1.8f;
    f32 spawn_advance = 0.25f;
};

struct BiofilmParams {
    f32 attack_radius = 6.0f;
    f32 attack_damage = 0.0f;
};

struct TumorParams {
    f32 growth_rate = 0.12f;
    f32 start_radius = 1.0f;
    f32 max_radius = 10.0f;
    u32 breach_stages = 4;
    f32 breach_damage_per_stage = 15.0f;
    f32 breach_reach_pad = 4.0f;
};

struct HulkParams {
    f32 attack_radius = 14.0f;
    f32 attack_damage = 25.0f;
    f32 obstruction_radius = 6.0f;
    f32 obstruction_cost_mul = 5.0f;
    f32 pulse_damage_per_tower = 6.0f;
};

struct ColossusParams {
    f32 attack_radius = 6.0f;
    f32 attack_damage = 6.0f;
    f32 burst_radius = 5.0f;
    f32 burst_kill_rate = 3.0f;
    f32 burst_duration = 4.0f;
};

/// Which behaviour arm an elite runs. Keyed off the elite's name, like
/// TowerRole is keyed off TowerType: it selects code, so it is not authorable.
enum class EliteBehaviorKind : u8 { Burrower, Biofilm, Tumor, Hulk, Colossus };

struct EliteBehaviorParams {
    BurrowerParams burrower{};
    BiofilmParams biofilm{};
    TumorParams tumor{};
    HulkParams hulk{};
    ColossusParams colossus{};
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

struct EliteConfig {
    u16 id = 0;
    std::string name;
    PathogenFamily family = PathogenFamily::Parasite;
    ThreatTier tier = ThreatTier::Elite;
    EliteBehaviorKind behavior_kind = EliteBehaviorKind::Burrower;
    EliteStatsParams stats{};
    EliteBehaviorParams behavior{};
};

struct FungalHazardParams {
    f32 radius = 4.0f;
    f32 kill_rate = 2.5f;
    f32 duration = 3.0f;
    f32 cooldown = 1.5f;
};

struct EnemyConfig {
    SpeedProfileParams speed_tiers[4]{};   ///< Indexed by SpeedTier.
    FamilyConfig families[kFamilyCount]{};
    BaseAttackParams base_attack{};
    FungalHazardParams fungal_death_hazard{};
    std::vector<EliteConfig> elites;
};

// ---------------------------------------------------------------------------
// waves.json — the procedural generator that 11 of 13 levels rely on.
// Levels that author their own `waves` block still override it entirely.
// ---------------------------------------------------------------------------

/// How a region's prep window shrinks across its table.
enum class PrepMode : u8 {
    Curve,          ///< Linear from `first` down to `floor` across the table.
    FirstThenFlat,  ///< Wave 0 gets `first`; every later wave gets `floor`.
};

/// One family's contribution to a generated wave.
struct WaveTrackConfig {
    PathogenFamily family = PathogenFamily::Virus;
    /// First wave index this track appears in.
    u32 from_wave = 0;
    /// The wave's base count is divided by this. 1 = the full base.
    u32 count_divisor = 1;
    /// Upper bound of a uniform random addition to the count. 0 = exact.
    f32 count_jitter = 0.0f;
    f32 start_time = 0.0f;
    f32 duration = 4.0f;
};

struct WaveRegionScaling {
    f32 prep_first = 9.0f;
    f32 prep_floor = 5.0f;
    u32 atp_base = 50;
    u32 atp_per_wave = 10;
    u32 count_base = 120;
    u32 count_per_wave = 75;
    /// Applied to the last wave only, so a table always ends escalated.
    f32 final_count_mul = 1.0f;
    f32 final_reward_mul = 1.0f;
};

struct WaveRegionConfig {
    std::string name;
    PrepMode prep_mode = PrepMode::Curve;
    WaveModifier final_modifier = WaveModifier::None;
    WaveRegionScaling scaling{};
    std::vector<WaveTrackConfig> tracks;
};

struct WaveGlobals {
    u32 default_wave_count = 8;
    /// Grace period before a wave that will not fully clear is force-completed.
    f32 clearing_timeout = 60.0f;
    /// spawn_burst() sizes its disc as contact_spacing * sqrt(count) * this.
    f32 burst_disc_factor = 0.75f;
};

struct WaveConfig {
    WaveGlobals globals{};
    std::vector<WaveRegionConfig> regions;

    /// Region lookup by name, falling back to the "flat" region. Never null
    /// once the config has loaded: "flat" is a required entry.
    const WaveRegionConfig* find_region(std::string_view name) const;
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
    WaveConfig waves{};
    SimConfig sim{};
    EconomyConfig economy{};
    AbilityConfig abilities{};
    MetaConfig meta{};
};

/// The seven files, in load order. Used by ConfigStore::expect_file and by the
/// dump, so neither can forget one.
std::vector<std::string> config_file_names();

/// Declares the seven files on `store`, then loads and parses `dir`.
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
