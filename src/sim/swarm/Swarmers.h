// sim/swarm/Swarmers.h — SoA swarmer store + seek/engage/detonate integration.
// Owner: the swarmer-roster redesign (originally the Cytotoxic T redesign).
//
// WHAT A SWARMER IS
// One small cell released by a tower. Every tower in the roster is a spawner
// now: on each cooldown it releases a volley of these, they fly out of the
// cell's face, pick a pathogen, chase it, and do the tower's work when they
// reach it. The tower keeps releasing more the whole time, so what the player
// reads is never a beam or a blast but a standing CLOUD of the tower's own
// units going at the horde: a horde answering a horde.
//
// What a swarmer does on contact is its KIND (SwarmerKind), and every number
// about it comes from a SwarmerProfile the tower registered for its type and
// tier. The chassis is shared — position, lifetime, speed, aggro radius,
// contact radius, private wander seed — and only the payload differs:
//
//   Latch        Cytotoxic T.  Latches on and drains the host until it dies,
//                              then finds another. The original swarmer.
//   Shooter      Neutrophil.   Holds a standoff and fires real rounds into the
//                              projectile store; chases if the target moves off.
//   Bomber       Generic burst primitive retained for authored encounters and tests;
//                no current tower maps to it.
//   SlowBomber   Interferon.   Detonates on contact: a timed slow zone
//                              (sim/zone/SlowZones.h). No damage at all.
//   MucusBomber  Goblet Cell.  Detonates on contact: a splash of real fluid
//                              (sim/fluid/Fluid.h) that strongly slows what it soaks.
//   Builder      Fibroblast.   Hunts nothing. Walks to the spot it was given
//                              at release (SwarmerSpawnParams::goal) and, on
//                              arrival, asks for a collagen SCAR there -- a
//                              wall across the lane (sim/scar/Scars.h) -- and
//                              is spent. A builder with no goal just drifts
//                              and dissolves.
//   ArborGrabber Macrophage.   Up to three independent pseudopod trees grow
//                              in any direction. Fine fingers latch one target
//                              per tree, and all trees may pull at once. A
//                              volley of them is a LANE WALL (below): a rank
//                              across the flow that the horde has to press
//                              through, body against body.
//
// A bomber whose lifetime runs out detonates where it stands, and so does one
// that has been chasing a target for longer than its profile's chase_seconds
// without reaching it -- a bomber is a shell, not a hound. A latcher or a
// shooter simply dissolves. Every kind aggros the moment it is released and
// goes at whatever is nearest inside its search radius, chaff or named agent
// alike. Burrowed (kHidden / AiState::Burrowed) targets are invisible to all.
//
// STANDING GROUND. A shooter whose target walks out of its standoff, or a
// bomber whose target leaves its aggro radius, re-picks the nearest thing in
// its search radius and only follows the old target if there is nothing
// else. Latchers always chase. Shooters released together are a SQUAD: they
// march and hold as one rank, a line across their approach, each in its own
// slot (SwarmerSpawnParams::group / slot).
//
// LANE WALL. Every live ArborGrabber a tower owns is ONE squad, whatever
// volley released it, and its rank is built across the LANE rather than
// across its approach: the line runs
// perpendicular to the flow field sampled at the squad's centroid -- the
// direction the horde walks through that spot -- so the bodies stand
// side by side athwart the traffic instead of strung out along it. The rank
// is spaced to span the lane (SwarmerProfile::formation_spacing is the
// ceiling; the SDF clearance at the centroid is the lane's half-width, and
// the spacing shrinks until the outermost bodies sit inside the vessel wall)
// and each tick the line as a whole is nudged across the lane toward the
// larger clearance, so a volley released against one bank slides to the
// middle. Along the flow the wall is ANCHORED on its own centroid: a member
// only moves along the lane when the squad's target is farther than its hold
// distance out, and it never backs off from one pressing into it. Kiting
// still wins when configured, but a wall that kites is not a wall, so
// Macrophage_2 ships with kite_fraction 0.
//
// KITING. A shooter is ranged and soft, and a horde walks. So a shooter that
// finds ANY enemy inside its kite radius (SwarmerProfile::kite_fraction of
// its standoff) backs away while it keeps firing: straight away from the
// nearest threat, bent by the flow field -- the direction the horde is about
// to move through this very spot -- so it retreats AHEAD of the crowd rather
// than sideways into the next rank of it. The retreat fades to nothing at
// the kite edge, so a shooter settles in the band between kite radius and
// standoff: in range, out of reach. Costs one search per engaged shooter per
// tick, the same price a targetless swarmer pays. A grabber kites between
// attacks, but braces while its synchronized pincer cycle is in motion.
//
// PINCERS. A grabber's attach_radius is the forward reach of one synchronized
// crab-like pair. It opens wide and grows forward, snaps shut around a small
// cluster, then quickly retracts. Captured chaff are flagged hidden so the
// movement and targeting kernels leave them alone, but remain rendered while
// their real positions are pulled into the body. Only on arrival are they
// killed. The fixed captive array keeps the hot path allocation-free.
//
// WALLS. Swarmers are cells, not ghosts: every unit moving under its own
// steering is projected back onto the tissue against the distance field, the
// same way the fluid is, so a volley cannot clip through a vessel wall.
//
// BODIES. Swarmers also collide (SwarmerCollisionTuning), body against body,
// where a body is `size * kWallContactFraction` -- the drawn membrane, the
// same edge the wall projection honours. Asymmetrically:
//   - friendly vs friendly is a symmetric contact push, half the overlap each,
//     the same displacement model the chaff crowd uses;
//   - friendly vs enemy is ONE-WAY by default. The swarmer is pushed out of
//     the pathogen, the pathogen never moves for a swarmer. That is what makes
//     the horde read as heavy -- a cloud of units can never body-block a lane,
//     and a charging squad shoves the rank in front of it aside -- and it is
//     also why the chaff kernel knows nothing about this layer. A profile with
//     SwarmerProfile::body_block > 0 is the exception, and the whole reason
//     the LANE WALL above works: the overlap is split by that share, the
//     pathogen taking `body_block` of it and the unit the rest, written
//     straight into the chaff store the way the hostile pass plants
//     passengers. Only the chaff's POSITION moves; its velocity is left
//     pointing into the wall on purpose, exactly as ChaffSystem's mask
//     containment leaves it, so the front rank keeps pressing and the jam
//     behind it builds and spills sideways. Named agents are never moved: a
//     boss is heavier than any wall;
//   - a bomber that so much as touches ANY enemy goes off there and then,
//     whether or not it was the enemy it had picked, and the payload leaves
//     from the point of contact on its membrane rather than from its centre.
//     A shell that grazes the front of a horde does not get to fly through it.
// Latch units (Cytotoxic T) take no part in any of it: a latcher's whole job
// is to sit ON a pathogen, and a clump of them on one host is the intended
// read. The pass costs every non-Latch swarmer one 3x3 walk of a swarmer-only
// hash and one of the chaff hash, both walk-capped, plus the named list.
//
// HEALTH. Swarmers can be killed. The horde fights back
// (sim/hostile/HostileAttacks.h): a virus latches onto a unit and drains it,
// a bacterium burns everything inside its aura. Every swarmer carries
// `health`, filled from its profile's max_health at release; the hostile
// pass subtracts from it and flags the unit kPendingKill at zero. A unit
// killed that way DISSOLVES -- no detonation, no round -- and update() skips
// anything already flagged, so a dead unit never acts on the tick after its
// death while it waits for compaction. Chaff carrying chaff_flags::kLatched
// are riding one of these units (or a tower): the BODIES pass treats them as
// not solid, since they are inside the host's membrane by construction, and
// a kiting shooter does not count its own passengers as a threat to flee --
// it would otherwise run from them forever. They stay perfectly targetable,
// which is the point: a tower's units should be scraping viruses off it.
//
// WHY THIS IS NOT A DamageField, AND NOT A ProjectileBuffer
// DamageField.h is right that area weapons must never test pairs. A round is
// FIRE-AND-FORGET: it flies straight, hits at most one agent, and its whole
// cost model rests on never searching for a target. A swarmer is the opposite
// on every axis — it steers, it chooses, it persists on a victim across ticks,
// and it re-chooses when that victim dies. So this is its own layer, with its
// own cost model, stated plainly:
//
//   - An ENGAGED swarmer (latched, or a shooter at its standoff) costs one
//     handle resolve and a few writes per tick. No search. This is the common
//     case by a wide margin.
//   - An UNENGAGED swarmer costs one spatial-hash circle query plus a walk of
//     the named-agent list (tens of entries at most). That is the expensive
//     path, and it runs only on the ticks where a swarmer has just spawned or
//     has just lost its target.
//
// Cost therefore scales with live swarmer count and with how often targets
// die, and is independent of total chaff count.
//
// WHAT THIS LAYER DOES NOT DO ITSELF
// A detonation or a shot lands in some OTHER store: a burst is a DamageField,
// a slow circle is a SlowZone, a splash is fluid, a round is a projectile.
// This layer knows none of those. It records what should happen in
// SwarmerEffects, and SimWorld resolves that list right after update()
// (SimWorld::apply_swarmer_effects). That keeps the kernel testable with a
// chaff store and a hash and nothing else, and keeps the dependency arrows
// pointing at sim/swarm rather than out of it.
//
// IDENTITY
// Chaff targets are held as ChaffHandle, not a raw index: chaff compaction
// swaps live agents down over dead slots every tick, so a stored index would
// silently re-point a swarmer at whichever agent got moved into that slot.
// Named targets are held as EntityId and resolved through the NamedTargetList
// SimWorld rebuilds every tick.
//
// DETERMINISM
// This runs inside the fixed 60 Hz tick and contributes to state_hash(). Same
// seed and same thread count give identical results. Target choice breaks ties
// by ascending chaff index, then by named-list order. The wander that gives
// the cloud its life is driven by a PER-SWARMER seed advanced locally, NOT by
// the shared sim Rng — drawing from the shared stream here would make every
// downstream system's numbers depend on how many swarmers happened to be
// alive.
//
// This layer is SIMULATION. The glow, the trail, and the death pop are not
// here; they live in the renderer and vfx/Particles.h.
#pragma once

