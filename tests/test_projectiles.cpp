// Guards the contract in sim/projectile/Projectiles.h: the SoA invariants
// P1-P3, the deliberately-approximate cell-local collision rule, retirement,
// and the one property the whole CombatEvents design rests on -- that attaching
// an event sink cannot change the simulation by one bit.
#include "sim/projectile/Projectiles.h"

#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

/// A minimal stand-in for the parts of SimWorld a projectile tick touches:
/// a chaff store, a spatial hash rebuilt before each step, and the store of
/// rounds. Deliberately does NOT compact chaff -- that is the caller's job,
/// once, after every damage source has run (Projectiles.h).
struct Harness {
    ChaffBuffers chaff;
    ProjectileBuffers rounds;
    SpatialHash hash;
    TissueMask tissue;
    ProjectileSystem sys;
    Rng rng{0xC0FFEEu};
    Rect bounds{Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}};

    Harness(usize max_chaff = 1024, usize max_rounds = 1024, f32 cell = 4.0f) {
        chaff.reserve(max_chaff);
        rounds.reserve(max_rounds);
        SpatialHashDesc d;
        d.bounds = bounds;
        d.cell_size = cell;
        hash.configure(d);
    }

    usize add_chaff(f32 x, f32 y, f32 density = 5.0f,
                    PathogenFamily fam = PathogenFamily::Virus) {
        ChaffSpawnParams p;
        p.position = Vec2{x, y};
        p.density = density;
        p.family = fam;
        const usize idx = chaff.count();
        REQUIRE(chaff.spawn(p).valid());
        return idx;
    }

    bool add_round(Vec2 pos, Vec2 vel, f32 damage = 2.0f, f32 lifetime = 1.0f,
                   f32 hit_radius = 0.5f, u8 flags = 0, u8 mask = 0xFF) {
        ProjectileSpawnParams p;
        p.position = pos;
        p.velocity = vel;
        p.damage = damage;
        p.lifetime = lifetime;
        p.hit_radius = hit_radius;
        p.flags = flags;
        p.family_mask = mask;
        p.visual_id = 7;
        return rounds.spawn(p);
    }

    ProjectileStats step(CombatEventSink* sink = nullptr, f32 dt = kFixedDt) {
        hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr);
        return sys.update(rounds, chaff, hash, tissue, bounds, rng, dt, sink);
    }

    void add_vertical_wall(i32 wall_x) {
        tissue.resize(100, 100, 1.0f, Vec2{0.0f, 0.0f});
        for (i32 y = 0; y < tissue.height(); ++y) {
            for (i32 x = 0; x < tissue.width(); ++x) tissue.set_walkable(x, y, true);
            tissue.set_walkable(wall_x, y, false);
        }
    }
};

u8 family_bit(PathogenFamily f) { return static_cast<u8>(1u << static_cast<u32>(f)); }

} // namespace

// ---------------------------------------------------------------------------
// Store invariants
// ---------------------------------------------------------------------------

TEST_CASE("projectile streams stay parallel and sized to capacity",
          "[sim][projectile][soa]") {
    ProjectileBuffers b;
    b.reserve(256);
    REQUIRE(b.capacity() == 256);
    REQUIRE(b.count() == 0);
    REQUIRE(b.pos_x.size() == 256);
    REQUIRE(b.pos_y.size() == b.pos_x.size());
    REQUIRE(b.vel_x.size() == b.pos_x.size());
    REQUIRE(b.vel_y.size() == b.pos_x.size());
    REQUIRE(b.damage.size() == b.pos_x.size());
    REQUIRE(b.life.size() == b.pos_x.size());
    REQUIRE(b.hit_radius.size() == b.pos_x.size());
    REQUIRE(b.family_mask.size() == b.pos_x.size());
    REQUIRE(b.flags.size() == b.pos_x.size());
    REQUIRE(b.visual_id.size() == b.pos_x.size());
    REQUIRE(b.owner.size() == b.pos_x.size());
}

