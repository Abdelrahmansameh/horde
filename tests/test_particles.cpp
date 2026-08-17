// Coverage for the cosmetic particle layer (vfx/Particles.h).
//
// Wave 6B implemented this module but was cut off before writing tests, so
// this file is the orchestrator's independent verification rather than the
// author's. It deliberately checks the properties the ARCHITECTURE depends on
// -- every event type produces something, capacity is a hard wall, retirement
// actually reclaims slots, and blend modes stay separated -- rather than
// asserting exact particle counts, which are art-direction numbers that should
// be free to change without breaking the build.
#include "sim/CombatEvents.h"
#include "vfx/Particles.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace immune;
using namespace immune::vfx;

namespace {

sim::CombatEvent make_event(sim::CombatEventType type, TowerType source, u16 tier = 1) {
    sim::CombatEvent e;
    e.type = type;
    e.source = source;
    e.visual_id = tier;
    e.origin = Vec2{10.0f, 10.0f};
    e.secondary = Vec2{18.0f, 14.0f};
    e.direction = Vec2{1.0f, 0.0f};
    e.radius = 4.0f;
    e.arc_radians = 0.5f;
    e.magnitude = 1.0f;
    e.target_family = PathogenFamily::Virus;
    return e;
}

} // namespace

TEST_CASE("every combat event type emits particles", "[vfx][particles]") {
    // A regression guard: if someone adds a CombatEventType and forgets to
    // handle it in emit_for_event(), that tower's effect silently renders
    // nothing at all. Catching it here is much cheaper than noticing in game.
    for (u8 t = 0; t < static_cast<u8>(sim::CombatEventType::Count); ++t) {
        ParticleSystem ps;
        ps.init(4096, 12345);
        const auto type = static_cast<sim::CombatEventType>(t);
        ps.emit_for_event(make_event(type, TowerType::Neutrophil));
        INFO("CombatEventType index " << static_cast<int>(t) << " emitted nothing");
        REQUIRE(ps.live_count() > 0);
    }
}

TEST_CASE("each tower source produces particles for its own signature event",
          "[vfx][particles]") {
    // Pairs each tower with the event its combat system actually raises, so a
    // tower whose look was never authored shows up as a failure here.
    const std::pair<TowerType, sim::CombatEventType> cases[] = {
        {TowerType::Neutrophil, sim::CombatEventType::MuzzleFlash},
        {TowerType::Macrophage, sim::CombatEventType::Explosion},
        {TowerType::Interferon, sim::CombatEventType::ConePulse},
        {TowerType::CytotoxicT, sim::CombatEventType::ChainArc},
        {TowerType::BCell,      sim::CombatEventType::BeamFired},
        {TowerType::NKCell,     sim::CombatEventType::BladeSlash},
    };
    for (const auto& [tower, event] : cases) {
        ParticleSystem ps;
        ps.init(4096, 999);
        ps.emit_for_event(make_event(event, tower));
        INFO("tower index " << static_cast<int>(tower) << " emitted nothing");
        REQUIRE(ps.live_count() > 0);
    }
}

TEST_CASE("capacity is a hard wall and overflow is counted, not crashed",
          "[vfx][particles]") {
    // The whole layer's safety story: dropping a spark is invisible, growing a
    // buffer mid-frame is forbidden. Prove it drops rather than reallocates.
    ParticleSystem ps;
    ps.init(64, 7);
    for (int i = 0; i < 500; ++i) {
        ps.emit_for_event(make_event(sim::CombatEventType::Explosion, TowerType::Macrophage, 5));
    }
    REQUIRE(ps.live_count() <= ps.capacity());
    REQUIRE(ps.capacity() == 64);
    REQUIRE(ps.stats().dropped_total > 0);
}

TEST_CASE("particles retire and free their slots", "[vfx][particles]") {
    ParticleSystem ps;
    ps.init(4096, 4242);
    ps.emit_for_event(make_event(sim::CombatEventType::ProjectileImpact, TowerType::Neutrophil));
    REQUIRE(ps.live_count() > 0);

    // Well past any plausible authored lifetime. Stepped rather than jumped so
    // negative-age (delayed) particles get to live their whole life first --
    // one huge dt would retire them before they were ever visible.
    for (int i = 0; i < 400; ++i) ps.update(0.05f, nullptr);
    REQUIRE(ps.live_count() == 0);
}

TEST_CASE("build_instances returns only the requested blend mode", "[vfx][particles]") {
    ParticleSystem ps;
    ps.init(8192, 55);
    // Mist is the alpha-blended kind; explosions author both it and additive
    // sparks, so this event exercises the split.
    for (int i = 0; i < 20; ++i) {
        ps.emit_for_event(make_event(sim::CombatEventType::Explosion, TowerType::Macrophage, 3));
    }
    ps.update(0.016f, nullptr);

    std::vector<ParticleInstance> additive, alpha;
    ps.build_instances(BlendMode::Additive, additive);
    ps.build_instances(BlendMode::AlphaBlend, alpha);

    // Neither list may contain an instance tagged with the other mode. The
    // blend mode lives in the high 16 bits of kind_blend.
    for (const auto& inst : additive) {
        REQUIRE((inst.kind_blend >> 16) == static_cast<u32>(BlendMode::Additive));
    }
    for (const auto& inst : alpha) {
        REQUIRE((inst.kind_blend >> 16) == static_cast<u32>(BlendMode::AlphaBlend));
    }
    REQUIRE(additive.size() + alpha.size() <= ps.live_count());
    REQUIRE(!additive.empty());
}