#include "core/Types.h"
#include "sim/Attribution.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"

#include <vector>

namespace immune { class Rng; }

namespace immune::sim {

class CombatEventSink;
class DistanceField;
class FlowField;

namespace swarmer_flags {
inline constexpr u8 kAlive       = 1u << 0; ///< Slot occupied.
inline constexpr u8 kPendingKill = 1u << 1; ///< Retire in the next compact().
/// Engaged with its target: a Latch is riding its host, a Shooter is at its
/// standoff and firing. Bombers never carry it — contact IS their end.
inline constexpr u8 kAttached    = 1u << 2;
/// Builder: the goal streams hold a real site. Without it the unit has
/// nowhere to go and only drifts.
inline constexpr u8 kHasGoal     = 1u << 3;
} // namespace swarmer_flags

/// Set on the `visual_id` of every CombatEvent a swarmer raises, so the VFX
/// layer can tell "a swarmer fired / popped" from "the tower released a
/// volley" — both arrive as the same event type with the same source. The low
/// byte is still the tier.
inline constexpr u16 kSwarmerEventBit = 0x100u;

enum class SwarmerKind : u8 {
    Latch = 0,
    Shooter = 1,
    Bomber = 2,
    SlowBomber = 3,
    MucusBomber = 4,
    Builder = 5,
    /// Macrophage unit. Several independently timed branching pseudopods.
    ArborGrabber = 6,
    Count = 7,
};

const char* swarmer_kind_name(SwarmerKind kind);
inline bool swarmer_kind_detonates(SwarmerKind kind) {
    return kind == SwarmerKind::Bomber || kind == SwarmerKind::SlowBomber ||
           kind == SwarmerKind::MucusBomber;
}

/// Everything a swarmer of one type/tier does, in one flat record. Swarmers
/// carry a profile INDEX rather than these numbers, so the store stays lean at
/// five figures of live units, and so a tuning change reaches every swarmer
/// already in flight. The game layer (game/towers) fills these from
/// towers.json; the fields a kind does not use are simply ignored.
struct SwarmerProfile {
    SwarmerKind kind = SwarmerKind::Latch;
    /// Which tower released it — stamped onto every event it raises so the
    /// VFX layer picks the right palette without an ECS lookup.
    TowerType source = TowerType::CytotoxicT;