TEST_CASE("spawn at capacity returns false and never grows a stream",
          "[sim][projectile][soa]") {
    ProjectileBuffers b;
    b.reserve(4);
    const f32* base = b.pos_x.data();
    const usize cap = b.capacity();

    ProjectileSpawnParams p;
    p.velocity = Vec2{1.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        p.position = Vec2{static_cast<f32>(i), 0.0f};
        REQUIRE(b.spawn(p));
    }
    REQUIRE(b.count() == 4);
    REQUIRE(b.full());

    REQUIRE_FALSE(b.spawn(p));            // dropping a round is the contract
    REQUIRE(b.count() == 4);              // P3
    REQUIRE(b.capacity() == cap);
    REQUIRE(b.pos_x.data() == base);      // no reallocation
    REQUIRE(b.pos_x.size() == cap);       // P2

    for (usize i = 0; i < b.count(); ++i) {
        REQUIRE((b.flags[i] & projectile_flags::kAlive) != 0);   // P1
    }
}

// ---------------------------------------------------------------------------
// Travel and collision
// ---------------------------------------------------------------------------

TEST_CASE("a round travels by velocity * dt each tick", "[sim][projectile]") {
    Harness h;
    h.add_round(Vec2{10.0f, 10.0f}, Vec2{60.0f, 0.0f});   // 1 world unit / tick
    h.step();
    REQUIRE(h.rounds.pos_x[0] == Catch::Approx(11.0f));
    REQUIRE(h.rounds.pos_y[0] == Catch::Approx(10.0f));
    h.step();
    REQUIRE(h.rounds.pos_x[0] == Catch::Approx(12.0f));
    REQUIRE(h.rounds.life[0] == Catch::Approx(1.0f - 2.0f * kFixedDt));
}

TEST_CASE("a round hits an agent in its own cell and damage lands via density loss",
          "[sim][projectile][damage]") {
    Harness h;
    h.add_chaff(50.0f, 50.0f, 5.0f);
    // Starts 2 units short; 1 unit per tick, so it reaches the agent on tick 2.
    h.add_round(Vec2{48.0f, 50.0f}, Vec2{60.0f, 0.0f}, /*damage*/ 2.0f);

    ProjectileStats s = h.step();
    REQUIRE(s.impacts == 0);
    REQUIRE(h.chaff.density[0] == Catch::Approx(5.0f));
    REQUIRE(s.live == 1);

    s = h.step();
    REQUIRE(s.impacts == 1);
    REQUIRE(s.density_removed == Catch::Approx(2.0f));
    REQUIRE(h.chaff.density[0] == Catch::Approx(3.0f));
    REQUIRE((h.chaff.flags[0] & chaff_flags::kPendingKill) == 0);
    REQUIRE(s.live == 0);                    // spent and retired
    REQUIRE(h.rounds.count() == 0);
}

TEST_CASE("a round hits harder on chaff the Goblet Cell has marked",
          "[sim][projectile][damage][marked]") {
    // The mirror of test_damage_field.cpp's "kMarked agents take the
    // marked_multiplier bonus": a round never touches a DamageField, it tests
    // its own spatial-hash cell directly, so the weaken bonus has to be
    // re-applied right here in Projectiles.cpp or a marked agent downrange of
    // a Gunner would be exactly as tanky as an unmarked one.
    Harness h;
    const usize plain = h.add_chaff(50.0f, 50.0f, 1000.0f);
    const usize marked = h.add_chaff(60.0f, 50.0f, 1000.0f);
    h.chaff.flags[marked] |= chaff_flags::kMarked;

    // Same 2-units-short setup as the plain hit test above, so both rounds
    // connect on the second step.
    h.add_round(Vec2{48.0f, 50.0f}, Vec2{60.0f, 0.0f}, /*damage*/ 10.0f);
    h.add_round(Vec2{58.0f, 50.0f}, Vec2{60.0f, 0.0f}, /*damage*/ 10.0f);

    h.step();
    const ProjectileStats s = h.step();
    REQUIRE(s.impacts == 2);

    const f32 plain_loss = 1000.0f - h.chaff.density[plain];
    const f32 marked_loss = 1000.0f - h.chaff.density[marked];
    REQUIRE(plain_loss == Catch::Approx(10.0f));
    REQUIRE(marked_loss == Catch::Approx(plain_loss * chaff_flags::kMarkedDamageMultiplier));
}

