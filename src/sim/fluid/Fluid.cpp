#include "sim/fluid/Fluid.h"

#include "core/Clock.h"
#include "core/Math.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <algorithm>
#include <cmath>

namespace immune::sim {
namespace {

/// Clearance at which a particle counts as touching a wall, as a fraction of
/// the rest spacing. Half the spacing would be geometrically "correct", but a
/// slightly fatter contact radius keeps the surface layer from sinking into the
/// tissue where the renderer's metaball threshold would clip it in half.
constexpr f32 kWallContactScale = 0.62f;

/// Seconds of remaining life over which a particle fades. It keeps depositing
/// coverage the whole time, scaled down, so the burn stops exactly as the
/// visual does instead of a tick early or late.
constexpr f32 kFadeWindow = 0.35f;

/// Per-neighbour position correction cap, as a fraction of the rest spacing.
/// Position-based solvers are stable right up until one correction exceeds the
/// particle spacing, at which point neighbours swap sides and the whole patch
/// detonates. This is the rail that makes bad tuning look wrong instead of
/// crashing the frame.
constexpr f32 kMaxCorrectionScale = 0.30f;

/// Lattice radius, in multiples of the rest spacing, over which rest density is
/// summed. The kernel has compact support at h, and h/spacing is around 2.4 for
/// any sane tuning, so 4 rings is comfortably past the cut-off.
constexpr i32 kRestLatticeRings = 4;

/// Clavet's far kernel: (1 - q)^2. Zero outside the support radius.
inline f32 kernel_far(f32 q) {
    const f32 t = 1.0f - q;
    return t * t;
}

/// Clavet's near kernel: (1 - q)^3. Always repulsive, never attractive, which
/// is exactly what keeps a compressed column from knotting into clots.
inline f32 kernel_near(f32 q) {
    const f32 t = 1.0f - q;
    return t * t * t;
}

/// Rest density, summed over a hexagonal lattice at `spacing` inside radius
/// `h`. Deriving it beats authoring it: change the spacing and the fluid stays
/// at equilibrium instead of quietly inflating or collapsing.
f32 derive_rest_density(f32 h, f32 spacing) {
    if (h <= 0.0f || spacing <= 0.0f) return 1.0f;
    const f32 row_step = spacing * 0.8660254f; // sqrt(3)/2 — hexagonal packing.
    f32 rho = 0.0f;
    for (i32 row = -kRestLatticeRings; row <= kRestLatticeRings; ++row) {
        const f32 y = static_cast<f32>(row) * row_step;
        // Odd rows are offset half a spacing — that is what makes the lattice
        // hexagonal rather than square, and square packs ~15% looser at the
        // same spacing, which would bias the entire tuning.
        const bool odd = (row & 1) != 0;
        const f32 x_offset = odd ? spacing * 0.5f : 0.0f;
        for (i32 col = -kRestLatticeRings; col <= kRestLatticeRings; ++col) {
            if (row == 0 && col == 0) continue; // self
            const f32 x = static_cast<f32>(col) * spacing + x_offset;
            const f32 r = std::sqrt(x * x + y * y);
            if (r >= h) continue;
            rho += kernel_far(r / h);
        }
    }
    return math::max(rho, 1e-3f);
}

/// Cheap integer hash, so emission jitter never touches the shared sim Rng.
inline u32 hash_u32(u32 v) {
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

inline f32 hash_unit(u32 v) {
    return static_cast<f32>(hash_u32(v) >> 8) * 0x1.0p-24f;
}

} // namespace

// ---------------------------------------------------------------------------
// FluidBuffers
// ---------------------------------------------------------------------------

void FluidBuffers::reserve(usize max_particles) {
    capacity_ = max_particles;
    count_ = 0;
    pos_x.assign(max_particles, 0.0f);
    pos_y.assign(max_particles, 0.0f);
    vel_x.assign(max_particles, 0.0f);
    vel_y.assign(max_particles, 0.0f);
    prev_x.assign(max_particles, 0.0f);
    prev_y.assign(max_particles, 0.0f);
    density.assign(max_particles, 0.0f);
    life.assign(max_particles, 0.0f);
    life_max.assign(max_particles, 1.0f);
    dps.assign(max_particles, 0.0f);
    family_mask.assign(max_particles, 0u);
    flags.assign(max_particles, 0u);
    visual_id.assign(max_particles, 0u);
    burst_id.assign(max_particles, 0u);
    owner.assign(max_particles, EntityId{});
}

bool FluidBuffers::spawn(Vec2 position, Vec2 velocity, const FluidJetParams& jet) {
    if (count_ >= capacity_) return false;
    const usize i = count_++;
    pos_x[i] = position.x;
    pos_y[i] = position.y;
    vel_x[i] = velocity.x;
    vel_y[i] = velocity.y;
    // Seeded a substep behind, so the very first frame already has a sane
    // motion vector for the renderer's streak instead of a zero-length one
    // that reads as a stationary dot at the muzzle.
    prev_x[i] = position.x - velocity.x * (1.0f / 180.0f);
    prev_y[i] = position.y - velocity.y * (1.0f / 180.0f);
    density[i] = 0.0f;
    life[i] = jet.lifetime;
    life_max[i] = math::max(jet.lifetime, 1e-3f);
    dps[i] = jet.damage_per_second;
    family_mask[i] = jet.family_mask;
    flags[i] = fluid_flags::kAlive;
    visual_id[i] = jet.visual_id;
    burst_id[i] = jet.burst_id;
    owner[i] = jet.owner;
    return true;
}

void FluidBuffers::kill(usize index) {
    if (index >= count_) return;
    flags[index] |= fluid_flags::kPendingKill;
}

usize FluidBuffers::compact() {
    usize removed = 0;
    usize i = 0;
    while (i < count_) {
        if ((flags[i] & fluid_flags::kPendingKill) == 0) {
            ++i;
            continue;
        }
        const usize last = count_ - 1;
        if (i != last) {
            pos_x[i] = pos_x[last];
            pos_y[i] = pos_y[last];
            vel_x[i] = vel_x[last];
            vel_y[i] = vel_y[last];
            prev_x[i] = prev_x[last];
            prev_y[i] = prev_y[last];
            density[i] = density[last];
            life[i] = life[last];
            life_max[i] = life_max[last];
            dps[i] = dps[last];
            family_mask[i] = family_mask[last];
            flags[i] = flags[last];
            visual_id[i] = visual_id[last];
            burst_id[i] = burst_id[last];
            owner[i] = owner[last];
        }
        flags[last] = 0;
        --count_;
        ++removed;
        // Deliberately no ++i: the particle just swapped down into slot i has
        // not been examined yet.
    }
    return removed;
}

void FluidBuffers::clear() {
    for (usize i = 0; i < count_; ++i) flags[i] = 0;
    count_ = 0;
}

// ---------------------------------------------------------------------------
// FluidSystem — configuration
// ---------------------------------------------------------------------------

void FluidSystem::configure(const Rect& world_bounds, const FluidTuning& tuning) {
    tuning_ = tuning;
    bounds_ = world_bounds;
    grid_cell_size_ = math::max(tuning_.smoothing_radius, 0.05f);
    rest_density_ = derive_rest_density(grid_cell_size_, math::max(tuning_.rest_spacing, 0.02f));

    const Vec2 size = world_bounds.size();
    grid_dims_ = IVec2{math::max(1, static_cast<i32>(std::ceil(size.x / grid_cell_size_))),
                       math::max(1, static_cast<i32>(std::ceil(size.y / grid_cell_size_)))};
    const usize cells = static_cast<usize>(grid_dims_.x) * static_cast<usize>(grid_dims_.y);
    cell_start_.assign(cells + 1, 0u);
    cell_fill_.assign(cells + 1, 0u);
    sorted_.clear();

    const f32 cov = math::max(tuning_.coverage_cell_size, 0.05f);
    coverage_dims_ = IVec2{math::max(1, static_cast<i32>(std::ceil(size.x / cov))),
                           math::max(1, static_cast<i32>(std::ceil(size.y / cov)))};
    const usize cov_cells = static_cast<usize>(coverage_dims_.x) * static_cast<usize>(coverage_dims_.y);
    coverage_.assign(cov_cells, 0.0f);
    coverage_dps_.assign(cov_cells, 0.0f);
    coverage_owner_.assign(cov_cells, EntityId{});
    coverage_owner_mass_.assign(cov_cells, 0.0f);
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

u32 FluidSystem::emit(FluidBuffers& fluid, const FluidJetParams& jet, f32 dt) const {
    if (dt <= 0.0f || jet.flow_scale <= 0.0f) return 0;

    const Vec2 dir = math::normalize_safe(jet.direction);
    if (dir.x == 0.0f && dir.y == 0.0f) return 0;
    const Vec2 side{-dir.y, dir.x};

    const f32 spacing = math::max(tuning_.rest_spacing, 0.02f);
    const f32 mouth = math::max(jet.nozzle_radius * 2.0f, spacing);
    const f32 span = math::max(jet.speed * dt, 1e-4f);

    // Area swept by the nozzle this tick, divided by the area one particle
    // occupies at hexagonal rest packing. This is the whole emission-rate
    // model; see the header comment for why it is derived rather than authored.
    const f32 particle_area = spacing * spacing * 0.8660254f;
    const f32 exact = (mouth * span / particle_area) * jet.flow_scale;
    u32 count = static_cast<u32>(exact);
    // Dither the remainder instead of truncating, so a 9.4/tick rate really
    // averages 9.4. Hashed off the seed, so it stays deterministic.
    if (hash_unit(jet.seed ^ 0xC2B2AE35u) < (exact - static_cast<f32>(count))) ++count;
    if (count == 0) return 0;

    // Lay the slab out with the R2 low-discrepancy sequence rather than on a
    // lanes-by-rows grid.
    //
    // A grid is the obvious choice and it is subtly wrong: the particle count
    // is derived from swept area and is almost never a multiple of the lane
    // count, so the last row comes out short and leaves a systematic gap down
    // one edge of every slab. Emitted sixty times a second, that gap is not
    // noise -- it is a periodic void running the length of the jet, which the
    // surface tension then rounds into a neat row of holes in the middle of the
    // beam. R2 fills any rectangle evenly for ANY count, and offsetting the
    // sequence per slab keeps consecutive slabs from stacking their own pattern
    // on top of each other.
    //
    // Constants are 1/phi2 and 1/phi2^2 for the plastic number phi2, which is
    // the 2D generalisation of the golden ratio (Roberts 2018).
    constexpr f32 kR2A = 0.7548776662f;
    constexpr f32 kR2B = 0.5698402910f;
    const f32 base_u = hash_unit(jet.seed ^ 0x27D4EB2Fu);
    const f32 base_v = hash_unit(jet.seed ^ 0x165667B1u);

    u32 emitted = 0;
    for (u32 n = 0; n < count; ++n) {
        const f32 fn = static_cast<f32>(n);
        const f32 u = std::fmod(base_u + kR2A * fn, 1.0f);
        const f32 v = std::fmod(base_v + kR2B * fn, 1.0f);
        const u32 h = hash_u32(jet.seed * 2654435761u + n * 40503u);

        // Across the mouth, and BACK along the aim over exactly the distance
        // continuously-emitted fluid would have covered this tick.
        const Vec2 p = jet.origin + dir * (-v * span) + side * ((u - 0.5f) * mouth);

        // Launch jitter, small on purpose -- the spread that reads on screen is
        // the impact splash, not the nozzle.
        const f32 angle = (hash_unit(h ^ 0x9E3779B9u) - 0.5f) * 2.0f * jet.spread;
        const f32 ca = std::cos(angle);
        const f32 sa = std::sin(angle);
        const Vec2 launch{dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca};
        // A SLIGHT speed spread down the column keeps the beam from arriving as
        // one flat wall and gives the leading edge its ragged, liquid front.
        // Deliberately tiny: this is a differential velocity applied for the
        // droplet's whole two-second life, so even a few percent stretches the
        // column by several world units, and a stretched column under surface
        // tension beads up and opens visible voids down its middle.
        const f32 speed = jet.speed * (0.985f + 0.03f * hash_unit(h ^ 0x85EBCA6Bu));

        if (!fluid.spawn(p, launch * speed, jet)) break; // store full; drop the rest
        ++emitted;
    }
    return emitted;
}

u32 FluidSystem::splash(FluidBuffers& fluid, const FluidJetParams& jet, Vec2 origin, f32 radius,
                        f32 speed, u32 count) const {
    if (count == 0) return 0;
    const f32 r = math::max(radius, tuning_.rest_spacing);

    // Sunflower (Vogel) fill: golden-angle spiral with sqrt radial growth puts
    // `count` points at even density over the disc for ANY count, which is the
    // same reason emit() uses R2 for its slab. Rotated by the seed so two
    // splashes on the same tick do not pop in identical formation.
    constexpr f32 kGolden = 2.399963230f;
    const f32 spin = hash_unit(jet.seed ^ 0x27D4EB2Fu) * math::kTwoPi;

    u32 emitted = 0;
    for (u32 n = 0; n < count; ++n) {
        const f32 fn = static_cast<f32>(n) + 0.5f;
        const f32 t = std::sqrt(fn / static_cast<f32>(count));   // 0..1, area-uniform
        const f32 ang = fn * kGolden + spin;
        const Vec2 radial{std::cos(ang), std::sin(ang)};
        const Vec2 p = origin + radial * (t * r);
        const u32 h = hash_u32(jet.seed * 2654435761u + n * 40503u);
        // Rim droplets fly, centre droplets settle: the blob bursts outward
        // from a wet middle rather than every droplet leaving at once.
        const f32 launch = speed * (0.35f + 0.65f * t) * (0.9f + 0.2f * hash_unit(h));
        if (!fluid.spawn(p, radial * launch, jet)) break;   // store full; drop the rest
        ++emitted;
    }
    return emitted;
}

// ---------------------------------------------------------------------------
// Neighbour grid
// ---------------------------------------------------------------------------

void FluidSystem::build_grid(const FluidBuffers& fluid) {
    const usize cells = static_cast<usize>(grid_dims_.x) * static_cast<usize>(grid_dims_.y);
    if (cells == 0) return;
    const usize n = fluid.count();
    sorted_.resize(n);

    // Counting sort: one pass to count, a prefix sum, one pass to scatter. Both
    // arrays are sized at configure() time, so this allocates nothing.
    std::fill(cell_start_.begin(), cell_start_.end(), 0u);
    const f32 inv = 1.0f / grid_cell_size_;
    const Vec2 origin = bounds_.min;

    auto cell_of = [&](usize i) -> usize {
        const i32 cx = math::clamp(static_cast<i32>(std::floor((fluid.pos_x[i] - origin.x) * inv)),
                                   0, grid_dims_.x - 1);
        const i32 cy = math::clamp(static_cast<i32>(std::floor((fluid.pos_y[i] - origin.y) * inv)),
                                   0, grid_dims_.y - 1);
        return static_cast<usize>(cy) * static_cast<usize>(grid_dims_.x) + static_cast<usize>(cx);
    };

    for (usize i = 0; i < n; ++i) ++cell_start_[cell_of(i) + 1];
    for (usize c = 0; c < cells; ++c) cell_start_[c + 1] += cell_start_[c];
    std::copy(cell_start_.begin(), cell_start_.end(), cell_fill_.begin());
    for (usize i = 0; i < n; ++i) sorted_[cell_fill_[cell_of(i)]++] = static_cast<u32>(i);
}

f32 FluidSystem::coverage_at(Vec2 world_pos) const {
    if (coverage_.empty()) return 0.0f;
    const f32 cs = math::max(tuning_.coverage_cell_size, 1e-3f);
    const Vec2 g = (world_pos - bounds_.min) / cs - Vec2{0.5f, 0.5f};
    const f32 fx = std::floor(g.x);
    const f32 fy = std::floor(g.y);
    const i32 x0 = static_cast<i32>(fx);
    const i32 y0 = static_cast<i32>(fy);
    const f32 tx = g.x - fx;
    const f32 ty = g.y - fy;
    auto at = [&](i32 x, i32 y) -> f32 {
        x = math::clamp(x, 0, coverage_dims_.x - 1);
        y = math::clamp(y, 0, coverage_dims_.y - 1);
        return coverage_[static_cast<usize>(y) * static_cast<usize>(coverage_dims_.x) +
                         static_cast<usize>(x)];
    };
    const f32 mass =
        math::bilerp(at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx, ty);
    return math::saturate(mass / math::max(tuning_.coverage_full, 1e-3f));
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

FluidStats FluidSystem::update(FluidBuffers& fluid,
                               ChaffBuffers& chaff,
                               const SpatialHash& hash,
                               const DistanceField& sdf,
                               const Rect& world_bounds,
                               f32 dt,
                               CombatEventSink* events) {
    WallClock timer;
    FluidStats stats{};
    const usize n = fluid.count();
    stats.live = static_cast<u32>(n);
    if (n == 0 || dt <= 0.0f) {
        std::fill(coverage_.begin(), coverage_.end(), 0.0f);
        std::fill(coverage_dps_.begin(), coverage_dps_.end(), 0.0f);
        stats.solve_ms = static_cast<f32>(timer.elapsed_ms());
        last_ = stats;
        return stats;
    }

    const f32 h = grid_cell_size_;
    const f32 inv_h = 1.0f / h;
    const f32 h2 = h * h;
    const u32 substeps = math::max(1u, tuning_.substeps);
    const f32 sdt = dt / static_cast<f32>(substeps);
    const f32 rho0 = rest_density_;
    const f32 max_speed = tuning_.max_speed;
    const f32 contact = tuning_.rest_spacing * kWallContactScale;
    const f32 max_correction = tuning_.rest_spacing * kMaxCorrectionScale;

    // Crowd braking reads the chaff spatial hash's occupancy, which does not
    // move underneath us during this call — so it is sampled per substep from a
    // stable grid rather than re-queried per agent. No pair tests anywhere.
    const u32* occupancy = hash.occupancy();
    const IVec2 hash_dims = hash.grid_dims();
    const f32 hash_cell = math::max(hash.cell_size(), 1e-3f);
    const Rect hash_bounds = hash.bounds();
    const f32 inv_full_occ = 1.0f / math::max(tuning_.chaff_full_occupancy, 1e-3f);

    auto crowd_at = [&](f32 x, f32 y) -> f32 {
        if (occupancy == nullptr || hash_dims.x <= 0 || hash_dims.y <= 0) return 0.0f;
        const i32 cx = static_cast<i32>(std::floor((x - hash_bounds.min.x) / hash_cell));
        const i32 cy = static_cast<i32>(std::floor((y - hash_bounds.min.y) / hash_cell));
        if (cx < 0 || cy < 0 || cx >= hash_dims.x || cy >= hash_dims.y) return 0.0f;
        const u32 occ = occupancy[static_cast<usize>(cy) * static_cast<usize>(hash_dims.x) +
                                  static_cast<usize>(cx)];
        return math::saturate(static_cast<f32>(occ) * inv_full_occ);
    };

    // Splashes are collected as they happen and pushed at the very end, so a
    // full sink can never reorder them against another system's events.
    struct Splash {
        Vec2 at;
        Vec2 dir;
        f32 magnitude;
        u16 visual;
    };
    Splash splashes[64];
    u32 splash_count = 0;
    const u32 splash_cap = math::min(tuning_.max_splash_events, 64u);

    for (u32 step = 0; step < substeps; ++step) {
        const bool last_step = (step + 1 == substeps);
        if (last_step) stats.wall_contacts = 0;

        // ---- 1. Ambient drag and crowd braking ----------------------------
        // Both are per-second rates converted to this substep, so retuning the
        // substep count does not change the feel.
        const f32 drag_factor = 1.0f / (1.0f + tuning_.drag * sdt);
        for (usize i = 0; i < n; ++i) {
            f32 vx = fluid.vel_x[i] * drag_factor;
            f32 vy = fluid.vel_y[i] * drag_factor;

            const f32 crowd = crowd_at(fluid.pos_x[i], fluid.pos_y[i]);
            if (crowd > 0.0f) {
                // Braking, not deflection. The jet piles up where the horde is
                // thick, and the relaxation pass below is what turns that pile
                // into a sideways splash — deflecting here instead would give
                // every particle the same scripted bounce.
                const f32 brake = 1.0f / (1.0f + tuning_.chaff_drag * crowd * sdt);
                vx *= brake;
                vy *= brake;
            }

            const f32 sp2 = vx * vx + vy * vy;
            if (sp2 > max_speed * max_speed) {
                const f32 s = max_speed / std::sqrt(sp2);
                vx *= s;
                vy *= s;
            }
            fluid.vel_x[i] = vx;
            fluid.vel_y[i] = vy;
        }

        build_grid(fluid);

        // ---- 2. Viscosity impulses ----------------------------------------
        // Clavet's pairwise inward impulse: neighbours approaching each other
        // are pushed toward a common velocity. This is the single knob that
        // separates water from mucus, and it is why the beam travels as a rope
        // instead of dispersing into a cone of independent dots.
        //
        // Symmetric (i loses exactly what j gains) and visited once per pair
        // via the j > i guard, so momentum is conserved to the bit.
        const f32 visc_lin = tuning_.viscosity_linear;
        const f32 visc_quad = tuning_.viscosity_quadratic;
        if (visc_lin > 0.0f || visc_quad > 0.0f) {
            // HALF-STENCIL. Every impulse is symmetric, so each unordered pair
            // must be visited exactly once. Walking all nine cells and skipping
            // j <= i does that correctly but iterates twice the candidates it
            // uses; these five offsets tile the plane so that (cell, cell+off)
            // covers each neighbouring pair of cells once, and the self-cell is
            // handled with the j > i guard. Same pairs, half the walk.
            constexpr i32 kHalf[5][2] = {{0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
            for (usize i = 0; i < n; ++i) {
                const f32 px = fluid.pos_x[i];
                const f32 py = fluid.pos_y[i];
                const i32 cx = math::clamp(
                    static_cast<i32>(std::floor((px - bounds_.min.x) * inv_h)), 0, grid_dims_.x - 1);
                const i32 cy = math::clamp(
                    static_cast<i32>(std::floor((py - bounds_.min.y) * inv_h)), 0, grid_dims_.y - 1);
                for (u32 o = 0; o < 5; ++o) {
                    const i32 gx = cx + kHalf[o][0];
                    const i32 gy = cy + kHalf[o][1];
                    if (gx < 0 || gy < 0 || gx >= grid_dims_.x || gy >= grid_dims_.y) continue;
                    const bool self_cell = (o == 0);
                    const usize cell = static_cast<usize>(gy) * static_cast<usize>(grid_dims_.x) +
                                       static_cast<usize>(gx);
                    for (u32 sidx = cell_start_[cell]; sidx < cell_start_[cell + 1]; ++sidx) {
                        const usize j = sorted_[sidx];
                        if (self_cell && j <= i) continue;
                        const f32 dx = fluid.pos_x[j] - px;
                        const f32 dy = fluid.pos_y[j] - py;
                        const f32 d2 = dx * dx + dy * dy;
                        if (d2 >= h2 || d2 < 1e-8f) continue;
                        const f32 d = std::sqrt(d2);
                        const f32 q = d * inv_h;
                        const f32 nx = dx / d;
                        const f32 ny = dy / d;
                        // Inward radial velocity. Negative means separating,
                        // and separating neighbours are left alone -- damping
                        // those too turns the fluid into glue and kills every
                        // splash.
                        const f32 u = (fluid.vel_x[i] - fluid.vel_x[j]) * nx +
                                      (fluid.vel_y[i] - fluid.vel_y[j]) * ny;
                        if (u <= 0.0f) continue;
                        f32 imp = sdt * (1.0f - q) * (visc_lin * u + visc_quad * u * u) * 0.5f;
                        // Never overshoot past a shared velocity: an impulse
                        // bigger than the approach itself would swap the pair's
                        // order and inject energy.
                        imp = math::min(imp, u * 0.5f);
                        fluid.vel_x[i] -= imp * nx;
                        fluid.vel_y[i] -= imp * ny;
                        fluid.vel_x[j] += imp * nx;
                        fluid.vel_y[j] += imp * ny;
                    }
                }
            }
        }

        // ---- 3. Advance positions -----------------------------------------
        for (usize i = 0; i < n; ++i) {
            fluid.prev_x[i] = fluid.pos_x[i];
            fluid.prev_y[i] = fluid.pos_y[i];
            fluid.pos_x[i] += fluid.vel_x[i] * sdt;
            fluid.pos_y[i] += fluid.vel_y[i] * sdt;
        }

        // Rebuilt on the advected positions. The jet moves up to ~0.4 world
        // units in a substep against a 0.95 cell, so the pre-advection grid
        // would already be missing neighbours at the leading edge — exactly
        // where the beam most needs its density estimate to be right.
        build_grid(fluid);

        // ---- 4. Double-density relaxation ---------------------------------
        // The heart of it. Two sums per particle, then position corrections
        // scattered onto the neighbours. `pressure` may go NEGATIVE below rest
        // density and that is deliberate: it is the surface tension that holds
        // the beam together and beads up the splash.
        const f32 sdt2 = sdt * sdt;
        for (usize i = 0; i < n; ++i) {
            const f32 px = fluid.pos_x[i];
            const f32 py = fluid.pos_y[i];
            const i32 cx = math::clamp(
                static_cast<i32>(std::floor((px - bounds_.min.x) * inv_h)), 0, grid_dims_.x - 1);
            const i32 cy = math::clamp(
                static_cast<i32>(std::floor((py - bounds_.min.y) * inv_h)), 0, grid_dims_.y - 1);

            // Gather once, use twice, and cache the geometry.
            //
            // The density pass and the force pass need exactly the same
            // per-neighbour numbers (offset and distance), and nothing moves
            // between them, so computing them twice would double this loop's
            // square roots for no reason. The list also lives in a fixed stack
            // array rather than a std::vector: this is the innermost loop in
            // the solver, and a push_back per neighbour per particle per
            // substep was measurably the single most expensive thing in it.
            //
            // OVERFLOW IS FINE. kMaxNeighbours is comfortably above what the
            // rest spacing against the smoothing radius can produce, so hitting
            // it means the fluid is locally compressed far past anything the
            // tuning intends; dropping the surplus makes that region a little
            // softer instead of unbounded, which is the safe direction.
            struct Neighbour { u32 j; f32 dx, dy, d; };
            constexpr u32 kMaxNeighbours = 64;
            Neighbour nb[kMaxNeighbours];
            u32 nb_count = 0;

            f32 rho = 0.0f;
            f32 rho_near = 0.0f;
            for (i32 gy = cy - 1; gy <= cy + 1; ++gy) {
                if (gy < 0 || gy >= grid_dims_.y) continue;
                for (i32 gx = cx - 1; gx <= cx + 1; ++gx) {
                    if (gx < 0 || gx >= grid_dims_.x) continue;
                    const usize cell = static_cast<usize>(gy) * static_cast<usize>(grid_dims_.x) +
                                       static_cast<usize>(gx);
                    for (u32 s = cell_start_[cell]; s < cell_start_[cell + 1]; ++s) {
                        const u32 j = sorted_[s];
                        if (static_cast<usize>(j) == i) continue;
                        const f32 dx = fluid.pos_x[j] - px;
                        const f32 dy = fluid.pos_y[j] - py;
                        const f32 d2 = dx * dx + dy * dy;
                        if (d2 >= h2 || d2 < 1e-8f) continue;
                        const f32 d = std::sqrt(d2);
                        const f32 q = d * inv_h;
                        rho += kernel_far(q);
                        rho_near += kernel_near(q);
                        if (nb_count < kMaxNeighbours) nb[nb_count++] = Neighbour{j, dx, dy, d};
                    }
                }
            }
            fluid.density[i] = rho / rho0;

            // Signed. Below rest density this is an ATTRACTION, and that is
            // the surface tension: it is what makes the beam a rope in flight
            // and beads the splash into droplets instead of a haze.
            const f32 pressure = tuning_.stiffness * (rho - rho0);
            const f32 pressure_near = tuning_.near_stiffness * rho_near;

            f32 dx_self = 0.0f;
            f32 dy_self = 0.0f;
            for (u32 k = 0; k < nb_count; ++k) {
                const f32 q = nb[k].d * inv_h;
                f32 mag = sdt2 * (pressure * (1.0f - q) + pressure_near * kernel_far(q));
                mag = math::clamp(mag, -max_correction, max_correction);
                const f32 inv_d = 0.5f * mag / nb[k].d;
                const f32 hx = nb[k].dx * inv_d;
                const f32 hy = nb[k].dy * inv_d;
                fluid.pos_x[nb[k].j] += hx;
                fluid.pos_y[nb[k].j] += hy;
                dx_self -= hx;
                dy_self -= hy;
            }
            fluid.pos_x[i] += dx_self;
            fluid.pos_y[i] += dy_self;
        }

        // ---- 5. Boundaries, by projection ---------------------------------
        for (usize i = 0; i < n; ++i) {
            f32 px = math::clamp(fluid.pos_x[i], world_bounds.min.x + 0.05f, world_bounds.max.x - 0.05f);
            f32 py = math::clamp(fluid.pos_y[i], world_bounds.min.y + 0.05f, world_bounds.max.y - 0.05f);

            const f32 clearance = sdf.sample(Vec2{px, py});
            if (clearance < contact) {
                Vec2 nrm = sdf.gradient(Vec2{px, py});
                const f32 len2 = nrm.x * nrm.x + nrm.y * nrm.y;
                if (len2 > 1e-8f) {
                    const f32 inv_len = 1.0f / std::sqrt(len2);
                    // Push out to the contact surface. Velocity is recovered
                    // from the position delta below, so this removes exactly
                    // the normal component and leaves the tangential one — the
                    // fluid keeps running along the wall instead of bouncing.
                    const f32 push = contact - clearance;
                    px += nrm.x * inv_len * push;
                    py += nrm.y * inv_len * push;
                    fluid.flags[i] |= fluid_flags::kOnWall;
                    if (last_step) ++stats.wall_contacts;
                } else {
                    // Degenerate gradient — deep inside a solid block, where the
                    // field is flat and there is no "out" to push along. Freeze
                    // the particle and let its lifetime take it.
                    px = fluid.prev_x[i];
                    py = fluid.prev_y[i];
                }
            } else {
                fluid.flags[i] &= static_cast<u8>(~fluid_flags::kOnWall);
            }

            fluid.pos_x[i] = px;
            fluid.pos_y[i] = py;
        }

        // ---- 6. Recover velocity from the position delta ------------------
        const f32 inv_sdt = 1.0f / sdt;
        for (usize i = 0; i < n; ++i) {
            const f32 vx_old = fluid.vel_x[i];
            const f32 vy_old = fluid.vel_y[i];
            f32 vx = (fluid.pos_x[i] - fluid.prev_x[i]) * inv_sdt;
            f32 vy = (fluid.pos_y[i] - fluid.prev_y[i]) * inv_sdt;

            if ((fluid.flags[i] & fluid_flags::kOnWall) != 0) {
                vx *= tuning_.wall_friction;
                vy *= tuning_.wall_friction;
            }

            const f32 sp2 = vx * vx + vy * vy;
            if (sp2 > max_speed * max_speed) {
                const f32 s = max_speed / std::sqrt(sp2);
                vx *= s;
                vy *= s;
            }

            // An impact is a large single-substep change of velocity. That one
            // test covers a wall strike, a plunge into a thick crowd, and a
            // pressure ejection out of a pile — which is right, because to the
            // player they are the same event: the jet stopped going where it
            // was going and threw liquid sideways.
            if ((fluid.flags[i] & fluid_flags::kSplashed) == 0) {
                const f32 dvx = vx - vx_old;
                const f32 dvy = vy - vy_old;
                const f32 shock = std::sqrt(dvx * dvx + dvy * dvy);
                if (shock > tuning_.splash_speed_threshold) {
                    fluid.flags[i] |= fluid_flags::kSplashed;
                    ++stats.splashes;
                    if (splash_count < splash_cap) {
                        splashes[splash_count++] =
                            Splash{Vec2{fluid.pos_x[i], fluid.pos_y[i]},
                                   math::normalize_safe(Vec2{vx_old, vy_old}), shock,
                                   fluid.visual_id[i]};
                    }
                }
            }

            fluid.vel_x[i] = vx;
            fluid.vel_y[i] = vy;
        }
    }

    // ---- 7. Coverage splat ------------------------------------------------
    // Every particle deposits its mass and its share of the damage rate into
    // the four nearest cells. One pass, no pair tests, and this grid is what
    // every consumer below reads.
    std::fill(coverage_.begin(), coverage_.end(), 0.0f);
    std::fill(coverage_dps_.begin(), coverage_dps_.end(), 0.0f);
    if (attribution_ != nullptr) {
        std::fill(coverage_owner_.begin(), coverage_owner_.end(), EntityId{});
        std::fill(coverage_owner_mass_.begin(), coverage_owner_mass_.end(), 0.0f);
    }
    const f32 cov_cs = math::max(tuning_.coverage_cell_size, 1e-3f);
    const i32 cov_w = coverage_dims_.x;
    const i32 cov_h = coverage_dims_.y;
    for (usize i = 0; i < n; ++i) {
        // Fade the deposit out with the particle, so a jet that is evaporating
        // stops burning at the same moment it stops being drawn.
        const f32 weight = math::saturate(fluid.life[i] / kFadeWindow);
        if (weight <= 0.0f) continue;
        const Vec2 g = (Vec2{fluid.pos_x[i], fluid.pos_y[i]} - bounds_.min) / cov_cs - Vec2{0.5f, 0.5f};
        const f32 fx = std::floor(g.x);
        const f32 fy = std::floor(g.y);
        const i32 x0 = static_cast<i32>(fx);
        const i32 y0 = static_cast<i32>(fy);
        const f32 tx = g.x - fx;
        const f32 ty = g.y - fy;
        const f32 w[4] = {(1.0f - tx) * (1.0f - ty), tx * (1.0f - ty), (1.0f - tx) * ty, tx * ty};
        const i32 xs[4] = {x0, x0 + 1, x0, x0 + 1};
        const i32 ys[4] = {y0, y0, y0 + 1, y0 + 1};
        const f32 rate = fluid.dps[i];
        for (u32 c = 0; c < 4; ++c) {
            const i32 x = xs[c];
            const i32 y = ys[c];
            if (x < 0 || y < 0 || x >= cov_w || y >= cov_h) continue;
            const usize idx = static_cast<usize>(y) * static_cast<usize>(cov_w) + static_cast<usize>(x);
            const f32 m = w[c] * weight;
            coverage_[idx] += m;
            coverage_dps_[idx] += m * rate;
            if (attribution_ != nullptr && m > coverage_owner_mass_[idx]) {
                coverage_owner_mass_[idx] = m;
                coverage_owner_[idx] = fluid.owner[i];
            }
        }
    }

    // ---- 8. Damage, one sweep over the chaff ------------------------------
    const f32 inv_full = 1.0f / math::max(tuning_.coverage_full, 1e-3f);
    const usize agents = chaff.count();
    for (usize a = 0; a < agents; ++a) {
        if ((chaff.flags[a] & chaff_flags::kPendingKill) != 0) continue;
        const i32 cx = static_cast<i32>(std::floor((chaff.pos_x[a] - bounds_.min.x) / cov_cs));
        const i32 cy = static_cast<i32>(std::floor((chaff.pos_y[a] - bounds_.min.y) / cov_cs));
        if (cx < 0 || cy < 0 || cx >= cov_w || cy >= cov_h) continue;
        const usize idx = static_cast<usize>(cy) * static_cast<usize>(cov_w) + static_cast<usize>(cx);
        const f32 mass = coverage_[idx];
        if (mass <= 1e-4f) continue;

        // Mass-weighted mean rate, then saturating wetness. A film half a
        // particle thick does half the damage; a puddle four deep does not do
        // four times as much, because a pathogen can only be so coated.
        //
        // Deliberately does NOT read kMarked back here. The mark this loop is
        // about to apply below is a debuff for every OTHER tower's damage, not
        // a self-buff — see the role note below.
        const f32 wetness = math::saturate(mass * inv_full);
        const f32 amount = (coverage_dps_[idx] / mass) * wetness * dt;

        const f32 before = chaff.density[a];
        chaff.apply_density_loss(a, amount);
        const f32 removed = before - chaff.density[a];
        stats.density_removed += removed;

        // Off by default; see sim/Attribution.h and coverage_owner_'s note.
        if (attribution_ != nullptr && removed > 0.0f && coverage_owner_[idx].valid()) {
            const bool killed = (chaff.flags[a] & chaff_flags::kPendingKill) != 0;
            attribution_->record_chaff(coverage_owner_[idx], chaff.family[a], removed, killed);
        }
        // Mucus marks its target, and nothing in the sim ever clears kMarked
        // (see the note on the cryo cone in TowerSystem.cpp for the other
        // permanent-flag precedent), so this is a permanent debuff on anything
        // the Goblet Cell has soaked. It is deliberately NOT read back into
        // `amount` above: this tower does not hit harder for having marked
        // something, every OTHER tower does (DamageField.cpp, Projectiles.cpp,
        // Swarmers.cpp, and strike_named's comp::Marked check all consume the
        // same flag/component). The role stays "softens up whatever it touches
        // for the rest of the roster," not "out-damages the roster itself" —
        // and, on purpose, not "holds the lane" either: the Goblet Cell does
        // NOT apply kSlowed. It weakens, it does not root, so it stacks with
        // the towers whose whole identity IS the root (Interferon's cone,
        // Neutrophil's NET) instead of making their kill zone redundant.
        chaff.flags[a] |= chaff_flags::kMarked;
    }

    // ---- 9. Lifetime and retirement ---------------------------------------
    for (usize i = 0; i < n; ++i) {
        fluid.life[i] -= dt;
        if (fluid.life[i] <= 0.0f) {
            fluid.kill(i);
            ++stats.expired;
        }
    }
    fluid.compact();
    stats.live = static_cast<u32>(fluid.count());

    // ---- 10. Publish splash events ----------------------------------------
    if (events != nullptr) {
        for (u32 s = 0; s < splash_count; ++s) {
            CombatEvent e;
            e.type = CombatEventType::FluidSplash;
            e.source = TowerType::GobletCell;
            e.visual_id = splashes[s].visual;
            e.origin = splashes[s].at;
            e.secondary = splashes[s].at;
            e.direction = splashes[s].dir;
            e.radius = tuning_.rest_spacing * 2.0f;
            e.magnitude = splashes[s].magnitude;
            events->push(e);
        }
    }

    stats.solve_ms = static_cast<f32>(timer.elapsed_ms());
    last_ = stats;
    return stats;
}

} // namespace immune::sim