    // ---- chassis, every kind ----
    /// Seconds before the swarmer retires regardless of what it is doing.
    /// Spawn rate against lifetime sets the standing cloud size.
    f32 lifetime = 3.0f;
    f32 speed = 14.0f;
    /// How far it looks for a target when it has none.
    f32 search_radius = 9.0f;
    /// Contact radius. Latch: latches inside it. Bombers: detonate inside it.
    /// Shooter: its STANDOFF — it stops and fires once inside, chases when out.
    f32 attach_radius = 0.55f;
    /// Body radius. The renderer draws it, and the wall projection keeps this
    /// much (times kWallContactFraction) clear of the vessel.
    f32 size = 0.5f;
    /// Hit points the unit is released with. The horde's own attacks
    /// (sim/hostile) drain this; at zero the unit dissolves. See HEALTH in the
    /// file header.
    f32 max_health = 10.0f;

    // ---- ArborGrabber ---- (attach_radius is each tree's maximum reach)
    u32 arbor_arm_count = 3;
    /// Total chaff enemies an arm can carry after it latches. The initial
    /// target counts toward this cap; 1 reproduces a single-target arm.
    u32 arbor_max_captives = 1;
    /// Chaff within this radius of a newly latched target joins the pull.
    f32 arbor_cluster_radius = 0.0f;
    f32 arbor_extend_seconds = 0.12f;
    f32 arbor_latch_seconds = 0.05f;
    f32 arbor_pull_seconds = 0.22f;
    f32 arbor_recover_seconds = 0.06f;
    /// Visual-only compensation for arm volume, 0..1.
    f32 arbor_fake_mass = 0.65f;

    // ---- every kind with a body (BODIES in the file header) ----
    /// Share of a swarmer/pathogen overlap the PATHOGEN takes. 0 is the
    /// default one-way contact (the swarmer yields all of it); 1 is an
    /// immovable body the pathogen is shoved out of. Filled for
    /// Macrophage, whose rank is the LANE WALL.
    f32 body_block = 0.0f;

    // ---- Latch ----
    f32 dps = 6.0f;
    /// Seconds the ENTRY takes once the unit is inside attach_radius: it
    /// lurches from its clump spot into the host's centre and shrinks to
    /// nothing, and only starts draining once it is all the way in. The
    /// moment the host dies (or the unit lets go) it is back at full size on
    /// the same tick. <= 0 makes the latch instant, the old behaviour.
    f32 attach_seconds = 0.1f;
    /// How many discrete lurches the entry is chopped into. The shrink is
    /// continuous; only the movement is stepped. 0 or 1 is one clean slide.
    u32 attach_steps = 4u;

