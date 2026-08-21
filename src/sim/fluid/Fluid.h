// sim/fluid/Fluid.h — SoA fluid-particle store + a real 2D fluid solver.
// Owner: the Goblet Cell (HYDRO) work.
//
// WHAT THIS IS
// A genuine particle-based fluid simulation, not a cosmetic spray. The Goblet
// Cell fires BURSTS of mucus: a dense, fast, tightly-packed column of particles
// that holds together as a coherent beam while it is moving freely, and — the
// whole point — piles up and splashes sideways the instant it runs into a
// vessel wall or into a thick enough patch of horde. None of that splashing is
// scripted. It falls out of the solver.
//
// WHY CLAVET, NOT TEXTBOOK WCSPH
// This uses double-density relaxation from Clavet, Beaudoin & Poulin,
// "Particle-Based Viscoelastic Fluid Simulation" (SCA 2005). Three properties
// earned it the slot over a standard pressure-force SPH loop:
//
//   1. It is POSITION-based. Densities are relaxed by moving particles, and
//      velocity is then recovered as (x - x_prev)/dt. A stiff SPH pressure
//      force needs a tiny dt to stay stable; this stays stable at 1/180 s with
//      a jet moving 34 units/second, which is what keeps the whole thing cheap.
//   2. The NEAR-DENSITY term (the (1-q)^3 sum, always repulsive) is what stops
//      the column from collapsing into strings and clots under its own
//      pressure. A single-density solver at these emission rates clumps
//      visibly within half a second.
//   3. Pressure is allowed to go NEGATIVE below rest density, so particles
//      attract at the surface. That is free surface tension: it is why the
//      beam reads as one connected rope of liquid in flight and why splashed
//      fragments bead up instead of dissolving into a fog of dots.
//
// COLLISION IS PROJECTION, AND THAT IS WHY SPLASHES WORK
// Walls are resolved by pushing a particle back along the tissue DistanceField
// gradient AFTER the relaxation pass, never by reflecting a velocity vector.
// Because velocity is derived from the position delta, a particle shoved out of
// a wall simply loses its normal component, while the particles arriving behind
// it still carry theirs — so pressure builds at the contact point and the next
// relaxation pass ejects the pile tangentially. That IS the splash. Reflecting
// velocities instead gives every particle a mirrored trajectory and reads as a
// bounce, which is exactly the wrong look.
//
// COST MODEL
// One uniform grid rebuild plus two neighbour walks per substep, at three
// substeps a tick. Neighbour count is bounded by construction (rest spacing
// against smoothing radius fixes it near 20), so cost is linear in live
// particle count and completely independent of chaff count. Damage is likewise
// aggregate: particles splat into a coarse coverage grid and the chaff store is
// swept ONCE against that grid, so the fluid never performs a particle-agent
// pair test. That is the same rule sim/damage/DamageField.h is built on and it
// is not negotiable here either.
//
// DETERMINISM
// Runs inside the fixed 60 Hz tick and contributes to state_hash(). The solver
// is deliberately SERIAL: double-density relaxation scatters position
// corrections onto neighbours (x_j moves when i is relaxed), so a parallel
// split would make the result depend on how ranges were carved. Serial in
// ascending index order is the definition here, and it is fast enough that
// trading it for threads would buy nothing worth the risk. The solver also
// draws nothing from the shared sim Rng — emission jitter is hashed from a
// per-jet seed, so how many droplets happen to be alive can never shift any
// other system's numbers.
//
// This layer is SIMULATION. The metaball surfacing, the specular sheen, and the
// droplet spray are NOT here — they live in render/ and vfx/. Deleting every
// one of them must leave state_hash() untouched.
#pragma once

#include "core/Types.h"
#include "sim/Attribution.h"

#include <vector>

namespace immune { class Rng; }

