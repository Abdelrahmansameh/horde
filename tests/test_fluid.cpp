// tests/test_fluid.cpp — the fluid solver, on its own.
//
// test_towers.cpp covers the Goblet Cell end to end (a burst leaves, travels,
// lands, expires). This file covers the layer underneath it: the properties the
// solver must have for that tower to be possible at all, stated as things that
// would be silently, invisibly wrong if they broke.
#include "sim/fluid/Fluid.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

constexpr f32 kW = 100.0f;
constexpr f32 kH = 60.0f;

/// A world with a rectangular open lumen and solid tissue around it, plus the
/// pieces FluidSystem::update() insists on being handed.
struct Harness {
    TissueMask mask;
    DistanceField sdf;
    SpatialHash hash;
    ChaffBuffers chaff;
    FluidBuffers fluid;
    FluidSystem sys;
    Rect bounds{Vec2{0.0f, 0.0f}, Vec2{kW, kH}};

    explicit Harness(const FluidTuning& tuning = FluidTuning{}, i32 margin = 6) {
        mask.resize(static_cast<i32>(kW), static_cast<i32>(kH), 1.0f, Vec2{0.0f, 0.0f});
        for (i32 y = margin; y < static_cast<i32>(kH) - margin; ++y) {
            for (i32 x = margin; x < static_cast<i32>(kW) - margin; ++x) {
                mask.set_walkable(x, y, true);
            }
        }
        sdf.bake(mask);

        SpatialHashDesc hd;
        hd.bounds = bounds;
        hd.cell_size = 4.0f;
        hash.configure(hd);

        chaff.reserve(16384);
        fluid.reserve(16384);
        sys.configure(bounds, tuning);
        rebuild();
    }

    void rebuild() { hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr); }

    void step(f32 dt = kFixedDt, CombatEventSink* events = nullptr) {
        sys.update(fluid, chaff, hash, sdf, bounds, dt, events);
    }

    /// A jet aimed along +x from the left side of the lumen.
    FluidJetParams jet(Vec2 origin, Vec2 dir, u32 seed) const {
        FluidJetParams j;
        j.origin = origin;
        j.direction = dir;
        j.speed = 34.0f;
        j.nozzle_radius = 1.05f;
        j.lifetime = 2.0f;
        j.damage_per_second = 20.0f;
        j.seed = seed;
        return j;
    }
};

/// Mean nearest-neighbour distance. The single most informative number about a
/// particle fluid: it says what spacing the thing actually settled at, which is
/// what every other property is downstream of.
f32 mean_nearest_neighbour(const FluidBuffers& f) {
    const usize n = f.count();
    if (n < 2) return 0.0f;
    f64 total = 0.0;
    for (usize i = 0; i < n; ++i) {
        f32 best = 1e30f;
        for (usize j = 0; j < n; ++j) {
            if (i == j) continue;
            const f32 dx = f.pos_x[j] - f.pos_x[i];
            const f32 dy = f.pos_y[j] - f.pos_y[i];
            const f32 d2 = dx * dx + dy * dy;
            if (d2 < best) best = d2;
        }
        total += std::sqrt(static_cast<f64>(best));
    }
    return static_cast<f32>(total / static_cast<f64>(n));
}

} // namespace

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