    // ---- Shooter ----
    f32 fire_interval = 0.25f;
    f32 round_damage = 2.0f;
    f32 round_speed = 40.0f;
    f32 round_hit_radius = 0.45f;
    f32 round_spread = 0.1f;   ///< Aim jitter half-angle, radians.
    /// Distance between squad-mates along the rank.
    f32 formation_spacing = 1.5f;
    /// Kite radius as a fraction of the standoff (attach_radius): an enemy
    /// inside it makes the shooter back away. 0 disables kiting. Keep it
    /// below 1 or the shooter can never hold still inside its own standoff.
    f32 kite_fraction = 0.75f;
    /// How much the flow field bends the retreat. 0 backs straight away from
    /// the nearest threat; 1 weighs "where the horde is going" equally.
    f32 kite_flow_weight = 1.0f;
    /// Retreat speed as a multiple of `speed`. Above 1 a shooter backs off
    /// faster than it advances -- a horde walks, a neutrophil bolts.
    f32 kite_speed_mult = 1.5f;

    // ---- every detonating kind ----
    /// Seconds a bomber will chase one target before giving up and going off
    /// where it is. <= 0 disables the limit. The clock restarts on a new target.
    f32 chase_seconds = 1.0f;

    // ---- Bomber ----
    f32 burst_radius = 3.0f;
    /// Total density removed from an agent at the centre over the burst.
    f32 burst_damage = 20.0f;
    f32 burst_seconds = 0.3f;
    f32 burst_falloff = 0.4f;
    /// Hit points taken off a named agent at the centre, before armor.
    f32 burst_named_damage = 20.0f;

    // ---- SlowBomber ----
    f32 zone_radius = 3.0f;
    f32 zone_duration = 3.0f;
    f32 slow_duration = 1.5f;
    f32 slow_factor = 0.4f;

    // ---- MucusBomber ----
    u32 splash_droplets = 24;
    f32 splash_radius = 1.2f;
    f32 splash_speed = 6.0f;
    f32 splash_lifetime = 2.0f;
    f32 mucus_slow_duration = 3.0f;
    f32 mucus_slow_factor = 0.1f;

    // ---- Builder ----
    // The wall it lays (sim/scar/Scars.h). Half-length runs along the bar,
    // which is laid ACROSS the local flow; half-width is its thickness.
    f32 scar_half_length = 5.0f;
    f32 scar_half_width = 0.9f;
    /// Random tilt off the exact perpendicular, radians, either way: each
    /// wall is laid at across-the-flow +- up to this. 0 = dead square to the
    /// arrows; a little makes a field of walls read as grown, not gridded.
    f32 scar_tilt = 0.0f;
    /// Integrity a fresh scar stands with. The horde spends it exactly as it
    /// spends a tower's: viruses latch on and feed, bacteria burn it.
    f32 scar_health = 250.0f;
    /// Hit points a builder adds to an existing scar when it cannot start a
    /// new one where it arrived (another scar too close, or its owner at
    /// max_scars). Capped at scar_health. 0 = a blocked builder just dissolves.
    f32 scar_reinforce = 60.0f;
    /// No two scar centres closer than this. A builder arriving inside the
    /// spacing of a live scar reinforces it instead.
    f32 scar_spacing = 6.0f;
    /// Live scars one tower may own at once; 0 = unlimited. At the cap the
    /// tower's builders are sent to reinforce instead of to build.
    u32 max_scars = 4;
    /// Seconds a scar stands before dissolving on its own; 0 = permanent
    /// (it stands until the horde chews it down).
    f32 scar_lifetime = 0.0f;
    /// Where a builder may be sent: an annulus around its tower, and the
    /// clearance the site needs. Read by the game layer's site picker at
    /// release (game/towers), carried here so the whole kind is one record.
    f32 build_radius = 14.0f;
    f32 build_min_radius = 4.0f;
    f32 build_min_clearance = 1.0f;
    /// Random sites tried per builder before the tower gives up on it.
    u32 build_candidates = 12;
    /// How much of the crowd's shove a builder takes in the BODIES pass,
    /// 0..1. A fibroblast crawls through the matrix, so 0 by default: a
    /// builder is never pushed by a pathogen (it is still latched and burned
    /// by them), which is what lets it reach a site behind the horde's
    /// front instead of being carried off downstream with the crowd.
    f32 builder_crowd_push = 0.0f;
};

/// How much of a swarmer's `size` the wall projection keeps clear of the
/// vessel. The drawn body edge sits at ~0.84 of `size` (swarmer.frag), so this
/// is "the membrane touches the wall".
inline constexpr f32 kWallContactFraction = 0.8f;

/// Body collision for the non-Latch kinds. See BODIES in the file header for
/// the model. A swarmer's BODY here is `size * kWallContactFraction` -- the
/// same membrane the wall projection keeps off the vessel, and where the
/// renderer draws the edge -- and a pathogen's is its family radius. Every
/// number below is a plain multiplier on those, so the block reads the same
/// for a tiny interferon granule and a fat macrophage.
struct SwarmerCollisionTuning {
    /// Master switch; off reproduces the pre-collision swarm exactly.
    bool enabled = true;