namespace immune::sim {

class ChaffBuffers;
class SpatialHash;
class DistanceField;
class CombatEventSink;

namespace fluid_flags {
inline constexpr u8 kAlive       = 1u << 0; ///< Slot occupied.
inline constexpr u8 kPendingKill = 1u << 1; ///< Retire in the next compact().
/// Latched the first time a particle is stopped hard — by a wall or by the
/// horde. Drives the one-shot splash event and lets the renderer scuff up
/// fluid that has already hit something. The solver never reads it back.
inline constexpr u8 kSplashed    = 1u << 2;
/// In contact with a vessel wall this tick. Drives the flattened, spread-out
/// look the renderer gives fluid that is running along tissue.
inline constexpr u8 kOnWall      = 1u << 3;
} // namespace fluid_flags

/// The knobs of the solver itself. Defaults are the shipped mucus; the whole
/// struct is exposed through assets/config/towers.json so the feel is tunable
/// without a rebuild.
struct FluidTuning {
    /// Interaction radius `h`. Everything else is expressed relative to it.
    /// Roughly 2.4x the rest spacing gives each particle ~20 neighbours, which
    /// is the sweet spot: fewer and the density estimate gets noisy, more and
    /// the inner loops get expensive for no visible gain.
    f32 smoothing_radius = 0.95f;
    /// Spacing a settled particle wants from its neighbours. Rest density is
    /// DERIVED from this at configure() time by summing the kernel over a
    /// hexagonal lattice at that spacing, so the two can never disagree —
    /// hand-tuning a rest density against a spacing is the classic way to end
    /// up with a fluid that silently expands or implodes. It also sets the
    /// emission rate (see FluidSystem::emit) and the drawn particle radius.
    f32 rest_spacing = 0.40f;
    /// Far-field stiffness, in world-units per second squared per unit of
    /// density error. The term is SIGNED: above rest it pushes apart, below
    /// rest it PULLS TOGETHER, which is the surface tension holding the beam
    /// in one piece. Raising it makes a harder, more incompressible liquid.
    f32 stiffness = 520.0f;
    /// Near-field stiffness. Always repulsive, at short range only. This is the
    /// anti-clumping term, and its RATIO to `stiffness` is the single most
    /// delicate number in this struct, because the two together decide what
    /// density the fluid actually settles at.
    ///
    /// The far term is zero at rest density and the near term is not — it is
    /// positive always — so a free-floating blob expands until the far term has
    /// gone far enough NEGATIVE to balance it. Working that balance out over a
    /// uniform patch gives the settled density as roughly
    ///
    ///     rho_settled / rho0  =  1 - 0.30 * near_stiffness / stiffness
    ///
    /// so this ratio is what keeps emitted fluid at the density it was emitted
    /// at. At 0.17 the jet settles about 5% under rest, which is the intent:
    /// slightly loose, so the fluid is free to flow rather than fighting its
    /// own pressure. Push the ratio toward 1 and the beam visibly inflates and
    /// tears itself into a chain of droplets with holes between them; push it
    /// to 0 and nothing stops a compressed pile from knotting into clots.
    f32 near_stiffness = 90.0f;
    /// Linear and quadratic viscosity impulse coefficients. Together they are
    /// the difference between water and mucus: high values make neighbouring
    /// particles agree on a velocity, so the jet travels as a rope and the
    /// splash crawls instead of scattering.
    f32 viscosity_linear = 0.90f;
    f32 viscosity_quadratic = 0.35f;
    /// Ambient velocity damping, per second. Bleeds off a splash so a puddle
    /// settles rather than sliding forever. Kept low enough that the beam is
    /// still travelling near launch speed at the end of its range.
    f32 drag = 0.55f;
    /// Tangential velocity kept when running along a wall. 1 = frictionless.
    f32 wall_friction = 0.72f;
    /// How hard a fully-occupied chaff cell brakes a particle, per second. This
    /// is what turns "the beam reached the horde" into "the beam is splashing
    /// off the horde" without a single pair test.
    f32 chaff_drag = 9.0f;
    /// Occupancy (agents in one spatial cell) treated as a fully solid crowd.
    f32 chaff_full_occupancy = 12.0f;
    /// Hard speed clamp. Safety rail: a particle that outruns `h` in one
    /// substep would tunnel through walls and lose its neighbours.
    f32 max_speed = 70.0f;
    /// Solver substeps per 60 Hz tick. Two is enough at these speeds: the
    /// stability limit is a particle moving a smoothing radius in one
    /// substep, and a 39 unit/second jet covers a third of one.
    u32 substeps = 2;
    /// Side of one coverage-grid cell, in world units.
    f32 coverage_cell_size = 1.0f;
    /// Particle mass in one coverage cell that counts as fully soaked. Damage
    /// and the weaken (chaff_flags::kMarked) it applies both saturate here.
    f32 coverage_full = 5.0f;
    /// Speed lost in one substep that counts as an impact worth an event.
    f32 splash_speed_threshold = 5.5f;
    /// Splash events raised per tick, at most. The sink is shared and a jet
    /// hitting a wall would otherwise drown every other tower's VFX.
    u32 max_splash_events = 24;
};

/// One tick's worth of emission from one nozzle.
struct FluidJetParams {
    Vec2 origin{0.0f, 0.0f};
    /// Unit aim. Normalized defensively by the emitter.
    Vec2 direction{1.0f, 0.0f};
    f32 speed = 34.0f;
    /// Half-angle of the launch cone, radians. Small: this is a jet, not a
    /// shotgun. The spread that matters visually happens on impact.
    f32 spread = 0.045f;
    /// Half-width of the nozzle mouth. Sets how thick the beam is, and — with
    /// the speed — how many particles a tick of firing is worth.
    f32 nozzle_radius = 1.10f;
    /// Multiplier on the geometrically-derived emission rate. 1 launches fluid
    /// at exactly rest density; below 1 gives a thinner, gappier stream.
    f32 flow_scale = 1.0f;
    /// Seconds each emitted particle lives before it evaporates.
    f32 lifetime = 2.0f;
    /// Density removed per second from a FULLY soaked coverage cell.
    f32 damage_per_second = 20.0f;
    u8 family_mask = 0xFF;
    EntityId owner{};
    /// Cosmetic tier selector, handed to the renderer untouched.
    u16 visual_id = 0;
    /// Which burst this belongs to. Bumped once per trigger pull; the renderer
    /// uses it to keep one burst's shading coherent.
    u16 burst_id = 0;
    /// Seeds the per-particle launch jitter and the fractional-count dither.
    /// Advance it every tick or every slab leaves the nozzle in identical
    /// formation and the emission rate quantizes to a whole number.
    u32 seed = 0;
};

/// The fluid particle store. One instance per sim world.
///
/// INVARIANTS:
///   F1. Every index in [0, count) has fluid_flags::kAlive set.
///   F2. Every stream reports size() == capacity and identical size to the rest.
///   F3. count <= capacity at all times; spawn() never grows a stream.
class FluidBuffers {
public:
    // Parallel SoA streams. Public by design, same rationale as ChaffBuffers:
    // the solver and the renderer's instance upload walk them directly. Treat
    // as read-only outside sim/fluid.
    std::vector<f32> pos_x;
    std::vector<f32> pos_y;
    std::vector<f32> vel_x;
    std::vector<f32> vel_y;
    /// Position at the start of the last substep. The solver needs it to
    /// recover velocity after relaxation; the RENDERER reads it as a motion
    /// vector, which is what stretches a fast particle into a streak.
    std::vector<f32> prev_x;
    std::vector<f32> prev_y;
    /// Relaxed far density, normalized against rest. Near 1 in the body of the
    /// jet, well under 1 at the surface — the renderer's thickness cue.
    std::vector<f32> density;
    std::vector<f32> life;       ///< Seconds remaining; <= 0 evaporates it.
    std::vector<f32> life_max;   ///< Lifetime at birth, so age can be normalized.
    std::vector<f32> dps;        ///< This particle's share of the damage rate.
    std::vector<u8>  family_mask;
    std::vector<u8>  flags;
    std::vector<u16> visual_id;
    std::vector<u16> burst_id;
    std::vector<EntityId> owner;