TEST_CASE("emission rate is derived from swept area, not authored", "[fluid][emit]") {
    // Doubling the nozzle width doubles the fluid; so does doubling the speed.
    // This is the whole reason emit() owns the count: a caller free to pick it
    // would eventually pick a number the nozzle cannot physically pass, and the
    // relaxation pass answers that by detonating the slab.
    Harness h;
    const u32 base = h.sys.emit(h.fluid, h.jet(Vec2{20.0f, 30.0f}, Vec2{1.0f, 0.0f}, 1), kFixedDt);
    REQUIRE(base > 0);

    h.fluid.clear();
    FluidJetParams wide = h.jet(Vec2{20.0f, 30.0f}, Vec2{1.0f, 0.0f}, 1);
    wide.nozzle_radius *= 2.0f;
    const u32 wide_n = h.sys.emit(h.fluid, wide, kFixedDt);

    h.fluid.clear();
    FluidJetParams fast = h.jet(Vec2{20.0f, 30.0f}, Vec2{1.0f, 0.0f}, 1);
    fast.speed *= 2.0f;
    const u32 fast_n = h.sys.emit(h.fluid, fast, kFixedDt);

    INFO("base " << base << " wide " << wide_n << " fast " << fast_n);
    REQUIRE(wide_n >= base * 2 - 1);
    REQUIRE(wide_n <= base * 2 + 1);
    REQUIRE(fast_n >= base * 2 - 1);
    REQUIRE(fast_n <= base * 2 + 1);
}

TEST_CASE("a fractional emission rate is dithered rather than truncated", "[fluid][emit]") {
    // A nozzle worth 9.4 particles a tick must average 9.4, not 9. Truncating
    // loses 4% of the fluid at one tuning and 40% at another, which shows up as
    // the tower mysteriously getting weaker when someone nudges its speed.
    Harness h;
    FluidJetParams j = h.jet(Vec2{20.0f, 30.0f}, Vec2{1.0f, 0.0f}, 0);
    const f32 spacing = h.sys.tuning().rest_spacing;
    const f32 expect = (j.nozzle_radius * 2.0f) * (j.speed * kFixedDt) /
                       (spacing * spacing * 0.8660254f);

    u32 total = 0;
    constexpr u32 kTrials = 400;
    for (u32 t = 0; t < kTrials; ++t) {
        h.fluid.clear();
        j.seed = t * 2654435761u + 7u;
        total += h.sys.emit(h.fluid, j, kFixedDt);
    }
    const f32 mean = static_cast<f32>(total) / static_cast<f32>(kTrials);
    INFO("expected " << expect << " per tick, measured mean " << mean);
    REQUIRE(mean > expect - 0.15f);
    REQUIRE(mean < expect + 0.15f);
}

TEST_CASE("a freshly emitted slab is already near rest spacing", "[fluid][emit]") {
    // If it is not, the first relaxation pass is a shockwave and the jet blows
    // itself apart at the muzzle before it has travelled anywhere.
    Harness h;
    FluidJetParams j = h.jet(Vec2{20.0f, 30.0f}, Vec2{1.0f, 0.0f}, 99);
    // Several consecutive ticks, so the slabs stack into a real column rather
    // than a single isolated sheet.
    for (u32 t = 0; t < 6; ++t) {
        j.seed = t * 7919u + 3u;
        h.sys.emit(h.fluid, j, kFixedDt);
        // Advance the origin the way a stationary nozzle's fluid would have
        // moved, so the six slabs do not land on top of each other.
        j.origin += Vec2{j.speed * kFixedDt, 0.0f};
    }
    const f32 spacing = h.sys.tuning().rest_spacing;
    const f32 nn = mean_nearest_neighbour(h.fluid);
    INFO("rest spacing " << spacing << ", emitted mean nearest neighbour " << nn);
    REQUIRE(nn > spacing * 0.45f);
    REQUIRE(nn < spacing * 1.35f);
}

// ---------------------------------------------------------------------------
// The solver
// ---------------------------------------------------------------------------

