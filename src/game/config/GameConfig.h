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
#include "render/LatchThrob.h"
#include "sim/burrow/Burrow.h"
#include "sim/chaff/HitFlash.h"
#include "sim/chaff/ReplicationSplit.h"
#include "sim/fluid/Fluid.h"
#include "sim/hostile/HostileAttacks.h"
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
    /// Hit points a swarmer is released with. The horde spends them
    /// (sim/hostile: viruses latch on and drain, bacteria burn); at zero the
    /// unit dissolves without acting.
    f32 max_health = 10.0f;
};

struct LatchParams {
    /// Density drained per second by one attached swarmer.
    f32 dps = 4.2f;
    /// Seconds the entry takes once in reach: the unit lurches into the
    /// host's centre and shrinks to nothing, and only then starts draining.
    /// It pops back to full size the tick the host dies. 0 = instant.
    f32 attach_seconds = 0.1f;
    /// Discrete lurches the entry movement is chopped into (0/1 = one slide).
    u32 attach_steps = 4;
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
    /// Kite radius as a fraction of the standoff; an enemy inside it makes
    /// the shooter back away while firing. 0 disables.
    f32 kite_fraction = 0.75f;
    /// How much the flow field bends the retreat toward "where the horde is
    /// going" (0 = straight away from the nearest threat).
    f32 kite_flow_weight = 1.0f;
    /// Retreat speed as a multiple of the swarm speed.
    f32 kite_speed_mult = 1.5f;
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

/// The Macrophage's radial, branching pseudopods. Each arm launches, latches
/// and recovers independently. `fake_mass` is visual compensation: 0 strictly
/// redistributes the body's apparent volume into its arms, while 1 keeps the
/// core nearly full-sized.
///
/// A volley of these is a LANE WALL (sim/swarm/Swarmers.h): a rank across
/// the flow the horde has to press through. `wall_spacing` is the most room
/// between squad-mates along that rank (it shrinks to fit the lane), and
/// `body_block` is how much of a pathogen/macrophage overlap the PATHOGEN
/// takes: 0 is the ordinary one-way contact where the swarmer yields, 1 an
/// immovable body the horde is shoved out of. Kiting and a wall pull in
/// opposite directions; ship kite_fraction 0 with a body_block above 0.
struct ArborGrabberParams {
    u32 arm_count = 3;
    /// Total ordinary enemies one pseudopod can swallow in one pull. The
    /// initial target counts toward the limit; named agents remain single
    /// targets because their movement lives outside the chaff simulation.
    u32 max_captives = 1;
    /// Enemies within this distance of the initial target join its pull.
    /// 0 keeps the original single-target behavior.
    f32 cluster_radius = 0.0f;
    f32 extend_seconds = 0.12f;
    f32 latch_seconds = 0.05f;
    f32 pull_seconds = 0.22f;
    f32 recover_seconds = 0.06f;
    f32 fake_mass = 0.65f;
    f32 kite_fraction = 0.0f;
    f32 kite_flow_weight = 1.0f;
    f32 kite_speed_mult = 1.1f;
    f32 wall_spacing = 4.5f;
    f32 body_block = 0.9f;
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
    /// Seconds an enemy stays slowed after leaving the mucus.
    f32 slow_duration = 3.0f;
    /// Max-speed multiplier while slowed; 0.1 means 10% speed.
    f32 slow_factor = 0.1f;
};

/// The Fibroblast's builders and the collagen scars they lay
/// (sim/scar/Scars.h). A scar is a bar laid ACROSS the local flow, carved
/// out of the tissue so the horde has to go round it, standing until the
/// horde chews it down (it is a host to the hostile pass like a tower is).
struct BuilderParams {
    /// Where a builder is sent: a random site in the annulus
    /// [build_min_radius, build_radius] around its tower, on walkable tissue
    /// with a flow direction (i.e. in a lane), with this much SDF clearance.
    f32 build_radius = 14.0f;
    f32 build_min_radius = 4.0f;
    f32 build_min_clearance = 1.0f;
    /// Random sites tried per builder before the tower gives up on it.
    u32 build_candidates = 12;
    /// The bar: half-length along the wall, half-width across it. It is laid
    /// ACROSS the local flow (square to the arrows), give or take a random
    /// tilt of up to scar_tilt radians either way; 0 = exactly square.
    f32 scar_half_length = 5.0f;
    f32 scar_half_width = 0.9f;
    f32 scar_tilt = 0.2f;
    /// Integrity a fresh scar stands with; the horde spends it like a tower's.
    f32 scar_health = 250.0f;
    /// Hit points a builder adds to an existing scar when it cannot start a
    /// new one (another scar too close, or the owner at max_scars). 0 = a
    /// blocked builder just dissolves.
    f32 scar_reinforce = 60.0f;
    /// No two scar centres closer than this.
    f32 scar_spacing = 6.0f;
    /// Live scars one tower may own at once; 0 = unlimited.
    u32 max_scars = 4;
    /// Seconds a scar stands before dissolving on its own; 0 = permanent.
    f32 scar_lifetime = 0.0f;
    /// How much of the crowd's shove a walking builder takes, 0..1: 0 crawls
    /// through the horde as if it were matrix, 1 is pushed out of every
    /// pathogen like any other unit. Passengers still climb on either way.
    f32 crowd_push = 0.0f;
};

/// The chassis plus every payload arm held flat. Only the arm matching the
/// tower's kind is read or written; a flat aggregate keeps offsetof trivial
/// and costs a few hundred bytes for the whole table.
struct TowerMechanics {
    SwarmParams swarm{};
    LatchParams latch{};
    ShooterParams shooter{};
    BomberParams bomber{};
    ArborGrabberParams arbor_grabber{};
    SlowBomberParams slow_bomber{};
    MucusBomberParams mucus_bomber{};
    BuilderParams builder{};
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
    bool collides = true;
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
    /// How a latched agent throbs and pumps at the host it is feeding on
    /// (render/LatchThrob.h). Reused verbatim from render/ for the no-mirror
    /// reason above; it is the one family look with no sim consumer at all.
    render::LatchThrobParams latch_throb{};
    /// How this family hurts the player's cells -- the latch and the aura
    /// (sim/hostile/HostileAttacks.h). Reused verbatim from sim/ for the same
    /// no-mirror reason as `hit_flash`: the pass reads exactly this struct.
    sim::HostileFamilyParams attack{};
    /// Burrowing under the tissue and resurfacing further down the lane
    /// (sim/burrow/Burrow.h). Off for every family but the Parasite. Reused
    /// verbatim for the same no-mirror reason as `hit_flash`.
    sim::BurrowParams burrow{};
    /// The drawn worm body and its travelling wave (sim/burrow/Burrow.h).
    sim::SlitherParams slither{};
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

/// The world-wide half of sim::HostileTuning: the switch and the caps. The
/// per-family half (the numbers that make a virus a virus) lives on each
/// family in enemies.json; hostile_tuning() in game/enemies/EnemyConfigApply.h
/// joins the two.
struct HostileGlobals {
    bool enabled = true;
    u32 max_attackers = 128;
    u32 max_latch_events = 64;
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
    /// Swarmer body collision, reused verbatim from sim/swarm for the same
    /// reason. See SwarmerCollisionTuning for what each knob does.
    sim::SwarmerCollisionTuning swarmer_collision{};
    /// The horde's attacks on towers and swarmers: the switch and the walk
    /// caps. Per-family strength is in enemies.json.
    HostileGlobals hostile{};
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