    /// Two swarmers are in contact inside (body_a + body_b) * this.
    f32 friendly_spacing_mult = 1.0f;
    /// Fraction of a friendly-friendly overlap corrected per tick, 0..1. Each
    /// side takes half, so 1.0 resolves a lone pair exactly in one step.
    f32 friendly_stiffness = 1.0f;

    /// A swarmer is in contact with a pathogen inside
    /// (body + pathogen radius) * this.
    f32 enemy_spacing_mult = 1.0f;
    /// Fraction of a swarmer-pathogen overlap corrected per tick, 0..1. The
    /// swarmer takes ALL of it (the pathogen does not move), so 1.0 pops it
    /// clean out; lower values let a charging horde carry units a little way
    /// before they squeeze out, which reads as being shoved.
    f32 enemy_stiffness = 0.6f;

    /// Hard cap on the total displacement one swarmer takes per tick, as a
    /// multiple of its body. Keeps a unit buried in a jam from being launched.
    f32 max_push_mult = 1.0f;
    /// Neighbours one swarmer will inspect in EACH of its two hash walks.
    /// Truncating in CSR order keeps the pass deterministic.
    u32 max_neighbours = 16;

    /// A detonating kind goes off when any enemy is inside
    /// (body + enemy radius) * this, independent of its target's
    /// attach_radius -- and it goes off AT THE POINT OF CONTACT, on its own
    /// membrane, not at its centre. A macrophage is several pathogens wide;
    /// a payload released from its middle would land short of the thing it
    /// touched, and a Goblet Cell's splash is smaller than the cell.
    f32 bomber_contact_mult = 1.0f;
};

/// Profile slots. One per tower type and tier, plus spares for tests and for
/// scripted hazards that want a swarmer of their own.
inline constexpr u16 kSwarmerProfileSlots = 32;
inline u16 swarmer_profile_slot(TowerType type, u8 tier) {
    const u16 t = static_cast<u16>(static_cast<u32>(type) < kTowerTypeCount ? static_cast<u32>(type) : 0u);
    const u16 k = static_cast<u16>(tier >= 3 ? 2u : (tier == 2 ? 1u : 0u));
    return static_cast<u16>(t * 3u + k);
}

struct SwarmerSpawnParams {
    Vec2 position{0.0f, 0.0f};
    /// Initial launch velocity. Steering takes over from the first tick; this
    /// only decides which way the swarmer leaves the cell.
    Vec2 velocity{0.0f, 0.0f};
    /// Index into SwarmerBuffers' profile table.
    u16 profile = 0;
    u8 family_mask = 0xFF;
    /// Owning tower, for kill attribution back to the economy.
    EntityId owner{};
    /// Cosmetic tier selector, passed through to the renderer untouched.
    u16 visual_id = 0;
    /// Seeds this swarmer's private wander stream. Give each one a different
    /// value or the whole cloud flies in perfect formation.
    u32 seed = 0;
    /// Squad identity for shooters: units with the same owner and group hold
    /// one rank, ordered by slot. Ignored by every other kind.
    u32 group = 0;
    u16 slot = 0;
    /// Builders only: where to go and lay the scar. `has_goal` false means
    /// the unit was released with nowhere to build and will only drift.
    Vec2 goal{0.0f, 0.0f};
    bool has_goal = false;
};

/// One named agent (elite/boss) as the swarmer kernel sees it. SimWorld
/// rebuilds the list every tick from the ECS, already filtered to what may be
/// targeted (alive, not Burrowed), and sorted by id so the kernel can resolve
/// a held EntityId with a binary search.
struct NamedTarget {
    EntityId id{};
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    f32 radius = 1.0f;
    /// Flat reduction per damage event (comp::Health::armor).
    f32 armor = 0.0f;
    /// comp::Marked's multiplier, or 1.
    f32 damage_multiplier = 1.0f;
    /// comp::Health::current at the start of the tick.
    f32 health = 1.0f;
    u8 family = 0;
};

struct NamedTargetList {
    std::vector<NamedTarget> items;   ///< Sorted by id.value ascending.
    /// Hit points to take off items[i] this tick, already armor-adjusted and
    /// marked-multiplied. SimWorld applies it after update().
    std::vector<f32> damage;
    /// items[i].health less the damage queued so far this tick -- what the
    /// kernel uses to tell "this hit was the kill" for attribution.
    std::vector<f32> health_left;

