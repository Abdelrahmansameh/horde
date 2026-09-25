// vfx/Particles.cpp — the cosmetic particle layer. Owner: Wave 6B.
//
// See Particles.h for the architectural rationale (why this is deliberately
// outside the sim). This file is the implementation plus the ART AUTHORING:
// emit_for_event() is where each tower's visual identity actually lives.
//
// ---------------------------------------------------------------------------
// ENCODING CONVENTIONS THE SHADER MUST MATCH (assets/shaders/particle.*)
// ---------------------------------------------------------------------------
// ParticleInstance is a frozen 48-byte layout, but the *meaning* of a couple of
// its fields is kind-dependent. The CPU cannot express "grow this ring" or
// "draw a segment" without a per-kind branch in the vectorized update, so those
// two things are pushed to the shader and encoded as follows:
//
//   Tracer / Spark / Shard / Mist  (point kinds)
//     x, y      world position
//     vx, vy    world velocity — the shader orients the streak along it and
//               may scale streak length by |v|
//     size      world-space radius/half-extent, CONSTANT over the lifetime
//     rotation  radians (Shard/Mist spin; ignored by Spark)
//
//   Ring
//     x, y      centre (rings are emitted with zero velocity)
//     size      the FINAL radius. The shader expands from ~0 to `size` using
//               age_norm, e.g. r = size * smoothstep(age_norm) — the CPU stores
//               the endpoint of the animation, not the current value.
//
//   Beam / Bolt  (segment kinds)
//     x, y      segment START point
//     vx, vy    UNIT direction along the segment (so the tiny drift the shared
//               integrator applies is <0.1 world units over the lifetime and
//               invisible, instead of the segment sliding off its own target)
//     size      segment LENGTH in world units. Thickness is a shader constant
//               per kind, modulated by seed.
//     seed      per-particle randomness — Bolt uses it to pick its jag pattern
//               so two overlapping arcs never look identical.
//
//   tint_rgba8  byte order r,g,b,a (r in the low byte). Alpha is ALREADY
//               multiplied by the CPU-side fade envelope; the shader may apply
//               further per-kind curves on top of age_norm.
//   kind_blend  ParticleKind in the low 16 bits, BlendMode in the high 16.
//   age_norm    saturate(age / lifetime).
//
// DELAYED PARTICLES
// A particle's age starts NEGATIVE when it is emitted with a delay. The
// integrator is untouched by this (age simply ticks up through zero, and the
// retire test `age >= lifetime` still works), and build_instances() skips
// anything with age < 0. That gives free, branch-free-in-the-hot-loop staging,
// which is what a staged burst impact needs: land, ~0.05 s charge,
// then the big ring. Nothing in the update loop knows about it.
#include "vfx/Particles.h"

#include "vfx/DeathVfx.h"

#include "core/Clock.h"
#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Rng.h"
#include "sim/swarm/Swarmers.h"   // kSwarmerEventBit

#include <cmath>

