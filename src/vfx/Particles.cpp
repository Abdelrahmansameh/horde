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
// which is what the Mortar's three-stage impact needs: land, ~0.05 s charge,
// then the big ring. Nothing in the update loop knows about it.
#include "vfx/Particles.h"

#include "core/Clock.h"
#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Rng.h"

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
// Palettes. Six towers, six clearly separable hues — the brief's requirement is
// that a player glancing at the screen knows which tower is firing, and hue is
// the only channel that survives a glance at 60 fps.
//
//   Neutrophil GUNNER  warm white-yellow
//   Macrophage MORTAR  amber / orange (digestive, not fire — no reds)
//   Interferon CRYO    blue-white / cyan
//   CytotoxicT TESLA   violet-white
//   BCell      LASER   pale green-cyan (antibody)
//   NKCell     BLADE   magenta-pink
// ---------------------------------------------------------------------------
struct TowerPalette {
    Vec4 primary;  ///< The hue a player identifies the tower by.
    Vec4 accent;   ///< Near-white core, for the hot centre of a flash.
};

TowerPalette palette_for(TowerType t) {
    switch (t) {
    case TowerType::Neutrophil: return {Vec4{1.00f, 0.96f, 0.68f, 1.0f}, Vec4{1.00f, 1.00f, 0.94f, 1.0f}};
    case TowerType::Macrophage: return {Vec4{1.00f, 0.66f, 0.24f, 1.0f}, Vec4{1.00f, 0.95f, 0.80f, 1.0f}};
    case TowerType::Interferon: return {Vec4{0.52f, 0.84f, 1.00f, 1.0f}, Vec4{0.88f, 0.98f, 1.00f, 1.0f}};
    case TowerType::CytotoxicT: return {Vec4{0.76f, 0.66f, 1.00f, 1.0f}, Vec4{1.00f, 1.00f, 1.00f, 1.0f}};
    case TowerType::BCell:      return {Vec4{0.62f, 1.00f, 0.80f, 1.0f}, Vec4{0.94f, 1.00f, 0.97f, 1.0f}};
    case TowerType::NKCell:     return {Vec4{1.00f, 0.52f, 0.86f, 1.0f}, Vec4{1.00f, 0.92f, 0.98f, 1.0f}};
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
    case PathogenFamily::FungalSpore: return Vec4{1.00f, 0.86f, 0.46f, 1.0f};
    case PathogenFamily::Parasite:    return Vec4{1.00f, 0.52f, 0.34f, 1.0f};
    case PathogenFamily::CancerCell:  return Vec4{0.92f, 0.40f, 0.76f, 1.0f};
    case PathogenFamily::Allergen:    return Vec4{1.00f, 0.74f, 0.34f, 1.0f};
    case PathogenFamily::Count:
    default:                          return Vec4{1.00f, 1.00f, 1.00f, 1.0f};
    }
}

constexpr f32 kFamilyTintWeight = 0.18f;

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
//   visual_id the tower's upgrade TIER, 1..5 (0 is treated as 1). The sim
//             assigns it no meaning; this layer defines it as the escalation
//             axis the brief asks for: more firing points, bigger blasts, more
//             crystal branches, more receptor arms, more blades.
//   magnitude "how big should this read" — used for the per-hop falloff on
//             Tesla chains and the weight of an impact, so one event type can
//             render at wildly different intensities.
// ---------------------------------------------------------------------------