TEST_CASE("a round in a different cell than the agent does not hit it",
          "[sim][projectile]") {
    // The approximation under test: collision is cell-local by design. These
    // two points are 1.1 units apart but land in different 4-unit cells, so no
    // hit may occur even though hit_radius is 2.0. If this test starts failing
    // because someone widened the query to a 3x3 neighbourhood, that is a
    // contract change, not a fix.
    Harness h;
    h.add_chaff(52.5f, 50.0f, 5.0f);              // cell x = 13
    h.add_round(Vec2{51.4f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, /*hit_radius*/ 2.0f);
    // Integrating with zero velocity leaves it at x = 51.4 -> cell x = 12.
    const ProjectileStats s = h.step();
    REQUIRE(s.impacts == 0);
    REQUIRE(h.chaff.density[0] == Catch::Approx(5.0f));
}

TEST_CASE("a round collides with a wall, emits its impact, and is destroyed",
          "[sim][projectile][walls][events]") {
    Harness h;
    h.add_vertical_wall(50);
    h.add_chaff(51.5f, 50.5f, 5.0f);
    // Travels two units this tick and would tunnel through the one-cell wall
    // if collision only sampled the endpoint.
    h.add_round(Vec2{49.5f, 50.5f}, Vec2{120.0f, 0.0f}, 2.0f, 1.0f, 0.5f);

    CombatEventSink sink;
    sink.reserve(8);
    const ProjectileStats s = h.step(&sink);

    REQUIRE(s.wall_impacts == 1);
    REQUIRE(s.impacts == 0);
    REQUIRE(s.live == 0);
    REQUIRE(h.rounds.count() == 0);
    REQUIRE(h.chaff.density[0] == Catch::Approx(5.0f));
    REQUIRE(sink.size() == 1);
    const CombatEvent& e = sink.events()[0];
    REQUIRE(e.type == CombatEventType::ProjectileImpact);
    REQUIRE(e.source == TowerType::Neutrophil);
    REQUIRE(e.target_family == PathogenFamily::Count);
    REQUIRE(e.origin.x == Catch::Approx(50.0f));
    REQUIRE(e.origin.y == Catch::Approx(50.5f));
}

TEST_CASE("a round flies through a runtime block and hits what stands behind it",
          "[sim][projectile][walls]") {
    // The same one-cell wall, but laid by the player at runtime -- a Fibrin
    // Clot, a Fibroblast's scar (sim/flowfield/RuntimeBlock.h) -- which the
    // mask flags as a runtime block. Walls to the horde, not to its own
    // side's rounds: the round crosses it and lands on the agent beyond.
    Harness h;
    h.add_vertical_wall(50);
    for (i32 y = 0; y < h.tissue.height(); ++y) h.tissue.set_runtime_block(50, y, true);
    REQUIRE_FALSE(h.tissue.walkable(50, 50));
    REQUIRE_FALSE(h.tissue.authored_wall(50, 50));
    REQUIRE(h.tissue.authored_wall(-1, 50));   // off the grid is still a wall
    h.add_chaff(51.5f, 50.5f, 5.0f);
    h.add_round(Vec2{49.5f, 50.5f}, Vec2{120.0f, 0.0f}, 2.0f, 1.0f, 0.5f);

    CombatEventSink sink;
    sink.reserve(8);
    const ProjectileStats s = h.step(&sink);

    REQUIRE(s.wall_impacts == 0);
    REQUIRE(s.impacts == 1);
    REQUIRE(h.chaff.density[0] == Catch::Approx(3.0f));

    // Handed back to the tissue, the cell is neither.
    h.tissue.set_walkable(50, 50, true);
    h.tissue.set_runtime_block(50, 50, false);
    REQUIRE_FALSE(h.tissue.authored_wall(50, 50));
}

TEST_CASE("hit_radius is an exact test inside the cell", "[sim][projectile]") {
    Harness h;
    h.add_chaff(50.9f, 50.0f, 5.0f);          // same cell (12) as the round
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, /*hit_radius*/ 0.5f);
    REQUIRE(h.step().impacts == 0);           // 0.9 apart, radius 0.5 -> miss
    REQUIRE(h.chaff.density[0] == Catch::Approx(5.0f));

    Harness h2;
    h2.add_chaff(50.4f, 50.0f, 5.0f);
    h2.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, 0.5f);
    REQUIRE(h2.step().impacts == 1);          // 0.4 apart -> hit
    REQUIRE(h2.chaff.density[0] == Catch::Approx(3.0f));
}