TEST_CASE("clear retires everything", "[vfx][particles]") {
    ParticleSystem ps;
    ps.init(1024, 3);
    ps.emit_for_event(make_event(sim::CombatEventType::BeamFired, TowerType::BCell, 4));
    REQUIRE(ps.live_count() > 0);
    ps.clear();
    REQUIRE(ps.live_count() == 0);
}

TEST_CASE("emit_for_events drains a whole span", "[vfx][particles]") {
    ParticleSystem ps;
    ps.init(16384, 88);
    std::vector<sim::CombatEvent> batch;
    for (int i = 0; i < 12; ++i) {
        batch.push_back(make_event(sim::CombatEventType::MuzzleFlash, TowerType::Neutrophil));
    }
    ps.emit_for_events(batch.data(), batch.size());
    REQUIRE(ps.live_count() > 0);
    REQUIRE(ps.stats().spawned_this_frame > 0);
}

// ---------------------------------------------------------------------------
// Cytotoxic T: the travelling-granule chain.
//
// The look these guard is "a lytic granule physically flies target to target",
// which has two properties a screenshot cannot check and a refactor can very
// easily break in silence:
//
//   - later hops are STAGED, not simultaneous. The sim raises every hop of a
//     chain in one tick, so the walk exists only because this layer delays hop
//     k by k * kCtlHopSeconds, and it recovers k by inverting the falloff the
//     sim baked into CombatEvent::magnitude. Get that inversion wrong and the
//     chain silently collapses back into an instantaneous flash.
//   - the granule ARRIVES. Staged particles keep integrating while their age
//     is negative, so a moving one has to be born behind its start point to
//     land in the right place. Drop that compensation and the payload departs
//     from somewhere it was never fired from.
// ---------------------------------------------------------------------------

namespace {

// Mirrors kCtlHopFalloff / kTeslaHopFalloff. A hop's magnitude is falloff^k.
constexpr f32 kHopFalloff = 0.72f;

sim::CombatEvent make_chain_hop(u32 hop, Vec2 from, Vec2 to) {
    sim::CombatEvent e = make_event(sim::CombatEventType::ChainArc, TowerType::CytotoxicT, 1);
    e.origin = from;
    e.secondary = to;
    e.direction = Vec2{1.0f, 0.0f};
    f32 weight = 1.0f;
    for (u32 k = 0; k < hop; ++k) weight *= kHopFalloff;
    e.magnitude = weight;
    return e;
}

usize visible_count(const ParticleSystem& ps) {
    std::vector<ParticleInstance> out;
    usize n = 0;
    ps.build_instances(BlendMode::Additive, out);
    n += out.size();
    ps.build_instances(BlendMode::AlphaBlend, out);
    n += out.size();
    return n;
}

} // namespace

TEST_CASE("a later chain hop is staged behind the earlier ones, not fired with them",
          "[vfx][particles][cytotoxic]") {
    // Hop 0 leaves immediately; hop 3 must still be entirely invisible at the
    // instant of emission, because the granule has three earlier hops to fly
    // first. Both events arrive in the SAME tick — the stagger is this layer's
    // work alone.
    ParticleSystem first;
    first.init(4096, 4242);
    first.emit_for_event(make_chain_hop(0, Vec2{0.0f, 0.0f}, Vec2{4.0f, 0.0f}));
    REQUIRE(visible_count(first) > 0);

    ParticleSystem later;
    later.init(4096, 4242);
    later.emit_for_event(make_chain_hop(3, Vec2{0.0f, 0.0f}, Vec2{4.0f, 0.0f}));
    REQUIRE(later.live_count() > 0);          // it was emitted...
    REQUIRE(visible_count(later) == 0);       // ...but nothing is on screen yet.

    // Advance past hop 3's start (3 * 45 ms) and it appears.
    later.update(0.15f, nullptr);
    REQUIRE(visible_count(later) > 0);
}

TEST_CASE("the chain resolves quickly: every hop has landed well inside a quarter second",
          "[vfx][particles][cytotoxic]") {
    // The brief is "not instant, but very very quick". Guard the upper end:
    // the last hop of a maximum-length chain must have started flying long
    // before the player would read the tower as firing a slow projectile.
    ParticleSystem ps;
    ps.init(8192, 77);
    ps.emit_for_event(make_chain_hop(5, Vec2{0.0f, 0.0f}, Vec2{4.0f, 0.0f}));
    REQUIRE(visible_count(ps) == 0);
    ps.update(0.25f, nullptr);
    REQUIRE(visible_count(ps) > 0);
}

TEST_CASE("the granule arrives at the target it was fired at",
          "[vfx][particles][cytotoxic]") {
    // Hop 1, so the spawn is staged and the pre-birth-drift compensation is
    // actually exercised: a particle born at `start` instead of
    // `start - velocity * delay` would sail straight past the target.
    const Vec2 from{0.0f, 0.0f};
    const Vec2 to{6.0f, 0.0f};

    ParticleSystem ps;
    ps.init(4096, 31337);
    ps.emit_for_event(make_chain_hop(1, from, to));

    // One hop's delay (0.045) plus one hop's flight (0.045). Stepped in small
    // slices because the integrator is Euler and one giant step is not the
    // same path the render loop takes.
    for (int i = 0; i < 18; ++i) ps.update(0.005f, nullptr);

    std::vector<ParticleInstance> out;
    ps.build_instances(BlendMode::Additive, out);
    REQUIRE_FALSE(out.empty());

    f32 nearest = 1e9f;
    for (const ParticleInstance& p : out) {
        const f32 dx = p.x - to.x;
        const f32 dy = p.y - to.y;
        nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
    }
    INFO("nearest additive particle sat " << nearest << " units from the target");
    REQUIRE(nearest < 1.0f);
}
