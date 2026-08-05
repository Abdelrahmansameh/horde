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
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune::sim {

/// Per-agent bit flags. Fits in a u8; keep it that way.
namespace chaff_flags {
inline constexpr u8 kAlive      = 1u << 0; ///< Slot occupied.
inline constexpr u8 kMarked     = 1u << 1; ///< Dendritic-cell debuff: takes bonus damage.
inline constexpr u8 kSlowed     = 1u << 2; ///< In a NET / snare field.
inline constexpr u8 kClumped    = 1u << 3; ///< Part of a bacterial biofilm; separation disabled.
inline constexpr u8 kHidden     = 1u << 4; ///< Burrowed; only NK Cells may target.
inline constexpr u8 kDrifting   = 1u << 5; ///< Fungal spore: ignores flow, follows ambient drift.
inline constexpr u8 kReplicated = 1u << 6; ///< Spawned by viral replication (replication budget).
inline constexpr u8 kPendingKill= 1u << 7; ///< Scheduled for removal by the next compact().
} // namespace chaff_flags

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
    std::vector<f32> vel_x;
    std::vector<f32> vel_y;
    std::vector<u8>  family;      ///< PathogenFamily as u8; the renderer batches on this.
    std::vector<f32> density;     ///< HP-as-density contribution (see ChaffSpawnParams).
    std::vector<u8>  flags;       ///< chaff_flags bitset.
    std::vector<u32> generation;  ///< Bumped on despawn; backs ChaffHandle.

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
    void apply_density_loss(usize index, f32 amount);

    /// Swap-removes every kPendingKill agent. Invalidates all raw indices and
    /// the spatial hash. Returns the number removed (feeds kill accounting).
    usize compact();

    /// Resolves a handle to a current index, or npos if the agent is gone.
    static constexpr usize npos = static_cast<usize>(-1);
    usize resolve(ChaffHandle handle) const;

    /// Live agents per family. Maintained incrementally; the renderer reads it
    /// to size its per-family instance batches without a counting pass.
    u32 family_count(PathogenFamily f) const { return family_counts_[static_cast<u32>(f)]; }

    /// Sum of `density` over all live agents — the "how big is the horde really"
    /// number the HUD threat meter and wave-clear check use.
    f32 total_density() const { return total_density_; }

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
};

} // namespace immune::sim