TEST_CASE("family_mask gates which agents a round may damage",
          "[sim][projectile][damage]") {
    Harness h;
    h.add_chaff(50.0f, 50.0f, 5.0f, PathogenFamily::Virus);
    h.add_chaff(50.2f, 50.0f, 5.0f, PathogenFamily::Bacteria);
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, 0.5f, 0,
                family_bit(PathogenFamily::Bacteria));

    const ProjectileStats s = h.step();
    REQUIRE(s.impacts == 1);
    REQUIRE(h.chaff.density[0] == Catch::Approx(5.0f));   // virus untouched
    REQUIRE(h.chaff.density[1] == Catch::Approx(3.0f));   // bacteria hit
}

TEST_CASE("a round damages at most one agent per tick", "[sim][projectile][damage]") {
    Harness h;
    for (int i = 0; i < 5; ++i) h.add_chaff(50.0f + 0.05f * static_cast<f32>(i), 50.0f, 50.0f);
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, 0.5f,
                projectile_flags::kPiercing);

    const ProjectileStats s = h.step();
    REQUIRE(s.impacts == 1);
    REQUIRE(s.density_removed == Catch::Approx(2.0f));
    int damaged = 0;
    for (usize i = 0; i < h.chaff.count(); ++i) {
        if (h.chaff.density[i] < 50.0f) ++damaged;
    }
    REQUIRE(damaged == 1);
}

TEST_CASE("a piercing round survives its first hit and a plain one does not",
          "[sim][projectile]") {
    Harness plain;
    plain.add_chaff(50.0f, 50.0f, 50.0f);
    plain.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, 0.5f, 0);
    REQUIRE(plain.step().impacts == 1);
    REQUIRE(plain.rounds.count() == 0);

    Harness pierce;
    pierce.add_chaff(50.0f, 50.0f, 50.0f);
    pierce.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, 0.5f,
                     projectile_flags::kPiercing);
    ProjectileStats s = pierce.step();
    REQUIRE(s.impacts == 1);
    REQUIRE(s.live == 1);
    REQUIRE(pierce.rounds.count() == 1);
    REQUIRE((pierce.rounds.flags[0] & projectile_flags::kPiercing) != 0);

    s = pierce.step();                       // still flying, still damaging
    REQUIRE(s.impacts == 1);
    REQUIRE(pierce.chaff.density[0] == Catch::Approx(46.0f));
}

TEST_CASE("a round skips agents already dying this tick", "[sim][projectile][damage]") {
    // Chaff compaction is deferred to the caller, so a killed agent is still in
    // the array (and still in the hash) when the next round arrives. Spending a
    // round on it would visibly fail to kill anything.
    Harness h;
    h.add_chaff(50.0f, 50.0f, 2.0f);          // dies to one round
    h.add_chaff(50.1f, 50.0f, 9.0f);
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, 0.5f,
                projectile_flags::kPiercing);

    ProjectileStats s = h.step();
    REQUIRE(s.impacts == 1);
    REQUIRE((h.chaff.flags[0] & chaff_flags::kPendingKill) != 0);

    s = h.step();                             // agent 0 is a corpse now
    REQUIRE(s.impacts == 1);
    REQUIRE(h.chaff.density[1] == Catch::Approx(7.0f));
}

// ---------------------------------------------------------------------------
// Retirement
// ---------------------------------------------------------------------------

TEST_CASE("a round expires when its lifetime runs out", "[sim][projectile]") {
    Harness h;
    // Just under 3 ticks of life, no chaff to hit. The half-tick offset keeps
    // the expiry off an exact float boundary: `3 * kFixedDt` minus kFixedDt
    // three times lands a hair ABOVE zero and the round survives a 4th tick.
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{6.0f, 0.0f}, 2.0f, /*lifetime*/ 2.5f * kFixedDt);
    REQUIRE(h.step().live == 1);
    REQUIRE(h.step().live == 1);
    const ProjectileStats s = h.step();
    REQUIRE(s.expired == 1);
    REQUIRE(s.live == 0);
    REQUIRE(h.rounds.count() == 0);
}