    /// Reserves every stream. Call once at level load.
    void reserve(usize max_particles);

    usize count() const { return count_; }
    usize capacity() const { return capacity_; }
    bool full() const { return count_ >= capacity_; }

    /// Appends one particle. Silently drops (returns false) when full: one
    /// missing droplet in a jet of hundreds is invisible, a mid-tick
    /// reallocation is not acceptable.
    bool spawn(Vec2 position, Vec2 velocity, const FluidJetParams& jet);

    /// Flags a particle for removal. Removal happens in compact(), so indices
    /// stay stable within a tick.
    void kill(usize index);

    /// Swap-removes every kPendingKill particle. Returns the number removed.
    usize compact();

    void clear();

private:
    usize count_ = 0;
    usize capacity_ = 0;
};

struct FluidStats {
    u32 live = 0;
    u32 expired = 0;
    u32 wall_contacts = 0;    ///< Particles touching tissue on the last substep.
    u32 splashes = 0;         ///< Impacts hard enough to latch kSplashed.
    f32 density_removed = 0.0f;
    f32 solve_ms = 0.0f;
};

/// The solver, its emitter, and the coverage grid it damages through.
class FluidSystem {
public:
    /// Sizes the neighbour grid and the coverage grid for a world, and derives
    /// the rest density from the tuning. Call at level load and again whenever
    /// the tuning changes; never during a tick.
    void configure(const Rect& world_bounds, const FluidTuning& tuning);

    const FluidTuning& tuning() const { return tuning_; }
    /// Rest density derived from `tuning().rest_spacing`. Exposed for tests.
    f32 rest_density() const { return rest_density_; }
    /// Radius the renderer should draw one particle at. Half the rest spacing
    /// would leave visible gaps between neighbours once the metaball threshold
    /// is applied, so this is deliberately fatter than the physical spacing.
    f32 draw_radius() const { return tuning_.rest_spacing * 0.85f; }

