// sim/chaff/ChaffBuffers.h — structure-of-arrays chaff storage. FROZEN CONTRACT.
// Owner: Wave 1B.
//
// RATIONALE (DESIGN.md §8.2 — hard constraint)
// There is no ChaffAgent class, and there must never be one. 10,000 agents are
// stored as *parallel flat arrays*, one array per field. Reasons:
//
//  1. The per-tick movement kernel touches only pos/vel/flags. With SoA those
//     three streams are contiguous, so every cache line fetched is 100% useful
//     — an AoS struct would drag family id, HP, and padding through L1 for
//     nothing. This is the difference between hitting and missing §8.6's 4 ms.
//  2. Contiguous f32 streams vectorize. The movement loop is written so MSVC
//     auto-vectorizes it; hand-SIMD is a drop-in later because the layout
//     already matches.
//  3. The renderer uploads instance data by memcpy-ing spans of these arrays
//     into a mapped GPU buffer. No gather, no per-agent transform build.
//  4. Positions are a dense f32 array, which is exactly what the spatial hash
//     and the damage fields want to stream over.
//
// Position is split into separate `pos_x` / `pos_y` arrays rather than a Vec2
// array on purpose: separation and damage-field tests read only one axis at a
// time in their early-out, and SoA-of-scalars is what SIMD wants.
//
// IDENTITY & STABILITY
// An agent's index is NOT stable — `compact()` swaps live agents down over dead
// slots. Anything that must refer to a specific chaff agent across ticks uses
// `ChaffHandle` (index + generation) and resolves it through `resolve()`.
// In practice almost nothing needs to: chaff is fought as a mass, not as units.
//
// CAPACITY
// All arrays are reserved once at level load to `max_agents`. Spawning past
// capacity fails and is reported, rather than reallocating mid-tick.
//
// SQUADS (amendment; see sim/squad/Squads.h)
// `squad_id` is a ninth stream, added when the horde was partitioned into squads
// for readability. It is additive: an agent carrying kNoSquad steers exactly as
// every agent did before the stream existed, so nothing about the four-line
// movement kernel above changed for the ungrouped case. It participates in
// spawn(), clear(), and -- the easy one to miss -- the swap-remove in compact(),
// where an agent must carry its squad membership to its new slot for the same
// reason it carries its generation.
//
// HIT FLASH (amendment; see sim/chaff/HitFlash.h)
// `hit_flash` is a tenth stream, and the only PURELY COSMETIC one. It exists
// because "this agent was hit a moment ago" is state that has to survive the
// frames it takes to fade, and an agent's own slot is the only place that state
// can live -- the renderer cannot hold it, because compact() reshuffles indices
// under it every tick.
//
// It obeys every rule the other ten do (parallel, reserved once, carried
// through the swap-remove) and exactly one they do not: NOTHING IN THE SIM
// READS IT. Damage writes it, the movement kernel decays it, and it is absent
// from SimWorld::state_hash(). That is what keeps a cosmetic stream from
// becoming a gameplay input by accident.
#pragma once

#include "core/Types.h"
#include "sim/chaff/HitFlash.h"  // the per-family table apply_density_loss reads
#include "sim/squad/Squads.h"    // kNoSquad, the default for the squad_id stream

#include <vector>