TEST_CASE("a round expires when it leaves the world bounds", "[sim][projectile]") {
    Harness h;
    // 1 unit/tick, starting 1.5 units inside the x = 100 wall and with plenty
    // of lifetime left: it must be retired by the bounds test, not the timeout.
    // The half-unit offset keeps it clear of the boundary in both directions so
    // f32 rounding on the step cannot decide the outcome.
    h.add_round(Vec2{98.5f, 50.0f}, Vec2{60.0f, 0.0f}, 2.0f, /*lifetime*/ 10.0f);
    REQUIRE(h.step().live == 1);              // 99.5, still inside
    const ProjectileStats s = h.step();       // 100.5, gone
    REQUIRE(s.expired == 1);
    REQUIRE(s.live == 0);
    REQUIRE(h.rounds.count() == 0);
}

TEST_CASE("an out-of-bounds round cannot damage anything on its way out",
          "[sim][projectile]") {
    Harness h;
    h.add_chaff(101.0f, 50.0f, 5.0f);        // outside the box; hash clamps it
                                             // into the edge cell
    h.add_round(Vec2{100.5f, 50.0f}, Vec2{0.0f, 0.0f}, 2.0f, 1.0f, 2.0f);
    const ProjectileStats s = h.step();
    REQUIRE(s.impacts == 0);
    REQUIRE(s.expired == 1);
    REQUIRE(h.chaff.density[0] == Catch::Approx(5.0f));
}

TEST_CASE("stats report live, spawned, impacts, expired and density removed",
          "[sim][projectile]") {
    Harness h;
    h.add_chaff(50.0f, 50.0f, 100.0f);
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 3.0f, 1.0f, 0.5f);   // hits
    h.add_round(Vec2{10.0f, 10.0f}, Vec2{0.0f, 0.0f}, 3.0f, 0.5f * kFixedDt); // expires
    h.add_round(Vec2{20.0f, 20.0f}, Vec2{0.0f, 0.0f}, 3.0f, 5.0f);         // survives

    const ProjectileStats s = h.step();
    REQUIRE(s.spawned_this_tick == 3);
    REQUIRE(s.impacts == 1);
    REQUIRE(s.expired == 1);
    REQUIRE(s.live == 1);
    REQUIRE(s.density_removed == Catch::Approx(3.0f));
    REQUIRE(h.sys.last_stats().live == 1);

    const ProjectileStats s2 = h.step();
    REQUIRE(s2.spawned_this_tick == 0);
}

TEST_CASE("density removed is the amount actually taken, not the amount requested",
          "[sim][projectile][damage]") {
    Harness h;
    h.add_chaff(50.0f, 50.0f, 1.0f);          // only 1 density available
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, /*damage*/ 10.0f, 1.0f, 0.5f);
    const ProjectileStats s = h.step();
    REQUIRE(s.impacts == 1);
    REQUIRE(s.density_removed == Catch::Approx(1.0f));
    REQUIRE(h.chaff.density[0] == Catch::Approx(0.0f));
    REQUIRE((h.chaff.flags[0] & chaff_flags::kPendingKill) != 0);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

TEST_CASE("impacts and expiries are reported to an attached sink",
          "[sim][projectile][events]") {
    Harness h;
    h.add_chaff(50.0f, 50.0f, 100.0f, PathogenFamily::Bacteria);
    h.add_round(Vec2{50.0f, 50.0f}, Vec2{0.0f, 0.0f}, 4.0f, 1.0f, 0.5f);
    h.add_round(Vec2{10.0f, 10.0f}, Vec2{60.0f, 0.0f}, 4.0f, kFixedDt);

    CombatEventSink sink;
    sink.reserve(64);
    h.step(&sink);

    REQUIRE(sink.size() == 2);
    REQUIRE(sink.dropped() == 0);

    bool saw_impact = false, saw_expiry = false;
    for (const CombatEvent& e : sink.events()) {
        if (e.type == CombatEventType::ProjectileImpact) {
            saw_impact = true;
            REQUIRE(e.source == TowerType::Neutrophil);
            REQUIRE(e.target_family == PathogenFamily::Bacteria);
            REQUIRE(e.magnitude == Catch::Approx(4.0f));
            REQUIRE(e.origin.x == Catch::Approx(50.0f));
            REQUIRE(e.origin.y == Catch::Approx(50.0f));
            REQUIRE(e.visual_id == 7);
        } else if (e.type == CombatEventType::ProjectileExpired) {
            saw_expiry = true;
            REQUIRE(e.direction.x == Catch::Approx(1.0f));   // normalized travel
            REQUIRE(e.direction.y == Catch::Approx(0.0f));
        }
    }
    REQUIRE(saw_impact);
    REQUIRE(saw_expiry);
}

