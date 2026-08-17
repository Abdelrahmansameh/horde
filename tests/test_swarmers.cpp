// Coverage for the swarmer layer (sim/swarm/Swarmers.h).
//
// These check the properties the DESIGN rests on rather than exact numbers,
// which are art-direction and balance figures that should stay free to move:
// a granule finds a host, latches, drains it, survives that host dying and
// picks another, dissolves on its own clock, and never lets a stale chaff index
// point it at the wrong agent after a compaction.
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const Rect kBounds{Vec2{0.0f, 0.0f}, Vec2{128.0f, 128.0f}};

struct Fixture {
    ChaffBuffers chaff;
    SwarmerBuffers swarm;
    SwarmerSystem system;
    SpatialHash hash;
    Rng rng{4242};

    Fixture() {
        chaff.reserve(4096);
        swarm.reserve(4096);
        SpatialHashDesc hd;
        hd.bounds = kBounds;
        hd.cell_size = 4.0f;
        hash.configure(hd);
    }

    usize add_chaff(Vec2 p, f32 density) {
        ChaffSpawnParams c;
        c.position = p;
        c.density = density;
        c.family = PathogenFamily::Virus;
        chaff.spawn(c);
        return chaff.count() - 1;
    }

    void add_swarmer(Vec2 p, Vec2 v, f32 lifetime = 3.0f, f32 dps = 20.0f) {
        SwarmerSpawnParams s;
        s.position = p;
        s.velocity = v;
        s.lifetime = lifetime;
        s.damage_per_second = dps;
        s.speed = 14.0f;
        s.attach_radius = 0.55f;
        s.search_radius = 12.0f;
        s.seed = 0x1234u + static_cast<u32>(swarm.count()) * 7919u;
        swarm.spawn(s);
    }

    void step(int ticks, CombatEventSink* sink = nullptr) {
        for (int i = 0; i < ticks; ++i) {
            hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr);
            system.update(swarm, chaff, hash, kBounds, rng, kDt, sink);
            chaff.compact();
        }
    }

    bool any_attached() const {
        for (usize i = 0; i < swarm.count(); ++i) {
            if ((swarm.flags[i] & swarmer_flags::kAttached) != 0) return true;
        }
        return false;
    }
};

} // namespace

TEST_CASE("a granule crosses the gap, latches on, and drains its host",
          "[swarm][sim]") {
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 500.0f);
    f.add_swarmer(Vec2{30.0f, 40.0f}, Vec2{14.0f, 0.0f});
    const f32 before = f.chaff.density[0];

    // It must not reach across the gap instantly. That delay is the entire
    // difference in feel between this and the field it replaced.
    f.step(1);
    REQUIRE(f.chaff.density[0] == before);
    REQUIRE_FALSE(f.any_attached());

    f.step(120);
    REQUIRE(f.any_attached());
    REQUIRE(f.chaff.density[0] < before);
}

TEST_CASE("a granule whose host dies finds another instead of dissolving with it",
          "[swarm][sim]") {
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 6.0f);       // dies quickly
    f.add_chaff(Vec2{43.0f, 40.0f}, 4000.0f);    // and this one is next
    const f32 tough_before = f.chaff.density[1];

    f.add_swarmer(Vec2{38.0f, 40.0f}, Vec2{14.0f, 0.0f}, /*lifetime=*/8.0f);

    f.step(400);

    REQUIRE(f.chaff.count() == 1);                        // the frail one is gone
    REQUIRE(f.chaff.density[0] < tough_before);           // the granule moved on
}