namespace immune::sim {

/// Per-agent bit flags. Fits in a u8; keep it that way.
namespace chaff_flags {
inline constexpr u8 kAlive      = 1u << 0; ///< Slot occupied.
/// Weaken debuff: every damage source in the sim (DamageField.cpp,
/// Projectiles.cpp, Swarmers.cpp) checks this bit and, if set, multiplies its
/// own damage by kMarkedDamageMultiplier. Originally the Dendritic Cell's and
/// the old B-Cell's job; the Goblet Cell's mucus coverage is the current (and
/// only) source — see sim/fluid/Fluid.cpp. Nothing ever clears the bit once
/// set: a soaked agent stays weakened for the rest of its life, not just while
/// fluid is actively touching it.
inline constexpr u8 kMarked     = 1u << 1;
/// Slow debuff. Unlike kMarked this one is TIMED: `slow_remaining` counts down
/// and sim/zone/SlowZones.cpp clears the bit when it reaches zero, so a slow
/// is something an agent walks out of. While set, the movement kernel scales
/// the family's max speed by `slow_factor`. The Interferon's slow zones are the
/// only source today.
inline constexpr u8 kSlowed     = 1u << 2;
/// Burrowed. Untargetable by every tower's swarmers (sim/swarm/Swarmers.cpp)
/// and by the aim-point search in game/towers; the aggregate damage paths
/// (fields, fluid) still touch it, since they do not target at all.
inline constexpr u8 kHidden     = 1u << 3;
inline constexpr u8 kDrifting   = 1u << 4; ///< Ignores flow, follows ambient drift.
inline constexpr u8 kReplicated = 1u << 5; ///< Spawned by viral replication (replication budget).
/// Latched onto a FRIENDLY host -- a tower or a swarmer -- and feeding on it
/// (sim/hostile/HostileAttacks.h). While set, the movement kernel treats the
/// agent exactly as it treats kHidden: no flow, no separation, no jitter, no
/// replication, no wall response. The hostile pass owns its position instead
/// and rides it on the host; `host_index` / `host_generation` / `host_kind`
/// below say which host. Cleared the tick the host dies, after which the agent
/// resumes walking from wherever it was dropped. Every damage source still
/// hits it -- a latched virus is exactly the one a tower's swarmers should
/// be killing -- but the swarmer BODIES pass ignores it (it is inside its
/// host's membrane by design) and a kiting shooter does not flee from it.
inline constexpr u8 kLatched    = 1u << 6;
inline constexpr u8 kPendingKill= 1u << 7; ///< Scheduled for removal by the next compact().

/// The one weaken multiplier every kMarked-consuming damage path uses, so the
/// number cannot drift between DamageField, Projectiles, Swarmers, and the
/// named-agent comp::Marked component in sim/ecs/Components.h.
inline constexpr f32 kMarkedDamageMultiplier = 1.5f;
/// What `slow_factor` holds for an agent nothing has slowed yet, and what a
/// spawn that arrives already carrying kSlowed (a test, a scripted hazard)
/// gets: a slow zone overwrites it with its own factor, but a bare flag still
/// has to mean "slowed" rather than silently doing nothing.
inline constexpr f32 kDefaultSlowFactor = 0.4f;
} // namespace chaff_flags

/// What ChaffBuffers::host_kind holds: which store `host_index` points into
/// for a kLatched agent. kNone for everyone else.
namespace host_kind {
inline constexpr u8 kNone = 0;
inline constexpr u8 kSwarmer = 1;   ///< host_index / host_generation name a SwarmerBuffers slot.
/// host_index is an EntityId::value. host_generation is unused for a tower
/// and, for a scar (a BAR host, sim/hostile), holds the passenger's spot on
/// the wall's perimeter as a fixed-point fraction -- the one per-passenger
/// word the pass has, and a bar's face has no "direction from the centre"
/// to hash a spot from the way a disc does.
inline constexpr u8 kTower = 2;
} // namespace host_kind

/// Stable reference to a chaff agent across compaction. Rarely needed.
struct ChaffHandle {
    u32 index = 0;
    u32 generation = 0;
    bool valid() const { return generation != 0; }
};

struct ChaffSpawnParams {
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    PathogenFamily family = PathogenFamily::Virus;
    /// HP expressed as a density contribution, NOT as a hit-point pool.
    /// Aggregate damage (sim/damage) subtracts from this; at <= 0 the agent is
    /// flagged kPendingKill. Tankier families simply start higher.
    f32 density = 1.0f;
    u8 flags = 0;
    /// Squad this agent joins, or kNoSquad for an ungrouped agent (which steers
    /// exactly as chaff did before the squad layer existed).
    u16 squad_id = kNoSquad;
    /// Purely visual replication split ramp. Its sign identifies the two
    /// complementary halves; its magnitude starts at 1 and decays to zero.
    f32 replication_pulse = 0.0f;
    /// The parent's position at the instant it divided. Both descendants keep
    /// it while the renderer pulls their visible halves apart from this point.
    Vec2 replication_origin{0.0f, 0.0f};
};

/// The chaff store. One instance per sim world.
///
/// INVARIANTS (asserted in debug, checked by --sim-test):
///   I1. Every index in [0, count) has chaff_flags::kAlive set.
///   I2. Every array reports size() >= count and identical size to every other.
///   I3. count <= capacity at all times; spawn() never grows an array.
///   I4. density[i] > 0 for every alive agent after compact().
class ChaffBuffers {
public:
    // ---- Parallel SoA streams. Public by design: the hot loops in
    // ChaffSystem, SpatialHash, DamageSystem, and the renderer's instance
    // upload all iterate them directly. Treat as read-only outside sim/chaff.