TEST_CASE("a free-floating blob settles instead of running away", "[fluid][solver]") {
    // The property the stiffness / near_stiffness RATIO exists to provide, and
    // the one that is invisible until it is badly wrong. The far pressure term
    // is zero at rest density while the near term never is, so the two together
    // decide what a blob left alone actually does. Get the ratio wrong upward
    // and it inflates until it tears into beads with holes between them; wrong
    // downward and nothing stops it collapsing into a point.
    //
    // What is NOT asserted is that the blob keeps exactly its seeded spacing.
    // It should not: this fluid is cohesive on purpose, and a finite blob under
    // surface tension is genuinely compressed by its own curvature (Laplace),
    // more so the smaller it is. The requirement is that the compression STOPS
    // — that there is an equilibrium and the blob finds it.
    Harness h;
    FluidJetParams j = h.jet(Vec2{30.0f, 30.0f}, Vec2{1.0f, 0.0f}, 5);
    j.speed = 0.0f;      // no travel: this is about density, not ballistics
    j.lifetime = 100.0f; // outlive the measurement
    const f32 s = h.sys.tuning().rest_spacing;
    for (i32 row = -6; row <= 6; ++row) {
        for (i32 col = -6; col <= 6; ++col) {
            const f32 x = 30.0f + static_cast<f32>(col) * s +
                          ((row & 1) != 0 ? s * 0.5f : 0.0f);
            const f32 y = 30.0f + static_cast<f32>(row) * s * 0.8660254f;
            h.fluid.spawn(Vec2{x, y}, Vec2{0.0f, 0.0f}, j);
        }
    }
    const usize seeded = h.fluid.count();

    for (int t = 0; t < 60; ++t) h.step();
    const f32 at_1s = mean_nearest_neighbour(h.fluid);
    for (int t = 0; t < 120; ++t) h.step();
    const f32 at_3s = mean_nearest_neighbour(h.fluid);

    INFO("rest " << s << ", after 1s " << at_1s << ", after 3s " << at_3s);
    REQUIRE(h.fluid.count() == seeded);   // nothing escaped or was culled

    // Settled: the second two seconds barely move it.
    REQUIRE(at_3s > at_1s * 0.92f);
    REQUIRE(at_3s < at_1s * 1.08f);
    // ...and it settled somewhere sane rather than at a point or a haze.
    REQUIRE(at_3s > s * 0.55f);
    REQUIRE(at_3s < s * 1.60f);
}

TEST_CASE("the solver never puts fluid inside solid tissue", "[fluid][solver][walls]") {
    // Fired point-blank into the wall at full speed, which is the case most
    // likely to tunnel: one substep of travel against one substep of pushing
    // back out.
    Harness h;
    FluidJetParams j = h.jet(Vec2{10.0f, 30.0f}, Vec2{-1.0f, 0.0f}, 11);
    for (int t = 0; t < 90; ++t) {
        if (t < 24) {
            j.seed = static_cast<u32>(t) * 7919u + 1u;
            h.sys.emit(h.fluid, j, kFixedDt);
        }
        h.step();
        for (usize i = 0; i < h.fluid.count(); ++i) {
            const f32 clearance = h.sdf.sample(Vec2{h.fluid.pos_x[i], h.fluid.pos_y[i]});
            INFO("tick " << t << " particle " << i << " clearance " << clearance);
            REQUIRE(clearance > -0.35f);
        }
    }
    REQUIRE(h.fluid.count() > 0);
}

TEST_CASE("fluid stopped by a wall spreads sideways along it", "[fluid][solver][walls]") {
    // The splash, reduced to its measurable core. Nothing in the solver
    // deflects anything: the wall only removes the normal component, and the
    // lateral spread is entirely the pressure of the fluid arriving behind.
    Harness h;
    FluidJetParams j = h.jet(Vec2{20.0f, 30.0f}, Vec2{-1.0f, 0.0f}, 21);
    const f32 launch_half_width = j.nozzle_radius;

    f32 widest = 0.0f;
    for (int t = 0; t < 120; ++t) {
        if (t < 30) {
            j.seed = static_cast<u32>(t) * 7919u + 5u;
            h.sys.emit(h.fluid, j, kFixedDt);
        }
        h.step();
        f32 lo = 1e30f;
        f32 hi = -1e30f;
        for (usize i = 0; i < h.fluid.count(); ++i) {
            if (h.fluid.pos_x[i] > 9.0f) continue;   // only what reached the wall
            lo = math::min(lo, h.fluid.pos_y[i]);
            hi = math::max(hi, h.fluid.pos_y[i]);
        }
        if (hi > lo) widest = math::max(widest, hi - lo);
    }
    INFO("launched " << (launch_half_width * 2.0f) << " wide, spread to " << widest);
    REQUIRE(widest > launch_half_width * 4.0f);
}

