// vfx/Particles.h — the cosmetic particle layer. FROZEN CONTRACT. Owner: Wave 6B.
//
// THIS IS NOT SIMULATION. Nothing here feeds back into gameplay, damage, or
// state_hash(). It has its own Rng, runs on the variable-rate render clock (not
// the 60 Hz sim tick), and is driven entirely by drained sim::CombatEvents.
// Deleting every particle in the game must leave the sim bit-identical. That
// separation is what lets this layer be as extravagant as the art direction
// wants without putting the 10k-agent budget at risk.
//
// BUILT FOR ABSURD COUNTS
// The Gunner's brief is "at very high fire rates the screen should visually
// become a continuous stream" — that is hundreds of thousands of live
// particles, not thousands. Consequences, all binding:
//
//   - SoA, one flat array per attribute, sized once at init and never grown.
//     kDefaultCapacity is a quarter million; the update loop must stay
//     auto-vectorizable (see CONVENTIONS.md §2-3 — no branches on per-particle
//     state in the hot path that the compiler cannot hoist).
//   - Retirement is a swap-remove compaction, exactly like ChaffBuffers. No
//     per-particle free list, no allocation while emitting.
//   - ONE instanced draw call per blend mode. The CPU never builds per-particle
//     geometry. ParticleInstance's layout is mirrored in the shader, so
//     changing it is a contract change.
//   - Overflow drops the newest particles silently. A dropped spark is
//     invisible; a stall is not.
//
// The update is a pure function of (previous particle state, dt) and is a prime
// JobSystem::parallel_for candidate. It may be parallelized freely: there is no
// determinism requirement here at all, which is exactly why it is kept out of
// the sim.
#pragma once

#include "core/Types.h"
#include "sim/CombatEvents.h"

#include <vector>

namespace immune { class Rng; class JobSystem; }

namespace immune::vfx {

/// Shader-side behaviour selector. The shader branches on this to pick a look;
/// the CPU update treats every kind identically (position/velocity/fade), so
/// adding a kind never costs a CPU branch in the hot loop.
enum class ParticleKind : u8 {
    /// Tiny bright dot with a motion streak. Gunner rounds, orbiting bits.
    Tracer = 0,
    /// Round soft glow that fades. Impact pops, muzzle flashes.
    Spark = 1,
    /// Expanding hollow ring. Explosion shockwaves, freeze PINGs.
    Ring = 2,
    /// Angular shard. Freeze shatter, blade slash fragments.
    Shard = 3,
    /// Soft drifting blob. Digestive mist, lingering cone haze.
    Mist = 4,
    /// Straight segment with a travelling energy texture. Beam cores.
    Beam = 5,
    /// Jagged multi-segment bolt. Tesla arcs.
    Bolt = 6,
    Count = 7,
};

enum class BlendMode : u8 {
    /// Bright, energetic, stacks toward white. Almost everything combat.
    Additive = 0,
    /// Occludes what is behind it. Mist and shards that should read as matter.
    AlphaBlend = 1,
    Count = 2,
};

/// Per-instance attributes. Mirrored exactly in assets/shaders/particle.vert —
/// changing this layout is a contract change. 48 bytes.
struct ParticleInstance {
    f32 x, y;
    f32 vx, vy;        ///< Shader uses velocity to orient streaks/beams.
    f32 size;
    f32 rotation;
    u32 tint_rgba8;    ///< Packed colour x alpha, already faded on the CPU.
    u32 kind_blend;    ///< ParticleKind in the low 16 bits, BlendMode in the high.
    f32 age_norm;      ///< 0 at birth -> 1 at death. Drives shader-side curves.
    f32 seed;          ///< Per-particle randomness, so bolts/shards differ.
    f32 pad0, pad1;
};

struct ParticleSpawnParams {
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    f32 size = 0.2f;
    f32 lifetime = 0.5f;
    /// Per-second velocity damping. 0 = none, higher = snappier settle.
    f32 drag = 0.0f;
    /// World-units/sec^2 applied on +Y. Negative sinks, positive rises.
    f32 buoyancy = 0.0f;
    f32 rotation = 0.0f;
    f32 spin = 0.0f;
    /// Endpoint for Beam/Bolt kinds, which are segments rather than points.
    Vec2 endpoint{0.0f, 0.0f};
    ParticleKind kind = ParticleKind::Spark;
    BlendMode blend = BlendMode::Additive;
};

struct ParticleStats {
    u32 live = 0;
    u32 spawned_this_frame = 0;
    u32 retired_this_frame = 0;
    u64 dropped_total = 0;     ///< Cumulative overflow drops; 0 in a healthy frame.
    f32 update_ms = 0.0f;
};

/// The particle store and its update. One instance per app.
class ParticleSystem {
public:
    /// Quarter of a million live particles. The Gunner alone is expected to
    /// account for a large fraction of this at high tiers.
    static constexpr usize kDefaultCapacity = 262144;

    /// Allocates every stream once. `seed` seeds this layer's own Rng, which is
    /// deliberately independent of the sim's — drawing from the sim's stream
    /// from here would let a cosmetic change alter gameplay.
    void init(usize capacity, u64 seed);
    void shutdown();

    /// Appends one particle. Returns false and bumps the drop counter when
    /// full. Never allocates.
    bool spawn(const ParticleSpawnParams& params);

    /// Translates one drained combat event into a burst. This is where the art
    /// direction lives: the per-tower, per-tier look of every muzzle flash,
    /// impact, explosion, arc, freeze, and slash. Wave 6B owns the authoring.
    void emit_for_event(const sim::CombatEvent& event);

    /// Convenience: drains a whole frame's events through emit_for_event().
    void emit_for_events(const sim::CombatEvent* events, usize count);

    /// Advances every live particle by `dt` (RENDER dt, not the sim tick) and
    /// swap-removes the expired. Safe to parallelize; `jobs` may be null.
    void update(f32 dt, JobSystem* jobs);

    /// Fills `out` with the live instances for one blend mode, ready for a
    /// single instanced draw. Called once per blend mode per frame by the
    /// renderer. Does not allocate if `out` is already reserved.
    void build_instances(BlendMode blend, std::vector<ParticleInstance>& out) const;

    usize live_count() const { return count_; }
    usize capacity() const { return capacity_; }
    const ParticleStats& stats() const { return stats_; }

    /// Retires everything. Called on level transitions so a new level does not
    /// inherit the previous one's sparks.
    void clear();

private:
    std::vector<f32> pos_x, pos_y;
    std::vector<f32> vel_x, vel_y;
    std::vector<f32> size_, rot_, spin_;
    std::vector<f32> age_, life_;
    std::vector<f32> drag_, buoy_;
    std::vector<f32> end_x_, end_y_;
    std::vector<f32> seed_;
    std::vector<u32> color_;
    std::vector<u8>  kind_, blend_;

    usize count_ = 0;
    usize capacity_ = 0;
    ParticleStats stats_{};
    u64 rng_state_ = 0;
};

} // namespace immune::vfx