    static constexpr usize npos = static_cast<usize>(-1);
    void clear() {
        items.clear(); damage.clear(); health_left.clear();
    }
    /// Appends in any order; call sort() once before handing the list over.
    void add(const NamedTarget& t) {
        items.push_back(t);
        damage.push_back(0.0f);
        health_left.push_back(t.health);
    }
    void sort();
    usize find(EntityId id) const;
};

inline constexpr u32 kArborMaxArms = 3u;
/// Fixed so a five-figure swarmer store never allocates while pseudopods
/// collect a cluster. Per-tier config is clamped to this ceiling.
inline constexpr u32 kArborMaxCaptives = 8u;

struct GrabberChaffCaptive {
    ChaffHandle handle{};
    /// Initial offset from the macrophage. Followers keep their offset from
    /// the lead captive while the whole cluster travels toward the body.
    Vec2 offset{0.0f, 0.0f};
};

struct GrabberCaptive {
    /// A named agent is still a single target: its transform belongs to the
    /// ECS, whereas chaff positions are owned by this simulation kernel.
    EntityId named{};
    GrabberChaffCaptive chaff[kArborMaxCaptives]{};
    u32 chaff_count = 0;
};

enum class ArborArmPhase : u8 { Idle = 0, Extending, Latching, Pulling, Recovering };

struct ArborArmState {
    Vec2 heading{1.0f, 0.0f};
    ArborArmPhase phase = ArborArmPhase::Idle;
    f32 time = 0.0f;
    f32 reach = 0.0f;
    f32 grip = 0.0f;
    GrabberCaptive captive{};
};

/// Persistent state for one Macrophage unit. Three fixed slots match the
/// renderer's three vec4 arm channels and keep the hot path allocation-free.
struct ArborGrabberState {
    Vec2 heading{1.0f, 0.0f};
    ArborArmState arms[kArborMaxArms]{};
};

// ---- What a swarmer asked SimWorld to do. ---------------------------------

struct SwarmerBurst {
    Vec2 origin{0.0f, 0.0f};
    f32 radius = 3.0f;
    f32 damage = 20.0f;         ///< Density at the centre, over `seconds`.
    f32 seconds = 0.3f;
    f32 falloff = 0.4f;
    f32 named_damage = 20.0f;   ///< Hit points at the centre, before armor.
    u8 family_mask = 0xFF;
    EntityId owner{};
    TowerType source = TowerType::Macrophage;
    u16 visual_id = 0;
};

struct SwarmerSlowZone {
    Vec2 origin{0.0f, 0.0f};
    f32 radius = 3.0f;
    f32 duration = 3.0f;
    f32 slow_duration = 1.5f;
    f32 slow_factor = 0.4f;
    u8 family_mask = 0xFF;
    EntityId owner{};
    TowerType source = TowerType::Interferon;
    u16 visual_id = 0;
};

struct SwarmerSplash {
    Vec2 origin{0.0f, 0.0f};
    u32 droplets = 24;
    f32 radius = 1.2f;
    f32 speed = 6.0f;
    f32 lifetime = 2.0f;
    f32 slow_duration = 3.0f;
    f32 slow_factor = 0.1f;
    u8 family_mask = 0xFF;
    EntityId owner{};
    TowerType source = TowerType::GobletCell;
    u16 visual_id = 0;
    u32 seed = 0;
};

struct SwarmerShot {
    Vec2 origin{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    f32 damage = 2.0f;
    f32 hit_radius = 0.45f;
    f32 lifetime = 0.5f;
    u8 family_mask = 0xFF;
    EntityId owner{};
    TowerType source = TowerType::Neutrophil;
    u16 visual_id = 0;
};

/// A builder that reached its site. SimWorld hands it to the scar system
/// (sim/scar), which decides whether a wall goes down there, an existing one
/// is reinforced, or nothing happens; the scar's own numbers are read off
/// the profile the unit carried.
struct SwarmerBuild {
    Vec2 origin{0.0f, 0.0f};   ///< The site it was sent to, not where it stopped.
    u16 profile = 0;
    EntityId owner{};
    TowerType source = TowerType::Fibroblast;
    u16 visual_id = 0;
};

struct SwarmerEffects {
    std::vector<SwarmerBurst> bursts;
    std::vector<SwarmerSlowZone> zones;
    std::vector<SwarmerSplash> splashes;
    std::vector<SwarmerShot> shots;
    std::vector<SwarmerBuild> builds;

    void reserve(usize n);
    void clear();
    bool empty() const {
        return bursts.empty() && zones.empty() && splashes.empty() && shots.empty() &&
               builds.empty();
    }
};

/// The swarmer store. One instance per sim world.
///
/// INVARIANTS:
///   S1. Every index in [0, count) has swarmer_flags::kAlive set.
///   S2. Every stream reports size() == capacity and identical size to the rest.
///   S3. count <= capacity at all times; spawn() never grows a stream.
class SwarmerBuffers {
public:
    // Parallel SoA streams. Public by design, same rationale as ChaffBuffers
    // and ProjectileBuffers: the integration loop and the renderer's instance
    // upload walk them directly. Treat as read-only outside sim/swarm.
    std::vector<f32> pos_x;
    std::vector<f32> pos_y;
    std::vector<f32> vel_x;
    std::vector<f32> vel_y;
    std::vector<f32> life;              ///< Seconds remaining; <= 0 retires it.
    std::vector<f32> cooldown;          ///< Shooter: seconds to the next round.
    std::vector<f32> chase;             ///< Bombers: seconds spent chasing the current target.
    /// Latch: seconds since it began entering its host, capped at the
    /// profile's attach_seconds. Reset to 0 whenever kAttached drops. The
    /// renderer reads it to shrink the body; the kernel gates the drain on it.
    std::vector<f32> attach;
    std::vector<u32> target_index;      ///< ChaffHandle halves, kept as two
    std::vector<u32> target_generation; ///< streams so the SoA stays scalar.
    std::vector<EntityId> target_named; ///< Named target, when the chaff handle is invalid.
    std::vector<u16> profile;
    std::vector<u8>  family_mask;
    std::vector<u8>  flags;
    std::vector<u16> visual_id;
    std::vector<u32> seed;
    std::vector<EntityId> owner;
    std::vector<u32> group;   ///< Squad, with owner. Shooters only.
    std::vector<u16> slot;    ///< Rank order within the squad.
    /// Builders: the site to walk to (valid when kHasGoal is set).
    std::vector<f32> goal_x;
    std::vector<f32> goal_y;
    /// Hit points left (see HEALTH in the file header). <= 0 retires the unit.
    std::vector<f32> health;
    /// Globally unique unit id, never reissued, same arrangement as
    /// ChaffBuffers::generation: a chaff agent latched onto a unit holds
    /// (index, generation) and re-validates it every tick, because compact()
    /// moves units between slots exactly as chaff compaction does.
    std::vector<u32> generation;
    /// Meaningful only for ArborGrabber profiles; independent radial arms.
    /// Kept parallel so compact() carries a unit's arms, mid-flight or
    /// attached, with the unit they belong to.
    std::vector<ArborGrabberState> arbor_grabber;