    std::vector<f32> pos_x;
    std::vector<f32> pos_y;
    /// Position at the beginning of the latest fixed simulation tick. The
    /// renderer interpolates from this snapshot to pos_x/pos_y, so a 120 Hz
    /// simulation remains visually continuous on displays that refresh at a
    /// different rate. Purely visual: gameplay always reads pos_x/pos_y.
    std::vector<f32> prev_pos_x;
    std::vector<f32> prev_pos_y;
    std::vector<f32> vel_x;
    std::vector<f32> vel_y;
    /// Low-pass-filtered per-agent wander. Keeping this state turns the old
    /// independent impulse every tick into a slowly changing organic drift.
    std::vector<f32> wander_x;
    std::vector<f32> wander_y;
    std::vector<u8>  family;      ///< PathogenFamily as u8; the renderer batches on this.
    std::vector<f32> density;     ///< HP-as-density contribution (see ChaffSpawnParams).
    std::vector<u8>  flags;       ///< chaff_flags bitset.
    std::vector<u32> generation;  ///< Bumped on despawn; backs ChaffHandle.
    /// Which squad this agent belongs to, or kNoSquad (sim/squad/Squads.h).
    ///
    /// A SEPARATE u16 stream rather than bits stolen from `flags`, for two
    /// reasons. `flags` is a u8 and is nearly full (only bit 6 is unused), so
    /// there is no room for an id there at all. And a squad id is not a
    /// predicate: the movement kernel uses it to index a table and to compare
    /// against a neighbour's id, neither of which wants a mask-and-shift in the
    /// inner loop. At 16k agents the whole stream is 32 KB and stays resident in
    /// L2 across the neighbour gather, which is the only pass that reads it
    /// randomly.
    std::vector<u16> squad_id;

    /// How brightly this agent is still flashing from the last damage it took,
    /// 1 at the instant of the hit and decaying linearly to 0 over the family's
    /// HitFlashParams::duration. See the HIT FLASH note in this file's header.
    ///
    /// A FULL f32 rather than a u8 of the flags byte, even though the renderer
    /// quantizes it to 8 bits on the way to the GPU. The stream is decayed by
    /// `dt / duration` every tick, and at a 0.12 s duration that is a step of
    /// about 1/7 -- quantizing the STORED value would round that step and make
    /// the fade land on a different number of frames depending on where in the
    /// ramp an agent happened to start. Four bytes an agent is 64 KB at
    /// capacity, and the decay pass is the only thing that streams it.
    std::vector<f32> hit_flash;

    /// Remaining signed 0..1 replication split animation. Like hit_flash this is
    /// visual-only: movement and combat never read it, and state_hash omits it.
    /// A parent and its daughter both receive 1 at the instant of a viral split
    /// so the one body visibly becomes two newborn bodies instead of popping a
    /// second full-size sprite into the crowd.
    std::vector<f32> replication_pulse;
    std::vector<f32> replication_origin_x;
    std::vector<f32> replication_origin_y;

    /// The timed half of chaff_flags::kSlowed. `slow_remaining` is seconds of
    /// slow left (meaningful only while the bit is set; a slow zone refreshes
    /// it every tick an agent stays inside), and `slow_factor` is the max-speed
    /// multiplier the movement kernel applies while it lasts. Both are sim
    /// state and both are in SimWorld::state_hash(): the bit alone is not the
    /// whole debuff any more.
    std::vector<f32> slow_remaining;
    std::vector<f32> slow_factor;

    /// The host a chaff_flags::kLatched agent is riding (sim/hostile). Three
    /// streams rather than bits stolen from `flags` for the same reason
    /// squad_id is: a host is an identity, not a predicate, and `flags` has
    /// exactly one bit left, which kLatched itself took. Read by nothing in the
    /// movement kernel -- the kernel only tests the flag -- so they cost the
    /// hot path nothing; the hostile pass is the only reader and it walks them
    /// once, in index order. Sim state (a different host is a different
    /// position next tick), but not hashed directly: the flag bit and the
    /// position the pass writes are already in state_hash(), and a host
    /// mismatch cannot survive a tick without moving one of those.
    std::vector<u32> host_index;
    std::vector<u32> host_generation;
    std::vector<u8>  host_kind;

