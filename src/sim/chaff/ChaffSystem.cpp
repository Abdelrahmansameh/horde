// Batch chaff movement kernel. Owner: Wave 1B. Fluid-feel rules and wall
// contact added by the movement overhaul (see ChaffSystem.h's tuning fields).
//
// FOUR PASSES, ONE TICK
//
//   A. accumulate  (parallel, per-range Rng)   — flow/drift, then the three
//      local crowd rules (separation, alignment, crowd pressure) from ONE
//      neighbour gather, then jitter, into vel_x/vel_y (UNCLAMPED); plus the
//      per-agent effective max speed and replication rolls into scratch. This
//      is the pass that touches the spatial hash, so it is inherently
//      gather-heavy and does not vectorize — that cost is unavoidable and is
//      what the spatial hash's O(1) cell lookup exists to minimize.
//   B. integrate   (serial, straight-line)     — clamp_length + p += v*dt over
//      contiguous vel_x/vel_y/scratch_max_speed/old_pos_{x,y}/pos_{x,y}. No
//      branches on flags, no hash access, no gather: this is the loop
//      docs/ARCHITECTURE.md means by "the movement loop is written so MSVC
//      auto-vectorizes it" (verified below).
//   B2. wall contact (parallel, SDF gather)    — projects agents out of tissue
//      they overlap and cancels the inbound part of their velocity. Separate
//      from B so B stays vectorizable.
//   C. despawn     (serial)                    — bounds/goal test against the
//      now-final position, flags kPendingKill. Cheap, branchy, not perf-critical.
//
// Splitting out B is also what makes the result scheduling-independent: pass A
// writes only the index each range owns and reads neighbours through snapshots
// of positions AND velocities taken *before* pass A runs (old_pos_*, old_vel_*),
// never through the live arrays another range may already have overwritten.
//
// WHY THIS LOOKS LIKE A FLUID WITHOUT ANY FLUID MATH
// There is no density solve, no pressure projection, no smoothing kernel —
// nothing from SPH or continuum crowds, both of which would mean replacing the
// flow field rather than extending it. Every agent runs the same three cheap
// local rules against its own neighbourhood, and the collective motion (waves
// travelling back through a jam, a mass splashing along a wall and rejoining
// downstream) is emergent, exactly as in Reynolds' boids. Cohesion is
// deliberately omitted: the flow field already supplies "everyone go that way",
// so a cohesion term would only fight it and ball the horde up.
#include "sim/chaff/ChaffSystem.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Rng.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/HitFlash.h"
#include "sim/chaff/ReplicationSplit.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/RuntimeBlock.h"
#include "sim/spatial/SpatialHash.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace immune::sim {
namespace {

constexpr f32 kSeparationEpsSq = 1e-8f;

/// Everything one agent learns about its neighbourhood, from ONE walk of the
/// 3x3 cells around it.
struct NeighbourSample {
    Vec2 separation{0.0f, 0.0f};   ///< Mean normalized push-away, weighted by closeness.
    /// Which way the crowd ISN'T, over the whole alignment neighbourhood:
    /// every neighbour's unit push-away summed, weighted 1 at zero distance
    /// down to 0 at alignment_radius. Raw here; `crowd_weight` below is its
    /// divisor and the comment there is where the reasoning lives.
    ///
    /// Deliberately NOT the separation sum, which was the first thing tried
    /// here. Separation lives between contact spacing and separation_radius --
    /// a shell about a tenth of a unit thick once contact has done its job --
    /// so its sum collapses to nothing at exactly the density this is meant to
    /// act on, and relief measured as a 3% effect. The gradient has to be read
    /// over the longest range already being scanned or it cannot see the space
    /// it is supposed to spend.
    Vec2 crowd_gradient{0.0f, 0.0f};
    /// The same weights, summed WITHOUT their directions.
    ///
    /// `crowd_gradient / crowd_weight` is a genuine mean push-away direction
    /// whose LENGTH is how much the neighbourhood agrees: ~1 when every
    /// neighbour is on one side (a face with open space in front of it), ~0
    /// when they surround the agent evenly (a jam interior with nowhere to go).
    /// That ratio is the honest form of the "how many of them agree" signal the
    /// raw sum was reaching for, and the reason the raw sum could not deliver
    /// it is that cancellation is not exact: over n neighbours the leftover
    /// grows like sqrt(n), so an enclosed agent's residual GROWS with density
    /// and then gets rescaled to full length by any normalize-and-clamp on top.
    /// Read as a sum, "surrounded" and "free on one side" become the same
    /// number, and the direction separating them is noise.
    f32 crowd_weight = 0.0f;
    Vec2 avg_velocity{0.0f, 0.0f}; ///< Mean neighbour velocity, for alignment.
    /// Positional correction that resolves actual interpenetration, AVERAGED
    /// over the neighbours that are actually overlapping this agent.
    ///
    /// WHY AVERAGED, WHEN THE OBVIOUS ANSWER IS "SUM THEM" (this used to sum)
    /// Each overlapping pair is a constraint, and the correction stored here is
    /// the projection that satisfies ONE of them: move half the overlap, and
    /// the neighbour independently moves the other half. That is exact for a
    /// lone pair. Applying n such projections at once is not: they are solved
    /// simultaneously against the SAME pre-tick snapshot, so each one is
    /// computed as if it were the only correction being made, and summing them
    /// applies roughly n times the displacement the configuration actually
    /// needs. This is the standard failure mode of a Jacobi constraint solve,
    /// and the standard fix is the same one used here -- average the
    /// projections per particle (Macklin et al., "Unified Particle Physics for
    /// Real-Time Applications", §3, which averages exactly this way and then
    /// re-adds a bounded over-relaxation on top).
    ///
    /// It matters at precisely the density it was supposed to help at. Below
    /// contact packing an agent has one or two overlaps and sum == average.
    /// Past it n grows, the solve over-relaxes by that factor, and it does not
    /// settle -- it overshoots, reverses, and overshoots the other way, at one
    /// full cycle per tick. Measured on 1,100 agents packed to 1.2 units
    /// (contact distance 1.53): mean heading change per tick 104 degrees, 64%
    /// of ticks reversing direction outright, per-tick travel 1.85x what
    /// max_speed allows -- a visible 60 Hz shimmer, worst on viruses because a
    /// virus is small enough for ordinary crowding to put it there.
    ///
    /// And the overshoot did not even buy tighter packing: with the pass ON,
    /// the same crowd's worst overlap measured 0.63 units against 0.40 with it
    /// disabled entirely. A diverging solver is worse than no solver.
    /// Averaging converges instead: same jam, 0.32, at 1.1 degrees per tick.
    Vec2 contact_push{0.0f, 0.0f};
    u32 crowd = 0;                 ///< Neighbours inside alignment_radius; drives pressure.
    bool has_alignment = false;
};

/// Radius-sized neighbourhood gather for agent `i`, reading positions and
/// velocities through the pre-tick snapshots (see file header for why the
/// snapshots exist at all).
///
/// WHY ONE FUNCTION AND NOT THREE
/// Separation, alignment and crowd-pressure each need "the agents near me".
/// Fetching that three times would triple the only genuinely expensive thing in
/// this kernel -- the gather -- to compute three cheap sums. So the walk happens
/// once and all three accumulate off it. The per-neighbour body below is a
/// handful of multiply-adds; the loop around it is the cost.
///
/// Uses the raw cell accessors per SpatialHash.h's guidance: this runs once per
/// agent per tick and must not touch a std::vector.
NeighbourSample gather_neighbours(const SpatialHash& hash,
                                  const f32* px, const f32* py,
                                  const f32* vx, const f32* vy,
                                  const u16* squad, u16 my_squad,
                                  f32 foreign_radius_mult, f32 foreign_strength_mult,
                                  usize i, f32 sep_radius, f32 align_radius,
                                  const u8* family, const f32* contact_radii,
                                  f32 max_contact_radius, f32 contact_stiffness,
                                  u32 max_sampled) {
    NeighbourSample out;
    // Squad membership is deliberately irrelevant to local crowd physics.
    // Making another squad repel harder -- and excluding it from alignment --
    // phase-separated mixed waves into shells and made a surrounded family
    // stall. Squads still provide broad lane/centroid cohesion below, while
    // the bodies themselves now behave as one continuous fluid horde.
    (void)squad;
    (void)my_squad;
    (void)foreign_radius_mult;
    (void)foreign_strength_mult;
    const f32 contact_radius = contact_radii[family[i] < kFamilyCount ? family[i] : 0];
    const f32 scan_radius =
        math::max(math::max(sep_radius, align_radius),
                  (contact_radius + max_contact_radius) * 0.5f);
    if (scan_radius <= 0.0f) return out;

    const Vec2 p{px[i], py[i]};
    const IVec2 c = hash.cell_coord(p);
    const IVec2 dims = hash.grid_dims();
    const f32 sep_r2 = sep_radius * sep_radius;
    const f32 align_r2 = align_radius * align_radius;
    const f32 inv_align = align_radius > 0.0f ? 1.0f / align_radius : 0.0f;
    const u32* indices = hash.indices();

    // Cell visit order: the agent's OWN cell first, then the ring around it.
    //
    // The budgets below truncate this scan, so scan order decides WHICH
    // neighbours an agent gets to keep when it cannot afford them all -- and
    // row-major order answered "the ones down and to the left". Every agent in
    // a packed cell then pushed off the same lopsided sample, which is a bias
    // pointing one way per cell: a 4-unit grid pressed into a crowd whose whole
    // job is to look organic. Worse, an agent's own cell is FIFTH in row-major
    // order, so at high density the budget ran out before reaching the agents
    // it was actually overlapping -- contact resolution switched itself off
    // exactly where it was needed, and the horde stayed interpenetrated.
    //
    // Own-cell-first fixes both. What gets dropped is now the far ring, which
    // feeds alignment -- an average, and averages survive subsampling -- rather
    // than contact, which is a sum over specific overlapping bodies and does
    // not.
    static constexpr i32 kCellOrder[9][2] = {
        {0, 0},
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
    };

    // Iteration cap. `max_sampled` rations the crowd rules; this rations the
    // WALK, and it is what keeps the contact exemption below from turning a jam
    // into an unbounded scan. A visit that only tests for contact is a subtract
    // and a compare -- no square root, no gather, no accumulation -- so a wider
    // walk is much cheaper per step than the sampled work it is protecting.
    const u32 max_visited = max_sampled * 8u;

    Vec2 sep{0.0f, 0.0f};
    Vec2 vel{0.0f, 0.0f};
    Vec2 contact{0.0f, 0.0f};
    Vec2 gradient{0.0f, 0.0f};
    f32 gradient_weight = 0.0f;
    u32 sep_count = 0;
    u32 contact_count = 0;
    u32 align_count = 0;
    u32 sampled = 0;
    u32 visited = 0;

    // A bacterium's contact/separation range can exceed a cell width. A fixed
    // 3x3 scan then drops real contacts as either body crosses a grid boundary.
    // Keep the original inner-cell order and budget, extending outward only
    // as far as this agent's rules require (also works after a size hot reload).
    const i32 rings = math::min(static_cast<i32>(std::ceil(scan_radius / hash.cell_size())),
                               math::max(dims.x, dims.y));
    const i32 width = 2 * rings + 1;
    i32 ring = 0, ring_cell = 0, ring_cells = 1;
    for (i32 cell = 0; cell < width * width; ++cell) {
        if (ring_cell == ring_cells) {
            ++ring;
            ring_cell = 0;
            ring_cells = 8 * ring;
        }
        const i32 offset = ring_cell++;
        i32 ox = 0, oy = 0;
        if (ring == 1) {
            ox = kCellOrder[offset + 1][0];
            oy = kCellOrder[offset + 1][1];
        } else if (ring > 1) {
            const i32 side = offset / (2 * ring);
            const i32 along = offset % (2 * ring);
            if (side == 0) { ox = -ring + along; oy = -ring; }
            if (side == 1) { ox = ring; oy = -ring + along; }
            if (side == 2) { ox = ring - along; oy = ring; }
            if (side == 3) { ox = -ring; oy = ring - along; }
        }
        const i32 cx = c.x + ox;
        const i32 cy = c.y + oy;
        if (cx < 0 || cy < 0 || cx >= dims.x || cy >= dims.y) continue;
        u32 begin, end;
        hash.cell_range(static_cast<u32>(cy) * static_cast<u32>(dims.x) +
                            static_cast<u32>(cx),
                        begin, end);
        const u32 cell_count = end - begin;
        // Rotate the starting slot per agent. A hard CSR-prefix budget made
        // every dense-cell resident react to the same low-index subset and
        // permanently ignored the tail, creating directional clumps and pairs
        // that could remain overlapped forever. Rotation keeps the same cost
        // ceiling while distributing attention evenly through the cell.
        u32 local = cell_count > 0
                        ? static_cast<u32>((static_cast<u64>(i) * 2654435761ULL) % cell_count)
                        : 0u;
        for (u32 step_index = 0; step_index < cell_count; ++step_index) {
            if (visited >= max_visited) goto done;
            const u32 j = indices[begin + local];
            if (++local == cell_count) local = 0u;
            if (j == i) continue;
            const f32 dx = p.x - px[j];
            const f32 dy = p.y - py[j];
            const f32 d2 = dx * dx + dy * dy;
            if (d2 < kSeparationEpsSq) continue;
            ++visited;

            // TWO budgets, not one. Contact does not draw on `max_sampled`.
            //
            // Sharing one counter looked cheap and was not: contact radius is a
            // fraction of alignment radius, so the disc that feeds alignment
            // holds several times as many agents as the disc that feeds
            // contact. In a jam the far neighbours therefore spend the entire
            // budget first, and the pass that keeps bodies out of each other
            // gets nothing -- the failure mode being that overlap resolution
            // vanishes as density rises, which is the one density where it is
            // load-bearing. Contact is self-limiting on its own terms anyway:
            // geometry bounds how many agents fit inside contact_radius once
            // they are no longer allowed to interpenetrate.
            // Both bodies must agree on their shared spacing. Using only my
            // diameter made a small virus push less than its large neighbour,
            // so mixed crowds acquired a spurious impulse and stayed overlapped.
            const f32 pair_contact = (contact_radius +
                contact_radii[family[j] < kFamilyCount ? family[j] : 0]) * 0.5f;
            const bool in_contact = d2 < pair_contact * pair_contact;
            const bool has_budget = sampled < max_sampled;
            if (!in_contact && !has_budget) continue;

            const bool in_crowd = has_budget && (d2 < align_r2 || d2 < sep_r2);
            if (!in_contact && !in_crowd) continue;
            if (in_crowd) ++sampled;

            // One square root, shared by every rule that needs a real distance.
            const f32 d = std::sqrt(d2);
            const f32 inv_d = 1.0f / d;
            const f32 nx = dx * inv_d;   // unit vector from neighbour to me
            const f32 ny = dy * inv_d;

            if (in_contact) {
                // Half the overlap, because the neighbour independently
                // computes and applies the other half.
                const f32 correction = (pair_contact - d) * 0.5f * contact_stiffness;
                contact.x += nx * correction;
                contact.y += ny * correction;
                ++contact_count;
            }
            if (!in_crowd) continue;
            if (d2 < sep_r2) {
                const f32 push = (sep_radius - d) / sep_radius;
                sep.x += nx * push;
                sep.y += ny * push;
                ++sep_count;
            }
            if (d2 < align_r2) {
                vel.x += vx[j];
                vel.y += vy[j];
                ++align_count;
            }
            if (d2 < align_r2) {
                // Squad-blind, like contact and unlike alignment: standing in
                // someone's way is not a question of whose squad they are in.
                const f32 aw = 1.0f - d * inv_align;
                gradient.x += nx * aw;
                gradient.y += ny * aw;
                gradient_weight += aw;
            }
        }
    }

done:
    // Averaged, not summed -- see NeighbourSample::contact_push for the whole
    // argument. One contact divides by one, so a lone overlapping pair still
    // resolves in exactly the single step it always did.
    if (contact_count > 0) {
        const f32 inv = 1.0f / static_cast<f32>(contact_count);
        // A plain mean is perfect for one pair but becomes too weak in a deep
        // pack: the drive toward the goal can hold several simultaneous
        // overlaps in equilibrium forever. Give multi-contact jams at most a
        // 3x relaxation step. This is still an average (and still capped by
        // one body radius below), so it cannot recover the direction noise of
        // the old unbounded sum.
        const f32 jam_gain = math::min(std::sqrt(static_cast<f32>(contact_count)), 3.0f);
        out.contact_push = Vec2{contact.x * inv * jam_gain,
                                contact.y * inv * jam_gain};
    }
    out.crowd_gradient = gradient;
    out.crowd_weight = gradient_weight;
    if (sep_count > 0) {
        // Plain mean over the neighbours that contributed.
        const f32 inv = 1.0f / static_cast<f32>(sep_count);
        out.separation = Vec2{sep.x * inv, sep.y * inv};
    }
    if (align_count > 0) {
        const f32 inv = 1.0f / static_cast<f32>(align_count);
        out.avg_velocity = Vec2{vel.x * inv, vel.y * inv};
        out.has_alignment = true;
    }
    out.crowd = align_count;
    return out;
}

/// Pushes one agent out of tissue it is overlapping and cancels the part of its
/// velocity heading further in. This is the actual "bumps into the lane wall"
/// behaviour, and it deliberately replaces nothing -- the SDF-gradient term in
/// pass A is still the recovery net for agents that end up fully outside the
/// mask with no flow guidance; this is contact response for agents at the edge.
///
/// Position-based, in the sense the crowd-simulation literature means: rather
/// than adding a repulsive force and hoping it is strong enough before the next
/// tick, the position is projected back onto the legal side immediately, so an
/// agent can never be seen inside a wall regardless of how fast it arrived or
/// how hard the crowd behind it is pushing. That property is exactly what makes
/// a dense jam against a wall hold its shape instead of squeezing through.
void resolve_wall_contact(const DistanceField& sdf, f32& px, f32& py,
                          f32& vx, f32& vy, f32 radius, f32 restitution, f32 splash,
                          u32 stable_id) {
    const Vec2 p{px, py};
    const f32 clearance = sdf.sample(p);
    if (clearance >= radius) return;   // not touching anything

    Vec2 n = sdf.gradient(p);          // points toward more open tissue
    const f32 n2 = math::length_sq(n);
    if (n2 <= math::kEpsilon) return;  // no usable normal (unbaked field, or a
                                       // perfectly flat plateau) -- leave pass
                                       // A's flow/SDF steering to handle it
    const f32 inv = 1.0f / std::sqrt(n2);
    n.x *= inv;
    n.y *= inv;

    // Positional projection. `clearance` is signed, so an agent that has ended
    // up well inside solid tissue gets a correspondingly large push and is
    // recovered in one tick rather than crawling out over many.
    const f32 penetration = radius - clearance;
    px += n.x * penetration;
    py += n.y * penetration;

    const f32 vn = vx * n.x + vy * n.y;
    if (vn >= 0.0f) return;            // already heading away; nothing to resolve

    // Split the velocity into "into the wall" and "along the wall".
    const f32 tx = vx - n.x * vn;
    const f32 ty = vy - n.y * vn;

    // Cancel the inbound part (optionally bouncing a little of it back).
    const f32 j = vn * (1.0f + restitution);
    vx -= n.x * j;
    vy -= n.y * j;

    // THE SPLASH. Cancelling the inbound component alone just deletes that
    // momentum, so an agent arriving head-on stops dead against the wall and
    // the horde reads as piling up rather than flowing around. A fluid does the
    // opposite: what cannot continue forward is redirected sideways, and the
    // mass keeps moving. So the blocked speed is re-injected along the wall
    // tangent.
    //
    // Direction: follow whatever sideways motion the agent already had, so a
    // crowd sweeping along a wall keeps sweeping the same way instead of
    // scattering. Only when the impact is dead-on (no tangential component at
    // all) is a side chosen arbitrarily -- and then it is chosen from the
    // agent's stable id, which splits an incoming column to BOTH sides of an
    // obstacle without changing its answer as projection moves the body by a
    // few floating-point bits. Picking a fixed side there would send every
    // agent the same way and read as a conveyor belt, not a splash.
    const f32 blocked = -vn * splash;
    if (blocked <= 0.0f) return;

    const f32 t2 = tx * tx + ty * ty;
    f32 ux, uy;
    if (t2 > 1e-6f) {
        const f32 inv_t = 1.0f / std::sqrt(t2);
        ux = tx * inv_t;
        uy = ty * inv_t;
    } else {
        // Deterministic per-agent coin flip. Unlike position bits this remains
        // invariant through projection, so a head-on agent cannot alternate
        // sides from one tick to the next and shiver against the wall.
        u32 bits = stable_id * 2654435761u;
        bits ^= bits >> 16u;
        const f32 sign = (bits & 1u) ? 1.0f : -1.0f;
        ux = -n.y * sign;
        uy = n.x * sign;
    }
    vx += ux * blocked;
    vy += uy * blocked;
}

/// Last-resort guarantee that an agent which STARTED the tick on walkable
/// ground also ENDS it there.
///
/// Tests the TISSUE MASK, not the distance field, and that is the whole point.
/// The mask is the authority on what is walkable and it is kept current: when a
/// Fibrin Clot is dropped, the mask is marked non-walkable under it and the
/// flow field is re-baked to route around it. The DISTANCE field is not
/// re-baked -- it cannot be, because tower placement validation reads it for
/// "is there clearance for a tower here", and folding runtime blocks into it
/// would make every clot unbuildable ground for its whole lifetime.
///
/// So the SDF has never heard of runtime blocks, and resolve_wall_contact()
/// above reads only the SDF. The flow field steers the horde around a block,
/// but nothing physically stops agents entering one, and once the crowd behind
/// is dense enough it simply presses them through. Measured on a 1,200-agent
/// jam, back when towers were still obstacles: 72 agents inside a footprint at
/// once. (Towers no longer block anything -- the horde walks through them.)
///
/// The mask closes that hole for runtime blocks and ordinary tissue alike, and it
/// does so geometrically rather than by any force balance: the previous
/// position was walkable, so some point on the segment to the new one is the
/// last walkable point, and bisection finds it without needing a normal, a
/// penetration depth, or a gradient -- none of which are reliable deep inside
/// solid ground anyway.
///
/// Velocity is deliberately left alone. An agent pinned here still has its
/// velocity pointing into the obstacle, so it re-attempts next tick and stays
/// pressed against the face -- which is how a crowd crushed against something
/// should behave. Zeroing it would make the front rank go slack and the jam
/// would visibly stop pushing.
///
/// THE SLIDE, AND WHY THE BISECTION ALONE IS NOT ENOUGH. Truncating the step is
/// the right answer for the component heading INTO the face and the wrong one
/// for the component running ALONG it, and bisection cannot tell them apart --
/// it scales the whole displacement by one scalar. Against a tower that is not
/// a slow corner, it is a dead stop: the blocked/walkable boundary is a cell
/// edge, so an agent already touching the face has essentially zero legal
/// fraction, `good` collapses to 0, and the agent is returned to exactly where
/// it started no matter how fast it was moving sideways. Measured before this
/// slide existed: agents sat frozen on a tower's upwind face at |v| = max_speed
/// with 98% of that velocity tangential, for as long as the level ran, because
/// every tick handed back the same position. That is the "enemies get stuck on
/// a tower and never go around it" bug, and it is a containment artefact rather
/// than a steering one -- the flow field was already telling them to go around.
///
/// So the residual displacement is re-applied one axis at a time. Each axis is
/// accepted only if it lands somewhere walkable, which is exactly "keep the
/// tangential motion, drop the normal one" for the axis-aligned cell boundaries
/// the mask is made of -- and needs no normal, which is the same reason the
/// bisection above does not want one. X before Y is arbitrary but fixed, so the
/// result stays deterministic and thread-order-independent like the rest of the
/// kernel.
///
/// The kernel itself lives in sim/flowfield/RuntimeBlock.h
/// (contain_to_walkable) so the named-agent movement system runs the very
/// same one: a wall the horde cannot press through is one an elite cannot
/// walk through either.
void contain_to_tissue(const TissueMask& mask, f32& px, f32& py, f32 ox, f32 oy) {
    Vec2 p{px, py};
    contain_to_walkable(mask, p, Vec2{ox, oy});
    px = p.x;
    py = p.y;
}

/// Pass B: branch-free clamp_length + p += v*dt over six contiguous streams.
/// Pulled out into its own small free function on purpose — MSVC's
/// auto-vectorizer has a complexity/size budget per function, and this loop
/// sitting inline inside ChaffSystem::update (a large function with a capturing
/// lambda, atomics, and three other passes) was enough to make it bail and fall
/// back to a scalar 4x-unrolled loop despite being branch-free and __restrict-
/// qualified. Isolated like this it vectorizes cleanly (confirmed with
/// `/Qvec-report:2`: "loop vectorized", packed `mulps`/`addps`/`divps` in the
/// generated code instead of the unrolled `mulss`/`addss`/`divss` it emitted
/// inline). See tests/test_chaff_system.cpp for the behavioural coverage this
/// function must keep passing.
void integrate_and_clamp(f32* __restrict pos_x, f32* __restrict pos_y,
                         f32* __restrict vel_x, f32* __restrict vel_y,
                         const f32* __restrict old_pos_x, const f32* __restrict old_pos_y,
                         const f32* __restrict push_x, const f32* __restrict push_y,
                         const f32* __restrict max_speed, u32 n, f32 dt) {
    for (u32 i = 0; i < n; ++i) {
        const f32 ms = max_speed[i];
        const f32 vxi = vel_x[i];
        const f32 vyi = vel_y[i];
        const f32 l2 = vxi * vxi + vyi * vyi;
        // Branch-free clamp: scale is 1 when already under the limit (or
        // stationary), ms/|v| otherwise. No `if`, no early-out — a straight
        // arithmetic sequence over contiguous memory is what lets the
        // auto-vectorizer pack four agents per SIMD lane.
        const f32 l2c = l2 > math::kEpsilon ? l2 : math::kEpsilon;
        const f32 inv_len = 1.0f / std::sqrt(l2c);
        const f32 raw_scale = ms * inv_len;
        const f32 scale = raw_scale < 1.0f ? raw_scale : 1.0f;
        const f32 nvx = vxi * scale;
        const f32 nvy = vyi * scale;
        vel_x[i] = nvx;
        vel_y[i] = nvy;
        // The contact push is added as DISPLACEMENT, not as another force, and
        // deliberately after the speed clamp: un-overlapping is a geometric
        // correction, so it must not be rationed by max_speed the way steering
        // is. That is the whole reason overlap survived a velocity-only
        // separation impulse. Two extra adds; the loop still vectorizes.
        pos_x[i] = old_pos_x[i] + nvx * dt + push_x[i];
        pos_y[i] = old_pos_y[i] + nvy * dt + push_y[i];
    }
}

/// Pass B3: fade every agent's hit flash toward zero. Purely cosmetic -- see
/// sim/chaff/HitFlash.h for what the stream is and why the sim owns it.
///
/// LINEAR, and per-family only through the decay RATE. Nothing about the shape
/// of the fade is decided here: the stored value is a plain 0..1 ramp and the
/// renderer applies HitFlashParams::curve to it every frame, so retuning the
/// curve re-shapes agents that are already mid-flash instead of only the ones
/// hit after the change. That is the whole reason the sim stores the raw ramp.
///
/// One pass over one contiguous f32 stream plus a two-entry table lookup per
/// agent. It rides here rather than in its own loop over the horde because the
/// family stream is already hot from pass A and the alternative is a second
/// full sweep of memory for four floating-point operations.
void decay_hit_flash(f32* __restrict flash, const u8* __restrict family,
                     const f32* __restrict rate, u32 n, f32 dt) {
    for (u32 i = 0; i < n; ++i) {
        const f32 v = flash[i] - dt * rate[family[i] < kFamilyCount ? family[i] : 0];
        flash[i] = v > 0.0f ? v : 0.0f;
    }
}

} // namespace

