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