TEST_CASE("a full event sink drops events instead of allocating",
          "[sim][projectile][events]") {
    Harness h(64, 64);
    for (int i = 0; i < 20; ++i) {
        h.add_round(Vec2{10.0f + static_cast<f32>(i), 10.0f}, Vec2{0.0f, 0.0f}, 1.0f,
                    0.5f * kFixedDt);
    }
    CombatEventSink sink;
    sink.reserve(5);
    const ProjectileStats s = h.step(&sink);
    REQUIRE(s.expired == 20);
    REQUIRE(sink.size() == 5);
    REQUIRE(sink.dropped() == 15);
    REQUIRE(sink.capacity() == 5);
}

TEST_CASE("attaching an event sink does not perturb the simulation",
          "[sim][projectile][events][determinism]") {
    // The load-bearing property of the whole CombatEvents design: events are an
    // OUTPUT of the tick, never an input. Two identical runs, one with a sink
    // and one with nullptr, must leave byte-identical chaff and round state.
    auto run = [](bool with_sink, std::vector<f32>& out_density,
                  std::vector<f32>& out_pos_x, std::vector<u8>& out_flags,
                  std::vector<f32>& out_round_x, ProjectileStats& out_stats) {
        Harness h(4096, 4096);
        Rng seed_rng(20260806u);
        for (int i = 0; i < 600; ++i) {
            h.add_chaff(seed_rng.range_f(20.0f, 80.0f), seed_rng.range_f(20.0f, 80.0f),
                        seed_rng.range_f(1.0f, 6.0f),
                        static_cast<PathogenFamily>(seed_rng.next_below(kFamilyCount)));
        }
        for (int i = 0; i < 400; ++i) {
            h.add_round(Vec2{seed_rng.range_f(20.0f, 80.0f), seed_rng.range_f(20.0f, 80.0f)},
                        Vec2{seed_rng.range_f(-40.0f, 40.0f), seed_rng.range_f(-40.0f, 40.0f)},
                        seed_rng.range_f(0.5f, 3.0f), seed_rng.range_f(0.2f, 1.5f), 0.6f,
                        seed_rng.chance(0.25f) ? projectile_flags::kPiercing : u8{0});
        }

        CombatEventSink sink;
        sink.reserve(4096);
        ProjectileStats last{};
        for (int tick = 0; tick < 120; ++tick) {
            last = h.step(with_sink ? &sink : nullptr);
            sink.clear();
            // Mirror SimWorld's tick order: the caller compacts chaff once,
            // after every damage source has run.
            h.chaff.compact();
            // Keep the round population up, exactly as a firing Gunner would.
            for (int s = 0; s < 3; ++s) {
                h.add_round(Vec2{seed_rng.range_f(20.0f, 80.0f), seed_rng.range_f(20.0f, 80.0f)},
                            Vec2{seed_rng.range_f(-40.0f, 40.0f), seed_rng.range_f(-40.0f, 40.0f)},
                            1.0f, 0.8f, 0.6f);
            }
        }

        const auto n_chaff = static_cast<std::ptrdiff_t>(h.chaff.count());
        const auto n_rounds = static_cast<std::ptrdiff_t>(h.rounds.count());
        out_density.assign(h.chaff.density.begin(), h.chaff.density.begin() + n_chaff);
        out_pos_x.assign(h.chaff.pos_x.begin(), h.chaff.pos_x.begin() + n_chaff);
        out_flags.assign(h.chaff.flags.begin(), h.chaff.flags.begin() + n_chaff);
        out_round_x.assign(h.rounds.pos_x.begin(), h.rounds.pos_x.begin() + n_rounds);
        out_stats = last;
    };

    std::vector<f32> d_a, d_b, x_a, x_b, r_a, r_b;
    std::vector<u8> f_a, f_b;
    ProjectileStats s_a{}, s_b{};
    run(true, d_a, x_a, f_a, r_a, s_a);
    run(false, d_b, x_b, f_b, r_b, s_b);

    REQUIRE(d_a.size() == d_b.size());
    REQUIRE(r_a.size() == r_b.size());
    REQUIRE(d_a.size() > 0);        // the scenario must actually do something
    for (usize i = 0; i < d_a.size(); ++i) {
        REQUIRE(d_a[i] == d_b[i]);  // exact, not approximate
        REQUIRE(x_a[i] == x_b[i]);
        REQUIRE(f_a[i] == f_b[i]);
    }
    for (usize i = 0; i < r_a.size(); ++i) REQUIRE(r_a[i] == r_b[i]);
    REQUIRE(s_a.live == s_b.live);
    REQUIRE(s_a.impacts == s_b.impacts);
    REQUIRE(s_a.expired == s_b.expired);
    REQUIRE(s_a.density_removed == s_b.density_removed);
}