    /// Reserves every stream. Call once at level load.
    void reserve(usize max_swarmers);

    usize count() const { return count_; }
    usize capacity() const { return capacity_; }
    bool full() const { return count_ >= capacity_; }

    /// Appends one swarmer. Silently drops (returns false) when full — a
    /// missing unit is invisible in a cloud, a mid-tick reallocation is not
    /// acceptable.
    bool spawn(const SwarmerSpawnParams& params);

    /// Flags a swarmer for removal. Removal happens in compact(), so indices
    /// stay stable within a tick.
    void kill(usize index);

    /// Swap-removes every kPendingKill swarmer. Returns the number removed.
    usize compact();

    void clear();

    ChaffHandle target(usize i) const {
        return ChaffHandle{target_index[i], target_generation[i]};
    }

    /// True if slot `index` still holds the unit with id `gen`. A cheap check
    /// because ids are globally unique: no slow-path scan is needed, and a
    /// stale (index, gen) pair simply reads as "gone", which is what the
    /// hostile pass wants -- a passenger whose host was compacted away is
    /// dropped, not re-pointed at whoever moved into the slot.
    bool alive_at(u32 index, u32 gen) const {
        return index < count_ && generation[index] == gen && gen != 0u;
    }

    /// The profile table. Slots are stable for the life of the store; a
    /// tower re-registers its slot whenever it releases a volley, so a config
    /// hot-reload reaches the units already in flight.
    void set_profile(u16 slot, const SwarmerProfile& p);
    const SwarmerProfile& profile_at(u16 slot) const {
        return profiles_[slot < kSwarmerProfileSlots ? slot : 0u];
    }
    const SwarmerProfile& profile_of(usize i) const { return profile_at(profile[i]); }

private:
    usize count_ = 0;
    usize capacity_ = 0;
    /// Next `generation` to hand out. Monotonic and never 0, so 0 can serve
    /// as "no unit" in a held reference.
    u32 next_generation_ = 1;
    SwarmerProfile profiles_[kSwarmerProfileSlots]{};
};

struct SwarmerStats {
    u32 live = 0;
    u32 spawned_this_tick = 0;
    u32 attached = 0;          ///< Engaged with a target this tick.
    u32 searching = 0;         ///< Targetless, paid for a search.
    u32 expired = 0;           ///< Dissolved on lifetime or left the world.
    u32 detonated = 0;         ///< Bombers that went off (contact, expiry, or chase timeout).
    u32 chase_timeouts = 0;    ///< Of those, the ones that gave up a chase.
    u32 shots_fired = 0;
    u32 hosts_finished = 0;    ///< Targets that died under an engaged swarmer.
    u32 retargeted = 0;        ///< Stood their ground and picked a closer target.
    u32 kiting = 0;            ///< Shooters and grabbers backing away from a too-close enemy this tick.
    u32 arms_attached = 0;     ///< Pathogens currently latched by arbor-grabber arms.
    u32 arms_launched = 0;     ///< Arbor-grabber arm cycles begun this tick.
    u32 wall_contacts = 0;     ///< Units pushed back onto the tissue this tick.
    u32 friendly_contacts = 0; ///< Swarmer-swarmer overlaps resolved this tick (both sides counted).
    u32 enemy_contacts = 0;    ///< Swarmer-pathogen overlaps a swarmer was pushed out of this tick.
    u32 body_blocks = 0;       ///< Of those, the pathogens a body_block unit shoved back instead.
    u32 contact_detonations = 0; ///< Of `detonated`, the ones that went off on touching a non-target enemy.
    u32 built = 0;             ///< Builders that reached their site and asked for a scar.
    f32 density_removed = 0.0f;
    f32 named_damage = 0.0f;   ///< Hit points queued against named agents.
};

class SwarmerSystem {
public:
    SwarmerSystem();