void ChaffSystem::ensure_scratch(usize capacity) {
    if (old_pos_x_.size() == capacity) return;   // already sized; reserved-once
    old_pos_x_.assign(capacity, 0.0f);
    old_pos_y_.assign(capacity, 0.0f);
    old_vel_x_.assign(capacity, 0.0f);
    old_vel_y_.assign(capacity, 0.0f);
    contact_push_x_.assign(capacity, 0.0f);
    contact_push_y_.assign(capacity, 0.0f);
    scratch_max_speed_.assign(capacity, 0.0f);
    replicate_wanted_.assign(capacity, 0u);
}

ChaffUpdateStats ChaffSystem::update(ChaffBuffers& buffers, const FlowField& flow,
                                     const DistanceField& sdf, const TissueMask& mask,
                                     const SpatialHash& hash, const SquadRegistry& squads,
                                     Rng& rng, f32 dt, JobSystem* jobs) {
    ChaffUpdateStats stats{};
    const usize count = buffers.count();
    if (count == 0) return stats;

    ensure_scratch(buffers.capacity());

    // Snapshot pre-tick positions for pass A's separation reads (see rationale
    // in the file header). Plain contiguous copies — trivially vectorizable and
    // cheap relative to the hash-gather pass that follows.
    std::memcpy(old_pos_x_.data(), buffers.pos_x.data(), count * sizeof(f32));
    std::memcpy(old_pos_y_.data(), buffers.pos_y.data(), count * sizeof(f32));
    std::memcpy(buffers.prev_pos_x.data(), buffers.pos_x.data(), count * sizeof(f32));
    std::memcpy(buffers.prev_pos_y.data(), buffers.pos_y.data(), count * sizeof(f32));
    // Velocities need the same treatment for alignment -- see old_vel_x_'s
    // declaration in the header.
    std::memcpy(old_vel_x_.data(), buffers.vel_x.data(), count * sizeof(f32));
    std::memcpy(old_vel_y_.data(), buffers.vel_y.data(), count * sizeof(f32));

    f32* vx = buffers.vel_x.data();
    f32* vy = buffers.vel_y.data();
    f32* wander_x = buffers.wander_x.data();
    f32* wander_y = buffers.wander_y.data();
    const u8* fam = buffers.family.data();
    const u8* flg = buffers.flags.data();
    const u16* sqid = buffers.squad_id.data();
    const f32* slow_factor = buffers.slow_factor.data();
    const f32* old_px = old_pos_x_.data();
    const f32* old_py = old_pos_y_.data();
    const f32* old_vx = old_vel_x_.data();
    const f32* old_vy = old_vel_y_.data();
    f32* push_x = contact_push_x_.data();
    f32* push_y = contact_push_y_.data();
    f32* max_speed_scratch = scratch_max_speed_.data();
    u8* replicate_wanted = replicate_wanted_.data();
    const ChaffTuning& tuning = tuning_;
    const SquadTuning& sq_tuning = squads.tuning();
    // Hoisted out of the per-agent loop: when squads are off, or the level
    // authored no paths, every squad term below collapses to the pre-squad
    // kernel and the only remaining cost is this one bool.
    const bool squads_on = sq_tuning.enabled && !squads.paths().empty();
    const f32 foreign_radius_mult = squads_on ? sq_tuning.foreign_radius_mult : 1.0f;
    const f32 foreign_strength_mult = squads_on ? sq_tuning.foreign_strength_mult : 1.0f;

    // Crowd-relief constants, hoisted: one multiply per family per tick instead
    // of one per agent per tick. Same reasoning as the family tables in the
    // renderer's batcher -- six lookups, not ten thousand.
    f32 relief_step[kFamilyCount];
    f32 contact_radii[kFamilyCount];
    f32 max_contact_radius = 0.0f;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        relief_step[f] = tuning.family[f].crowd_relief * tuning.family[f].radius;
        contact_radii[f] = tuning.family[f].radius * tuning.family[f].contact_spacing;
        max_contact_radius = math::max(max_contact_radius, contact_radii[f]);
    }

    // ---- Pass A: accumulate (parallel, gather-heavy, not vectorized) --------
    std::atomic<u32> replication_rolls{0};

    // ONE draw from the shared generator per update, before any range starts.
    //
    // Rng::fork() is const -- it does not advance its parent. This pass only
    // ever forked, so on any tick where nothing else in the sim happened to
    // draw from the sim RNG (no tower firing, no damage roll), every range got
    // a BYTE-IDENTICAL generator to the tick before. The consequences were not
    // subtle once looked for:
    //
    //   - Jitter stopped being noise. Agent i received the same impulse every
    //     tick, i.e. a constant DC force in a fixed direction, which is exactly
    //     how a handful of agents come to track steadily away from the crowd
    //     they belong to.
    //   - Replication stopped being a rate. The same index slots rolled true
    //     every single tick while the rest never did, so growth compounded in
    //     one band of the buffer instead of spreading over the horde.
    //
    // Drawing once here re-seeds the whole family of per-range streams each
    // tick. It is one draw, taken before the split and outside every range, so
    // it stays a pure function of tick count and is identical at any thread
    // count -- the determinism contract in the header is unchanged.
    const u64 tick_salt = rng.next_u64();

    auto accumulate = [&](usize begin, usize end, u32 range_index) {
        // Exactly one fork per range, as the frozen header requires.
        Rng local_rng = rng.fork(tick_salt ^ static_cast<u64>(range_index));
        u32 rolls = 0;
        for (usize i = begin; i < end; ++i) {
            const u8 flags_i = flg[i];
            const u32 f = fam[i];
            const ChaffFamilyParams& fp = tuning.family[f < kFamilyCount ? f : 0];

            // Burrowed, or latched onto a friendly host (sim/hostile). Same
            // treatment for both: no flow, no separation, no jitter, no
            // replication -- reads as the agent going still. Zero velocity so
            // pass B is a no-op integrate. A latched agent's position belongs
            // to the hostile pass, which rides it on its host after this
            // kernel has run; anything this kernel did to it would be
            // overwritten, so doing nothing is the honest option.
            const bool hidden = (flags_i & (chaff_flags::kHidden | chaff_flags::kLatched)) != 0;
            if (hidden) {
                vx[i] = 0.0f;
                vy[i] = 0.0f;
                wander_x[i] = 0.0f;
                wander_y[i] = 0.0f;
                max_speed_scratch[i] = 0.0f;
                push_x[i] = 0.0f;
                push_y[i] = 0.0f;
                continue;
            }

            const bool drifting = (flags_i & chaff_flags::kDrifting) != 0;
            const bool slowed = (flags_i & chaff_flags::kSlowed) != 0;

            const Vec2 p{old_px[i], old_py[i]};
            Vec2 dir;
            if (drifting) {
                dir = tuning.ambient_drift;
            } else {
                dir = flow.sample(p);
                if (math::length_sq(dir) <= math::kEpsilon) {
                    // Off the baked walkable region (or a genuine dead
                    // pocket): recover toward higher tissue clearance instead
                    // of leaving the agent with zero net guidance. See this
                    // file's header comment for why this exists.
                    dir = sdf.gradient(p);
                }
            }

            // Squad cohesion: steer ACROSS the flow, never against it.
            //
            // The flow field keeps ALL of its forward authority -- the squad
            // only decides where across the lane its members sit. That split is
            // the whole reason this reads as a horde in formation rather than
            // as units following waypoints: forward progress, wall contact and
            // clot reroutes remain entirely the flow field's business, and no
            // squad can steer its members into a wall trying to reach an anchor.
            //
            // Applied as a lateral velocity IMPULSE, in the same units as the
            // separation rule below, for the reason documented on
            // SquadTuning::lateral_push: an acceleration-form cohesion term is
            // ~50x quieter than the separation impulses of the crowd it is
            // steering, and simply loses.
            //
            // The weight is EXACTLY ZERO while the agent is within its squad
            // radius of the lane, so a packed interior runs the identical
            // kernel it ran before squads existed -- that is what keeps the
            // fluid feel from being traded away for the grouping.
            const u16 my_squad = squads_on ? sqid[i] : kNoSquad;
            Vec2 squad_impulse{0.0f, 0.0f};
            if (my_squad != kNoSquad && squads.alive(my_squad) && !drifting) {
                const Squad& sq = squads.get(my_squad);
                const Vec2 to_anchor = sq.anchor - p;
                const Vec2 fdir = math::normalize_safe(dir);
                if (math::length_sq(fdir) > 0.0f) {
                    const Vec2 perp{-fdir.y, fdir.x};

                    // ACROSS the flow: toward the anchor, i.e. which line of
                    // the lane this squad rides.
                    const f32 lateral = to_anchor.x * perp.x + to_anchor.y * perp.y;
                    const f32 mag = lateral < 0.0f ? -lateral : lateral;
                    const f32 t =
                        math::clamp((mag - sq.radius) / sq_tuning.follow_ramp, 0.0f, 1.0f);
                    const f32 w = sq_tuning.follow_weight_max * t;
                    if (w > 0.0f) {
                        const f32 push = w * sq_tuning.lateral_push;
                        squad_impulse = perp * (lateral < 0.0f ? -push : push);
                    }

                    // ALONG the flow: toward the squad's own CENTRE OF MASS,
                    // not the anchor. Without this the cohesion is purely
                    // cross-lane and nothing stops a squad stringing out down
                    // the lane into a ribbon -- measured, that is exactly what
                    // happened, and a ribbon reads as part of the mass however
                    // tidily it is confined sideways.
                    //
                    // Against the centroid rather than the anchor because the
                    // anchor deliberately leads the squad by anchor_lookahead:
                    // chasing it lengthwise would accelerate every member
                    // forward at once and stretch the squad rather than gather
                    // it. The centroid is the only point the squad can close on
                    // without also moving.
                    const Vec2 to_centre = sq.centroid - p;
                    const f32 along = to_centre.x * fdir.x + to_centre.y * fdir.y;
                    const f32 amag = along < 0.0f ? -along : along;
                    const f32 at =
                        math::clamp((amag - sq.radius) / sq_tuning.follow_ramp, 0.0f, 1.0f);
                    const f32 aw = sq_tuning.follow_weight_max * at;
                    if (aw > 0.0f) {
                        const f32 push = aw * sq_tuning.lateral_push;
                        if (along >= 0.0f) {
                            // Behind the squad: close up. Free to push as hard
                            // as the tuning says -- catching up is with the
                            // flow, so it can never fight the field.
                            squad_impulse += fdir * push;
                        } else {
                            // AHEAD of the squad: brake, never reverse.
                            //
                            // Capped at the agent's own forward speed, so the
                            // hardest this can do is bring it to a standstill
                            // and let the squad catch up. Uncapped it was a
                            // 1.65/tick impulse opposing a 0.33/tick flow
                            // term, so a leader accelerated BACKWARDS until
                            // max_speed clamped and then drove upstream at full
                            // speed -- agents visibly turning round and heading
                            // back up the lane. The flow field is supposed to
                            // keep sole authority over which way "forward" is
                            // (see the header); this is what actually enforces
                            // it for the along-flow term.
                            const f32 v_along = vx[i] * fdir.x + vy[i] * fdir.y;
                            const f32 brake = math::min(push, math::max(0.0f, v_along));
                            squad_impulse -= fdir * brake;
                        }
                    }
                } else if (math::length_sq(to_anchor) > math::kEpsilon) {
                    // No flow guidance at all (off-mask, or a genuine dead
                    // pocket). There is no forward direction left to preserve,
                    // so the anchor becomes the guidance -- and heading for it
                    // is also the shortest way back to where the field is baked.
                    dir = math::normalize_safe(to_anchor);
                }
            }

            Vec2 v{vx[i], vy[i]};
            v += dir * fp.acceleration * dt;
            const Vec2 flow_velocity = v;

            // ONE gather feeds all four local rules: separation, alignment,
            // crowd pressure, and the positional contact correction.
            const NeighbourSample nb =
                gather_neighbours(hash, old_px, old_py, old_vx, old_vy, sqid, my_squad,
                                  foreign_radius_mult, foreign_strength_mult, i,
                                  fp.separation_radius, fp.alignment_radius,
                                  fam, contact_radii, max_contact_radius, fp.contact_stiffness,
                                  tuning.max_neighbors_sampled);
            // Crowd relief: displacement DOWN the local pressure gradient,
            // toward the density `pressure_threshold` describes as comfortable.
            //
            // The threshold is what makes this expansion rather than a faster
            // way to finish un-overlapping. Gating on the crowd a family reaches
            // when packed at its own CONTACT spacing was the first attempt and
            // it measured as doing nothing at all (6.77 spread vs 6.76 without
            // it): contact already drives the crowd to exactly that density, so
            // the gate closed at the moment relief would have started to matter.
            // pressure_threshold sits looser than contact packing by
            // construction -- it is the count at which the crowd rules already
            // consider a neighbourhood packed -- so relief keeps pushing after
            // bodies separate, and the crowd settles at a spacing rather than at
            // a touch. Lower it to make a horde stand on more ground.
            //
            // It is the SAME "packed" the pressure multiplier below reads,
            // deliberately: one notion of crowded, two responses to it.
            //
            // The gradient is divided by its own WEIGHT, not clamped to unit
            // length, and the difference is the whole behaviour of this term at
            // high density.
            //
            // What relief needs from the neighbourhood is two separate facts:
            // which way the open space is, and whether there IS any. Dividing
            // by the summed weight answers both at once -- the result is a mean
            // push-away whose length falls to nothing as the neighbours close
            // in around the agent from every side. Normalizing to unit length
            // and clamping (what this did) answers only the first, and then
            // asserts the second: an enclosed agent's gradient is a residual of
            // near-cancelling terms, so it is both large enough to survive any
            // clamp and pointing in a direction that is essentially noise, and
            // rescaling it to full length hands that noise the term's entire
            // per-tick displacement budget. That is the deepest part of a jam
            // relieving hardest -- the case the clamp was written to prevent,
            // arriving through the clamp.
            //
            // Measured on 1,100 agents packed to 1.2 units: with contact
            // averaged but relief still clamped, an agent still changed heading
            // 21 degrees per tick and reversed outright on 6% of ticks; the
            // same run with relief disabled entirely sat at 0.7 degrees and
            // zero. Weighting closes that gap without giving up the expansion,
            // because a crowd's OUTER face -- the only place with space to
            // spend -- is exactly where the mean stays long.
            Vec2 relief{0.0f, 0.0f};
            const f32 fits = math::max(fp.pressure_threshold, 1.0f);
            const f32 step = relief_step[f < kFamilyCount ? f : 0];
            if (step > 0.0f && static_cast<f32>(nb.crowd) > fits &&
                nb.crowd_weight > kSeparationEpsSq) {
                const f32 over =
                    math::saturate((static_cast<f32>(nb.crowd) - fits) / fits);
                relief = nb.crowd_gradient * (step * over / nb.crowd_weight);
            }

            // CAPPED. Pass B applies these as raw displacement that
            // deliberately bypasses max_speed, so a deeply packed agent can be
            // translated a long way in a single tick -- and a translation big
            // enough to step over a wall defeats every wall test there is,
            // because nothing samples the space in between. Bounding one tick's
            // un-overlapping to the agent's own radius keeps it a relaxation
            // rather than a teleport; the jam simply takes a few more ticks to
            // resolve, which is what it looks like anyway.
            //
            // The cap is a SAFETY rail and not the thing that keeps this term
            // well-behaved, which is worth stating because it used to be asked
            // to be both. Back when contact_push summed its corrections and
            // relief renormalized its gradient, a jammed agent hit this cap
            // every single tick, so the cap -- not the physics -- set the
            // magnitude, and all that was left to vary was a direction made of
            // sampling noise. Something clamped at full magnitude in a
            // meaningless direction is the definition of jitter. Both terms now
            // fall off on their own as the neighbourhood closes in, and in a
            // settled jam this cap is no longer reached at all.
            //
            // Relief is capped together with contact rather than beside it:
            // the cap exists because of how far an agent moves in a tick, and
            // a wall does not care which term paid for the step.
            {
                const f32 cap = fp.radius;
                const Vec2 total = nb.contact_push + relief;
                const f32 mag2 = math::length_sq(total);
                const Vec2 capped =
                    mag2 > cap * cap ? total * (cap / std::sqrt(mag2)) : total;
                push_x[i] = capped.x;
                push_y[i] = capped.y;
            }

            // Crowd pressure amplifies separation rather than adding a
            // second independent force. Separation already points "away
            // from where everyone is"; when the neighbourhood is packed,
            // that same direction is exactly where the mass needs to
            // relieve into, so scaling it keeps the release coherent
            // instead of adding noise on top.
            f32 push = fp.separation_strength;
            if (fp.pressure_gain > 0.0f &&
                static_cast<f32>(nb.crowd) > fp.pressure_threshold) {
                const f32 excess = static_cast<f32>(nb.crowd) - fp.pressure_threshold;
                const f32 mul = 1.0f + excess * fp.pressure_gain;
                push *= mul < fp.pressure_max ? mul : fp.pressure_max;
            }
            v += nb.separation * push;
            // Coherent across the whole squad, where separation is local and
            // largely self-cancelling -- which is why a comparable per-tick
            // magnitude still produces a clean migration rather than a fight.
            v += squad_impulse;

            // Alignment: steer toward the neighbourhood's mean velocity.
            // Written as a difference (a steering term, not a velocity
            // assignment) so it can never overrule the flow field -- it
            // biases how the agent gets where it is already going, which is
            // what makes the crowd move as a body without losing the
            // objective.
            if (nb.has_alignment && fp.alignment_strength > 0.0f) {
                v += (nb.avg_velocity - v) * fp.alignment_strength * dt;
            }
            if (fp.jitter > 0.0f) {
                // White-noise impulses make direction discontinuous at the
                // fixed-tick frequency. Filter the random target into a
                // persistent, slowly changing wander vector instead: agents
                // still look alive, but their paths curve rather than buzz.
                constexpr f32 kWanderResponse = 0.10f;
                const Vec2 target = local_rng.unit_disc() * fp.jitter;
                wander_x[i] += (target.x - wander_x[i]) * kWanderResponse;
                wander_y[i] += (target.y - wander_y[i]) * kWanderResponse;
                v += Vec2{wander_x[i], wander_y[i]};
            } else {
                wander_x[i] = 0.0f;
                wander_y[i] = 0.0f;
            }

            if (!drifting && math::length_sq(dir) > math::kEpsilon) {
                // Treat crowd impulses as bounded offsets to a desired flow
                // velocity for every family. Accumulating those impulses as
                // permanent momentum let virus leaders reverse upstream and
                // let enclosed groups stall. Contact still separates bodies
                // through push_x/y; it does not need to reverse their travel.
                const Vec2 forward = math::normalize_safe(dir);
                const Vec2 lateral{-forward.y, forward.x};
                const Vec2 previous{old_vx[i], old_vy[i]};
                // Retain enough spacing response to open the crowd, with a
                // forward target of at least half speed and at most a 19-degree
                // sidestep. None of these corrections accumulate across ticks.
                constexpr f32 kCrowdVelocityGain = 4.0f;
                const Vec2 crowd = (v - flow_velocity) * kCrowdVelocityGain;
                const f32 speed = fp.max_speed * (slowed ? slow_factor[i] : 1.0f);
                const f32 along = math::clamp(speed + crowd.x * forward.x + crowd.y * forward.y,
                                              speed * 0.5f, speed);
                const f32 across = math::clamp(crowd.x * lateral.x + crowd.y * lateral.y,
                                               -along * 0.35f, along * 0.35f);
                const Vec2 desired = forward * along + lateral * across;
                const f32 response = math::saturate(fp.acceleration * dt /
                                                     math::max(speed, 0.01f));
                v = previous + (desired - previous) * response;
            }

            vx[i] = v.x;
            vy[i] = v.y;
            // The slow's strength is per agent (ChaffBuffers::slow_factor),
            // written by whichever slow zone last touched it; its expiry is the
            // zone system's job, not this kernel's.
            max_speed_scratch[i] = fp.max_speed * (slowed ? slow_factor[i] : 1.0f);

            if (fp.replication_rate > 0.0f && local_rng.chance(fp.replication_rate * dt)) {
                replicate_wanted[i] = 1u;
                ++rolls;
            }
        }
        if (rolls != 0) replication_rolls.fetch_add(rolls, std::memory_order_relaxed);
    };

    if (jobs) {
        jobs->parallel_for(count, accumulate);
    } else {
        accumulate(0, count, 0);
    }

    // ---- Pass B: integrate (serial, branch-free, auto-vectorizes) ----------
    // See integrate_and_clamp()'s doc comment for why this is its own function
    // rather than an inline loop.
    f32* px = buffers.pos_x.data();
    f32* py = buffers.pos_y.data();
    integrate_and_clamp(px, py, vx, vy, old_px, old_py, push_x, push_y,
                        max_speed_scratch, static_cast<u32>(count), dt);

    // ---- Pass B3: cosmetic ramps (serial) ----------------------------------
    // Placed after B for no reason other than that B is where the tick's
    // straight-line arithmetic lives; it reads and writes nothing any other
    // pass touches, and removing it entirely cannot move state_hash().
    //
    // The rates are rebuilt per tick rather than cached, because the table is
    // hot-reloadable: an author dragging `duration` in the config panel has to
    // see the fade change on the next tick, not on the next level load.
    {
        f32 flash_rate[kFamilyCount];
        for (u32 f = 0; f < kFamilyCount; ++f) {
            const f32 seconds = family_hit_flash(static_cast<PathogenFamily>(f)).duration;
            // A non-positive duration means "no flash at all" (HitFlash.h), and
            // a rate of 1/dt clears the stream on the very next tick rather
            // than leaving whatever was mid-fade when the knob was turned off
            // frozen on screen forever.
            flash_rate[f] = seconds > 0.0f ? 1.0f / seconds : (dt > 0.0f ? 1.0f / dt : 1.0f);
        }
        decay_hit_flash(buffers.hit_flash.data(), fam, flash_rate, static_cast<u32>(count), dt);
    }

    // A replication starts with two small newborn bodies and lets both grow
    // over a short, fixed beat. This is intentionally a visual-only stream:
    // it never informs movement, damage, or the deterministic state hash.
    // Keeping the timer here beside hit_flash also means an agent compacted to
    // another slot carries its in-flight split animation with it.
    if (dt > 0.0f) {
        f32* split = buffers.replication_pulse.data();
        for (usize i = 0; i < count; ++i) {
            const u32 f = fam[i] < kFamilyCount ? fam[i] : 0u;
            const ReplicationSplitParams& look =
                family_replication_split(static_cast<PathogenFamily>(f));
            if (!look.enabled || look.duration <= 0.0f) {
                split[i] = 0.0f;
                continue;
            }
            const f32 decay = dt / look.duration;
            split[i] = split[i] > 0.0f ? math::max(0.0f, split[i] - decay)
                                       : math::min(0.0f, split[i] + decay);
        }
    }

    // ---- Pass B2: wall contact (parallel, gather-light) ---------------------
    // Must run after B, because contact is decided against the position the
    // agent actually ended up at, not the one it started from. Kept out of B so
    // B stays the branch-free vectorizable loop described above -- an SDF
    // sample is a bilinear gather and would sink it.
    //
    // Embarrassingly parallel and deterministic: every agent reads only the
    // immutable DistanceField and writes only its own slot. No RNG, no
    // cross-agent reads, so no snapshot needed and no scheduling sensitivity.
    //
    // Skipped entirely when the field was never baked (width 0). Most unit
    // tests pass a default-constructed DistanceField, and its sample() returns
    // 0, which would otherwise read as "every agent is buried in a wall".
    const bool has_sdf = sdf.width() > 0 && sdf.height() > 0;
    const bool has_mask = mask.width() > 0 && mask.height() > 0;
    if (has_sdf || has_mask) {
        auto resolve_range = [&](usize begin, usize end, u32) {
            for (usize i = begin; i < end; ++i) {
                // Burrowed: not in the world. Latched: on a host that is on
                // tissue already, and the mask containment would pull it off
                // that host toward wherever it stood last tick.
                if ((flg[i] & (chaff_flags::kHidden | chaff_flags::kLatched)) != 0) continue;
                const u32 f = fam[i];
                const ChaffFamilyParams& fp = tuning.family[f < kFamilyCount ? f : 0];
                if (has_sdf) {
                    resolve_wall_contact(sdf, px[i], py[i], vx[i], vy[i], fp.radius,
                                         fp.wall_restitution, fp.wall_splash,
                                         buffers.generation[i]);
                }
                // Runs AFTER, not instead: the resolver does the splash and the
                // shallow-contact response, this only catches what it could not
                // -- including everything about towers, which it cannot see.
                if (has_mask) contain_to_tissue(mask, px[i], py[i], old_px[i], old_py[i]);
            }
        };
        if (jobs) {
            jobs->parallel_for(count, resolve_range);
        } else {
            resolve_range(0, count, 0);
        }
    }

    // ---- Pass C: despawn + replication resolve (serial, deterministic) -----
    u32 despawned_goal = 0;
    u32 despawned_bounds = 0;
    const bool has_goal = goal_half_.x > 0.0f && goal_half_.y > 0.0f;
    for (usize i = 0; i < count; ++i) {
        bool killed = false;
        if (has_goal) {
            // Point-in-oriented-rectangle: the objective's footprint is a
            // rotatable rectangle (game::ObjectivePoint), and this test is what
            // actually defines "reached the organ". cos/sin came from set_goal.
            const f32 dx = px[i] - goal_.x;
            const f32 dy = py[i] - goal_.y;
            const f32 lx = dx * goal_cos_ - dy * goal_sin_;
            const f32 ly = dx * goal_sin_ + dy * goal_cos_;
            if (std::fabs(lx) <= goal_half_.x && std::fabs(ly) <= goal_half_.y) {
                buffers.kill(i);
                ++despawned_goal;
                const u32 f = fam[i];
                if (f < kFamilyCount) ++stats.despawned_at_goal_by_family[f];
                killed = true;
            }
        }
        if (!killed && !bounds_.contains(Vec2{px[i], py[i]})) {
            buffers.kill(i);
            ++despawned_bounds;
            const u32 f = fam[i];
            if (f < kFamilyCount) ++stats.despawned_out_of_bounds_by_family[f];
        }
    }

    // Deterministic serial replication resolve, index order — independent of
    // how pass A's ranges were scheduled. spawn() is not safe to call from
    // inside pass A (it mutates buffers.count()), which is why the roll was
    // deferred to `replicate_wanted` instead of spawning inline.
    u32 replicated = 0;
    const u32 cap = tuning_.max_replications_per_tick;
    // Daughters assigned to each squad SO FAR THIS TICK. squads.update() ran at
    // the top of the tick, so its member_count predates every spawn below; with
    // a stale count alone a squad one short of the cap would accept every
    // daughter this tick produced instead of exactly one.
    squad_growth_.assign(squads.squads().size(), 0u);
    for (usize i = 0; i < count; ++i) {
        if (!replicate_wanted[i]) continue;
        replicate_wanted[i] = 0;
        if (replicated >= cap || buffers.full()) continue;
        const u32 f = fam[i];
        const ChaffFamilyParams& fp = tuning.family[f < kFamilyCount ? f : 0];
        ChaffSpawnParams sp;
        // The two descendants start on opposite sides of their old shared
        // centre, one contact distance apart. That makes replication read as
        // a division rather than a second sprite appearing beside an unchanged
        // parent; the paired replication_pulse carries the configured morph.
        //
        // Spawning on the parent's exact position (what this did) is the one
        // input the contact solver cannot act on: two agents at zero distance
        // have no separating direction, so gather_neighbours' epsilon test
        // discards the pair outright and neither one pushes off the other.
        // They stay welded together -- drawn as one sprite, counted twice by
        // everyone around them -- until jitter happens to break the tie, and
        // then they resolve a full contact-distance overlap at once and visibly
        // pop apart. Every replication is one of these, so a breeding horde
        // sparkles with them continuously; it is the artifact that reads as
        // "the viruses in particular are jittery".
        //
        // A full contact-distance gap is the useful split: it is far enough
        // that the contact solver need not unpick a fresh overlap, but close
        // enough that the two bodies still unmistakably came from one source.
        //
        // Drawn from the sim generator rather than a per-range fork because
        // this resolve pass is serial and runs in index order, so the draws
        // happen in the same sequence on any thread count.
        const f32 birth_angle = rng.range_f(0.0f, math::kTwoPi);
        const Vec2 split_dir{std::cos(birth_angle), std::sin(birth_angle)};
        const ReplicationSplitParams& split_look =
            family_replication_split(static_cast<PathogenFamily>(f < kFamilyCount ? f : 0u));
        const f32 split_distance = fp.radius * fp.contact_spacing *
                                   math::max(0.0f, split_look.separation_distance);
        const Vec2 split_half = split_dir * (split_distance * 0.5f);
        const Vec2 split_center{px[i], py[i]};
        sp.position = split_center + split_half;
        sp.velocity = Vec2{vx[i], vy[i]};
        sp.family = static_cast<PathogenFamily>(f);
        sp.density = fp.base_density > 0.0f ? fp.base_density : 1.0f;
        sp.flags = chaff_flags::kReplicated;
        // Opposite signs identify the two complementary visual halves. The
        // renderer uses their common origin to pull them apart continuously,
        // instead of showing an old virus disappear and two new ones pop in.
        sp.replication_pulse = split_look.enabled && split_look.duration > 0.0f ? -1.0f : 0.0f;
        sp.replication_origin = split_center;
        // A daughter joins its parent's squad while that squad has room. It is
        // spawned ON the parent, so no other squad is a coherent answer -- and
        // defaulting them all to kNoSquad (which this used to do) is the worst
        // answer of all: a replicating family sheds an ungrouped agent per
        // parent per five seconds, each steering purely on the flow field and
        // drifting out of the group it was born in. Measured on capillary_2
        // that was 662 of 842 live agents unaffiliated.
        //
        // Above SquadTuning::max_squad_size the daughter goes independent
        // instead. Inheritance is unbounded compounding otherwise -- every
        // member is a source of more members of the same squad -- so a cohort
        // of 60 grows until the lane is one squad again. See the max_squad_size
        // rationale in sim/squad/Squads.h.
        const u16 parent_squad = sqid[i];
        const u32 pending =
            parent_squad < squad_growth_.size() ? squad_growth_[parent_squad] : 0u;
        const bool inherit = squads.can_absorb(parent_squad, pending);
        sp.squad_id = inherit ? parent_squad : kNoSquad;
        if (buffers.spawn(sp).valid()) {
            // The original agent becomes the other daughter. Moving it only
            // after spawn succeeds preserves the old state on a full buffer.
            px[i] = split_center.x - split_half.x;
            py[i] = split_center.y - split_half.y;
            buffers.replication_pulse[i] =
                split_look.enabled && split_look.duration > 0.0f ? 1.0f : 0.0f;
            buffers.replication_origin_x[i] = split_center.x;
            buffers.replication_origin_y[i] = split_center.y;
            ++replicated;
            if (inherit) ++squad_growth_[parent_squad];
        }
    }

    stats.moved = static_cast<u32>(count);
    stats.replicated = replicated;
    stats.despawned_at_goal = despawned_goal;
    stats.despawned_out_of_bounds = despawned_bounds;
    return stats;
}