namespace immune::vfx {
namespace {

// ---------------------------------------------------------------------------
// This layer's OWN randomness.
//
// PCG32 stepped directly on ParticleSystem::rng_state_. It is deliberately NOT
// the sim's Rng: drawing from the sim stream here would let a purely cosmetic
// change (one more spark) alter gameplay and break state_hash() reproducibility.
// Same algorithm as core/Rng.h so the quality is known, but a private stream.
// ---------------------------------------------------------------------------
inline u32 pcg_next(u64& state) {
    const u64 old = state;
    state = old * 6364136223846793005ull + 1442695040888963407ull;
    const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
    const u32 rot = static_cast<u32>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
}

inline f32 pcg_f32(u64& s) { return static_cast<f32>(pcg_next(s) >> 8) * 0x1.0p-24f; }
inline f32 pcg_range(u64& s, f32 lo, f32 hi) { return lo + (hi - lo) * pcg_f32(s); }
inline f32 pcg_signed(u64& s) { return pcg_f32(s) * 2.0f - 1.0f; }

inline Vec2 rotate_by(Vec2 v, f32 radians) {
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    return Vec2{v.x * c - v.y * s, v.x * s + v.y * c};
}

inline Vec2 perp(Vec2 v) { return Vec2{-v.y, v.x}; }

inline u32 pack_rgba(const Vec4& c) {
    const u32 r = static_cast<u32>(math::saturate(c.r) * 255.0f + 0.5f);
    const u32 g = static_cast<u32>(math::saturate(c.g) * 255.0f + 0.5f);
    const u32 b = static_cast<u32>(math::saturate(c.b) * 255.0f + 0.5f);
    const u32 a = static_cast<u32>(math::saturate(c.a) * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16) | (a << 24);
}

inline Vec4 mix4(const Vec4& a, const Vec4& b, f32 t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------
// Palettes. Five towers, five clearly separable hues — the brief's requirement
// is that a player glancing at the screen knows which tower's swarm this is,
// and hue is the only channel that survives a glance at 60 fps. Mirrored by
// swarmer_tint() in render/Renderer.cpp so the swarmers, their release and
// what they leave behind are all one colour.
//
//   Neutrophil SHOOTER       warm white-yellow
//   Macrophage ARBOR GRABBER orange (digestive, not fire — no reds)
//   Interferon SLOW BOMBER   blue-white / cyan
//   CytotoxicT LATCH         violet-white
//   GobletCell MUCUS BOMBER  jade green (mucin)
//   Fibroblast BUILDER       salmon / dusty rose (collagen)
// ---------------------------------------------------------------------------
struct TowerPalette {
    Vec4 primary;  ///< The hue a player identifies the tower by.
    Vec4 accent;   ///< Near-white core, for the hot centre of a flash.
};

TowerPalette palette_for(TowerType t) {
    switch (t) {
    case TowerType::Neutrophil: return {Vec4{1.00f, 0.96f, 0.68f, 1.0f}, Vec4{1.00f, 1.00f, 0.94f, 1.0f}};
    case TowerType::Macrophage: return {Vec4{1.00f, 0.56f, 0.14f, 1.0f}, Vec4{1.00f, 0.95f, 0.88f, 1.0f}};
    case TowerType::Interferon: return {Vec4{0.52f, 0.84f, 1.00f, 1.0f}, Vec4{0.88f, 0.98f, 1.00f, 1.0f}};
    case TowerType::CytotoxicT: return {Vec4{0.76f, 0.66f, 1.00f, 1.0f}, Vec4{1.00f, 1.00f, 1.00f, 1.0f}};
    case TowerType::GobletCell: return {Vec4{0.55f, 0.98f, 0.74f, 1.0f}, Vec4{0.90f, 1.00f, 0.92f, 1.0f}};
    case TowerType::Fibroblast: return {Vec4{1.00f, 0.72f, 0.64f, 1.0f}, Vec4{1.00f, 0.94f, 0.90f, 1.0f}};
    case TowerType::Count:
    default:                    return {Vec4{0.88f, 0.90f, 0.96f, 1.0f}, Vec4{1.00f, 1.00f, 1.00f, 1.0f}};
    }
}

/// A *subtle* per-family shift applied to impact/shatter debris only, so a pop
/// on a virus reads slightly differently from one on a bacterium without ever
/// competing with the tower's own identity hue.
Vec4 family_tint(PathogenFamily f) {
    switch (f) {
    case PathogenFamily::Virus:       return Vec4{0.85f, 0.55f, 1.00f, 1.0f};
    case PathogenFamily::Bacteria:    return Vec4{0.58f, 1.00f, 0.62f, 1.0f};
    case PathogenFamily::Count:
    default:                          return Vec4{1.00f, 1.00f, 1.00f, 1.0f};
    }
}

constexpr f32 kFamilyTintWeight = 0.18f;

// ---------------------------------------------------------------------------
// The travelling-granule CHAIN look.
//
// NO TOWER RAISES ChainArc ANY MORE. The Cytotoxic T used to, back when it was
// a chain weapon; it now releases real simulated granules instead
// (sim/swarm/Swarmers.h), which steer and choose their own targets and are
// therefore drawn from sim state by the renderer, not from events here.
//
// The case below is kept because ChainArc is still a live CombatEventType —
// the gym's `vfx chain` raises it, test_particles.cpp requires every type to
// render something, and any future chain weapon gets a finished look for free.
// If a chain tower ever comes back, this is what it should look like: a payload
// physically flying target to target with a pore-and-spill at each landing,
// never an instantaneous electrical arc.
constexpr f32 kCtlHopSeconds = 0.045f;

// The per-hop energy falloff a chain source is expected to bake into
// CombatEvent::magnitude, which is the only per-hop ordering information that
// reaches this layer — inverting it is how a hop learns its own index, and the
// index is what staggers the flight. A source that does not follow this
// convention merely loses the stagger and draws its hops together.
constexpr f32 kCtlHopFalloff = 0.72f;

// ---------------------------------------------------------------------------
// The integrator. Deliberately branch-free and pointer-based: MSVC will
// auto-vectorize this (all streams are distinct f32 arrays, hence __restrict)
// and it is the only thing that runs over all 250k particles every frame.
//
// Drag is applied as an implicit-Euler damp, 1/(1 + k*dt), rather than
// (1 - k*dt): the explicit form goes unstable (and flips velocity) as soon as
// k*dt > 1, which a 0.03s frame with a drag of 40 hits immediately.
// ---------------------------------------------------------------------------
void integrate_range(f32* __restrict px, f32* __restrict py,
                     f32* __restrict vx, f32* __restrict vy,
                     const f32* __restrict drag, const f32* __restrict buoy,
                     f32* __restrict rot, const f32* __restrict spin,
                     f32* __restrict age,
                     usize begin, usize end, f32 dt) {
    for (usize i = begin; i < end; ++i) {
        const f32 damp = 1.0f / (1.0f + drag[i] * dt);
        const f32 nvx = vx[i] * damp;
        const f32 nvy = (vy[i] + buoy[i] * dt) * damp;
        vx[i] = nvx;
        vy[i] = nvy;
        px[i] += nvx * dt;
        py[i] += nvy * dt;
        rot[i] += spin[i] * dt;
        age[i] += dt;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

void ParticleSystem::init(usize capacity, u64 seed) {
    capacity_ = capacity;
    rng_state_ = seed ? seed : 0x9E3779B97F4A7C15ull;
    // Warm the stream: a raw LCG state of `seed` correlates the first few
    // outputs across nearby seeds, which would make two levels loaded one
    // seed apart emit visibly identical first bursts.
    pcg_next(rng_state_);
    pcg_next(rng_state_);

    pos_x.assign(capacity, 0.0f);
    pos_y.assign(capacity, 0.0f);
    vel_x.assign(capacity, 0.0f);
    vel_y.assign(capacity, 0.0f);
    size_.assign(capacity, 0.0f);
    rot_.assign(capacity, 0.0f);
    spin_.assign(capacity, 0.0f);
    age_.assign(capacity, 0.0f);
    life_.assign(capacity, 1.0f);
    drag_.assign(capacity, 0.0f);
    buoy_.assign(capacity, 0.0f);
    end_x_.assign(capacity, 0.0f);
    end_y_.assign(capacity, 0.0f);
    seed_.assign(capacity, 0.0f);
    color_.assign(capacity, 0u);
    kind_.assign(capacity, 0u);
    blend_.assign(capacity, 0u);

    count_ = 0;
    stats_ = ParticleStats{};
}

void ParticleSystem::shutdown() {
    pos_x.clear(); pos_y.clear(); vel_x.clear(); vel_y.clear();
    size_.clear(); rot_.clear(); spin_.clear(); age_.clear(); life_.clear();
    drag_.clear(); buoy_.clear(); end_x_.clear(); end_y_.clear(); seed_.clear();
    color_.clear(); kind_.clear(); blend_.clear();
    count_ = 0;
    capacity_ = 0;
    stats_ = ParticleStats{};
}

void ParticleSystem::clear() {
    count_ = 0;
    stats_.live = 0;
    stats_.spawned_this_frame = 0;
    stats_.retired_this_frame = 0;
    // dropped_total is documented as cumulative — a level transition must not
    // hide the fact that the budget was blown on the previous level.
}

bool ParticleSystem::spawn(const ParticleSpawnParams& params) {
    if (count_ >= capacity_) {
        ++stats_.dropped_total;   // Overflow drops the NEWEST. A missing spark
        return false;             // is invisible; a reallocation stall is not.
    }

    const usize i = count_++;
    pos_x[i] = params.position.x;
    pos_y[i] = params.position.y;
    vel_x[i] = params.velocity.x;
    vel_y[i] = params.velocity.y;
    // Guarded so build_instances' age/lifetime division can never divide by
    // zero and no particle can be born already-dead-but-invisible.
    size_[i] = math::max(params.size, 1.0e-4f);
    rot_[i] = params.rotation;
    spin_[i] = params.spin;
    age_[i] = 0.0f;
    life_[i] = math::max(params.lifetime, 1.0e-4f);
    drag_[i] = math::max(params.drag, 0.0f);
    buoy_[i] = params.buoyancy;
    end_x_[i] = params.endpoint.x;
    end_y_[i] = params.endpoint.y;
    seed_[i] = pcg_f32(rng_state_);
    color_[i] = pack_rgba(params.color);
    kind_[i] = static_cast<u8>(params.kind);
    blend_[i] = static_cast<u8>(params.blend);

    ++stats_.spawned_this_frame;
    stats_.live = static_cast<u32>(count_);
    return true;
}

// ---------------------------------------------------------------------------
// Update: integrate everything, then swap-remove the expired.
// ---------------------------------------------------------------------------

void ParticleSystem::update(f32 dt, JobSystem* jobs) {
    WallClock timer;

    if (count_ > 0 && dt > 0.0f) {
        f32* px = pos_x.data();
        f32* py = pos_y.data();
        f32* vx = vel_x.data();
        f32* vy = vel_y.data();
        const f32* dg = drag_.data();
        const f32* by = buoy_.data();
        f32* rt = rot_.data();
        const f32* sp = spin_.data();
        f32* ag = age_.data();

        // Below the grain the fork/join costs more than the work. Above it this
        // is embarrassingly parallel — there is no determinism requirement in
        // this layer at all, which is exactly why it lives outside the sim.
        constexpr usize kParallelGrain = 8192;
        if (jobs != nullptr && jobs->thread_count() > 1 && count_ >= kParallelGrain * 2) {
            jobs->parallel_for(
                count_,
                [&](usize begin, usize end, u32) {
                    integrate_range(px, py, vx, vy, dg, by, rt, sp, ag, begin, end, dt);
                },
                kParallelGrain);
        } else {
            integrate_range(px, py, vx, vy, dg, by, rt, sp, ag, 0, count_, dt);
        }
    }

    // Swap-remove compaction, exactly the ChaffBuffers pattern: the survivor
    // swapped down into slot i must itself be tested, so i does not advance.
    usize n = count_;
    usize i = 0;
    u32 removed = 0;
    while (i < n) {
        if (age_[i] >= life_[i]) {
            --n;
            if (i != n) {
                pos_x[i] = pos_x[n]; pos_y[i] = pos_y[n];
                vel_x[i] = vel_x[n]; vel_y[i] = vel_y[n];
                size_[i] = size_[n]; rot_[i] = rot_[n]; spin_[i] = spin_[n];
                age_[i] = age_[n];   life_[i] = life_[n];
                drag_[i] = drag_[n]; buoy_[i] = buoy_[n];
                end_x_[i] = end_x_[n]; end_y_[i] = end_y_[n];
                seed_[i] = seed_[n];
                color_[i] = color_[n];
                kind_[i] = kind_[n]; blend_[i] = blend_[n];
            }
            ++removed;
        } else {
            ++i;
        }
    }
    count_ = n;

    stats_.retired_this_frame = removed;
    stats_.live = static_cast<u32>(count_);
    stats_.update_ms = static_cast<f32>(timer.elapsed_ms());
}

// ---------------------------------------------------------------------------
// Instance packing
// ---------------------------------------------------------------------------

void ParticleSystem::build_instances(BlendMode blend, std::vector<ParticleInstance>& out) const {
    // One reservation, then push_back can never grow. The contract only
    // promises no allocation when `out` is already reserved; the renderer keeps
    // one vector per blend mode alive across frames, so this fires once.
    if (out.capacity() < count_) out.reserve(count_);
    out.clear();

    const u8 want = static_cast<u8>(blend);
    for (usize i = 0; i < count_; ++i) {
        if (blend_[i] != want) continue;
        const f32 age = age_[i];
        if (age < 0.0f) continue;   // staged particle, not born yet

        const f32 t = math::saturate(age / life_[i]);

        // Generic fade envelope: full brightness for most of the life, then a
        // smooth drop. Per-kind curves (ring thinning, tracer streak decay) are
        // the shader's job — it has age_norm and kind.
        const f32 fade = math::smoothstep01(1.0f - t);

        const u32 base = color_[i];
        const u32 alpha = static_cast<u32>(static_cast<f32>((base >> 24) & 0xFFu) * fade + 0.5f);

        ParticleInstance inst;
        inst.x = pos_x[i];
        inst.y = pos_y[i];
        inst.vx = vel_x[i];
        inst.vy = vel_y[i];
        inst.size = size_[i];
        inst.rotation = rot_[i];
        inst.tint_rgba8 = (base & 0x00FF'FFFFu) | (alpha << 24);
        inst.kind_blend = static_cast<u32>(kind_[i]) | (static_cast<u32>(blend_[i]) << 16);
        inst.age_norm = t;
        inst.seed = seed_[i];
        inst.pad0 = 0.0f;
        inst.pad1 = 0.0f;
        out.push_back(inst);
    }
}

// ---------------------------------------------------------------------------
// ART AUTHORING
//
// One CombatEvent in, one burst out. Keyed on (type, source, visual_id):
//
//   type      what happened
//   source    which tower — decides the palette and the whole vocabulary
//   visual_id the tower's upgrade TIER, 1..5 (0 is treated as 1), in the low
//             byte. The sim assigns it no meaning; this layer defines it as
//             the escalation axis the brief asks for: bigger releases, bigger
//             blasts, more fragments. sim::kSwarmerEventBit on top says the
//             event was raised by a SWARMER rather than by the tower itself —
//             a shooter's round leaving, a latch landing, a bomber popping.
//   magnitude "how big should this read" — used for the per-hop falloff on
//             Tesla chains and the weight of an impact, so one event type can
//             render at wildly different intensities.
// ---------------------------------------------------------------------------

void ParticleSystem::emit_for_event(const sim::CombatEvent& event) {
    const bool from_swarmer = (event.visual_id & sim::kSwarmerEventBit) != 0;
    const u32 raw_tier = event.visual_id & 0xFFu;
    const u32 tier = math::clamp<u32>(raw_tier == 0 ? 1u : raw_tier, 1u, 5u);
    const f32 tierf = static_cast<f32>(tier);
    const f32 mag = math::clamp(event.magnitude, 0.25f, 4.0f);
    const TowerPalette pal = palette_for(event.source);
    const Vec4 fam = family_tint(event.target_family);

    u64& rs = rng_state_;

    // Emits one particle, optionally staged to appear `delay` seconds from now
    // (see the DELAYED PARTICLES note at the top of this file).
    auto push = [this](const ParticleSpawnParams& p, f32 delay = 0.0f) {
        if (spawn(p) && delay > 0.0f) age_[count_ - 1] = -delay;
    };

    // Aim direction, with a safe fallback so a default-constructed event still
    // produces a plausible burst rather than a NaN one.
    Vec2 dir = math::normalize_safe(event.direction);
    if (dir.x == 0.0f && dir.y == 0.0f) dir = Vec2{1.0f, 0.0f};

    // Segment events (Beam/Chain) carry origin -> secondary.
    Vec2 seg = event.secondary - event.origin;
    f32 seg_len = math::length(seg);
    Vec2 seg_dir = seg_len > math::kEpsilon ? seg / seg_len : dir;
    if (seg_len < 0.5f) seg_len = 0.5f;

    switch (event.type) {

    // -----------------------------------------------------------------------
    // MuzzleFlash — two very different things arrive here:
    //   * from a SWARMER (kSwarmerEventBit): a Neutrophil shooter fired one
    //     round. Dozens per second across a cloud, so this is a single tracer
    //     and nothing else.
    //   * from a TOWER: a volley of swarmers left the cell's face. Once per
    //     cooldown, so it can afford a visible activation pulse.
    //     The swarmers THEMSELVES are not here — they are simulated entities
    //     drawn from sim state, because they steer, choose targets, and do
    //     damage. `event.origin` is ALREADY the release point, so nothing here
    //     may add an offset of its own. `magnitude` carries the volley size.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::MuzzleFlash: {
        if (from_swarmer) {
            ParticleSpawnParams t2;
            t2.kind = ParticleKind::Tracer;
            t2.blend = BlendMode::Additive;
            t2.position = event.origin + dir * 0.15f;
            t2.velocity = rotate_by(dir, pcg_range(rs, -0.08f, 0.08f)) * pcg_range(rs, 40.0f, 60.0f);
            t2.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            t2.size = pcg_range(rs, 0.08f, 0.13f);
            t2.lifetime = pcg_range(rs, 0.10f, 0.16f);
            t2.drag = 1.6f;
            push(t2);
            break;
        }

        const Vec2 tip = event.origin;
        const f32 released = math::clamp(event.magnitude, 1.0f, 40.0f);

        // The face lighting up.
        ParticleSpawnParams p;
        p.kind = ParticleKind::Spark;
        p.blend = BlendMode::Additive;
        p.position = tip;
        p.color = mix4(pal.accent, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.3f);
        p.size = 0.30f + 0.012f * released + 0.03f * tierf;
        p.lifetime = 0.10f;
        p.drag = 6.0f;
        push(p);

        // Exocytosis spray, scaled by how much was actually released.
        const u32 droplets = 3u + static_cast<u32>(released * 0.5f);
        for (u32 k = 0; k < droplets; ++k) {
            const Vec2 out = rotate_by(dir, pcg_range(rs, -0.55f, 0.55f));
            ParticleSpawnParams t2;
            t2.kind = ParticleKind::Tracer;
            t2.blend = BlendMode::Additive;
            t2.position = tip + perp(dir) * pcg_range(rs, -0.14f, 0.14f);
            t2.velocity = out * pcg_range(rs, 5.0f, 12.0f);
            t2.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            t2.size = pcg_range(rs, 0.06f, 0.12f);
            t2.lifetime = pcg_range(rs, 0.06f, 0.13f);
            t2.drag = 7.0f;
            push(t2, pcg_range(rs, 0.0f, 0.03f));
        }

        // Secreted plasma at the mouth. AlphaBlend so it occludes: this is
        // fluid being pushed out, not light.
        for (u32 k = 0; k < 3u; ++k) {
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = tip + perp(dir) * pcg_range(rs, -0.16f, 0.16f);
            m.velocity = dir * pcg_range(rs, 1.0f, 2.6f);
            m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.30f};
            m.size = pcg_range(rs, 0.16f, 0.30f);
            m.lifetime = pcg_range(rs, 0.12f, 0.22f);
            m.drag = 5.0f;
            m.spin = pcg_signed(rs) * 2.0f;
            push(m);
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ProjectileImpact — a round connected, a latch swarmer landed on its
    // host, or a Macrophage finished engulfing a captive. Ordinary
    // impacts stay tiny; absorption gets its own inward digestive pulse.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ProjectileImpact: {
        if (event.source == TowerType::Macrophage) {
            const f32 absorbed = math::clamp(event.magnitude, 1.0f, 24.0f);
            ParticleSpawnParams core;
            core.kind = ParticleKind::Spark;
            core.blend = BlendMode::Additive;
            core.position = event.origin;
            core.color = pal.accent;
            core.size = 0.42f + 0.025f * absorbed;
            core.lifetime = 0.13f;
            core.drag = 8.0f;
            push(core);

            ParticleSpawnParams ring;
            ring.kind = ParticleKind::Ring;
            ring.blend = BlendMode::Additive;
            ring.position = event.origin;
            ring.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.78f};
            ring.size = math::max(1.4f, event.radius * 0.55f);
            ring.lifetime = 0.18f;
            push(ring);

            const u32 motes = 4u + tier + static_cast<u32>(absorbed / 6.0f);
            for (u32 k = 0; k < motes; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                const Vec2 radial{std::cos(a), std::sin(a)};
                ParticleSpawnParams mote;
                mote.kind = ParticleKind::Tracer;
                mote.blend = BlendMode::Additive;
                mote.position = event.origin + radial * pcg_range(rs, 0.8f, 2.0f);
                mote.velocity = -radial * pcg_range(rs, 7.0f, 13.0f);
                mote.color = mix4(pal.primary, pal.accent, pcg_f32(rs) * 0.55f);
                mote.size = pcg_range(rs, 0.07f, 0.13f);
                mote.lifetime = pcg_range(rs, 0.10f, 0.17f);
                mote.drag = 4.0f;
                push(mote);
            }
            break;
        }

        const f32 pop = (event.radius > 0.05f ? event.radius : 0.35f) * (0.7f + 0.3f * mag);
        // A wall has no pathogen-family tint, so its debris stays exactly the
        // projectile's own hue. Agent impacts retain their subtle family shift.
        const Vec4 debris = event.target_family == PathogenFamily::Count
                                ? pal.primary
                                : mix4(pal.primary, fam, kFamilyTintWeight);

        // A tiny white pop plus a few scattering bits. Cheap by design.
        ParticleSpawnParams s;
        s.kind = ParticleKind::Spark;
        s.blend = BlendMode::Additive;
        s.position = event.origin;
        s.color = event.target_family == PathogenFamily::Count ? pal.primary : pal.accent;
        s.size = pop;
        s.lifetime = pcg_range(rs, 0.07f, 0.11f);
        s.drag = 7.0f;
        push(s);

        const u32 bits = 2u + tier;
        for (u32 k = 0; k < bits; ++k) {
            const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
            ParticleSpawnParams t2;
            t2.kind = ParticleKind::Tracer;
            t2.blend = BlendMode::Additive;
            t2.position = event.origin;
            t2.velocity = Vec2{std::cos(a), std::sin(a)} * pcg_range(rs, 3.0f, 9.0f) * mag;
            t2.color = debris;
            t2.size = pcg_range(rs, 0.06f, 0.11f);
            t2.lifetime = pcg_range(rs, 0.08f, 0.16f);
            t2.drag = 5.0f;
            push(t2);
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ProjectileExpired — a round fizzled out. Must still read as *something*
    // (a round that vanishes with no acknowledgement looks like a bug), but
    // dim and brief so it never competes with a real hit.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ProjectileExpired: {
        for (u32 k = 0; k < 2; ++k) {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin + Vec2{pcg_signed(rs), pcg_signed(rs)} * 0.1f;
            s.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.35f};
            s.size = pcg_range(rs, 0.08f, 0.14f);
            s.lifetime = pcg_range(rs, 0.10f, 0.18f);
            s.drag = 8.0f;
            s.velocity = dir * pcg_range(rs, 1.0f, 3.0f);
            push(s, 0.01f * static_cast<f32>(k));
        }
        break;
    }

    // -----------------------------------------------------------------------
    // Explosion — a bomber swarmer went off. What that looks like is the
    // TOWER's, because what it leaves behind is:
    //
    //   Interferon   a slow circle. Frost: a cold ring settling outward and a
    //                few crystal motes drifting down. No fragments, no heat —
    //                nothing was damaged.
    //   Goblet Cell  a mucus splash. The fluid itself is real and drawn from
    //                sim state; this is only the wet spatter that a particle
    //                surface cannot resolve, same job FluidSplash does.
    //   Generic Bomber   a digestive burst in three stages:
    //     (1) t=0.00  the vesicle ruptures: tiny white core.
    //     (2) t=0.00..0.055  the area "charges": matter is pulled inward.
    //     (3) t=0.055 the burst: white centre, amber ring expanding to the
    //         full radius, fragments thrown outward, all gone by ~0.22s.
    //   Staging is done with negative birth ages, so the update loop stays
    //   completely unaware that any of this is happening.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::Explosion: {
        const f32 radius = math::max(event.radius, 1.5f);
        constexpr f32 kCharge = 0.055f;   // the ~0.05s pause the brief asks for

        if (event.source == TowerType::Interferon) {
            // The pop itself: small and cold.
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = 0.45f;
            s.lifetime = 0.10f;
            s.drag = 6.0f;
            push(s);
            // One ring settling out to the zone's edge — slower than the
            // generic burst's shock so it reads as spreading cold, not a blast.
            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = event.origin;
            r.color = mix4(pal.primary, pal.accent, 0.4f);
            r.size = radius;
            r.lifetime = 0.34f;
            push(r);
            // Crystal motes drifting down inside the circle.
            const u32 motes = 8u + 3u * tier;
            for (u32 k = 0; k < motes; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                const Vec2 radial{std::cos(a), std::sin(a)};
                ParticleSpawnParams sh;
                sh.kind = ParticleKind::Shard;
                sh.blend = BlendMode::AlphaBlend;
                sh.position = event.origin + radial * pcg_range(rs, 0.0f, radius * 0.8f);
                sh.velocity = radial * pcg_range(rs, 0.4f, 1.4f);
                sh.color = mix4(pal.primary, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.5f);
                sh.size = pcg_range(rs, 0.08f, 0.16f);
                sh.lifetime = pcg_range(rs, 0.30f, 0.55f);
                sh.drag = 2.0f;
                sh.rotation = a;
                sh.spin = pcg_signed(rs) * 3.0f;
                push(sh, pcg_range(rs, 0.0f, 0.12f));
            }
            break;
        }

        if (event.source == TowerType::GobletCell) {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = 0.40f;
            s.lifetime = 0.08f;
            s.drag = 6.0f;
            push(s);
            // Wet spatter thrown out past the splash radius, occluding.
            const u32 drops = 6u + 2u * tier;
            for (u32 k = 0; k < drops; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                const Vec2 radial{std::cos(a), std::sin(a)};
                ParticleSpawnParams m;
                m.kind = ParticleKind::Mist;
                m.blend = BlendMode::AlphaBlend;
                m.position = event.origin + radial * pcg_range(rs, 0.0f, radius * 0.4f);
                m.velocity = radial * pcg_range(rs, radius * 3.0f, radius * 6.0f);
                m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.40f};
                m.size = pcg_range(rs, 0.14f, 0.28f);
                m.lifetime = pcg_range(rs, 0.14f, 0.26f);
                m.drag = 6.0f;
                m.spin = pcg_signed(rs) * 2.0f;
                push(m);
            }
            break;
        }

        // --- stage 1: it lands.
        {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = radius * 0.20f;
            s.lifetime = kCharge;
            s.drag = 6.0f;
            push(s);
        }

        // --- stage 2: the area charges. Matter converges on the centre and
        // arrives exactly as stage 3 fires.
        {
            const u32 in = 5u + tier;
            for (u32 k = 0; k < in; ++k) {
                const f32 a = math::kTwoPi * static_cast<f32>(k) / static_cast<f32>(in)
                            + pcg_range(rs, -0.3f, 0.3f);
                const Vec2 radial = Vec2{std::cos(a), std::sin(a)};
                ParticleSpawnParams t2;
                t2.kind = ParticleKind::Tracer;
                t2.blend = BlendMode::Additive;
                t2.position = event.origin + radial * radius * 0.55f;
                t2.velocity = -radial * (radius * 0.55f / kCharge);
                t2.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.7f};
                t2.size = 0.10f;
                t2.lifetime = kCharge;
                push(t2);
            }
        }

        // --- stage 3: the burst.
        {
            // Bright white centre.
            ParticleSpawnParams core;
            core.kind = ParticleKind::Spark;
            core.blend = BlendMode::Additive;
            core.position = event.origin;
            core.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            core.size = radius * 0.5f;
            core.lifetime = 0.10f;
            push(core, kCharge);

            // Yellow/orange rings expanding outward. Tier adds concentric
            // rings on a short stagger, which is what makes a high-tier burst
            // read as a bigger *event*, not just a bigger circle.
            const u32 rings = 1u + tier / 2u;
            for (u32 k = 0; k < rings; ++k) {
                ParticleSpawnParams r;
                r.kind = ParticleKind::Ring;
                r.blend = BlendMode::Additive;
                r.position = event.origin;
                r.color = mix4(pal.accent, pal.primary, 0.25f + 0.35f * static_cast<f32>(k));
                r.size = radius * (1.0f - 0.16f * static_cast<f32>(k));   // FINAL radius
                r.lifetime = 0.16f;
                push(r, kCharge + 0.022f * static_cast<f32>(k));
            }

            // Fragments. AlphaBlend: these are chunks of digested matter, they
            // should occlude, not glow.
            const u32 frags = 16u + 10u * tier;
            for (u32 k = 0; k < frags; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                const Vec2 radial = Vec2{std::cos(a), std::sin(a)};
                ParticleSpawnParams sh;
                sh.kind = ParticleKind::Shard;
                sh.blend = BlendMode::AlphaBlend;
                sh.position = event.origin + radial * radius * 0.1f;
                sh.velocity = radial * pcg_range(rs, radius * 3.5f, radius * 7.0f);
                sh.color = mix4(pal.primary, fam, kFamilyTintWeight);
                sh.size = pcg_range(rs, 0.10f, 0.24f);
                sh.lifetime = pcg_range(rs, 0.14f, 0.20f);
                sh.drag = 4.0f;
                sh.rotation = a;
                sh.spin = pcg_signed(rs) * 10.0f;
                push(sh, kCharge);
            }

            // Lingering digestive mist, the only thing allowed to outlast the
            // 0.2s window — and only just.
            const u32 puffs = 5u + 2u * tier;
            for (u32 k = 0; k < puffs; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                ParticleSpawnParams m;
                m.kind = ParticleKind::Mist;
                m.blend = BlendMode::AlphaBlend;
                m.position = event.origin + Vec2{std::cos(a), std::sin(a)} * pcg_range(rs, 0.0f, radius * 0.6f);
                m.velocity = Vec2{std::cos(a), std::sin(a)} * pcg_range(rs, 0.5f, 2.0f);
                m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.30f};
                m.size = pcg_range(rs, radius * 0.18f, radius * 0.34f);
                m.lifetime = pcg_range(rs, 0.18f, 0.28f);
                m.drag = 3.0f;
                m.buoyancy = 1.2f;
                m.spin = pcg_signed(rs) * 2.0f;
                push(m, kCharge);
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    // BeamFired — the LASER. Continuous, straight, clean. The identity problem
    // here is that a beam that is simply "on" is visually dead, so the length
    // of it carries tiny antibody particles RACING FORWARD; that motion is what
    // separates it from the Tesla's instantaneous jagged arc.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::BeamFired: {
        // Core + sheath. Tier adds sheath layers, so a maxed B-Cell beam is
        // visibly thicker and hotter rather than merely longer.
        const u32 layers = 1u + tier / 2u;
        for (u32 k = 0; k < layers; ++k) {
            ParticleSpawnParams b;
            b.kind = ParticleKind::Beam;
            b.blend = BlendMode::Additive;
            b.position = event.origin;
            b.velocity = seg_dir;                 // UNIT direction
            b.size = seg_len;                     // segment LENGTH
            b.color = k == 0 ? pal.accent
                             : Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.55f};
            b.lifetime = 0.09f + 0.015f * static_cast<f32>(k);
            b.rotation = std::atan2(seg_dir.y, seg_dir.x);
            push(b);
        }

        // Muzzle bloom.
        {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = 0.30f + 0.05f * tierf;
            s.lifetime = 0.09f;
            s.drag = 6.0f;
            push(s);
        }

        // The travelling energy texture: antibodies racing along the beam.
        // Spawned all along the segment with staggered births so the flow never
        // shows a gap even while the beam is continuously on.
        const u32 riders = 8u + 6u * tier;
        for (u32 k = 0; k < riders; ++k) {
            const f32 along = pcg_f32(rs) * seg_len;
            ParticleSpawnParams t2;
            t2.kind = ParticleKind::Tracer;
            t2.blend = BlendMode::Additive;
            t2.position = event.origin + seg_dir * along
                        + perp(seg_dir) * pcg_range(rs, -0.10f, 0.10f);
            t2.velocity = seg_dir * pcg_range(rs, 45.0f, 90.0f);
            t2.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            t2.size = pcg_range(rs, 0.07f, 0.13f);
            t2.lifetime = pcg_range(rs, 0.09f, 0.18f);
            t2.drag = 0.5f;
            push(t2, pcg_range(rs, 0.0f, 0.06f));
        }

        // Terminal bloom where the beam stops.
        {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin + seg_dir * seg_len;
            s.color = pal.accent;
            s.size = 0.24f;
            s.lifetime = 0.10f;
            s.drag = 6.0f;
            push(s);
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ChainArc — one CYTOTOXIC T hop. A LYTIC GRANULE IN FLIGHT, not an
    // electrical arc. See the kCtl* constants above for why the archetype's
    // "chain" is drawn as serial killing rather than as lightning.
    //
    // Three beats per hop, and the ordering between them is the whole effect:
    //
    //   1. the granule flies origin -> secondary in kCtlHopSeconds,
    //   2. a secretory thread of plasma smears along the path behind it,
    //   3. it lands: a perforin pore opens and granzymes spill into the target.
    //
    // Hop k is staggered by k * kCtlHopSeconds so the payload visibly walks the
    // chain instead of every link igniting at once. The sim raises every hop's
    // event in ONE tick, so the stagger has to be reconstructed here — `mag`
    // carries the sim's per-hop falloff, so inverting the falloff recovers k.
    //
    // DELAYED PARTICLES THAT MOVE: build_instances() hides a particle while its
    // age is negative, but the integrator still runs on it, so a staged mover
    // has already drifted `velocity * delay` by the time it becomes visible.
    // Every moving spawn below is therefore born at `start - velocity * delay`
    // and must keep drag and buoyancy at zero, which makes that pre-birth drift
    // exactly linear and the compensation exact.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ChainArc: {
        const f32 weight = math::clamp(mag, 0.3f, 1.6f);

        // Recover this hop's index from the falloff the sim baked into
        // magnitude: weight == kCtlHopFalloff^k, so k == log(weight)/log(f).
        // Uses the RAW magnitude, not the clamped `mag`, because the clamp
        // floor collapses the last two hops onto the same value.
        const f32 raw = event.magnitude > 0.01f ? event.magnitude : 1.0f;
        const f32 hopf = std::log(math::clamp(raw, 0.05f, 1.0f)) / std::log(kCtlHopFalloff);
        const u32 hop = static_cast<u32>(math::clamp(hopf + 0.5f, 0.0f, 7.0f));
        const f32 t_fly = kCtlHopSeconds * static_cast<f32>(hop);   // flight begins
        const f32 t_hit = t_fly + kCtlHopSeconds;                   // payload lands

        // Every hop launches from wherever the raiser said it did: hop 0 from
        // the source's muzzle, later hops from the previous victim. This layer
        // deliberately applies no muzzle offset of its own — only the raiser
        // knows where its payload actually leaves from, and guessing here is
        // what put the old Cytotoxic T's flash a cell in front of its spike.
        const Vec2 from = event.origin;
        const Vec2 to = event.secondary;
        Vec2 path = to - from;
        f32 path_len = math::length(path);
        const Vec2 path_dir = path_len > math::kEpsilon ? path / path_len : seg_dir;
        if (path_len < 0.25f) path_len = 0.25f;

        // Constant flight TIME, not constant speed: every hop takes
        // kCtlHopSeconds whatever its length, so the chain's rhythm stays even
        // and predictable no matter how the targets happen to be spaced.
        const Vec2 fly = path_dir * (path_len / kCtlHopSeconds);

        // --- beat 1: the granule itself. A round wet bead, plus a couple of
        // smaller ones tumbling just behind it, because one dot travelling
        // alone reads as a bullet and a tight clutch reads as ejected matter.
        {
            ParticleSpawnParams g;
            g.kind = ParticleKind::Spark;
            g.blend = BlendMode::Additive;
            g.position = from - fly * t_fly;
            g.velocity = fly;
            g.color = mix4(pal.accent, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.35f);
            g.size = 0.30f * weight;
            g.lifetime = kCtlHopSeconds;
            push(g, t_fly);

            const u32 escorts = 1u + tier / 2u;
            for (u32 k = 0; k < escorts; ++k) {
                const Vec2 off = perp(path_dir) * pcg_range(rs, -0.13f, 0.13f)
                               - path_dir * pcg_range(rs, 0.10f, 0.30f);
                ParticleSpawnParams e2;
                e2.kind = ParticleKind::Tracer;
                e2.blend = BlendMode::Additive;
                e2.position = from + off - fly * t_fly;
                e2.velocity = fly;
                e2.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
                e2.size = pcg_range(rs, 0.09f, 0.15f);
                e2.lifetime = kCtlHopSeconds;
                push(e2, t_fly);
            }
        }

        // --- beat 2: the secretory thread. Slow, alpha-blended puffs dropped
        // along the path as the granule passes each point, so they occlude and
        // read as fluid rather than glowing like an arc. This is the single
        // biggest reason the hop no longer looks electrical: the trail is WET
        // and it lingers past the flight, where a bolt is dry and instant.
        {
            const u32 beads = 3u + tier;
            for (u32 k = 0; k < beads; ++k) {
                const f32 frac = (static_cast<f32>(k) + 0.5f) / static_cast<f32>(beads);
                ParticleSpawnParams m;
                m.kind = ParticleKind::Mist;
                m.blend = BlendMode::AlphaBlend;
                m.position = from + path_dir * (path_len * frac)
                           + perp(path_dir) * pcg_range(rs, -0.14f, 0.14f);
                m.velocity = perp(path_dir) * pcg_signed(rs) * 0.5f;
                m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.34f};
                m.size = pcg_range(rs, 0.13f, 0.24f) * weight;
                m.lifetime = pcg_range(rs, 0.11f, 0.19f);
                m.drag = 4.0f;
                m.spin = pcg_signed(rs) * 2.5f;
                // Appears as the granule reaches it, never before.
                push(m, t_fly + kCtlHopSeconds * frac);
            }
        }

        // --- beat 3: arrival. Perforin punches the membrane and granzymes go
        // in. A pore RING that opens, a soft warm core (deliberately not the
        // old 1,1,1 electrical white), matter spilling outward, and — at
        // higher tiers — apoptotic blebs, which is what granzyme entry
        // actually does to a cell.
        {
            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = to;
            r.color = mix4(pal.accent, fam, kFamilyTintWeight);
            r.size = 0.85f * weight;
            r.lifetime = 0.16f;
            push(r, t_hit);

            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = to;
            s.color = mix4(pal.accent, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.45f);
            s.size = 0.40f * weight;
            s.lifetime = 0.09f;
            s.drag = 7.0f;
            push(s, t_hit);

            // Granzyme spill: matter, so AlphaBlend and a slow settle.
            const u32 spill = 3u + tier;
            for (u32 k = 0; k < spill; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                const Vec2 radial{std::cos(a), std::sin(a)};
                ParticleSpawnParams m;
                m.kind = ParticleKind::Mist;
                m.blend = BlendMode::AlphaBlend;
                m.position = to + radial * 0.10f;
                m.velocity = radial * pcg_range(rs, 1.2f, 3.4f);
                m.color = mix4(pal.primary, fam, 0.45f);
                m.size = pcg_range(rs, 0.11f, 0.20f) * weight;
                m.lifetime = pcg_range(rs, 0.14f, 0.24f);
                m.drag = 5.0f;
                m.spin = pcg_signed(rs) * 3.0f;
                push(m, t_hit);
            }

            // Membrane blebbing. Tier-gated because a tier-1 tap should look
            // like a wound and a tier-3 one like the cell coming apart.
            const u32 blebs = tier;
            for (u32 k = 0; k < blebs; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                const Vec2 radial{std::cos(a), std::sin(a)};
                ParticleSpawnParams sh;
                sh.kind = ParticleKind::Shard;
                sh.blend = BlendMode::AlphaBlend;
                sh.position = to + radial * 0.14f;
                sh.velocity = radial * pcg_range(rs, 2.0f, 5.0f);
                sh.color = mix4(pal.primary, fam, kFamilyTintWeight);
                sh.size = pcg_range(rs, 0.07f, 0.14f);
                sh.lifetime = pcg_range(rs, 0.12f, 0.20f);
                sh.drag = 4.5f;
                sh.rotation = a;
                sh.spin = pcg_signed(rs) * 8.0f;
                push(sh, t_hit);
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ConePulse — the CRYO signal. No projectile at all: a slow cold haze
    // filling the cone. Used to also throw a volley of additive tracers
    // sweeping out toward the cone's reach on every pulse, but the Interferon
    // pulses continuously while it has a target, so that read as a constant
    // spray of bright blue flares flashing out near the edge of its range
    // rather than as a discrete attack. The standing field (field.frag's
    // Cone shape) already draws the cone itself, so the haze is enough to
    // sell "this tick fired".
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ConePulse: {
        const f32 reach = math::max(event.radius, 2.0f);
        const f32 half = event.arc_radians > 0.01f ? event.arc_radians : 0.55f;

        // Cold haze filling the cone volume.
        const u32 haze = 4u + 3u * tier;
        for (u32 k = 0; k < haze; ++k) {
            const f32 a = pcg_range(rs, -half, half);
            const Vec2 d2 = rotate_by(dir, a);
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin + d2 * pcg_range(rs, reach * 0.15f, reach * 0.9f);
            m.velocity = d2 * pcg_range(rs, 0.5f, 2.0f);
            m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.22f};
            m.size = pcg_range(rs, reach * 0.10f, reach * 0.20f);
            m.lifetime = pcg_range(rs, 0.35f, 0.60f);
            m.drag = 1.5f;
            m.spin = pcg_signed(rs) * 1.0f;
            push(m);
        }

        break;
    }

    // -----------------------------------------------------------------------
    // Freeze — an agent became fully encased. The "PING": a clean circular
    // ripple, plus the crystalline shell snapping into place around it.
    // Deliberately the most *ordered* effect in the game — every other burst is
    // scattered, this one is a perfect circle, which is what makes it read
    // instantly as a state change rather than damage.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::Freeze: {
        const f32 shell = math::max(event.radius, 0.7f);

        const u32 pings = 1u + tier / 2u;
        for (u32 k = 0; k < pings; ++k) {
            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = event.origin;
            r.color = mix4(pal.accent, pal.primary, 0.35f * static_cast<f32>(k));
            r.size = shell * (2.6f + 0.9f * static_cast<f32>(k));
            r.lifetime = 0.28f;
            push(r, 0.05f * static_cast<f32>(k));
        }

        {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = shell * 0.8f;
            s.lifetime = 0.14f;
            s.drag = 6.0f;
            push(s);
        }

        // The shell itself: crystal facets locked in a ring around the agent,
        // near-stationary. Tier = more facets = a denser-looking encasement.
        const u32 facets = 5u + 2u * tier;
        for (u32 k = 0; k < facets; ++k) {
            const f32 a = math::kTwoPi * static_cast<f32>(k) / static_cast<f32>(facets)
                        + pcg_range(rs, -0.15f, 0.15f);
            const Vec2 radial = Vec2{std::cos(a), std::sin(a)};
            ParticleSpawnParams sh;
            sh.kind = ParticleKind::Shard;
            sh.blend = BlendMode::AlphaBlend;
            sh.position = event.origin + radial * shell * 0.75f;
            sh.velocity = radial * 0.35f;
            sh.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.62f};
            sh.size = pcg_range(rs, shell * 0.22f, shell * 0.40f);
            sh.lifetime = pcg_range(rs, 0.26f, 0.40f);
            sh.drag = 3.5f;
            sh.rotation = a;
            sh.spin = pcg_signed(rs) * 0.8f;
            push(sh, 0.02f + 0.012f * static_cast<f32>(k));   // facets lock in sequence
        }
        break;
    }

    // -----------------------------------------------------------------------
    // Shatter — the freeze expired. The "CRACK": the shell breaks into several
    // tiny particles that fall. AlphaBlend and negative buoyancy on purpose —
    // this is the one effect in the game that reads as debris rather than
    // energy, which is what makes it legible as "the freeze ended".
    // -----------------------------------------------------------------------
    case sim::CombatEventType::Shatter: {
        const f32 shell = math::max(event.radius, 0.7f);

        {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = shell * 0.6f;
            s.lifetime = 0.08f;
            s.drag = 8.0f;
            push(s);

            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = event.origin;
            r.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.7f};
            r.size = shell * 1.8f;
            r.lifetime = 0.10f;
            push(r);
        }

        const u32 pieces = 6u + 3u * tier;
        for (u32 k = 0; k < pieces; ++k) {
            const f32 a = math::kTwoPi * static_cast<f32>(k) / static_cast<f32>(pieces)
                        + pcg_range(rs, -0.4f, 0.4f);
            const Vec2 radial = Vec2{std::cos(a), std::sin(a)};
            ParticleSpawnParams sh;
            sh.kind = ParticleKind::Shard;
            sh.blend = BlendMode::AlphaBlend;
            sh.position = event.origin + radial * shell * 0.5f;
            sh.velocity = radial * pcg_range(rs, 3.5f, 9.0f);
            sh.color = mix4(Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.8f}, fam,
                            kFamilyTintWeight);
            sh.size = pcg_range(rs, shell * 0.14f, shell * 0.30f);
            sh.lifetime = pcg_range(rs, 0.30f, 0.50f);
            sh.drag = 2.2f;
            sh.buoyancy = -3.0f;      // shards fall; nothing else in this layer does
            sh.rotation = a;
            sh.spin = pcg_signed(rs) * 9.0f;
            push(sh);
        }
        break;
    }