    /// One tick: resolve targets, acquire new ones for the targetless, steer,
    /// integrate, drain / fire / detonate, retire the expired, compact.
    ///
    /// `named` is this tick's targetable named agents (may be empty). Damage
    /// a swarmer does to one is ACCUMULATED into `named.damage`, never applied
    /// here — SimWorld owns the ECS and applies it after.
    ///
    /// `sdf` is the level's clearance field, for the wall projection; null
    /// (or unbaked) means no walls, which is what a bare test wants. `flow`
    /// is the level's flow field, which a kiting shooter reads to retreat
    /// ahead of the horde; null means it backs straight away instead.
    ///
    /// `events` may be null. When present, latches, shots and detonations are
    /// reported so the VFX layer can pop; the sim's behaviour must be
    /// byte-identical whether or not a sink is attached.
    ///
    /// Detonations and shots are recorded into effects(); the caller resolves
    /// them (SimWorld::apply_swarmer_effects). Does NOT compact the chaff
    /// store — the caller runs ChaffBuffers::compact() once, after every
    /// damage source has applied.
    SwarmerStats update(SwarmerBuffers& swarmers,
                        ChaffBuffers& chaff,
                        const SpatialHash& hash,
                        NamedTargetList& named,
                        const DistanceField* sdf,
                        const FlowField* flow,
                        const Rect& world_bounds,
                        Rng& rng,
                        f32 dt,
                        CombatEventSink* events);

    const SwarmerStats& last_stats() const { return last_; }

    /// What the last update() asked for. Cleared at the start of every update.
    const SwarmerEffects& effects() const { return effects_; }
    SwarmerEffects& effects() { return effects_; }

    /// Per-owner accounting sink, null by default. See sim/Attribution.h.
    void set_attribution(DamageAttribution* sink) { attribution_ = sink; }

    /// Body collision knobs. Defaults are live; see SwarmerCollisionTuning.
    void set_collision(const SwarmerCollisionTuning& t) { collision_ = t; }
    const SwarmerCollisionTuning& collision() const { return collision_; }

    /// Pathogen body radius per family, for the swarmer-pathogen contact
    /// distance. The chaff store carries no radius stream (it lives on
    /// ChaffFamilyParams), so the caller hands the table over once per level.
    /// `count` entries are read; the rest keep their previous value (0.5).
    void set_chaff_radii(const f32* radii, usize count);

private:
    Vec2 formation_slot(const SwarmerBuffers& sw, usize i, const SwarmerProfile& pr,
                        Vec2 target_pos, f32 hold) const;
    /// The LANE WALL slot (file header) for arbor grabber `i`: its place in
    /// a rank across the flow at the squad's centroid, spaced to the lane.
    Vec2 wall_slot(const SwarmerBuffers& sw, usize i, const SwarmerProfile& pr, Vec2 target_pos,
                   f32 hold, const FlowField* flow, const DistanceField* sdf, f32 dt) const;

    SwarmerStats last_{};
    DamageAttribution* attribution_ = nullptr;
    SwarmerEffects effects_;
    /// Scratch for the targetless swarmers' hash queries. A member so the
    /// vector is allocated once and reused, never per-swarmer inside the tick.
    std::vector<u32> scratch_;
    /// Squad pre-pass scratch (see update()): one key per live shooter or
    /// arbor grabber, and per-swarmer rank / squad size / squad centroid
    /// indexed by slot.
    struct SquadKey {
        u32 owner;
        u32 group;   ///< Squad id: the volley for a shooter, 0 for a wall (one per owner).
        u32 order;   ///< Rank order ahead of slot: the release tick for a wall, 0 otherwise.
        u16 slot;
        u32 index;
    };
    std::vector<SquadKey> squad_scratch_;
    std::vector<u32> squad_rank_;
    std::vector<u32> squad_size_;
    std::vector<f32> squad_cx_;
    std::vector<f32> squad_cy_;

    SwarmerCollisionTuning collision_{};
    f32 chaff_radius_[kFamilyCount]{};   // 0.5 each until set_chaff_radii(); see the ctor
    /// A swarmer-only broadphase for the friendly-friendly contact walk,
    /// rebuilt every tick from post-steering positions. Configured lazily to
    /// mirror the chaff hash's bounds and cell size so a 3x3 walk covers the
    /// same reach in both. Owned here rather than by SimWorld so the kernel
    /// stays testable with a chaff store and one hash, as promised above.
    SpatialHash swarmer_hash_;
    /// Jacobi scratch: the contact pass reads every position and writes only
    /// its own slot's displacement, then applies all of them at once, so the
    /// result is a pure function of positions and not of store order.
    std::vector<f32> push_x_;
    std::vector<f32> push_y_;
    /// Bombers the contact pass found touching an enemy, and where on their
    /// membrane the touch was. update() detonates them there after the pass,
    /// in index order.
    struct ContactBoom {
        u32 index;
        Vec2 at;
    };
    std::vector<ContactBoom> contact_booms_;

    /// The BODIES pass (file header): fills push_*_ and contact_booms_ from the
    /// current positions, then applies the pushes. Detonates nothing itself.
    void resolve_bodies(SwarmerBuffers& sw, ChaffBuffers& chaff, const SpatialHash& hash,
                        const NamedTargetList& named, SwarmerStats& stats);
};

} // namespace immune::sim