u32 ChaffSystem::spawn_burst(ChaffBuffers& buffers, PathogenFamily family, Vec2 spawn_pos,
                             f32 spawn_point_radius, u32 count, Rng& rng, u16 squad_id,
                             u32 pattern_offset, u32 pattern_count, f32 pattern_phase) const {
    const ChaffFamilyParams& fp = tuning_.family[static_cast<u32>(family)];
    if (count == 0) return 0;

    // SPAWN PACKING (was: rng.unit_disc() * spawn_point_radius, i.e. uniform random
    // inside the disc). Uniform random placement puts agents on top of each
    // other by construction -- with n points in a disc the expected number of
    // overlapping pairs is not small, it is the birthday problem, so a burst
    // reliably spawned a knot of interpenetrating agents that then had to be
    // shoved apart by the contact pass over the following ticks. That looked
    // exactly like the thing the contact pass was added to prevent.
    //
    // Replaced with a golden-angle (phyllotaxis) spiral: the arrangement
    // sunflower seeds use. Points land at
    //     r = R * sqrt((i + 0.5) / n),  theta = i * golden_angle
    // which fills the disc at uniform DENSITY -- the sqrt keeps area per point
    // constant -- while the irrational angle guarantees no two points ever line
    // up. Spacing is even by construction, so no rejection sampling, no
    // retries, and cost stays O(1) per agent.
    const f32 kGoldenAngle = 2.39996323f;   // pi * (3 - sqrt(5))

    // Grow the disc if the requested radius cannot hold `count` agents at
    // contact distance. Growing the spawn point is the right trade: a burst that
    // does not fit has to go somewhere, and spilling slightly wider reads far
    // better than spawning a solid interpenetrating plug in the middle.
    //
    // For a phyllotaxis disc the closest pair sits about 1.55*R/sqrt(n) apart
    // (nearest neighbours are Fibonacci-index offsets, not adjacent indices,
    // which is why the naive uniform-density estimate R*sqrt(pi/n) is too
    // optimistic and let small bursts overlap). Inverting that and keeping a
    // little margin gives the 0.75 below; tests/test_chaff_system.cpp measures
    // the worst pair at several burst sizes so this constant cannot rot.
    const f32 contact_d = fp.radius * fp.contact_spacing;
    const u32 layout_count = math::max(math::max(pattern_count, pattern_offset + count), 1u);
    const f32 needed = contact_d * std::sqrt(static_cast<f32>(layout_count)) * 0.75f;
    const f32 radius = math::max(spawn_point_radius, needed);

    // One draw, for the whole burst: a random spiral phase so successive waves
    // out of the same spawn point are not stamped identically. Rotating the pattern
    // cannot disturb the spacing, whereas per-agent jitter would reintroduce
    // exactly the overlap this is here to remove.
    const f32 phase = pattern_phase >= 0.0f
                          ? pattern_phase
                          : rng.range_f(0.0f, math::kTwoPi);

    // Centre the finished formation on the authored marker. The phyllotaxis
    // formula deliberately starts its first point away from zero, which is
    // great for packing but used to make even a one-agent squad appear beside
    // the marker. Translating the entire pattern by its centroid preserves
    // every pairwise spacing while making the squad's actual spawn location
    // exactly `spawn_pos`.
    Vec2 centroid_offset{};
    for (u32 i = 0; i < layout_count; ++i) {
        const f32 t = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(layout_count);
        const f32 r = radius * std::sqrt(t);
        const f32 a = phase + static_cast<f32>(i) * kGoldenAngle;
        centroid_offset += Vec2{std::cos(a), std::sin(a)} * r;
    }
    centroid_offset = centroid_offset / static_cast<f32>(layout_count);

    u32 spawned = 0;
    for (u32 i = 0; i < count; ++i) {
        if (buffers.full()) break;
        const u32 slot = pattern_offset + i;
        const f32 t = (static_cast<f32>(slot) + 0.5f) /
                      static_cast<f32>(layout_count);
        const f32 r = radius * std::sqrt(t);
        const f32 a = phase + static_cast<f32>(slot) * kGoldenAngle;
        ChaffSpawnParams p;
        p.position = spawn_pos + Vec2{std::cos(a), std::sin(a)} * r - centroid_offset;
        p.family = family;
        p.density = fp.base_density > 0.0f ? fp.base_density : 1.0f;
        p.squad_id = squad_id;
        if (buffers.spawn(p).valid()) ++spawned;
    }
    return spawned;
}

} // namespace immune::sim