TEST_CASE("particles expire on their lifetime and free their slots", "[fluid][solver]") {
    Harness h;
    FluidJetParams j = h.jet(Vec2{20.0f, 30.0f}, Vec2{1.0f, 0.0f}, 31);
    j.lifetime = 0.5f;
    h.sys.emit(h.fluid, j, kFixedDt);
    REQUIRE(h.fluid.count() > 0);

    for (int t = 0; t < 40; ++t) h.step();
    REQUIRE(h.fluid.count() == 0);
    // And the store is genuinely reusable afterwards, not merely emptied.
    REQUIRE(h.sys.emit(h.fluid, j, kFixedDt) > 0);
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------

TEST_CASE("damage is aggregate: soaked chaff loses density and is weakened",
          "[fluid][damage]") {
    Harness h;
    // A wet patch, hand-placed so the test is about the coverage grid rather
    // than about ballistics.
    FluidJetParams j = h.jet(Vec2{50.0f, 30.0f}, Vec2{1.0f, 0.0f}, 41);
    j.lifetime = 10.0f;
    const f32 s = h.sys.tuning().rest_spacing;
    for (i32 row = -4; row <= 4; ++row) {
        for (i32 col = -4; col <= 4; ++col) {
            h.fluid.spawn(Vec2{50.0f + static_cast<f32>(col) * s, 30.0f + static_cast<f32>(row) * s},
                          Vec2{0.0f, 0.0f}, j);
        }
    }

    ChaffSpawnParams under;
    under.position = Vec2{50.0f, 30.0f};
    under.density = 100.0f;
    h.chaff.spawn(under);
    ChaffSpawnParams away;
    away.position = Vec2{80.0f, 30.0f};
    away.density = 100.0f;
    h.chaff.spawn(away);
    h.rebuild();

    for (int t = 0; t < 30; ++t) h.step();

    INFO("under the fluid " << h.chaff.density[0] << ", well away " << h.chaff.density[1]);
    REQUIRE(h.chaff.density[0] < 100.0f);
    REQUIRE(h.chaff.density[1] == 100.0f);
    // Soaked chaff is weakened -- the same flag DamageField.cpp,
    // Projectiles.cpp and Swarmers.cpp all read to hit it harder.
    REQUIRE((h.chaff.flags[0] & chaff_flags::kMarked) != 0);
    REQUIRE((h.chaff.flags[1] & chaff_flags::kMarked) == 0);
    // Deliberately NOT slowed. The Goblet Cell weakens; it does not root, or
    // it would make Interferon's cone and Neutrophil's NET redundant instead
    // of stacking with them.
    REQUIRE((h.chaff.flags[0] & chaff_flags::kSlowed) == 0);
    REQUIRE((h.chaff.flags[1] & chaff_flags::kSlowed) == 0);
}

TEST_CASE("a non-damaging mucus film applies a configurable slow only to matching families",
          "[fluid][slow]") {
    Harness h;
    FluidJetParams j = h.jet(Vec2{50.0f, 30.0f}, Vec2{1.0f, 0.0f}, 48);
    j.lifetime = 10.0f;
    j.damage_per_second = 0.0f;
    j.slow_duration = 4.0f;
    j.slow_factor = 0.07f;
    j.family_mask = static_cast<u8>(1u << static_cast<u8>(PathogenFamily::Bacteria));
    const f32 s = h.sys.tuning().rest_spacing;
    for (i32 row = -4; row <= 4; ++row) {
        for (i32 col = -4; col <= 4; ++col) {
            h.fluid.spawn(Vec2{50.0f + static_cast<f32>(col) * s,
                               30.0f + static_cast<f32>(row) * s}, Vec2{}, j);
        }
    }
    ChaffSpawnParams bacteria;
    bacteria.position = Vec2{50.0f, 30.0f};
    bacteria.density = 100.0f;
    bacteria.family = PathogenFamily::Bacteria;
    h.chaff.spawn(bacteria);
    ChaffSpawnParams virus = bacteria;
    virus.family = PathogenFamily::Virus;
    h.chaff.spawn(virus);
    h.rebuild();
    h.step();

    REQUIRE(h.chaff.density[0] == 100.0f);
    REQUIRE(h.chaff.density[1] == 100.0f);
    REQUIRE((h.chaff.flags[0] & chaff_flags::kSlowed) != 0);
    REQUIRE(h.chaff.slow_remaining[0] == Catch::Approx(4.0f));
    REQUIRE(h.chaff.slow_factor[0] == Catch::Approx(0.07f));
    REQUIRE((h.chaff.flags[0] & chaff_flags::kMarked) == 0);
    REQUIRE((h.chaff.flags[1] & chaff_flags::kSlowed) == 0);
}

TEST_CASE("the Goblet Cell does not benefit from its own weaken mark",
          "[fluid][damage][marked]") {
    // Every OTHER damage path bonuses off chaff_flags::kMarked; this one
    // deliberately does not, so re-soaking an already-marked puddle removes
    // exactly the same density it would on a fresh one. If this regresses, the
    // Goblet Cell quietly stops being a control tower and starts being the
    // best damage dealer in the roster, since it is also the only source of
    // the flag it would be reading.
    auto burn_once = [](bool pre_marked) {
        Harness h;
        FluidJetParams j = h.jet(Vec2{50.0f, 30.0f}, Vec2{1.0f, 0.0f}, 61);
        j.lifetime = 10.0f;
        const f32 s = h.sys.tuning().rest_spacing;
        for (i32 row = -4; row <= 4; ++row) {
            for (i32 col = -4; col <= 4; ++col) {
                h.fluid.spawn(Vec2{50.0f + static_cast<f32>(col) * s, 30.0f + static_cast<f32>(row) * s},
                              Vec2{0.0f, 0.0f}, j);
            }
        }
        ChaffSpawnParams p;
        p.position = Vec2{50.0f, 30.0f};
        p.density = 1000.0f;
        p.flags = pre_marked ? chaff_flags::kMarked : 0u;
        h.chaff.spawn(p);
        h.rebuild();
        h.step();
        return 1000.0f - h.chaff.density[0];
    };

    const f32 fresh = burn_once(false);
    const f32 pre_marked = burn_once(true);
    INFO("fresh patch removed " << fresh << ", pre-marked patch removed " << pre_marked);
    REQUIRE(fresh > 0.0f);
    REQUIRE(pre_marked == fresh);
}

TEST_CASE("damage saturates with coverage instead of scaling with it forever",
          "[fluid][damage]") {
    // Coverage is a ramp with a ceiling, not a multiplier. A single droplet
    // passing over an agent should tickle it; a pool standing on it should hurt
    // — but not eight times as much for eight times the fluid, because a
    // pathogen can only be so coated. Without the ceiling, a Goblet Cell's
    // output would scale with how tightly its own fluid happened to bunch up,
    // which is not a stat anybody can reason about.
    //
    // Measured over a SINGLE step, deliberately. Give the solver more than that
    // and it does what it should do to a pile of particles sitting on top of
    // each other — shove them apart — and the deep case stops being deep.
    auto burn = [](u32 particles) {
        Harness h;
        FluidJetParams j = h.jet(Vec2{50.0f, 30.0f}, Vec2{1.0f, 0.0f}, 51);
        j.lifetime = 10.0f;
        for (u32 k = 0; k < particles; ++k) {
            // A tight ring inside one coverage cell, so every particle lands in
            // the same cell as the agent no matter how many there are.
            const f32 a = 2.39996f * static_cast<f32>(k);
            const f32 r = 0.28f * std::sqrt(static_cast<f32>(k) /
                                            static_cast<f32>(particles > 1 ? particles : 1));
            h.fluid.spawn(Vec2{50.0f + r * std::cos(a), 30.0f + r * std::sin(a)},
                          Vec2{0.0f, 0.0f}, j);
        }
        ChaffSpawnParams p;
        p.position = Vec2{50.0f, 30.0f};
        // Modest, on purpose: one droplet for one tick removes a fraction of
        // a density unit, and a 1e6 pool would round that straight to zero in
        // f32 and make the test measure the mantissa instead of the ramp.
        p.density = 1000.0f;
        h.chaff.spawn(p);
        h.rebuild();
        h.step();
        return 1000.0f - h.chaff.density[0];
    };

    const f32 one = burn(1);
    const f32 few = burn(3);
    const f32 many = burn(16);
    INFO("1 particle removed " << one << ", 3 removed " << few << ", 16 removed " << many);
    REQUIRE(one > 0.0f);
    REQUIRE(few > one);           // more fluid does more, up to the ceiling
    REQUIRE(many >= few);
    REQUIRE(many < one * 16.0f);  // ...but nowhere near proportionally
}

TEST_CASE("splash events are raised on impact and stay under their cap",
          "[fluid][events]") {
    FluidTuning tuning;
    tuning.max_splash_events = 5;
    Harness h(tuning);
    CombatEventSink sink;
    sink.reserve(4096);

    FluidJetParams j = h.jet(Vec2{20.0f, 30.0f}, Vec2{-1.0f, 0.0f}, 61);
    usize worst_tick = 0;
    usize total = 0;
    bool saw_any = false;
    for (int t = 0; t < 90; ++t) {
        sink.clear();
        if (t < 30) {
            j.seed = static_cast<u32>(t) * 7919u + 9u;
            h.sys.emit(h.fluid, j, kFixedDt);
        }
        h.step(kFixedDt, &sink);
        usize n = 0;
        for (const CombatEvent& e : sink.events()) {
            if (e.type != CombatEventType::FluidSplash) continue;
            REQUIRE(e.source == TowerType::GobletCell);
            ++n;
        }
        worst_tick = math::max(worst_tick, n);
        total += n;
        saw_any = saw_any || n > 0;
    }
    INFO("total splash events " << total << ", worst single tick " << worst_tick);
    REQUIRE(saw_any);
    REQUIRE(worst_tick <= 5);
}

TEST_CASE("attaching an event sink does not change one bit of the solve",
          "[fluid][determinism]") {
    // CombatEvents.h's contract, applied here: events are an OUTPUT of the
    // tick. If the sink could perturb the solve, a replay would diverge from
    // its recording the moment the VFX layer was toggled.
    auto run = [](bool with_events) {
        Harness h;
        CombatEventSink sink;
        sink.reserve(4096);
        FluidJetParams j = h.jet(Vec2{20.0f, 30.0f}, Vec2{-1.0f, 0.0f}, 71);
        for (int t = 0; t < 60; ++t) {
            if (t < 20) {
                j.seed = static_cast<u32>(t) * 7919u + 13u;
                h.sys.emit(h.fluid, j, kFixedDt);
            }
            h.step(kFixedDt, with_events ? &sink : nullptr);
        }
        std::vector<f32> out;
        for (usize i = 0; i < h.fluid.count(); ++i) {
            out.push_back(h.fluid.pos_x[i]);
            out.push_back(h.fluid.pos_y[i]);
        }
        return out;
    };
    const std::vector<f32> quiet = run(false);
    const std::vector<f32> loud = run(true);
    REQUIRE(quiet.size() > 0);
    REQUIRE(quiet == loud);
}

TEST_CASE("the same seed produces the same fluid, twice", "[fluid][determinism]") {
    auto run = []() {
        Harness h;
        FluidJetParams j = h.jet(Vec2{20.0f, 30.0f}, Vec2{1.0f, 0.3f}, 81);
        for (int t = 0; t < 80; ++t) {
            if (t < 30) {
                j.seed = static_cast<u32>(t) * 7919u + 17u;
                h.sys.emit(h.fluid, j, kFixedDt);
            }
            h.step();
        }
        std::vector<f32> out;
        for (usize i = 0; i < h.fluid.count(); ++i) {
            out.push_back(h.fluid.pos_x[i]);
            out.push_back(h.fluid.pos_y[i]);
            out.push_back(h.fluid.vel_x[i]);
            out.push_back(h.fluid.vel_y[i]);
        }
        return out;
    };
    const std::vector<f32> a = run();
    const std::vector<f32> b = run();
    REQUIRE(a.size() > 0);
    REQUIRE(a == b);
}

// ---------------------------------------------------------------------------
// Cost
// ---------------------------------------------------------------------------

TEST_CASE("solver cost scales with particle count and is independent of chaff count",
          "[fluid][perf]") {
    // The claim from Fluid.h's cost model, measured. The second half matters
    // more than the first: this is the only damage source in the game that
    // could plausibly have ended up testing particle-agent pairs, and if it
    // ever starts, the chaff-count column is where it shows.
    auto measure = [](usize n_fluid, int n_chaff) {
        Harness h;
        Rng r(4242);
        for (int i = 0; i < n_chaff; ++i) {
            ChaffSpawnParams p;
            p.position = Vec2{r.range_f(10.0f, 90.0f), r.range_f(10.0f, 50.0f)};
            p.density = 1.0e9f;   // never dies, so the live set is constant
            h.chaff.spawn(p);
        }
        h.rebuild();

        FluidJetParams j = h.jet(Vec2{50.0f, 30.0f}, Vec2{1.0f, 0.0f}, 91);
        j.lifetime = 1.0e6f;      // never expires, same reason
        const f32 s = h.sys.tuning().rest_spacing;
        // A dense square patch, which is the pessimistic neighbourhood: every
        // particle carries a full neighbour list.
        const i32 side = static_cast<i32>(std::sqrt(static_cast<f32>(n_fluid))) + 1;
        for (i32 row = 0; row < side && h.fluid.count() < n_fluid; ++row) {
            for (i32 col = 0; col < side && h.fluid.count() < n_fluid; ++col) {
                h.fluid.spawn(Vec2{25.0f + static_cast<f32>(col) * s,
                                   15.0f + static_cast<f32>(row) * s * 0.8660254f},
                              Vec2{0.0f, 0.0f}, j);
            }
        }

        f64 best = 1.0e30;
        for (int t = 0; t < 40; ++t) {
            const auto t0 = std::chrono::steady_clock::now();
            h.step();
            const auto t1 = std::chrono::steady_clock::now();
            best = math::min(best, std::chrono::duration<f64, std::milli>(t1 - t0).count());
        }
        REQUIRE(h.fluid.count() == n_fluid);
        return best;
    };

    const f64 a = measure(2000, 1000);
    const f64 b = measure(2000, 10000);
    const f64 c = measure(6000, 1000);
    std::printf("[fluid perf] 2000 particles / 1000 chaff : %.4f ms/tick\n", a);
    std::printf("[fluid perf] 2000 particles / 10000 chaff: %.4f ms/tick\n", b);
    std::printf("[fluid perf] 6000 particles / 1000 chaff : %.4f ms/tick\n", c);

    // Ten times the chaff must not cost meaningfully more. The margin is wide
    // because the extra agents do change cache behaviour in the one pass that
    // sweeps the chaff store -- what is being ruled out is pair testing, which
    // would show up as a multiple, not as a few percent.
    REQUIRE(b < a * 2.0 + 0.05);
    // Three times the fluid should cost roughly three times, not nine: the
    // neighbourhood is bounded by construction, so the walk is linear.
    REQUIRE(c < a * 6.0 + 0.05);
}