TEST_CASE("a granule holds a handle, so a compaction cannot re-point it at a stranger",
          "[swarm][sim]") {
    // The failure this guards is silent and nasty: chaff compaction swap-removes,
    // so the slot a dead agent occupied gets a live agent moved into it. A
    // granule storing a raw index would keep draining that slot and appear to
    // teleport its damage onto an unrelated pathogen halfway across the lane.
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 6.0f);        // index 0, will die
    f.add_chaff(Vec2{100.0f, 100.0f}, 4000.0f);   // index 1, far away, will move to 0
    const f32 far_before = f.chaff.density[1];

    f.add_swarmer(Vec2{39.0f, 40.0f}, Vec2{6.0f, 0.0f}, /*lifetime=*/6.0f);

    // Long enough to kill the near agent and keep going, but the far agent is
    // outside the granule's search radius, so it must never be touched.
    f.step(300);

    REQUIRE(f.chaff.count() == 1);
    INFO("far agent density " << f.chaff.density[0] << ", was " << far_before);
    REQUIRE(f.chaff.density[0] == far_before);
}

TEST_CASE("granules dissolve on their own lifetime even with a host in reach",
          "[swarm][sim]") {
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 1.0e6f);   // effectively unkillable
    f.add_swarmer(Vec2{39.0f, 40.0f}, Vec2{4.0f, 0.0f}, /*lifetime=*/0.5f);

    REQUIRE(f.swarm.count() == 1);
    f.step(60);                                 // one second
    REQUIRE(f.swarm.count() == 0);
}

TEST_CASE("the store is a hard wall and never grows", "[swarm][sim]") {
    Fixture f;
    for (int i = 0; i < 64; ++i) f.add_swarmer(Vec2{10.0f, 10.0f}, Vec2{1.0f, 0.0f});
    const usize cap = f.swarm.capacity();
    SwarmerBuffers small;
    small.reserve(4);
    for (int i = 0; i < 50; ++i) {
        SwarmerSpawnParams s;
        s.position = Vec2{1.0f, 1.0f};
        s.lifetime = 1.0f;
        small.spawn(s);
    }
    REQUIRE(small.count() == 4);
    REQUIRE(small.capacity() == 4);
    REQUIRE(cap == 4096);
}

TEST_CASE("attaching or detaching an event sink cannot change the simulation",
          "[swarm][sim][determinism]") {
    // The same contract sim/projectile holds: events are an OUTPUT of the tick.
    const auto run = [](bool with_sink) {
        Fixture f;
        for (int i = 0; i < 12; ++i) {
            f.add_chaff(Vec2{40.0f + static_cast<f32>(i) * 0.9f, 40.0f}, 30.0f);
        }
        for (int i = 0; i < 20; ++i) {
            f.add_swarmer(Vec2{30.0f, 39.0f + static_cast<f32>(i) * 0.1f}, Vec2{14.0f, 0.5f});
        }
        CombatEventSink sink;
        sink.reserve(4096);
        f.step(240, with_sink ? &sink : nullptr);

        f32 total = 0.0f;
        for (usize i = 0; i < f.chaff.count(); ++i) total += f.chaff.density[i];
        return std::pair<usize, f32>{f.chaff.count(), total};
    };

    const auto without = run(false);
    const auto with = run(true);
    REQUIRE(without.first == with.first);
    REQUIRE(without.second == with.second);
}

TEST_CASE("the same seed gives the same swarm, every time", "[swarm][sim][determinism]") {
    const auto run = []() {
        Fixture f;
        for (int i = 0; i < 30; ++i) {
            f.add_chaff(Vec2{45.0f + static_cast<f32>(i % 6), 40.0f + static_cast<f32>(i / 6)},
                        25.0f);
        }
        for (int i = 0; i < 40; ++i) {
            f.add_swarmer(Vec2{30.0f, 38.0f + static_cast<f32>(i) * 0.12f}, Vec2{13.0f, 0.3f});
        }
        f.step(300);
        f32 acc = 0.0f;
        for (usize i = 0; i < f.swarm.count(); ++i) acc += f.swarm.pos_x[i] + f.swarm.pos_y[i] * 3.0f;
        return std::pair<usize, f32>{f.swarm.count(), acc};
    };

    const auto a = run();
    const auto b = run();
    REQUIRE(a.first == b.first);
    REQUIRE(a.second == b.second);
}