    // -----------------------------------------------------------------------
    // BladeSlash — the NK rotor cut something. White flash + tiny impact ring
    // (contact), then a slash arc thrown along the blade's travel direction.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::BladeSlash: {
        {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            s.size = 0.42f * (0.7f + 0.3f * mag);
            s.lifetime = 0.07f;
            s.drag = 8.0f;
            push(s);

            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = event.origin;
            r.color = Vec4{pal.accent.r, pal.accent.g, pal.accent.b, 0.9f};
            r.size = 0.9f * (0.7f + 0.3f * mag);
            r.lifetime = 0.12f;
            push(r);
        }

        // The cut itself: bits flung along blade travel, fanned perpendicular.
        const u32 cut = 4u + 2u * tier;
        const Vec2 side = perp(dir);
        for (u32 k = 0; k < cut; ++k) {
            const f32 lateral = pcg_signed(rs) * 0.5f;
            ParticleSpawnParams t2;
            t2.kind = ParticleKind::Tracer;
            t2.blend = BlendMode::Additive;
            t2.position = event.origin + side * lateral;
            t2.velocity = rotate_by(dir, pcg_range(rs, -0.35f, 0.35f)) * pcg_range(rs, 9.0f, 22.0f);
            t2.color = mix4(pal.primary, fam, kFamilyTintWeight);
            t2.size = pcg_range(rs, 0.08f, 0.15f);
            t2.lifetime = pcg_range(rs, 0.10f, 0.18f);
            t2.drag = 4.0f;
            t2.rotation = std::atan2(dir.y, dir.x);
            push(t2);
        }