    /// Emits one tick's slab of jet, and returns how many particles it took.
    ///
    /// WHY THE COUNT IS DERIVED AND NOT PASSED IN
    /// A nozzle of width `w` firing at speed `v` sweeps `w * v * dt` of area in
    /// one tick, and at rest packing that area holds a fixed number of
    /// particles. Emitting more than that is not "a thicker jet" — it is a
    /// density singularity, and the relaxation pass answers it by blowing the
    /// slab apart in a puff before the beam ever forms. So the count comes out
    /// of the geometry, and `flow_scale` is the only dial. The fractional
    /// remainder is dithered from the jet seed rather than truncated, so a rate
    /// of 9.4 particles a tick really does average 9.4 instead of 9.
    ///
    /// WHY THE SLAB IS SPREAD OUT
    /// Particles fill the whole rectangle the nozzle swept this tick -- across
    /// the mouth, and BACK along the aim over exactly `speed * dt`, which is
    /// where continuously-emitted fluid would actually be by now. Every
    /// particle is therefore born already at roughly rest spacing from its
    /// neighbours, and the column is stable from the first frame. The fill uses
    /// a low-discrepancy sequence rather than a grid; see the comment in the
    /// implementation for why a grid puts a periodic hole down the jet.
    u32 emit(FluidBuffers& fluid, const FluidJetParams& jet, f32 dt) const;

    /// One tick: substep the solver, resolve walls, brake against the horde,
    /// splat coverage, damage the chaff, retire the expired, compact.
    ///
    /// `events` may be null. When present, hard impacts are reported so the VFX
    /// layer can throw droplets; the sim's behaviour must be byte-identical
    /// whether or not a sink is attached.
    ///
    /// Does NOT compact the chaff store — the caller runs ChaffBuffers::compact()
    /// once, after every damage source has applied.
    FluidStats update(FluidBuffers& fluid,
                      ChaffBuffers& chaff,
                      const SpatialHash& hash,
                      const DistanceField& sdf,
                      const Rect& world_bounds,
                      f32 dt,
                      CombatEventSink* events);

    const FluidStats& last_stats() const { return last_; }

    /// Per-owner accounting sink, null by default. The Goblet Cell hurts
    /// things only through the coverage grid, so a sink that skipped this path
    /// would rank that tower at zero. See sim/Attribution.h, and the note on
    /// coverage_owner_ below for the one approximation this path makes.
    void set_attribution(DamageAttribution* sink) { attribution_ = sink; }

    // ---- Coverage grid -----------------------------------------------------
    // How wet each patch of the world is, rebuilt every tick from the live
    // particles. This is the ONLY channel through which fluid hurts anything,
    // and it is what keeps the cost independent of chaff count.

    /// Normalized wetness at a world point, 0..1. Bilinear.
    f32 coverage_at(Vec2 world_pos) const;
    const f32* coverage() const { return coverage_.data(); }
    IVec2 coverage_dims() const { return coverage_dims_; }
    Vec2 coverage_origin() const { return bounds_.min; }

private:
    /// Rebuilds the uniform neighbour grid over the current positions.
    void build_grid(const FluidBuffers& fluid);

    FluidTuning tuning_{};
    FluidStats last_{};
    Rect bounds_{Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}};
    f32 rest_density_ = 1.0f;

    // Neighbour grid, CSR. Rebuilt per substep; allocated once per configure().
    IVec2 grid_dims_{0, 0};
    f32 grid_cell_size_ = 1.0f;
    std::vector<u32> cell_start_;   ///< dims.x * dims.y + 1 entries.
    std::vector<u32> cell_fill_;    ///< Scatter cursor, parallel to cell_start_.
    std::vector<u32> sorted_;       ///< Particle indices, grouped by cell.

    // Coverage grid.
    IVec2 coverage_dims_{0, 0};
    std::vector<f32> coverage_;     ///< Particle mass per cell, normalized on read.
    std::vector<f32> coverage_dps_; ///< Damage rate per cell, mass-weighted.
    /// Which emitter contributed the most mass to each cell. Attribution only:
    /// nothing in the sim reads it, and it is only filled when a sink is
    /// attached.
    ///
    /// This is the one place per-owner accounting is an APPROXIMATION rather
    /// than an exact split. The coverage grid deliberately aggregates every
    /// jet into one field -- that aggregation is the whole reason fluid damage
    /// costs the same at 10 agents and 10,000 -- so a cell soaked by two
    /// Goblet Cells at once has no exact per-owner share to recover. Crediting
    /// the largest contributor is right whenever one jet dominates a cell,
    /// which is the normal case; two jets overlapping is a placement the
    /// planner's spacing rule already avoids.
    std::vector<EntityId> coverage_owner_;
    std::vector<f32> coverage_owner_mass_;
    DamageAttribution* attribution_ = nullptr;
};

} // namespace immune::sim