void ParticleSystem::emit_for_event(const sim::CombatEvent& event) {
    const u32 tier = math::clamp<u32>(event.visual_id == 0 ? 1u : event.visual_id, 1u, 5u);
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
    // MuzzleFlash — a tower fired. The Gunner raises this per round, so it must
    // stay cheap per event and *accumulate* into a stream; the beam/cone/rotor
    // towers raise it once per activation and can afford more.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::MuzzleFlash: {
        ParticleSpawnParams p;
        p.position = event.origin;
        p.blend = BlendMode::Additive;

        switch (event.source) {
        case TowerType::Neutrophil: {
            // GUNNER. Tiny hot pop at the muzzle, then a fan of very fast
            // tracers. Tier adds FIRING POINTS: the rounds leave from
            // (1 + tier) laterally-offset barrels, which is what turns a high
            // fire rate into a solid continuous stream rather than a dotted
            // line.
            p.kind = ParticleKind::Spark;
            p.color = pal.accent;
            p.size = 0.20f + 0.03f * tierf;
            p.lifetime = 0.07f;
            p.drag = 6.0f;
            push(p);

            const Vec2 side = perp(dir);
            const u32 points = 1u + tier;
            for (u32 b = 0; b < points; ++b) {
                const f32 lateral = (static_cast<f32>(b) - static_cast<f32>(points - 1) * 0.5f) * 0.22f;
                for (u32 k = 0; k < 2; ++k) {
                    ParticleSpawnParams t2;
                    t2.kind = ParticleKind::Tracer;
                    t2.blend = BlendMode::Additive;
                    t2.position = event.origin + side * lateral + dir * pcg_range(rs, 0.0f, 0.5f);
                    const Vec2 jitter = rotate_by(dir, pcg_range(rs, -0.10f, 0.10f));
                    t2.velocity = jitter * pcg_range(rs, 52.0f, 78.0f);
                    t2.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
                    t2.size = pcg_range(rs, 0.09f, 0.15f);
                    t2.lifetime = pcg_range(rs, 0.14f, 0.24f);
                    t2.drag = 1.4f;
                    push(t2);
                }
            }

            // Tier 5: antibody bits ORBITING the cell before being fired.
            if (tier >= 5) {
                for (u32 o = 0; o < 8; ++o) {
                    const f32 a = math::kTwoPi * static_cast<f32>(o) / 8.0f + pcg_range(rs, -0.2f, 0.2f);
                    const Vec2 radial = Vec2{std::cos(a), std::sin(a)};
                    ParticleSpawnParams t3;
                    t3.kind = ParticleKind::Tracer;
                    t3.blend = BlendMode::Additive;
                    t3.position = event.origin + radial * pcg_range(rs, 0.7f, 1.0f);
                    t3.velocity = perp(radial) * pcg_range(rs, 3.0f, 5.5f);
                    t3.color = pal.primary;
                    t3.size = 0.08f;
                    t3.lifetime = pcg_range(rs, 0.25f, 0.40f);
                    t3.drag = 0.6f;
                    push(t3, pcg_range(rs, 0.0f, 0.05f));
                }
            }
            break;
        }

        case TowerType::Macrophage: {
            // MORTAR. A soft biological "cough" as the vesicle is lobbed —
            // deliberately low-energy so the *impact* owns all the drama.
            p.kind = ParticleKind::Spark;
            p.color = pal.accent;
            p.size = 0.34f;
            p.lifetime = 0.12f;
            p.drag = 5.0f;
            push(p);

            const u32 puffs = 3u + tier;
            for (u32 k = 0; k < puffs; ++k) {
                ParticleSpawnParams m;
                m.kind = ParticleKind::Mist;
                m.blend = BlendMode::AlphaBlend;   // matter, not glow
                m.position = event.origin + Vec2{pcg_signed(rs), pcg_signed(rs)} * 0.25f;
                m.velocity = rotate_by(dir, pcg_range(rs, -0.7f, 0.7f)) * pcg_range(rs, 1.5f, 4.0f);
                m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.42f};
                m.size = pcg_range(rs, 0.30f, 0.55f);
                m.lifetime = pcg_range(rs, 0.30f, 0.55f);
                m.drag = 2.6f;
                m.buoyancy = 0.8f;
                m.spin = pcg_signed(rs) * 1.5f;
                push(m);
            }
            break;
        }

        case TowerType::Interferon: {
            // CRYO. The cone itself is ConePulse; this is just the emitter
            // lighting up, so it stays small and cold.
            p.kind = ParticleKind::Spark;
            p.color = pal.accent;
            p.size = 0.26f;
            p.lifetime = 0.10f;
            p.drag = 4.0f;
            push(p);

            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = event.origin;
            r.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.55f};
            r.size = 0.8f + 0.15f * tierf;
            r.lifetime = 0.16f;
            push(r);
            break;
        }

        case TowerType::CytotoxicT: {
            // TESLA. Charge stubs at the receptor arms: (1 + tier) short bolts
            // radiating from the cell body. This is the "more receptor arms"
            // tier escalation, and it reads as *electrical* before the chain
            // even leaves.
            p.kind = ParticleKind::Spark;
            p.color = pal.accent;
            p.size = 0.30f;
            p.lifetime = 0.08f;
            p.drag = 5.0f;
            push(p);

            const u32 arms = 1u + tier;
            for (u32 a = 0; a < arms; ++a) {
                const f32 ang = math::kTwoPi * static_cast<f32>(a) / static_cast<f32>(arms)
                              + pcg_range(rs, -0.35f, 0.35f);
                ParticleSpawnParams b;
                b.kind = ParticleKind::Bolt;
                b.blend = BlendMode::Additive;
                b.position = event.origin;
                b.velocity = Vec2{std::cos(ang), std::sin(ang)};  // UNIT: segment direction
                b.size = pcg_range(rs, 0.6f, 1.2f);               // segment LENGTH
                b.color = mix4(pal.primary, pal.accent, 0.4f);
                b.lifetime = pcg_range(rs, 0.05f, 0.09f);
                push(b, pcg_range(rs, 0.0f, 0.03f));
            }
            break;
        }

        case TowerType::BCell: {
            // LASER. Charge ring removed (was rendering as a broken cyan
            // blob) — just the muzzle spark for the beam's "on" moment.
            p.kind = ParticleKind::Spark;
            p.color = pal.accent;
            p.size = 0.36f;
            p.lifetime = 0.10f;
            p.drag = 5.0f;
            push(p);
            break;
        }

        case TowerType::NKCell: {
            // BLADE. This event is "the rotor swept" — the trail, not a shot.
            // (2 + tier) blades, each dropping three trail points along its
            // length with tangential velocity. At the rotor's real spin rate
            // these arrive fast enough that the trails perceptually merge into
            // a glowing disk, which is exactly the brief.
            //
            // The count MUST match the arm count entity.frag draws for the NK
            // body (shape 21, fed by EntityInstance::shape_param) or the solid
            // blades and the trails they throw off disagree about how many arms
            // the rotor has. Three is the floor: two arms 180 degrees apart
            // fuse into a single S-curve rather than reading as a rotor.
            const f32 reach = event.radius > 0.05f ? event.radius : 2.0f;
            const u32 blades = 2u + tier;
            for (u32 b = 0; b < blades; ++b) {
                const f32 ang = math::kTwoPi * static_cast<f32>(b) / static_cast<f32>(blades);
                const Vec2 arm = rotate_by(dir, ang);
                for (u32 k = 0; k < 3; ++k) {
                    const f32 frac = 0.45f + 0.275f * static_cast<f32>(k);
                    ParticleSpawnParams t2;
                    t2.kind = ParticleKind::Tracer;
                    t2.blend = BlendMode::Additive;
                    t2.position = event.origin + arm * (reach * frac);
                    t2.velocity = perp(arm) * (reach * frac * 5.0f);
                    t2.color = mix4(pal.primary, pal.accent, frac * 0.5f);
                    t2.size = 0.13f + 0.05f * frac;
                    t2.lifetime = pcg_range(rs, 0.12f, 0.20f);
                    t2.drag = 3.0f;
                    t2.rotation = ang;
                    push(t2);
                }
            }
            break;
        }

        case TowerType::Count:
        default: {
            p.kind = ParticleKind::Spark;
            p.color = pal.accent;
            p.size = 0.25f;
            p.lifetime = 0.10f;
            p.drag = 4.0f;
            push(p);
            break;
        }
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ProjectileImpact — a round connected. Thousands per second at high Gunner
    // fire rates, so the Gunner case in particular is kept to a handful of
    // very short-lived particles.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ProjectileImpact: {
        const f32 pop = (event.radius > 0.05f ? event.radius : 0.35f) * (0.7f + 0.3f * mag);
        const Vec4 debris = mix4(pal.primary, fam, kFamilyTintWeight);

        if (event.source == TowerType::Macrophage) {
            // MORTAR stage 1 of 3: the vesicle lands. Tiny. All the weight is
            // in the Explosion event that follows.
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = pop * 0.8f;
            s.lifetime = 0.08f;
            s.drag = 6.0f;
            push(s);
            for (u32 k = 0; k < 2; ++k) {
                ParticleSpawnParams m;
                m.kind = ParticleKind::Mist;
                m.blend = BlendMode::AlphaBlend;
                m.position = event.origin;
                m.velocity = Vec2{pcg_signed(rs), pcg_signed(rs)} * 2.0f;
                m.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.35f};
                m.size = pop * 0.9f;
                m.lifetime = 0.16f;
                m.drag = 4.0f;
                push(m);
            }
            break;
        }

        if (event.source == TowerType::BCell) {
            // LASER pierce. Every pathogen on the line must flash at the SAME
            // instant, so this case takes no random delay and no random
            // lifetime — the sim raises all of a sweep's impacts in one tick,
            // and identical timings turn them into one continuous sweep rather
            // than a string of individual explosions.
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = event.origin;
            s.color = pal.accent;
            s.size = pop * 1.15f;
            s.lifetime = 0.12f;
            s.drag = 7.0f;
            push(s);

            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = event.origin;
            r.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.8f};
            r.size = pop * 2.2f;
            r.lifetime = 0.12f;
            push(r);
            break;
        }

        // GUNNER (and anything else with a projectile): a tiny white pop plus a
        // few scattering bits. Cheap by design.
        ParticleSpawnParams s;
        s.kind = ParticleKind::Spark;
        s.blend = BlendMode::Additive;
        s.position = event.origin;
        s.color = pal.accent;
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
    // Explosion — the MORTAR's digestive burst. THREE STAGES, per the brief:
    //   (1) t=0.00  the vesicle ruptures: tiny white core.
    //   (2) t=0.00..0.055  the area "charges": matter is pulled inward.
    //   (3) t=0.055 the burst: white centre, amber ring expanding to the full
    //       radius, fragments thrown outward, all gone by ~0.22s.
    // Staging is done with negative birth ages, so the update loop stays
    // completely unaware that any of this is happening.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::Explosion: {
        const f32 radius = math::max(event.radius, 1.5f);
        constexpr f32 kCharge = 0.055f;   // the ~0.05s pause the brief asks for

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
            // rings on a short stagger, which is what makes a tier-5 mortar
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
    // ChainArc — one TESLA hop. INSTANTANEOUS, JAGGED, BRANCHING. Short
    // lifetimes (< 0.09s) are what sell "instantaneous"; the branches are what
    // stop it reading like the Laser's clean straight line. `magnitude` carries
    // the sim's per-hop falloff, so each jump in a chain is slightly smaller
    // without this layer having to track hop indices.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ChainArc: {
        const f32 weight = math::clamp(mag, 0.3f, 1.6f);

        // Main link, doubled at higher tiers with a different seed so the two
        // jag patterns cross and the arc looks genuinely forked.
        const u32 strands = 1u + tier / 2u;
        for (u32 k = 0; k < strands; ++k) {
            ParticleSpawnParams b;
            b.kind = ParticleKind::Bolt;
            b.blend = BlendMode::Additive;
            b.position = event.origin;
            b.velocity = seg_dir;
            b.size = seg_len;
            b.color = k == 0 ? pal.accent : mix4(pal.primary, pal.accent, 0.3f);
            b.lifetime = pcg_range(rs, 0.05f, 0.08f);
            b.rotation = std::atan2(seg_dir.y, seg_dir.x);
            push(b);
        }

        // Branches: short bolts peeling off the main link at a sharp angle.
        // Tier is the branch count — "more crystal branches" for the T-cell.
        const u32 branches = tier;
        for (u32 k = 0; k < branches; ++k) {
            const f32 along = pcg_range(rs, 0.15f, 0.85f) * seg_len;
            const f32 off = (pcg_f32(rs) < 0.5f ? -1.0f : 1.0f) * pcg_range(rs, 0.45f, 0.95f);
            ParticleSpawnParams b;
            b.kind = ParticleKind::Bolt;
            b.blend = BlendMode::Additive;
            b.position = event.origin + seg_dir * along;
            b.velocity = rotate_by(seg_dir, off);
            b.size = seg_len * pcg_range(rs, 0.20f, 0.45f);
            b.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.75f};
            b.lifetime = pcg_range(rs, 0.04f, 0.07f);
            push(b);
        }

        // Every hit: bright white flash, tiny circular shockwave, and the
        // target briefly reading as white.
        {
            const Vec2 hit = event.origin + seg_dir * seg_len;
            ParticleSpawnParams s;
            s.kind = ParticleKind::Spark;
            s.blend = BlendMode::Additive;
            s.position = hit;
            s.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
            s.size = 0.45f * weight;
            s.lifetime = 0.07f;
            s.drag = 8.0f;
            push(s);

            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = hit;
            r.color = Vec4{1.0f, 1.0f, 1.0f, 0.85f};
            r.size = 1.1f * weight;
            r.lifetime = 0.13f;
            push(r);

            for (u32 k = 0; k < 2u + tier; ++k) {
                const f32 a = pcg_range(rs, 0.0f, math::kTwoPi);
                ParticleSpawnParams t2;
                t2.kind = ParticleKind::Tracer;
                t2.blend = BlendMode::Additive;
                t2.position = hit;
                t2.velocity = Vec2{std::cos(a), std::sin(a)} * pcg_range(rs, 4.0f, 11.0f);
                t2.color = mix4(pal.primary, fam, kFamilyTintWeight);
                t2.size = 0.07f;
                t2.lifetime = pcg_range(rs, 0.06f, 0.12f);
                t2.drag = 6.0f;
                push(t2);
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    // ConePulse — the CRYO signal. No projectile at all: a wide cone of
    // blue-white interferon signals sweeping outward, with a slow haze behind
    // it. Tier widens the signal count so a maxed Interferon visibly saturates
    // its cone.
    // -----------------------------------------------------------------------
    case sim::CombatEventType::ConePulse: {
        const f32 reach = math::max(event.radius, 2.0f);
        const f32 half = event.arc_radians > 0.01f ? event.arc_radians : 0.55f;

        const u32 signals = 20u + 14u * tier;
        for (u32 k = 0; k < signals; ++k) {
            const f32 a = pcg_range(rs, -half, half);
            const Vec2 d2 = rotate_by(dir, a);
            const f32 travel = 0.34f;
            ParticleSpawnParams t2;
            t2.kind = ParticleKind::Tracer;
            t2.blend = BlendMode::Additive;
            t2.position = event.origin + d2 * pcg_range(rs, 0.0f, reach * 0.12f);
            t2.velocity = d2 * (reach / travel) * pcg_range(rs, 0.65f, 1.15f);
            t2.color = mix4(pal.primary, pal.accent, pcg_f32(rs));
            t2.size = pcg_range(rs, 0.08f, 0.15f);
            t2.lifetime = pcg_range(rs, 0.28f, 0.44f);
            t2.drag = 0.9f;
            push(t2, pcg_range(rs, 0.0f, 0.06f));
        }

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

        // Emitter ripple, so the pulse has an origin the eye can find.
        {
            ParticleSpawnParams r;
            r.kind = ParticleKind::Ring;
            r.blend = BlendMode::Additive;
            r.position = event.origin;
            r.color = Vec4{pal.primary.r, pal.primary.g, pal.primary.b, 0.5f};
            r.size = reach * 0.35f;
            r.lifetime = 0.20f;
            push(r);
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