        // A couple of AlphaBlend flecks so a slash on a big target has weight.
        for (u32 k = 0; k < 1u + tier / 2u; ++k) {
            ParticleSpawnParams sh;
            sh.kind = ParticleKind::Shard;
            sh.blend = BlendMode::AlphaBlend;
            sh.position = event.origin;
            sh.velocity = rotate_by(dir, pcg_range(rs, -0.9f, 0.9f)) * pcg_range(rs, 3.0f, 7.0f);
            sh.color = mix4(Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.7f}, fam, 0.4f);
            sh.size = pcg_range(rs, 0.10f, 0.18f);
            sh.lifetime = pcg_range(rs, 0.20f, 0.34f);
            sh.drag = 2.5f;
            sh.buoyancy = -2.0f;
            sh.spin = pcg_signed(rs) * 8.0f;
            push(sh);
        }
        break;
    }

    // -----------------------------------------------------------------------
    // FluidSplash — a mucus particle was stopped hard.
    //
    // THE UNUSUAL PART: this event is a GARNISH, not the effect. Every other
    // case in this file is the whole visual, because the sim event it came from
    // was an instant that left nothing behind. A fluid splash is the opposite —
    // the actual splashing is hundreds of real particles the solver is moving
    // right now, and the fluid render pass draws them from live state. So the
    // job here is only to add what a particle-based surface physically cannot
    // resolve: the fine atomized spray that flies off a fast impact, at a scale
    // far below the solver's rest spacing.
    //
    // It follows that this must stay CHEAP and SPARSE. The sim already caps
    // these at FluidTuning::max_splash_events per tick precisely so a jet
    // hitting a wall cannot flood the sink, and over-spending here would fog up
    // the very surface it is supposed to be decorating.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::FluidSplash: {
        // `magnitude` is the speed the particle lost, so a graze throws almost
        // nothing and a full-speed wall strike throws a real burst.
        const f32 force = math::saturate(mag / 26.0f);
        const Vec2 back = Vec2{-dir.x, -dir.y};
        const Vec2 side = perp(dir);

        const u32 droplets = 2u + static_cast<u32>(force * 5.0f);
        for (u32 k = 0; k < droplets; ++k) {
            // Fanned back off the surface, the way a real splash sheets away
            // from the impact rather than rebounding along the incoming line.
            const f32 lateral = pcg_signed(rs);
            const Vec2 away = math::normalize_safe(back * 0.45f + side * lateral);
            ParticleSpawnParams t2;
            t2.kind = ParticleKind::Tracer;
            t2.blend = BlendMode::AlphaBlend;
            t2.position = event.origin;
            t2.velocity = away * pcg_range(rs, 3.0f, 5.0f + 11.0f * force);
            t2.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b,
                            0.55f + 0.35f * force};
            t2.size = pcg_range(rs, 0.06f, 0.13f);
            t2.lifetime = pcg_range(rs, 0.14f, 0.30f);
            t2.drag = 5.0f;
            push(t2);
        }

        // One soft wet bloom at the contact point, alpha-blended rather than
        // additive: this is matter landing, not energy discharging, and an
        // additive one would make every wall the jet touches glow.
        if (force > 0.25f) {
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin;
            m.velocity = back * (1.5f * force);
            m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.22f * force};
            m.size = 0.30f + 0.5f * force;
            m.lifetime = pcg_range(rs, 0.18f, 0.34f);
            m.drag = 5.0f;
            m.spin = pcg_signed(rs) * 1.2f;
            push(m);
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ChaffDeath — one rank-and-file pathogen died.
    //
    // THE ONE CASE IN THIS FILE WHOSE LOOK IS NOT THE KILLER'S. Every other
    // event above is a tower's signature and spends `pal` on it. This one
    // ignores `pal` entirely and reads the per-family table in vfx/DeathVfx.h,
    // which assets/config/enemies.json authors, so a player can tell what just
    // popped without knowing what shot it — and so the numbers below can be
    // retuned from a file rather than from this switch.
    //
    // It also has to survive being raised hundreds of times on a single tick: a
    // wave breaking is exactly when this fires most. Hence single-digit shipped
    // counts, alpha-blended debris (an additive version washes the lane out
    // when a whole wave dies at once), and a hard per-tick cap on the sim side
    // (SimDesc::max_chaff_death_events) rather than a cap invented here.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ChaffDeath: {
        const FamilyDeathVfx& look = family_death_vfx(event.target_family);
        if (!look.enabled) break;

        // Sizes in the table are multiples of the agent's own body radius, so
        // this one number carries the whole burst's scale.
        const f32 r = math::max(event.radius, 0.05f) * math::max(look.scale, 0.0f);
        // `magnitude` is the agent's SPEED for this event type, deliberately
        // read raw rather than through the clamped `mag` above: a death at a
        // sprint should spray forward, and 4.0 is walking pace here.
        const Vec2 carried = dir * (event.magnitude * look.inherit_velocity);

        if (look.core_life > 0.0f && look.core_size > 0.0f) {
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.velocity = carried;
            s.color = mix4(look.color, Vec4{1.0f, 1.0f, 1.0f, look.color.a},
                           math::saturate(look.core_white));
            s.size = r * look.core_size;
            s.lifetime = look.core_life;
            s.drag = 9.0f;
            push(s);
        }

        if (look.ring_life > 0.0f && look.ring_size > 0.0f) {
            ParticleSpawnParams ring;
            ring.kind = ParticleKind::Ring;
            ring.blend = BlendMode::Additive;
            ring.position = event.origin;
            ring.color = Vec4{look.color.r, look.color.g, look.color.b, look.ring_alpha};
            ring.size = r * look.ring_size;   // Ring stores the FINAL radius
            ring.lifetime = look.ring_life;
            push(ring);
        }

        // The body coming apart. `style` picks only the SHAPE — a Shard for a
        // capsid that cracks, a fat Tracer glob for a wall that bursts — while
        // `bit_scatter` picks how ordered the spray is, from perfectly even
        // radial spokes (a shell failing all at once) to a uniform splatter.
        // Keeping those two separate is what lets a family be dialled between
        // "popped" and "spilled" without changing its style.
        const bool angular = look.style == DeathStyle::Burst;
        const f32 scatter = math::saturate(look.bit_scatter);
        for (u32 k = 0; k < look.bit_count; ++k) {
            const f32 spoke =
                math::kTwoPi * static_cast<f32>(k) / static_cast<f32>(look.bit_count);
            const f32 a = spoke + pcg_signed(rs) * math::kPi * scatter;
            const Vec2 radial{std::cos(a), std::sin(a)};

            ParticleSpawnParams b;
            b.kind = angular ? ParticleKind::Shard : ParticleKind::Tracer;
            b.blend = BlendMode::AlphaBlend;
            // Started off the centre rather than at it, so the burst reads as a
            // surface failing rather than as everything erupting from a point.
            b.position = event.origin + radial * (r * 0.4f);
            b.velocity = carried + radial * pcg_range(rs, look.bit_speed_min, look.bit_speed_max);
            b.color = look.color;
            b.size = r * pcg_range(rs, look.bit_size_min, look.bit_size_max);
            b.lifetime = pcg_range(rs, look.bit_life_min, look.bit_life_max);
            b.drag = look.bit_drag;
            b.buoyancy = look.bit_buoyancy;
            b.rotation = a;
            b.spin = pcg_signed(rs) * look.bit_spin;
            push(b);
        }

        // What is left of the agent, hanging. Always alpha-blended (see above).
        for (u32 k = 0; k < look.bloom_count; ++k) {
            const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin;
            m.velocity = carried * 0.5f + Vec2{std::cos(a), std::sin(a)} * look.bloom_speed;
            m.color = Vec4{look.color.r, look.color.g, look.color.b, look.bloom_alpha};
            m.size = r * look.bloom_size;
            m.lifetime = look.bloom_life;
            m.drag = 3.5f;
            m.buoyancy = look.bloom_rise;
            m.spin = pcg_signed(rs) * 1.5f;
            push(m);
        }
        break;
    }

    // -----------------------------------------------------------------------
    // PathogenLatch — a virus grabbed a tower or a swarmer (sim/hostile). A
    // small, wet event in the PATHOGEN's colour, not the host's: what the
    // player has to read is "something is on my cell", and the thing on it
    // is the enemy. `origin` is the grab point on the host's membrane and
    // `direction` points outward from the host's centre, so the spatter
    // squirts away from the cell rather than into it. Capped per tick by the
    // sim, so a horde swarming a tower reads as a ripple of these, not a wall.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::PathogenLatch: {
        ParticleSpawnParams s;
        s.kind = ParticleKind::Spark;
        s.blend = BlendMode::Additive;
        s.position = event.origin;
        s.color = mix4(fam, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.35f);
        s.size = math::max(event.radius * 0.6f, 0.25f);
        s.lifetime = 0.12f;
        s.drag = 6.0f;
        push(s);
        for (u32 k = 0; k < 4; ++k) {
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin + Vec2{pcg_signed(rs), pcg_signed(rs)} * (event.radius * 0.3f);
            m.velocity = rotate_by(dir, pcg_range(rs, -0.7f, 0.7f)) * pcg_range(rs, 1.5f, 4.0f);
            m.color = Vec4{fam.r, fam.g, fam.b, 0.45f};
            m.size = pcg_range(rs, 0.15f, 0.30f) * math::max(event.radius, 0.5f);
            m.lifetime = pcg_range(rs, 0.25f, 0.45f);
            m.drag = 4.0f;
            push(m, 0.015f * static_cast<f32>(k));
        }
        break;
    }

    // -----------------------------------------------------------------------
    // SwarmerDeath — a unit was eaten (sim/hostile), as opposed to running out
    // (ProjectileExpired). It has to read as the unit COMING APART rather than
    // fizzling: a flash of the tower's colour, then the body's worth of
    // fragments thrown outward and a little mist left hanging. Scaled by the
    // unit's body radius so a granule pops small and a macrophage pops big.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::SwarmerDeath: {
        const f32 body = math::max(event.radius, 0.4f);
        ParticleSpawnParams core;
        core.kind = ParticleKind::Spark;
        core.blend = BlendMode::Additive;
        core.position = event.origin;
        core.color = mix4(pal.primary, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.6f);
        core.size = body * 0.9f;
        core.lifetime = 0.09f;
        core.drag = 5.0f;
        push(core);
        const u32 shards = 5u + static_cast<u32>(body * 3.0f);
        for (u32 k = 0; k < shards; ++k) {
            const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
            const Vec2 radial{std::cos(a), std::sin(a)};
            ParticleSpawnParams sh;
            sh.kind = ParticleKind::Shard;
            sh.blend = BlendMode::AlphaBlend;
            sh.position = event.origin + radial * (body * pcg_range(rs, 0.1f, 0.5f));
            sh.velocity = radial * pcg_range(rs, 4.0f, 11.0f) + dir * 1.5f;
            sh.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            sh.size = body * pcg_range(rs, 0.12f, 0.24f);
            sh.lifetime = pcg_range(rs, 0.22f, 0.40f);
            sh.drag = 5.0f;
            sh.rotation = a;
            sh.spin = pcg_signed(rs) * 6.0f;
            push(sh);
        }
        for (u32 k = 0; k < 3; ++k) {
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin + Vec2{pcg_signed(rs), pcg_signed(rs)} * (body * 0.4f);
            m.velocity = Vec2{pcg_signed(rs), pcg_signed(rs)} * 1.2f;
            m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.30f};
            m.size = body * pcg_range(rs, 0.6f, 1.0f);
            m.lifetime = pcg_range(rs, 0.35f, 0.6f);
            m.drag = 2.5f;
            push(m, 0.02f * static_cast<f32>(k));
        }
        break;
    }

    // -----------------------------------------------------------------------
    // TowerDestroyed — the horde emptied a tower's integrity and the cell was
    // torn down (game/towers). The loudest thing in this file that is NOT
    // good news, so it earns a full picture: a white core, a shock ring the
    // size of the footprint, a lot of fragments in the tower's own colours,
    // and a slow cloud that hangs where the cell stood for a moment so the
    // player's eye lands on the gap. `radius` is the footprint.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::TowerDestroyed: {
        const f32 radius = math::max(event.radius, 1.0f);
        ParticleSpawnParams core;
        core.kind = ParticleKind::Spark;
        core.blend = BlendMode::Additive;
        core.position = event.origin;
        core.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
        core.size = radius * 1.2f;
        core.lifetime = 0.14f;
        core.drag = 4.0f;
        push(core);
        ParticleSpawnParams ring;
        ring.kind = ParticleKind::Ring;
        ring.blend = BlendMode::Additive;
        ring.position = event.origin;
        ring.color = mix4(pal.primary, Vec4{1.0f, 1.0f, 1.0f, 1.0f}, 0.3f);
        ring.size = radius * 2.2f;
        ring.lifetime = 0.30f;
        push(ring, 0.03f);
        const u32 shards = 18u + 4u * tier;
        for (u32 k = 0; k < shards; ++k) {
            const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
            const Vec2 radial{std::cos(a), std::sin(a)};
            ParticleSpawnParams sh;
            sh.kind = ParticleKind::Shard;
            sh.blend = BlendMode::AlphaBlend;
            sh.position = event.origin + radial * (radius * pcg_range(rs, 0.0f, 0.7f));
            sh.velocity = radial * pcg_range(rs, 6.0f, 18.0f);
            sh.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            sh.size = radius * pcg_range(rs, 0.10f, 0.22f);
            sh.lifetime = pcg_range(rs, 0.35f, 0.70f);
            sh.drag = 3.5f;
            sh.rotation = a;
            sh.spin = pcg_signed(rs) * 5.0f;
            push(sh, pcg_range(rs, 0.0f, 0.05f));
        }
        for (u32 k = 0; k < 6; ++k) {
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin + Vec2{pcg_signed(rs), pcg_signed(rs)} * (radius * 0.5f);
            m.velocity = Vec2{pcg_signed(rs), pcg_signed(rs)} * 1.5f;
            m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.35f};
            m.size = radius * pcg_range(rs, 0.7f, 1.3f);
            m.lifetime = pcg_range(rs, 0.7f, 1.2f);
            m.drag = 1.5f;
            push(m, 0.03f * static_cast<f32>(k));
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ScarBuilt / ScarDestroyed — a Fibroblast's collagen wall went down or
    // came down (sim/scar). `direction` runs along the bar, `radius` is its
    // half-length and `magnitude` its half-width (on ScarBuilt; on
    // ScarDestroyed magnitude is 1 for chewed through, 0 for dissolved).
    // Both are drawn ALONG the bar rather than from its centre: a wall is a
    // line, and a burst from its middle would read as something else
    // happening at that point. The build is quiet -- fibres settling into
    // place -- and the loss is a scatter of collagen across the lane.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ScarBuilt: {
        const Vec2 along = math::normalize_safe(event.direction);
        const Vec2 across = perp(along);
        const f32 half_len = math::max(event.radius, 0.5f);
        const f32 half_wid = math::max(event.magnitude, 0.2f);
        const u32 fibres = 6u + static_cast<u32>(half_len * 2.0f);
        for (u32 k = 0; k < fibres; ++k) {
            const f32 u = pcg_range(rs, -1.0f, 1.0f);
            ParticleSpawnParams sh;
            sh.kind = ParticleKind::Shard;
            sh.blend = BlendMode::AlphaBlend;
            sh.position = event.origin + along * (u * half_len) +
                          across * pcg_range(rs, -half_wid, half_wid) * 2.5f;
            sh.velocity = across * (-pcg_signed(rs) * 2.0f) + along * pcg_signed(rs) * 0.8f;
            sh.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            sh.size = pcg_range(rs, 0.18f, 0.34f);
            sh.lifetime = pcg_range(rs, 0.35f, 0.65f);
            sh.drag = 4.0f;
            sh.rotation = std::atan2(along.y, along.x) + pcg_signed(rs) * 0.25f;
            sh.spin = pcg_signed(rs) * 1.5f;
            push(sh, pcg_range(rs, 0.0f, 0.12f));
        }
        for (u32 k = 0; k < 4; ++k) {
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin + along * pcg_range(rs, -half_len, half_len) * 0.8f;
            m.velocity = across * pcg_signed(rs) * 0.6f;
            m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.22f};
            m.size = half_wid * pcg_range(rs, 2.0f, 3.2f);
            m.lifetime = pcg_range(rs, 0.5f, 0.9f);
            m.drag = 2.0f;
            push(m, 0.03f * static_cast<f32>(k));
        }
        break;
    }
    case sim::CombatEventType::ScarDestroyed: {
        const Vec2 along = math::normalize_safe(event.direction);
        const Vec2 across = perp(along);
        const f32 half_len = math::max(event.radius, 0.5f);
        const bool chewed = event.magnitude > 0.5f;
        const u32 shards = 10u + static_cast<u32>(half_len * 3.0f);
        for (u32 k = 0; k < shards; ++k) {
            const f32 u = pcg_range(rs, -1.0f, 1.0f);
            const f32 side = pcg_signed(rs) >= 0.0f ? 1.0f : -1.0f;
            ParticleSpawnParams sh;
            sh.kind = ParticleKind::Shard;
            sh.blend = BlendMode::AlphaBlend;
            sh.position = event.origin + along * (u * half_len);
            sh.velocity = across * (side * pcg_range(rs, chewed ? 5.0f : 1.5f, chewed ? 14.0f : 4.0f)) +
                          along * pcg_signed(rs) * 2.0f;
            sh.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            sh.size = pcg_range(rs, 0.16f, 0.32f);
            sh.lifetime = pcg_range(rs, 0.35f, 0.75f);
            sh.drag = 3.5f;
            sh.rotation = pcg_range(rs, 0.0f, math::kTwoPi);
            sh.spin = pcg_signed(rs) * 5.0f;
            push(sh, pcg_range(rs, 0.0f, 0.05f));
        }
        const u32 puffs = 3u + static_cast<u32>(half_len);
        for (u32 k = 0; k < puffs; ++k) {
            ParticleSpawnParams m;
            m.kind = ParticleKind::Mist;
            m.blend = BlendMode::AlphaBlend;
            m.position = event.origin + along * pcg_range(rs, -half_len, half_len);
            m.velocity = across * pcg_signed(rs) * 1.5f;
            m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.32f};
            m.size = pcg_range(rs, 1.2f, 2.2f);
            m.lifetime = pcg_range(rs, 0.6f, 1.1f);
            m.drag = 1.5f;
            push(m, 0.03f * static_cast<f32>(k));
        }
        break;
    }

    case sim::CombatEventType::Count:
    default:
        break;
    }
}

void ParticleSystem::emit_for_events(const sim::CombatEvent* events, usize count) {
    // This is the once-per-frame drain by contract, so it is also the frame
    // boundary for the per-frame spawn counter the debug overlay reads.
    stats_.spawned_this_frame = 0;
    if (events == nullptr) return;
    for (usize i = 0; i < count; ++i) emit_for_event(events[i]);
}

} // namespace immune::vfx