TEST_CASE("update is a pure function of its inputs across repeated runs",
          "[sim][projectile][determinism]") {
    auto run = [](u64 seed) {
        Harness h(2048, 2048);
        Rng r(seed);
        for (int i = 0; i < 300; ++i) {
            h.add_chaff(r.range_f(10.0f, 90.0f), r.range_f(10.0f, 90.0f), 4.0f);
        }
        for (int i = 0; i < 200; ++i) {
            h.add_round(Vec2{r.range_f(10.0f, 90.0f), r.range_f(10.0f, 90.0f)},
                        Vec2{r.range_f(-50.0f, 50.0f), r.range_f(-50.0f, 50.0f)}, 1.0f,
                        1.0f, 0.7f);
        }
        f32 total = 0.0f;
        for (int t = 0; t < 60; ++t) {
            const ProjectileStats s = h.step();
            total += s.density_removed + static_cast<f32>(s.impacts);
            h.chaff.compact();
        }
        return total;
    };
    REQUIRE(run(11u) == run(11u));
}

// ---------------------------------------------------------------------------
// Churn
// ---------------------------------------------------------------------------

TEST_CASE("heavy spawn/hit/compact churn preserves P1-P3",
          "[sim][projectile][soa][invariants]") {
    Harness h(2000, 512);
    Rng rng(9001);

    for (int round = 0; round < 400; ++round) {
        const int to_spawn = static_cast<int>(rng.next_below(12));
        for (int s = 0; s < to_spawn; ++s) {
            const bool ok =
                h.add_round(Vec2{rng.range_f(0.0f, 100.0f), rng.range_f(0.0f, 100.0f)},
                            Vec2{rng.range_f(-80.0f, 80.0f), rng.range_f(-80.0f, 80.0f)},
                            rng.range_f(0.5f, 3.0f), rng.range_f(0.05f, 1.0f),
                            rng.range_f(0.2f, 1.5f),
                            rng.chance(0.2f) ? projectile_flags::kPiercing : u8{0},
                            static_cast<u8>(rng.next_below(256)));
            // The only legal failure is capacity, and it must not have grown.
            if (!ok) {
                REQUIRE(h.rounds.full());
                REQUIRE(h.rounds.pos_x.size() == h.rounds.capacity());
            }
        }
        // Keep a live chaff population in the way.
        for (int c = 0; c < 5; ++c) {
            if (h.chaff.full()) break;
            h.add_chaff(rng.range_f(0.0f, 100.0f), rng.range_f(0.0f, 100.0f),
                        rng.range_f(1.0f, 8.0f),
                        static_cast<PathogenFamily>(rng.next_below(kFamilyCount)));
        }

        const ProjectileStats s = h.step();
        h.chaff.compact();

        // P1: every slot in [0, count) is alive and not pending-kill.
        for (usize i = 0; i < h.rounds.count(); ++i) {
            REQUIRE((h.rounds.flags[i] & projectile_flags::kAlive) != 0);
            REQUIRE((h.rounds.flags[i] & projectile_flags::kPendingKill) == 0);
            REQUIRE(h.rounds.life[i] > 0.0f);
        }
        // P2: streams stay parallel and sized to capacity.
        REQUIRE(h.rounds.pos_x.size() == h.rounds.capacity());
        REQUIRE(h.rounds.pos_y.size() == h.rounds.capacity());
        REQUIRE(h.rounds.vel_x.size() == h.rounds.capacity());
        REQUIRE(h.rounds.vel_y.size() == h.rounds.capacity());
        REQUIRE(h.rounds.damage.size() == h.rounds.capacity());
        REQUIRE(h.rounds.life.size() == h.rounds.capacity());
        REQUIRE(h.rounds.hit_radius.size() == h.rounds.capacity());
        REQUIRE(h.rounds.family_mask.size() == h.rounds.capacity());
        REQUIRE(h.rounds.flags.size() == h.rounds.capacity());
        REQUIRE(h.rounds.visual_id.size() == h.rounds.capacity());
        REQUIRE(h.rounds.owner.size() == h.rounds.capacity());
        // P3: never over capacity.
        REQUIRE(h.rounds.count() <= h.rounds.capacity());
        REQUIRE(s.live == static_cast<u32>(h.rounds.count()));
        // Chaff invariant I4 must survive projectile damage too.
        for (usize i = 0; i < h.chaff.count(); ++i) REQUIRE(h.chaff.density[i] > 0.0f);
    }
}