    /// Which way a kLatched agent is facing: radians, the direction from the
    /// agent toward the point on its host it is feeding at (into a disc
    /// host's centre; down a bar host's face normal). PURELY COSMETIC, under
    /// the same rules as hit_flash: the hostile pass writes it when it plants
    /// a passenger, the chaff batcher reads it to turn the sprite so the latch
    /// throb (render/LatchThrob.h) pumps TOWARD the host, nothing in the sim
    /// reads it and state_hash() omits it. Meaningless unless kLatched is set.
    std::vector<f32> latch_heading;

    /// Reserves every stream to `max_agents`. Call once at level load.
    void reserve(usize max_agents);

    /// Number of live agents. Streams are valid over [0, count).
    usize count() const { return count_; }
    usize capacity() const { return capacity_; }
    bool full() const { return count_ >= capacity_; }

    /// Appends one agent. Returns an invalid handle if at capacity.
    ChaffHandle spawn(const ChaffSpawnParams& params);

    /// Flags an agent for removal. Does not move anything — removal happens in
    /// compact(), once per tick, so indices are stable within a tick.
    void kill(usize index);

    /// Subtracts from density and flags kPendingKill when it reaches zero.
    /// This is the ONLY way chaff takes damage; there is no per-unit hit path.
    ///
    /// Being the only one is also why the hit flash is raised HERE rather than
    /// in each of the four callers (DamageField, Projectiles, Swarmers, Fluid).
    /// A feedback effect that some damage sources trigger and others silently
    /// do not is worse than none at all -- the player learns to distrust it --
    /// and a choke point that is already documented as the only path is the one
    /// place that cannot be forgotten by whatever the fifth damage source turns
    /// out to be. Costs one table read and a handful of float ops per damaged
    /// agent, and nothing at all for a family with `enabled = false`.
    void apply_density_loss(usize index, f32 amount);

    /// Swap-removes every kPendingKill agent. Invalidates all raw indices and
    /// the spatial hash. Returns the number removed (feeds kill accounting).
    ///
    /// `removed_by_family`, when non-null, points at kFamilyCount counters that
    /// are ADDED to (never cleared) with this pass's removals. Compaction is the
    /// one place that sees every retirement regardless of cause, which is what
    /// makes it the honest denominator for the balance report: SimWorld
    /// subtracts the leak/out-of-bounds tallies ChaffSystem hands it to recover
    /// "killed by the player's towers" exactly, rather than inferring it.
    usize compact(u32* removed_by_family = nullptr);

    /// Resolves a handle to a current index, or npos if the agent is gone.
    static constexpr usize npos = static_cast<usize>(-1);
    usize resolve(ChaffHandle handle) const;

    /// Live agents per family. Maintained incrementally; the renderer reads it
    /// to size its per-family instance batches without a counting pass.
    u32 family_count(PathogenFamily f) const { return family_counts_[static_cast<u32>(f)]; }

    /// Lifetime spawn tally per family, including replication. Survives
    /// compact(); reset by reserve()/clear(). The balance report's "how much
    /// did this level actually throw at the player" denominator.
    const u64* spawned_by_family() const { return spawned_by_family_; }

    /// Sum of `density` over all live agents — the "how big is the horde really"
    /// number the HUD threat meter and wave-clear check use.
    ///
    /// `total_density_` is a running accumulator (+= on spawn, -= on damage/
    /// compact), not recomputed from the live array. Over many waves of small
    /// per-tick float32 subtractions it can drift to a tiny nonzero residual
    /// even once every agent is gone — compact() only clamps the *negative*
    /// case, so a positive residual persists forever and the wave-clear
    /// checker's `<= 0.0f` test never fires, silently forcing the full
    /// Clearing-phase timeout on every single wave. Since zero live agents
    /// means zero density by definition, short-circuit on count_ so this
    /// check is exact regardless of accumulated drift.
    f32 total_density() const { return count_ == 0 ? 0.0f : total_density_; }

    void clear();

private:
    /// Debug-only check of I1-I4. Compiles to nothing under NDEBUG.
    void assert_invariants() const;

    usize count_ = 0;
    usize capacity_ = 0;
    f32 total_density_ = 0.0f;
    /// Monotonic agent-id source backing ChaffHandle::generation. See the note
    /// at the top of ChaffBuffers.cpp for why this is not a per-slot counter.
    u32 next_generation_ = 1;
    u32 family_counts_[kFamilyCount] = {};
    u64 spawned_by_family_[kFamilyCount] = {};
};

} // namespace immune::sim