TEST_CASE("update with no rounds and no chaff is a no-op", "[sim][projectile]") {
    Harness h;
    const ProjectileStats s = h.step();
    REQUIRE(s.live == 0);
    REQUIRE(s.impacts == 0);
    REQUIRE(s.expired == 0);
    REQUIRE(s.spawned_this_tick == 0);
}

TEST_CASE("rounds fly normally when there is no chaff at all", "[sim][projectile]") {
    Harness h;
    for (int i = 0; i < 50; ++i) {
        h.add_round(Vec2{50.0f, 50.0f}, Vec2{6.0f, 6.0f}, 1.0f, 1.0f);
    }
    const ProjectileStats s = h.step();
    REQUIRE(s.live == 50);
    REQUIRE(s.impacts == 0);
    REQUIRE(h.rounds.pos_x[0] == Catch::Approx(50.1f));
}

// ---------------------------------------------------------------------------
// Perf. Reported, not asserted tightly: the bound is generous enough not to be
// flaky on a loaded machine, and the measured number is printed so a regression
// is visible in the test log.
// ---------------------------------------------------------------------------

TEST_CASE("projectile update cost tracks round count, not chaff count",
          "[sim][projectile][bench]") {
    auto measure = [](int n_rounds, int n_chaff) {
        Harness h(20000, 20000, 4.0f);
        Rng r(4242u);
        for (int i = 0; i < n_chaff; ++i) {
            h.add_chaff(r.range_f(5.0f, 95.0f), r.range_f(5.0f, 95.0f), 1.0e9f);
        }
        for (int i = 0; i < n_rounds; ++i) {
            // family_mask 0 matches nothing, so every round walks its entire
            // cell every tick and never retires early: the pessimistic cost.
            // Velocities are kept small so no round drifts out of bounds over
            // the measured window: the point is to hold the live set constant.
            h.add_round(Vec2{r.range_f(20.0f, 80.0f), r.range_f(20.0f, 80.0f)},
                        Vec2{r.range_f(-2.0f, 2.0f), r.range_f(-2.0f, 2.0f)}, 1.0f,
                        1000.0f, 0.5f, 0, /*mask*/ 0);
        }
        h.hash.rebuild(h.chaff.pos_x.data(), h.chaff.pos_y.data(), h.chaff.count(), nullptr);

        const int ticks = 120;
        f64 best_ms = 1.0e30;
        for (int t = 0; t < ticks; ++t) {
            const auto t0 = std::chrono::steady_clock::now();
            h.sys.update(h.rounds, h.chaff, h.hash, h.tissue, h.bounds, h.rng, kFixedDt, nullptr);
            const auto t1 = std::chrono::steady_clock::now();
            const f64 ms = std::chrono::duration<f64, std::milli>(t1 - t0).count();
            if (ms < best_ms) best_ms = ms;
        }
        REQUIRE(h.rounds.count() == static_cast<usize>(n_rounds));   // none retired
        return best_ms;
    };

    const f64 a = measure(8000, 1000);
    const f64 b = measure(8000, 10000);
    const f64 c = measure(2000, 10000);
    std::printf("[projectile perf] 8000 rounds / 1000 chaff : %.4f ms/tick\n", a);
    std::printf("[projectile perf] 8000 rounds / 10000 chaff: %.4f ms/tick\n", b);
    std::printf("[projectile perf] 2000 rounds / 10000 chaff: %.4f ms/tick\n", c);
    std::fflush(stdout);

    // A whole Gunner army's worth of rounds must stay far inside the 16.6 ms
    // frame. Deliberately loose so this is a regression tripwire, not a flake.
    REQUIRE(a < 4.0);
    REQUIRE(b < 4.0);
}
