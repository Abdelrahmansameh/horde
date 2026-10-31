// Coverage for the swarmer layer (sim/swarm/Swarmers.h).
//
// These check the properties the DESIGN rests on rather than exact numbers,
// which are art-direction and balance figures that should stay free to move:
// a latcher finds a host, latches, drains it, survives that host dying and
// picks another, dissolves on its own clock, and never lets a stale chaff
// index point it at the wrong agent after a compaction; a shooter holds its
// standoff and asks for rounds; a bomber detonates on contact and on expiry
// and asks for its effect; every kind can go at a named agent; and hidden
// targets are invisible to all of them.
#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/projectile/Projectiles.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using namespace immune;
using namespace immune::sim;

TEST_CASE("burrow threats follow live mobile defenders", "[sim][burrow][swarm]") {
    SimDesc desc;
    desc.world_bounds = Rect{{0.0f, 0.0f}, {128.0f, 128.0f}};
    desc.max_swarmers = 8;
    SimWorld world;
    world.init(desc, nullptr);
    SwarmerProfile attacker;
    attacker.kind = SwarmerKind::Shooter;
    attacker.search_radius = 12.0f;
    attacker.attach_radius = 20.0f;
    world.swarmers().set_profile(0, attacker);
    SwarmerProfile builder;
    builder.kind = SwarmerKind::Builder;
    world.swarmers().set_profile(1, builder);

    SwarmerSpawnParams unit;
    unit.position = {40.0f, 50.0f};
    unit.persistent = true;
    REQUIRE(world.swarmers().spawn(unit));
    unit.profile = 1;
    REQUIRE(world.swarmers().spawn(unit));
    unit.profile = 0;
    unit.family_mask = static_cast<u8>(1u << static_cast<u32>(PathogenFamily::Virus));
    REQUIRE(world.swarmers().spawn(unit));

    world.build_burrow_threats();
    REQUIRE(world.burrow_threats().size() == 1);
    CHECK(world.burrow_threats()[0].position.x == Catch::Approx(40.0f));
    CHECK(world.burrow_threats()[0].position.y == Catch::Approx(50.0f));
    CHECK(world.burrow_threats()[0].radius == Catch::Approx(20.0f));

    world.swarmers().pos_x[0] = 70.0f;
    world.build_burrow_threats();
    REQUIRE(world.burrow_threats().size() == 1);
    CHECK(world.burrow_threats()[0].position.x == Catch::Approx(70.0f));
    world.swarmers().health[0] = 0.0f;
    world.build_burrow_threats();
    CHECK(world.burrow_threats().empty());
    world.swarmers().health[0] = attacker.max_health;
    world.swarmers().kill(0);
    world.build_burrow_threats();
    CHECK(world.burrow_threats().empty());
}

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const Rect kBounds{Vec2{0.0f, 0.0f}, Vec2{128.0f, 128.0f}};

constexpr u16 kLatch = 0;
constexpr u16 kShooter = 1;
constexpr u16 kBomber = 2;
constexpr u16 kMucus = 4;
constexpr u16 kArbor = 5;

struct Fixture {
    ChaffBuffers chaff;
    SwarmerBuffers swarm;
    SwarmerSystem system;
    SpatialHash hash;
    NamedTargetList named;
    Rng rng{4242};
    /// Null by default: a kiting shooter then backs straight away. Tests
    /// that want the flow to bend the retreat point this at one.
    const FlowField* flow = nullptr;
    const DistanceField* sdf = nullptr;
    Rect playable_bounds = kBounds;

    Fixture() {
        chaff.reserve(4096);
        swarm.reserve(4096);
        SpatialHashDesc hd;
        hd.bounds = kBounds;
        hd.cell_size = 4.0f;
        hash.configure(hd);

        // One profile per kind, on a shared chassis.
        SwarmerProfile base;
        base.lifetime = 3.0f;
        base.speed = 14.0f;
        base.search_radius = 12.0f;
        base.attach_radius = 0.55f;
        base.dps = 20.0f;
        // Instant latch by default: the entry animation has its own tests
        // below, and every other latch test wants the drain on tick one.
        base.attach_seconds = 0.0f;

        SwarmerProfile latch = base;
        latch.kind = SwarmerKind::Latch;
        latch.source = TowerType::CytotoxicT;
        swarm.set_profile(kLatch, latch);

        SwarmerProfile shooter = base;
        shooter.kind = SwarmerKind::Shooter;
        shooter.source = TowerType::Neutrophil;
        shooter.attach_radius = 4.0f;   // the standoff
        // A one-round magazine reloading in 0.2 s: the plain steady stream
        // most shooter tests want. The MAGAZINE has its own tests below.
        shooter.fire_interval = 0.2f;
        shooter.reload_seconds = 0.2f;
        shooter.round_damage = 3.0f;
        shooter.round_speed = 40.0f;
        swarm.set_profile(kShooter, shooter);

        SwarmerProfile bomber = base;
        bomber.kind = SwarmerKind::Bomber;
        bomber.source = TowerType::Count;
        bomber.burst_radius = 3.0f;
        bomber.burst_damage = 25.0f;
        bomber.burst_named_damage = 30.0f;
        swarm.set_profile(kBomber, bomber);

        SwarmerProfile mucus = base;
        mucus.kind = SwarmerKind::MucusBomber;
        mucus.source = TowerType::GobletCell;
        swarm.set_profile(kMucus, mucus);

        // A macrophage unit: reach 6, body 2, no kiting -- a volley of
        // these is a LANE WALL, spaced 4 apart, and the body shoves the
        // horde back rather than yielding to it.
        SwarmerProfile arbor = base;
        arbor.kind = SwarmerKind::ArborGrabber;
        arbor.source = TowerType::Macrophage;
        arbor.lifetime = 30.0f;
        arbor.search_radius = 40.0f;
        arbor.attach_radius = 6.0f;
        arbor.size = 2.0f;
        arbor.kite_fraction = 0.0f;
        arbor.formation_spacing = 4.0f;
        arbor.body_block = 1.0f;
        swarm.set_profile(kArbor, arbor);
    }

    usize add_chaff(Vec2 p, f32 density, u8 flags = 0) {
        ChaffSpawnParams c;
        c.position = p;
        c.density = density;
        c.family = PathogenFamily::Virus;
        c.flags = flags;
        chaff.spawn(c);
        return chaff.count() - 1;
    }

    void add_swarmer(Vec2 p, Vec2 v, u16 profile = kLatch, u32 group = 0, u16 slot = 0) {
        SwarmerSpawnParams s;
        s.position = p;
        s.velocity = v;
        s.profile = profile;
        s.owner = EntityId{7u};
        s.seed = 0x1234u + static_cast<u32>(swarm.count()) * 7919u;
        s.group = group;
        s.slot = slot;
        swarm.spawn(s);
    }

    /// A named agent standing at `p`. The list is rebuilt from scratch by the
    /// caller between steps (as SimWorld does), so this is just the record.
    NamedTarget make_named(u32 id, Vec2 p, f32 health, f32 armor = 0.0f) {
        NamedTarget t;
        t.id = EntityId{id};
        t.position = p;
        t.radius = 1.0f;
        t.armor = armor;
        t.health = health;
        t.family = static_cast<u8>(PathogenFamily::Virus);
        return t;
    }

    /// Steps with the named list REBUILT every tick from `agents`, carrying
    /// the damage the kernel queued back into their health, which is what
    /// SimWorld::apply_swarmer_effects does with the ECS.
    void step(int ticks, CombatEventSink* sink = nullptr, std::vector<NamedTarget>* agents = nullptr) {
        for (int i = 0; i < ticks; ++i) {
            hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr);
            named.clear();
            if (agents) {
                for (const NamedTarget& t : *agents) if (t.health > 0.0f) named.add(t);
                named.sort();
            }
            system.update(swarm, chaff, hash, named, sdf, flow, playable_bounds, rng, kDt, sink);
            if (agents) {
                for (NamedTarget& t : *agents) {
                    const usize k = named.find(t.id);
                    if (k != NamedTargetList::npos) t.health -= named.damage[k];
                }
            }
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

Vec2 f_pos(const Fixture& f, usize chaff_index) {
    return Vec2{f.chaff.pos_x[chaff_index], f.chaff.pos_y[chaff_index]};
}

void check_body_inside(const SwarmerBuffers& sw, usize i, const Rect& bounds) {
    const f32 radius = std::min(sw.body_size(i),
                               std::max(0.0f, std::min(bounds.size().x, bounds.size().y) * 0.5f));
    constexpr f32 tolerance = 1.0e-4f;
    REQUIRE(std::isfinite(sw.pos_x[i]));
    REQUIRE(std::isfinite(sw.pos_y[i]));
    CHECK(sw.pos_x[i] >= bounds.min.x + radius - tolerance);
    CHECK(sw.pos_x[i] <= bounds.max.x - radius + tolerance);
    CHECK(sw.pos_y[i] >= bounds.min.y + radius - tolerance);
    CHECK(sw.pos_y[i] <= bounds.max.y - radius + tolerance);
}

} // namespace

// ---------------------------------------------------------------------------
// Latch
// ---------------------------------------------------------------------------

TEST_CASE("a latcher crosses the gap, latches on, and drains its host", "[swarm][sim][latch]") {
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

TEST_CASE("an attached latcher drains a marked host faster", "[swarm][sim][latch][marked]") {
    // A latcher's drain never goes through a DamageField -- it applies density
    // loss directly, same as a projectile impact -- so it needs its own read of
    // chaff_flags::kMarked (the Goblet Cell's weaken debuff) or a marked host
    // would feel no different from an unmarked one.
    Fixture f;
    const usize plain = f.add_chaff(Vec2{40.0f, 40.0f}, 1000.0f);
    const usize marked = f.add_chaff(Vec2{80.0f, 40.0f}, 1000.0f);
    f.chaff.flags[marked] |= chaff_flags::kMarked;

    // Spawned already inside attach_radius, so both latch on the first tick.
    f.add_swarmer(Vec2{40.1f, 40.0f}, Vec2{0.0f, 0.0f});
    f.add_swarmer(Vec2{80.1f, 40.0f}, Vec2{0.0f, 0.0f});

    const f32 plain_before = f.chaff.density[plain];
    const f32 marked_before = f.chaff.density[marked];

    f.step(1);
    REQUIRE(f.any_attached());

    const f32 plain_loss = plain_before - f.chaff.density[plain];
    const f32 marked_loss = marked_before - f.chaff.density[marked];
    INFO("plain lost " << plain_loss << ", marked lost " << marked_loss);
    REQUIRE(plain_loss > 0.0f);
    REQUIRE(marked_loss == Catch::Approx(plain_loss * chaff_flags::kMarkedDamageMultiplier).epsilon(0.001));
}

TEST_CASE("a latcher spends attach_seconds entering its host before it drains, and sits at the centre once in",
          "[swarm][sim][latch][entry]") {
    Fixture f;
    const usize host = f.add_chaff(Vec2{40.0f, 40.0f}, 1000.0f);
    SwarmerProfile p = f.swarm.profile_at(kLatch);
    p.attach_seconds = 6.0f * kDt;   // six ticks
    p.attach_steps = 3u;
    f.swarm.set_profile(kLatch, p);
    // Inside attach_radius already, so it latches on the first tick.
    f.add_swarmer(Vec2{40.1f, 40.0f}, Vec2{0.0f, 0.0f});
    const f32 before = f.chaff.density[host];

    // Engaged from tick one, but not yet feeding, and starting out on the
    // clump ring rather than the centre.
    f.step(1);
    REQUIRE(f.any_attached());
    REQUIRE(f.chaff.density[host] == before);
    REQUIRE(f.swarm.attach[0] == Catch::Approx(0.0f));
    const f32 ring_dist = math::length(Vec2{f.swarm.pos_x[0], f.swarm.pos_y[0]} - f_pos(f, host));
    REQUIRE(ring_dist > 0.1f);

    // Part-way in: the clock runs, still no drain, and the unit has lurched
    // closer in a discrete step (not the full way).
    f.step(3);
    REQUIRE(f.chaff.density[host] == before);
    REQUIRE(f.swarm.attach[0] == Catch::Approx(3.0f * kDt));
    const f32 mid_dist = math::length(Vec2{f.swarm.pos_x[0], f.swarm.pos_y[0]} - f_pos(f, host));
    REQUIRE(mid_dist < ring_dist);
    REQUIRE(mid_dist > 0.0f);

    // Entry complete: the clock caps at attach_seconds, the unit is at the
    // host's centre, and the drain has started.
    f.step(4);
    REQUIRE(f.swarm.attach[0] == Catch::Approx(p.attach_seconds));
    REQUIRE(f.chaff.density[host] < before);
    const f32 in_dist = math::length(Vec2{f.swarm.pos_x[0], f.swarm.pos_y[0]} - f_pos(f, host));
    REQUIRE(in_dist == Catch::Approx(0.0f).margin(1e-4f));
}

TEST_CASE("a latcher tracks a moving host exactly while entering and while in",
          "[swarm][sim][latch][entry]") {
    Fixture f;
    const usize host = f.add_chaff(Vec2{40.0f, 40.0f}, 1000.0f);
    SwarmerProfile p = f.swarm.profile_at(kLatch);
    p.attach_seconds = 10.0f * kDt;
    f.swarm.set_profile(kLatch, p);
    f.add_swarmer(Vec2{40.1f, 40.0f}, Vec2{0.0f, 0.0f});
    f.step(1);
    REQUIRE(f.any_attached());

    // Teleport the host each tick as the chaff pass would move it; the
    // latcher's offset from the host must be a function of the entry clock
    // only, never of where the host was last tick.
    for (int t = 0; t < 20; ++t) {
        f.chaff.pos_x[host] += 0.5f;
        f.chaff.pos_y[host] += 0.25f;
        const f32 clock_before = f.swarm.attach[0];
        f.step(1);
        const f32 d = math::length(Vec2{f.swarm.pos_x[0], f.swarm.pos_y[0]} - f_pos(f, host));
        const f32 entry = math::saturate(f.swarm.attach[0] / p.attach_seconds);
        INFO("tick " << t << " clock " << clock_before << " -> " << f.swarm.attach[0] << " dist " << d);
        // Never further than the untouched clump ring, and at the centre once in.
        REQUIRE(d <= p.attach_radius * 0.6f + 1e-4f);
        if (entry >= 1.0f) REQUIRE(d == Catch::Approx(0.0f).margin(1e-4f));
    }
}

TEST_CASE("a latcher is back at full size the tick its host dies", "[swarm][sim][latch][entry]") {
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 1000.0f);
    SwarmerProfile p = f.swarm.profile_at(kLatch);
    p.attach_seconds = 3.0f * kDt;
    f.swarm.set_profile(kLatch, p);
    f.add_swarmer(Vec2{40.1f, 40.0f}, Vec2{0.0f, 0.0f});

    f.step(6);
    REQUIRE(f.any_attached());
    REQUIRE(f.swarm.attach[0] == Catch::Approx(p.attach_seconds));   // fully in

    // Kill the host out from under it. The very next update sees the handle
    // dead, drops the flag and zeroes the entry clock together: the renderer
    // derives size from (flag, clock), so this is "full size, same frame".
    f.chaff.kill(0);
    f.step(1);
    REQUIRE(f.chaff.count() == 0);
    REQUIRE_FALSE(f.any_attached());
    REQUIRE(f.swarm.attach[0] == 0.0f);
    REQUIRE(f.system.last_stats().live == 1);
}

TEST_CASE("a latcher whose host dies finds another instead of dissolving with it",
          "[swarm][sim][latch]") {
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 6.0f);       // dies quickly
    f.add_chaff(Vec2{43.0f, 40.0f}, 4000.0f);    // and this one is next
    const f32 tough_before = f.chaff.density[1];

    SwarmerProfile p = f.swarm.profile_at(kLatch);
    p.lifetime = 8.0f;
    f.swarm.set_profile(kLatch, p);
    f.add_swarmer(Vec2{38.0f, 40.0f}, Vec2{14.0f, 0.0f});

    f.step(400);

    REQUIRE(f.chaff.count() == 1);                        // the frail one is gone
    REQUIRE(f.chaff.density[0] < tough_before);           // the latcher moved on
    REQUIRE(f.system.last_stats().live == 1);
}

TEST_CASE("a latcher holds a handle, so a compaction cannot re-point it at a stranger",
          "[swarm][sim][latch]") {
    // The failure this guards is silent and nasty: chaff compaction swap-removes,
    // so the slot a dead agent occupied gets a live agent moved into it. A
    // swarmer storing a raw index would keep draining that slot and appear to
    // teleport its damage onto an unrelated pathogen halfway across the lane.
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 6.0f);        // index 0, will die
    f.add_chaff(Vec2{100.0f, 100.0f}, 4000.0f);   // index 1, far away, will move to 0
    const f32 far_before = f.chaff.density[1];

    SwarmerProfile p = f.swarm.profile_at(kLatch);
    p.lifetime = 6.0f;
    f.swarm.set_profile(kLatch, p);
    f.add_swarmer(Vec2{39.0f, 40.0f}, Vec2{6.0f, 0.0f});

    // Long enough to kill the near agent and keep going, but the far agent is
    // outside the search radius, so it must never be touched.
    f.step(300);

    REQUIRE(f.chaff.count() == 1);
    INFO("far agent density " << f.chaff.density[0] << ", was " << far_before);
    REQUIRE(f.chaff.density[0] == far_before);
}

TEST_CASE("latchers dissolve on their own lifetime even with a host in reach", "[swarm][sim][latch]") {
    Fixture f;
    f.add_chaff(Vec2{40.0f, 40.0f}, 1.0e6f);   // effectively unkillable
    SwarmerProfile p = f.swarm.profile_at(kLatch);
    p.lifetime = 0.5f;
    f.swarm.set_profile(kLatch, p);
    f.add_swarmer(Vec2{39.0f, 40.0f}, Vec2{4.0f, 0.0f});

    REQUIRE(f.swarm.count() == 1);
    f.step(60);                                 // one second
    REQUIRE(f.swarm.count() == 0);
    // A dissolve leaves nothing behind.
    REQUIRE(f.system.effects().empty());
}

// ---------------------------------------------------------------------------
// Shooter
// ---------------------------------------------------------------------------

TEST_CASE("a shooter stops at its standoff and asks for rounds, then chases when the target leaves",
          "[swarm][sim][shooter]") {
    Fixture f;
    const usize target = f.add_chaff(Vec2{50.0f, 40.0f}, 1.0e6f);
    // Long-lived, so the approach, the firing and the chase below all fit
    // inside one lifetime.
    SwarmerProfile long_lived = f.swarm.profile_at(kShooter);
    long_lived.lifetime = 20.0f;
    f.swarm.set_profile(kShooter, long_lived);
    f.add_swarmer(Vec2{30.0f, 40.0f}, Vec2{14.0f, 0.0f}, kShooter);
    CombatEventSink sink;
    sink.reserve(1024);

    // Approach: no shots until it is inside the standoff.
    u32 shots = 0;
    bool engaged = false;
    for (int i = 0; i < 240 && !engaged; ++i) {
        f.step(1, &sink);
        shots += static_cast<u32>(f.system.effects().shots.size());
        engaged = f.any_attached();
    }
    REQUIRE(engaged);
    const f32 standoff = f.swarm.profile_at(kShooter).attach_radius;
    const Vec2 p{f.swarm.pos_x[0], f.swarm.pos_y[0]};
    const f32 dist = math::length(Vec2{50.0f, 40.0f} - p);
    INFO("engaged at distance " << dist << " (standoff " << standoff << ")");
    REQUIRE(dist <= standoff + 0.5f);
    REQUIRE(dist > standoff * 0.4f);   // it did not run into the target

    // Engaged: it fires on its own interval, and each shot is a request in
    // effects() (SimWorld turns it into a projectile) plus a swarmer-flagged
    // MuzzleFlash.
    for (int i = 0; i < 60; ++i) {
        f.step(1, &sink);
        shots += static_cast<u32>(f.system.effects().shots.size());
        if (!f.system.effects().shots.empty()) {
            const SwarmerShot& s = f.system.effects().shots.front();
            REQUIRE(s.damage == Catch::Approx(3.0f));
            REQUIRE(math::length(s.velocity) == Catch::Approx(40.0f));
            REQUIRE(s.source == TowerType::Neutrophil);
            REQUIRE(s.owner == EntityId{7u});
        }
    }
    REQUIRE(shots >= 4);
    bool flagged = false;
    for (const CombatEvent& e : sink.events()) {
        if (e.type == CombatEventType::MuzzleFlash && (e.visual_id & kSwarmerEventBit) != 0) flagged = true;
    }
    REQUIRE(flagged);
    // A shooter never damages chaff itself; the round does that later.
    REQUIRE(f.chaff.density[target] == 1.0e6f);

    // Move the target away: the shooter loses its engagement and chases.
    f.chaff.pos_x[target] = 70.0f;
    f.step(1);
    REQUIRE(f.swarm.count() == 1);
    REQUIRE_FALSE(f.any_attached());
    const f32 x0 = f.swarm.pos_x[0];
    f.step(30);
    REQUIRE(f.swarm.pos_x[0] > x0 + 2.0f);
}

TEST_CASE("a shooter stands its ground: a target that leaves the standoff is swapped for a closer one",
          "[swarm][sim][shooter][retarget]") {
    Fixture f;
    SwarmerProfile long_lived = f.swarm.profile_at(kShooter);
    long_lived.lifetime = 30.0f;
    f.swarm.set_profile(kShooter, long_lived);
    // A inside the standoff, B a little further out -- so A is the pick.
    const usize a = f.add_chaff(Vec2{50.0f, 40.0f}, 1.0e6f);
    const usize b = f.add_chaff(Vec2{46.0f, 46.0f}, 1.0e6f);
    f.add_swarmer(Vec2{46.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(1);
    REQUIRE(f.any_attached());
    REQUIRE(f.swarm.target_index[0] == a);

    // A walks out of the standoff (still inside the 12-unit search radius);
    // B is nearer than A now, so the shooter does not follow A -- it turns
    // on B.
    f.chaff.pos_x[a] = 56.0f;
    f.step(1);
    REQUIRE(f.swarm.target_index[0] == b);
    REQUIRE(f.system.last_stats().retargeted == 1);

    // B leaves too, further than A: the closer of the two (A) wins.
    f.chaff.pos_y[b] = 60.0f;
    f.step(1);
    REQUIRE(f.swarm.target_index[0] == a);

    // With nothing else in reach, an out-of-range target IS followed.
    f.chaff.kill(b);
    f.chaff.compact();
    const f32 x0 = f.swarm.pos_x[0];
    f.step(30);
    REQUIRE(f.swarm.pos_x[0] > x0 + 1.0f);
}

namespace {

/// The hit fraction a shooter manages against one target walking ACROSS its
/// line of fire, with its rounds flown through the real projectile pass the
/// way SimWorld wires the two systems (hash, chaff moves, rounds fly,
/// swarmers act, and what they asked for is in the store by the next tick).
/// `target_speed` is the walk; the standoff and round speed are the game's
/// (towers.json: 20 out, 45 u/s at tier 1, 60 at tier 3).
struct LeadTrial {
    u32 fired = 0;
    u32 hits = 0;
};

LeadTrial run_lead_trial(f32 target_speed, f32 round_speed, f32 hit_radius, u32 seed = 4242u) {
    Fixture f;
    f.rng = Rng{seed};
    SwarmerProfile pr = f.swarm.profile_at(kShooter);
    pr.lifetime = 30.0f;
    pr.attach_radius = 20.0f;
    pr.search_radius = 60.0f;
    pr.speed = 22.0f;
    pr.round_speed = round_speed;
    pr.round_hit_radius = hit_radius;
    pr.round_spread = 0.0f;    // the spread is the "spray" read; the AIM is under test
    pr.kite_fraction = 0.0f;   // hold the slot rather than back off
    f.swarm.set_profile(kShooter, pr);

    const Vec2 target_vel{0.0f, target_speed};
    const usize target = f.add_chaff(Vec2{62.3f, 21.7f}, 1.0e6f);
    f.chaff.vel_x[target] = target_vel.x;
    f.chaff.vel_y[target] = target_vel.y;
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);

    ProjectileBuffers rounds;
    rounds.reserve(1024);
    ProjectileSystem projectiles;
    TissueMask tissue;
    LeadTrial trial;
    for (int tick = 0; tick < 60 * 6; ++tick) {
        f.hash.rebuild(f.chaff.pos_x.data(), f.chaff.pos_y.data(), f.chaff.count(), nullptr);
        f.chaff.pos_x[target] += target_vel.x * kDt;
        f.chaff.pos_y[target] += target_vel.y * kDt;
        trial.hits += projectiles.update(rounds, f.chaff, f.hash, tissue, kBounds, f.rng, kDt, nullptr).impacts;
        f.named.clear();
        f.system.update(f.swarm, f.chaff, f.hash, f.named, nullptr, nullptr, kBounds, f.rng, kDt, nullptr);
        for (const SwarmerShot& s : f.system.effects().shots) {
            ProjectileSpawnParams p;
            p.position = s.origin;
            p.velocity = s.velocity;
            p.damage = s.damage;
            p.lifetime = s.lifetime;
            p.hit_radius = s.hit_radius;
            p.family_mask = s.family_mask;
            p.owner = s.owner;
            p.visual_id = s.visual_id;
            REQUIRE(rounds.spawn(p));
            ++trial.fired;
        }
        f.chaff.compact();
    }
    return trial;
}

} // namespace

TEST_CASE("a shooter leads a crossing target so its rounds land", "[swarm][sim][shooter][lead]") {
    // Over the ~0.36 s a round takes to cross the standoff, a target walking
    // at a horde's pace moves several hit radii. Aimed at where the target IS,
    // every round lands where it WAS; aimed at the intercept, most of them
    // land. The threshold leaves room for what the projectile pass concedes
    // by design (Projectiles.cpp: one cell tested per tick, a per-tick step
    // comparable to the hit diameter).
    const f32 kFast = 16.875f;   // enemies.json "fast" tier
    const f32 kNormal = 12.375f;
    for (const f32 walk : {kNormal, kFast}) {
        for (const f32 round_speed : {45.0f, 60.0f}) {
            const LeadTrial t = run_lead_trial(walk, round_speed, 0.45f);
            INFO("target " << walk << " u/s, round " << round_speed << " u/s: " << t.hits << " of "
                           << t.fired << " rounds landed");
            REQUIRE(t.fired >= 15);
            CHECK(t.hits * 10 >= t.fired * 7);
        }
    }

    // A target standing still is the degenerate case: the intercept IS the
    // target, so the solver must not disturb a shot that already lands.
    const LeadTrial still = run_lead_trial(0.0f, 45.0f, 0.45f);
    INFO("standing target: " << still.hits << " of " << still.fired);
    REQUIRE(still.fired >= 15);
    CHECK(still.hits * 10 >= still.fired * 7);
}

namespace {

/// A shooter with a real MAGAZINE: five rounds, gathered for 0.2 s, 0.05 s
/// apart across a 0.3 rad cone, reloaded over 1 s. No jitter and no kiting,
/// so every direction and every tick below is the magazine's doing.
SwarmerProfile magazine_profile(const SwarmerProfile& base) {
    SwarmerProfile pr = base;
    pr.lifetime = 30.0f;
    pr.magazine_size = 5u;
    pr.gather_seconds = 0.2f;
    pr.fire_interval = 0.05f;
    pr.reload_seconds = 1.0f;
    pr.volley_cone = 0.3f;
    pr.round_spread = 0.0f;
    pr.kite_fraction = 0.0f;
    return pr;
}

struct FiredRound {
    int tick;
    Vec2 dir;
    Vec2 origin;
};

} // namespace

TEST_CASE("a shooter's magazine gathers, fans a volley across its cone, then reloads",
          "[swarm][sim][shooter][magazine]") {
    Fixture f;
    f.swarm.set_profile(kShooter, magazine_profile(f.swarm.profile_at(kShooter)));
    const Vec2 target{43.6f, 40.0f};   // inside the 4-unit standoff: engaged at once
    f.add_chaff(target, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);

    std::vector<FiredRound> fired;
    int engaged_at = -1;
    // 2.5 s: two whole volleys (0.2 gather + 0.2 of rounds + 1.0 reload
    // each), with the third gather still under way at the end.
    for (int t = 0; t < 150; ++t) {
        f.step(1);
        if (engaged_at < 0 && f.any_attached()) engaged_at = t;
        for (const SwarmerShot& s : f.system.effects().shots) {
            fired.push_back(FiredRound{t, math::normalize_safe(s.velocity), s.origin});
        }
    }
    REQUIRE(engaged_at >= 0);
    REQUIRE(fired.size() == 10);

    // Nothing leaves during the gather; the first round leaves as it ends.
    const int gather_ticks = static_cast<int>(std::lround(0.2f / kDt));
    CHECK(fired[0].tick - engaged_at >= gather_ticks - 1);
    CHECK(fired[0].tick - engaged_at <= gather_ticks + 1);

    // The volley is quick: all five inside ~four fire intervals...
    CHECK(fired[4].tick - fired[0].tick <= static_cast<int>(std::lround(0.2f / kDt)) + 1);
    // ...then a real reload gap before the next gather even starts.
    const int gap = fired[5].tick - fired[4].tick;
    INFO("gap between volleys " << gap << " ticks");
    CHECK(gap >= static_cast<int>(std::lround((1.0f + 0.2f) / kDt)) - 2);

    // Fanned across the cone around the target line: the first round goes
    // straight down it, none leave the cone, and the volley really spreads.
    // The line is taken from each round's own origin; the first one sits
    // exactly on the frozen aim, the later ones only as far off it as the
    // unit has wandered since, hence the small allowance on the cone edge.
    const auto off_line = [&](const FiredRound& r) {
        const Vec2 line = math::normalize_safe(target - r.origin);
        return std::acos(math::clamp(r.dir.x * line.x + r.dir.y * line.y, -1.0f, 1.0f));
    };
    f32 widest = 0.0f;
    for (usize k = 0; k < 5; ++k) {
        INFO("round " << k << " is " << off_line(fired[k]) << " rad off the line");
        CHECK(off_line(fired[k]) <= 0.3f + 0.08f);
        widest = math::max(widest, off_line(fired[k]));
    }
    CHECK(off_line(fired[0]) < 0.02f);
    CHECK(widest > 0.2f);
}

TEST_CASE("a volley that has begun finishes even when its target dies", "[swarm][sim][shooter][magazine]") {
    Fixture f;
    f.swarm.set_profile(kShooter, magazine_profile(f.swarm.profile_at(kShooter)));
    const usize target = f.add_chaff(Vec2{43.6f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);

    u32 shots = 0;
    for (int t = 0; t < 120 && shots == 0; ++t) {
        f.step(1);
        shots += static_cast<u32>(f.system.effects().shots.size());
    }
    REQUIRE(shots == 1);
    f.chaff.kill(target);
    f.chaff.compact();
    for (int t = 0; t < 30; ++t) {
        f.step(1);
        shots += static_cast<u32>(f.system.effects().shots.size());
    }
    CHECK(shots == 5);
    CHECK(f.swarm.magazine[0].phase == MagazinePhase::Reloading);
}

TEST_CASE("a zero fire_interval empties the magazine in one tick", "[swarm][sim][shooter][magazine]") {
    Fixture f;
    SwarmerProfile pr = magazine_profile(f.swarm.profile_at(kShooter));
    pr.fire_interval = 0.0f;
    pr.gather_seconds = 0.0f;
    f.swarm.set_profile(kShooter, pr);
    f.add_chaff(Vec2{43.6f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    usize most = 0;
    for (int t = 0; t < 30; ++t) {
        f.step(1);
        most = std::max(most, f.system.effects().shots.size());
    }
    CHECK(most == 5);
}

TEST_CASE("a shooter reloads while it has nothing to shoot at", "[swarm][sim][shooter][magazine]") {
    Fixture f;
    f.swarm.set_profile(kShooter, magazine_profile(f.swarm.profile_at(kShooter)));
    const usize target = f.add_chaff(Vec2{43.6f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    for (int t = 0; t < 60 && f.swarm.magazine[0].phase != MagazinePhase::Reloading; ++t) f.step(1);
    REQUIRE(f.swarm.magazine[0].phase == MagazinePhase::Reloading);

    // Lane empties: the unit drifts, and the magazine still refills.
    f.chaff.kill(target);
    f.chaff.compact();
    f.step(static_cast<int>(std::lround(1.1f / kDt)));
    CHECK(f.swarm.magazine[0].phase == MagazinePhase::Loaded);
    CHECK(f.system.effects().shots.empty());
}

TEST_CASE("a shooter's body pulse follows actual rounds through the final reload edge",
          "[swarm][sim][shooter][magazine]") {
    Fixture f;
    SwarmerProfile pr = magazine_profile(f.swarm.profile_at(kShooter));
    pr.magazine_size = 2u;
    pr.fire_interval = 0.1f;
    f.swarm.set_profile(kShooter, pr);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);

    // A new or wandering cell has no shot to animate.
    CHECK(f.swarm.magazine[0].seconds_since_shot == 1.0f);
    f.step(3);
    CHECK(f.swarm.magazine[0].seconds_since_shot == 1.0f);
    CHECK(f.system.effects().shots.empty());

    const usize target = f.add_chaff(Vec2{43.6f, 40.0f}, 1.0e6f);
    u32 rounds = 0;
    f32 previous_age = f.swarm.magazine[0].seconds_since_shot;
    for (int tick = 0; tick < 90 && rounds < 2u; ++tick) {
        f.step(1);
        const f32 age = f.swarm.magazine[0].seconds_since_shot;
        if (!f.system.effects().shots.empty()) {
            rounds += static_cast<u32>(f.system.effects().shots.size());
            CHECK(age >= 0.0f);
            CHECK(age <= kDt + 1.0e-5f);
            CHECK(age < previous_age);
        } else {
            CHECK(age == Catch::Approx(math::min(previous_age + kDt, 1.0f)));
        }
        previous_age = age;
    }
    REQUIRE(rounds == 2u);
    REQUIRE(f.swarm.magazine[0].phase == MagazinePhase::Reloading);
    CHECK(f.swarm.magazine[0].seconds_since_shot < 0.02f);

    // The final round's recovery carries through Reloading after the target
    // dies, then settles without producing a fresh pulse when loading ends.
    f.chaff.kill(target);
    f.chaff.compact();
    f.step(6);
    CHECK(f.system.effects().shots.empty());
    CHECK(f.swarm.magazine[0].seconds_since_shot == Catch::Approx(previous_age + 6.0f * kDt));
    f.step(66);
    CHECK(f.swarm.magazine[0].phase == MagazinePhase::Loaded);
    CHECK(f.swarm.magazine[0].seconds_since_shot == 1.0f);
    CHECK(f.system.effects().shots.empty());
}

TEST_CASE("a bomber whose target leaves its aggro radius switches to a closer one",
          "[swarm][sim][bomber][retarget]") {
    Fixture f;
    const usize a = f.add_chaff(Vec2{46.0f, 40.0f}, 1.0e6f);
    const usize b = f.add_chaff(Vec2{40.0f, 49.0f}, 1.0e6f);
    // Spawned at rest so it does not reach A before A moves.
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kBomber);
    f.step(1);
    REQUIRE(f.swarm.target_index[0] == a);
    f.chaff.pos_x[a] = 70.0f;   // 30 out: beyond the 12-unit search radius
    f.step(1);
    REQUIRE(f.swarm.target_index[0] == b);
}

TEST_CASE("shooters released together hold a rank across their approach", "[swarm][sim][shooter][squad]") {
    Fixture f;
    SwarmerProfile squad = f.swarm.profile_at(kShooter);
    squad.lifetime = 30.0f;
    squad.formation_spacing = 1.5f;
    f.swarm.set_profile(kShooter, squad);
    f.add_chaff(Vec2{50.0f, 40.0f}, 1.0e6f);
    for (u16 k = 0; k < 4; ++k) {
        // Inside the 12-unit search radius, so they aggro at once.
        f.add_swarmer(Vec2{40.0f, 40.0f + 0.2f * static_cast<f32>(k)}, Vec2{10.0f, 0.0f}, kShooter,
                      /*group=*/1u, /*slot=*/k);
    }
    f.step(300);
    REQUIRE(f.swarm.count() == 4);
    // All four engaged, on a line across the approach (x ~ constant), in
    // slot order along it, at the profile's spacing.
    f32 ys[4];
    f32 xmin = 1e9f, xmax = -1e9f;
    for (usize i = 0; i < 4; ++i) {
        INFO("slot " << f.swarm.slot[i] << " at " << f.swarm.pos_x[i] << "," << f.swarm.pos_y[i]
             << " flags " << int(f.swarm.flags[i]));
        REQUIRE((f.swarm.flags[i] & swarmer_flags::kAttached) != 0);
        ys[f.swarm.slot[i]] = f.swarm.pos_y[i];
        xmin = math::min(xmin, f.swarm.pos_x[i]);
        xmax = math::max(xmax, f.swarm.pos_x[i]);
    }
    INFO("x span " << xmax - xmin << ", ys " << ys[0] << " " << ys[1] << " " << ys[2] << " " << ys[3]);
    // The rank bends onto the hold circle, so the wings sit a little
    // forward of the centre pair.
    REQUIRE(xmax - xmin < 2.0f);
    for (int k = 1; k < 4; ++k) {
        REQUIRE(ys[k] > ys[k - 1]);
        REQUIRE(ys[k] - ys[k - 1] == Catch::Approx(1.5f).margin(0.6f));
    }
}

FlowField make_flow_toward(Vec2 goal);

TEST_CASE("an arbor grabber volley forms a lane wall: a rank across the flow, not across its approach",
          "[swarm][sim][arbor_grabber][squad][wall]") {
    Fixture f;
    // The horde walks +x through this spot. The volley comes in from below
    // and behind, so a rank across its APPROACH would be tilted ~30 degrees
    // off vertical and span ~4 in x; a rank across the FLOW is vertical.
    const FlowField flow = make_flow_toward(Vec2{120.0f, 40.0f});
    f.flow = &flow;
    for (int k = 0; k < 24; ++k) {
        f.add_chaff(Vec2{52.0f + 0.05f * static_cast<f32>(k), 40.0f + 0.03f * static_cast<f32>(k % 5)}, 1.0e6f);
    }
    for (u16 k = 0; k < 3; ++k) {
        f.add_swarmer(Vec2{30.0f, 28.0f + 0.2f * static_cast<f32>(k)}, Vec2{10.0f, 5.0f}, kArbor,
                      /*group=*/1u, /*slot=*/k);
    }
    f.step(240);
    REQUIRE(f.swarm.count() == 3);
    f32 ys[3];
    f32 xmin = 1e9f, xmax = -1e9f;
    for (usize i = 0; i < 3; ++i) {
        INFO("slot " << f.swarm.slot[i] << " at " << f.swarm.pos_x[i] << "," << f.swarm.pos_y[i]);
        ys[f.swarm.slot[i]] = f.swarm.pos_y[i];
        xmin = math::min(xmin, f.swarm.pos_x[i]);
        xmax = math::max(xmax, f.swarm.pos_x[i]);
    }
    INFO("x span " << xmax - xmin << ", ys " << ys[0] << " " << ys[1] << " " << ys[2]);
    // Athwart the lane: one x, spread in y at the profile's spacing, in slot
    // order. And ANCHORED short of the horde at 70% of reach, not sat on it.
    REQUIRE(xmax - xmin < 2.0f);
    for (int k = 1; k < 3; ++k) {
        REQUIRE(ys[k] > ys[k - 1]);
        REQUIRE(ys[k] - ys[k - 1] == Catch::Approx(4.0f).margin(1.0f));
    }
    REQUIRE(xmax < 52.0f);
    REQUIRE(xmin > 52.0f - 6.0f);
}

TEST_CASE("a body_block unit shoves an overlapping pathogen out of itself instead of yielding",
          "[swarm][sim][arbor_grabber][bodies][wall]") {
    // Body 1.6 + chaff 0.5 = 2.1 of contact; the pathogen is planted 1.0 in.
    auto run = [](f32 body_block) {
        Fixture f;
        SwarmerProfile pr = f.swarm.profile_at(kArbor);
        pr.body_block = body_block;
        f.swarm.set_profile(kArbor, pr);
        f.add_chaff(Vec2{41.0f, 40.0f}, 1.0e6f);
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kArbor);
        f.step(1);
        struct Out { f32 chaff_x; f32 unit_x; u32 blocks; };
        return Out{f.chaff.pos_x[0], f.swarm.pos_x[0], f.system.last_stats().body_blocks};
    };
    // The ordinary one-way contact: the pathogen never moves, the unit does.
    const auto yields = run(0.0f);
    REQUIRE(yields.chaff_x == Catch::Approx(41.0f));
    REQUIRE(yields.unit_x < 39.6f);
    REQUIRE(yields.blocks == 0);
    // The wall: the pathogen is pushed out along the contact normal, the
    // unit holds its ground.
    const auto wall = run(1.0f);
    REQUIRE(wall.chaff_x > 41.5f);
    REQUIRE(wall.unit_x == Catch::Approx(40.0f).margin(0.1f));
    REQUIRE(wall.blocks == 1);
}

TEST_CASE("an arbor arm captures and pulls a configurable nearby chaff chunk",
          "[swarm][sim][arbor_grabber][cluster]") {
    Fixture f;
    SwarmerProfile pr = f.swarm.profile_at(kArbor);
    pr.arbor_arm_count = 1;
    pr.arbor_max_captives = 3;
    pr.arbor_cluster_radius = 2.0f;
    pr.arbor_extend_seconds = 0.001f;
    pr.arbor_latch_seconds = 0.001f;
    pr.arbor_pull_seconds = 0.50f;
    f.swarm.set_profile(kArbor, pr);

    const usize lead = f.add_chaff(Vec2{45.0f, 40.0f}, 100.0f);
    const usize follower_a = f.add_chaff(Vec2{46.0f, 40.5f}, 100.0f);
    const usize follower_b = f.add_chaff(Vec2{45.4f, 39.6f}, 100.0f);
    const usize outside = f.add_chaff(Vec2{48.0f, 40.0f}, 100.0f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kArbor);

    // Select, extend, then latch. The fourth enemy is beyond the cluster
    // radius and remains visible even though the first three are captured.
    f.step(3);
    REQUIRE((f.chaff.flags[lead] & chaff_flags::kHidden) != 0);
    REQUIRE((f.chaff.flags[follower_a] & chaff_flags::kHidden) != 0);
    REQUIRE((f.chaff.flags[follower_b] & chaff_flags::kHidden) != 0);
    REQUIRE((f.chaff.flags[outside] & chaff_flags::kHidden) == 0);
    const ArborArmState& arm = f.swarm.arbor_grabber[0].arms[0];
    REQUIRE(arm.captive.chaff_count == 3u);

    const Vec2 lead_before = f_pos(f, lead);
    const Vec2 follower_before = f_pos(f, follower_a);
    // The main arm begins pulling while its smaller branches reach for the
    // followers, which hold their positions until those branches make contact.
    f.step(5);
    REQUIRE(f_pos(f, lead).x < lead_before.x);
    REQUIRE(f_pos(f, follower_a).x == Catch::Approx(follower_before.x).margin(0.001f));
    REQUIRE(f_pos(f, follower_a).y == Catch::Approx(follower_before.y).margin(0.001f));
    f.step(10);
    const Vec2 lead_delta = f_pos(f, lead) - lead_before;
    const Vec2 follower_delta = f_pos(f, follower_a) - follower_before;
    // Once contacted, each captive converges toward the macrophage from its
    // own starting position instead of translating as a rigid clump.
    REQUIRE(follower_delta.x < 0.0f);
    REQUIRE(follower_delta.y < 0.0f);
    REQUIRE(follower_delta.x != Catch::Approx(lead_delta.x).margin(0.001f));

    u32 swallowed = 0;
    for (int tick = 0; tick < 60; ++tick) {
        f.step(1);
        swallowed += f.system.last_stats().hosts_finished;
    }
    REQUIRE(swallowed == 3u);
    REQUIRE(f.chaff.count() == 1);
    REQUIRE(f.chaff.density[0] == Catch::Approx(100.0f));
}

TEST_CASE("macrophages ignore spores when seeking and gathering prey",
          "[swarm][sim][arbor_grabber][spore]") {
    Fixture f;
    SwarmerProfile pr = f.swarm.profile_at(kArbor);
    pr.arbor_arm_count = 1;
    pr.arbor_max_captives = 3;
    pr.arbor_cluster_radius = 3.0f;
    pr.arbor_extend_seconds = 0.001f;
    pr.arbor_latch_seconds = 0.001f;
    f.swarm.set_profile(kArbor, pr);

    const usize spore = f.add_chaff(Vec2{42.0f, 40.0f}, 100.0f);
    f.chaff.family[spore] = static_cast<u8>(PathogenFamily::Spore);
    const usize prey = f.add_chaff(Vec2{44.0f, 40.0f}, 100.0f);
    NamedTarget named_spore = f.make_named(42u, Vec2{41.0f, 40.0f}, 100.0f);
    named_spore.family = static_cast<u8>(PathogenFamily::Spore);
    std::vector<NamedTarget> named{named_spore};
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kArbor);

    f.step(3, nullptr, &named);
    const ArborArmState& arm = f.swarm.arbor_grabber[0].arms[0];
    REQUIRE(arm.phase == ArborArmPhase::Pulling);
    REQUIRE(arm.captive.chaff_count == 1u);
    CHECK(arm.captive.chaff[0].handle.index == prey);
    CHECK((f.chaff.flags[spore] & chaff_flags::kHidden) == 0);
    CHECK(named[0].health == Catch::Approx(100.0f));
}

TEST_CASE("a lone spore is ignored by macrophages but targeted by other defenders",
          "[swarm][sim][arbor_grabber][spore]") {
    Fixture f;
    const usize spore = f.add_chaff(Vec2{44.0f, 40.0f}, 100.0f);
    f.chaff.family[spore] = static_cast<u8>(PathogenFamily::Spore);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kArbor);
    f.add_swarmer(Vec2{40.0f, 41.0f}, Vec2{}, kLatch);

    f.step(1);
    CHECK_FALSE(f.swarm.target(0).valid());
    CHECK_FALSE(f.swarm.target_named[0].valid());
    CHECK(f.swarm.arbor_grabber[0].arms[0].phase == ArborArmPhase::Idle);
    REQUIRE(f.swarm.target(1).valid());
    CHECK(f.swarm.target(1).index == spore);
}

// ---------------------------------------------------------------------------
// Bombers
// ---------------------------------------------------------------------------

TEST_CASE("a bomber detonates on contact and asks for its effect, killing itself",
          "[swarm][sim][bomber]") {
    struct Case { u16 profile; TowerType source; };
    const Case cases[] = {{kBomber, TowerType::Count}, {kMucus, TowerType::GobletCell}};
    for (const Case& c : cases) {
        INFO("profile " << c.profile);
        Fixture f;
        f.add_chaff(Vec2{40.0f, 40.0f}, 1.0e6f);
        f.add_swarmer(Vec2{34.0f, 40.0f}, Vec2{14.0f, 0.0f}, c.profile);
        CombatEventSink sink;
        sink.reserve(256);

        bool detonated = false;
        Vec2 where{};
        for (int i = 0; i < 120 && !detonated; ++i) {
            f.step(1, &sink);
            const SwarmerEffects& fx = f.system.effects();
            if (c.profile == kBomber && !fx.bursts.empty()) { detonated = true; where = fx.bursts[0].origin; }
            if (c.profile == kMucus && !fx.splashes.empty()) { detonated = true; where = fx.splashes[0].origin; }
        }
        REQUIRE(detonated);
        // At the target, and gone afterwards.
        REQUIRE(math::length(where - Vec2{40.0f, 40.0f}) <= f.swarm.profile_at(c.profile).attach_radius + 0.6f);
        REQUIRE(f.swarm.count() == 0);
        REQUIRE(f.system.last_stats().detonated == 1);
        // The kernel itself touched nothing: the effect is SimWorld's to land.
        REQUIRE(f.chaff.density[0] == 1.0e6f);

        bool boom = false;
        for (const CombatEvent& e : sink.events()) {
            if (e.type == CombatEventType::Explosion && e.source == c.source &&
                (e.visual_id & kSwarmerEventBit) != 0) boom = true;
        }
        REQUIRE(boom);
    }
}

TEST_CASE("goblet cells skip coated prey and abandon a target coated during the chase",
          "[swarm][sim][mucus]") {
    for (const bool on_named : {false, true}) {
        INFO("named target " << on_named);
        Fixture f;
        SwarmerCollisionTuning off;
        off.enabled = false;
        f.system.set_collision(off);
        SwarmerProfile p = f.swarm.profile_at(kMucus);
        p.speed = 0.0f;
        p.chase_seconds = 0.5f;
        f.swarm.set_profile(kMucus, p);
        std::vector<NamedTarget> agents;
        if (on_named) {
            agents = {f.make_named(11u, Vec2{45.0f, 40.0f}, 1000.0f),
                      f.make_named(12u, Vec2{49.0f, 40.0f}, 1000.0f)};
            agents[0].mucus_slowed = true;
        } else {
            f.add_chaff(Vec2{45.0f, 40.0f}, 1000.0f, chaff_flags::kSlowed);
            f.chaff.slow_remaining[0] = 3.0f;
            f.add_chaff(Vec2{49.0f, 40.0f}, 1000.0f);
        }
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
        f.step(1, nullptr, &agents);
        REQUIRE(f.swarm.count() == 1);
        if (on_named) REQUIRE(f.swarm.target_named[0] == agents[1].id);
        else REQUIRE(f.swarm.target_index[0] == 1u);

        // Another goblet coats the held quarry just before this one's fuse
        // would run out. Switch back to the now untreated nearer target.
        f.swarm.chase[0] = p.chase_seconds - kDt * 0.5f;
        if (on_named) {
            agents[0].mucus_slowed = false;
            agents[1].mucus_slowed = true;
        } else {
            f.chaff.expire_slows(3.0f);
            f.chaff.flags[1] |= chaff_flags::kSlowed;
            f.chaff.slow_remaining[1] = 3.0f;
        }
        f.step(1, nullptr, &agents);
        REQUIRE(f.system.effects().splashes.empty());
        REQUIRE(f.swarm.count() == 1);
        if (on_named) REQUIRE(f.swarm.target_named[0] == agents[0].id);
        else REQUIRE(f.swarm.target_index[0] == 0u);
        REQUIRE(f.swarm.chase[0] == Catch::Approx(kDt));
        REQUIRE(f.system.last_stats().chase_timeouts == 0);
    }
}

TEST_CASE("a goblet waits for the existing mucus slow to expire before engaging",
          "[swarm][sim][mucus]") {
    for (const bool on_named : {false, true}) {
        INFO("named target " << on_named);
        Fixture f;
        std::vector<NamedTarget> agents;
        if (on_named) {
            agents = {f.make_named(11u, Vec2{40.2f, 40.0f}, 1000.0f)};
            agents[0].mucus_slowed = true;
        } else {
            f.add_chaff(Vec2{40.2f, 40.0f}, 1000.0f, chaff_flags::kSlowed);
            f.chaff.slow_remaining[0] = 0.1f;
        }
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
        f.step(1, nullptr, &agents);
        REQUIRE(f.system.effects().splashes.empty());
        REQUIRE(f.swarm.count() == 1);
        REQUIRE_FALSE(f.swarm.target(0).valid());
        REQUIRE_FALSE(f.swarm.target_named[0].valid());

        if (on_named) agents[0].mucus_slowed = false;
        else f.chaff.expire_slows(0.1f);
        f.step(1, nullptr, &agents);
        REQUIRE(f.system.effects().splashes.size() == 1);
        REQUIRE(f.swarm.count() == 0);
    }
}

TEST_CASE("goblet cells share same-tick splash coverage while still coating separate prey",
          "[swarm][sim][mucus]") {
    for (const bool on_named : {false, true}) {
        INFO("named target " << on_named);
        Fixture f;
        SwarmerCollisionTuning off;
        off.enabled = false;
        f.system.set_collision(off);
        std::vector<NamedTarget> agents;
        if (on_named) {
            agents = {f.make_named(11u, Vec2{40.2f, 40.0f}, 1000.0f),
                      f.make_named(12u, Vec2{60.2f, 40.0f}, 1000.0f)};
        } else {
            f.add_chaff(Vec2{40.2f, 40.0f}, 1000.0f);
            f.add_chaff(Vec2{60.2f, 40.0f}, 1000.0f);
        }
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
        f.add_swarmer(Vec2{60.0f, 40.0f}, Vec2{}, kMucus);
        f.step(1, nullptr, &agents);
        REQUIRE(f.system.effects().splashes.size() == 2);
        REQUIRE(f.system.last_stats().detonated == 2);
        REQUIRE(f.swarm.count() == 1);
    }
}

TEST_CASE("a pending goblet splash only reserves prey in its family mask",
          "[swarm][sim][mucus]") {
    Fixture f;
    f.add_chaff(Vec2{40.2f, 40.0f}, 1000.0f);
    f.add_chaff(Vec2{40.2f, 40.0f}, 1000.0f);
    f.chaff.family[1] = static_cast<u8>(PathogenFamily::Bacteria);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
    f.swarm.family_mask[0] = static_cast<u8>(1u << static_cast<u8>(PathogenFamily::Virus));
    f.swarm.family_mask[1] = static_cast<u8>(1u << static_cast<u8>(PathogenFamily::Bacteria));
    f.step(1);
    REQUIRE(f.system.effects().splashes.size() == 2);
    REQUIRE(f.swarm.count() == 0);
}

TEST_CASE("goblet body contacts ignore coated enemies and share queued splash coverage",
          "[swarm][sim][mucus][bodies]") {
    for (const bool on_named : {false, true}) {
        for (const bool already_coated : {false, true}) {
            INFO("named target " << on_named << ", coated " << already_coated);
            Fixture f;
            SwarmerProfile p = f.swarm.profile_at(kMucus);
            p.size = 2.0f;
            p.search_radius = 0.0f;
            f.swarm.set_profile(kMucus, p);
            // Outside attach reach and no acquisition radius: only the
            // membrane collision pass can trigger these goblets.
            std::vector<NamedTarget> agents;
            if (on_named) {
                agents = {f.make_named(11u, Vec2{41.2f, 41.2f}, 1000.0f)};
                agents[0].mucus_slowed = already_coated;
            } else {
                f.add_chaff(Vec2{41.2f, 41.2f}, 1000.0f,
                            already_coated ? chaff_flags::kSlowed : 0);
                if (already_coated) f.chaff.slow_remaining[0] = 3.0f;
            }
            f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
            f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
            f.step(1, nullptr, &agents);
            const usize expected = already_coated ? 0u : 1u;
            REQUIRE(f.system.effects().splashes.size() == expected);
            REQUIRE(f.system.last_stats().contact_detonations == expected);
            REQUIRE(f.swarm.count() == 2u - expected);
        }
    }
}

TEST_CASE("an expiring goblet only splashes when untreated prey is close enough to benefit",
          "[swarm][sim][mucus]") {
    for (const int target : {0, 1, 2}) {
        INFO("target: none=0, coated=1, untreated=2: " << target);
        Fixture f;
        if (target != 0) {
            f.add_chaff(Vec2{40.2f, 40.0f}, 1000.0f,
                        target == 1 ? chaff_flags::kSlowed : 0);
            if (target == 1) f.chaff.slow_remaining[0] = 3.0f;
        }
        SwarmerProfile p = f.swarm.profile_at(kMucus);
        p.lifetime = kDt * 0.5f;
        f.swarm.set_profile(kMucus, p);
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
        f.step(1);
        REQUIRE(f.system.effects().splashes.size() == (target == 2 ? 1u : 0u));
        REQUIRE(f.swarm.count() == 0);
    }
}

TEST_CASE("a goblet chase timeout waits until its splash can reach untreated prey",
          "[swarm][sim][mucus][chase]") {
    for (const bool on_named : {false, true}) {
        INFO("named target " << on_named);
        Fixture f;
        SwarmerCollisionTuning off;
        off.enabled = false;
        f.system.set_collision(off);
        SwarmerProfile p = f.swarm.profile_at(kMucus);
        p.speed = 0.0f;
        p.chase_seconds = kDt;
        p.attach_radius = 0.1f;
        f.swarm.set_profile(kMucus, p);
        std::vector<NamedTarget> agents;
        if (on_named) {
            agents = {f.make_named(11u, Vec2{40.2f, 40.0f}, 1000.0f),
                      f.make_named(12u, Vec2{50.0f, 40.0f}, 1000.0f)};
            agents[0].mucus_slowed = true;
            agents[1].radius = 0.2f;
        } else {
            f.add_chaff(Vec2{40.2f, 40.0f}, 1000.0f, chaff_flags::kSlowed);
            f.chaff.slow_remaining[0] = 3.0f;
            f.add_chaff(Vec2{50.0f, 40.0f}, 1000.0f);
        }
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
        f.step(3, nullptr, &agents);
        REQUIRE(f.system.effects().splashes.empty());
        REQUIRE(f.swarm.count() == 1);
        REQUIRE(f.swarm.chase[0] >= p.chase_seconds);
        REQUIRE(f.system.last_stats().chase_timeouts == 0);

        // Untreated prey enters splash range, still outside attach reach.
        // The elapsed fuse can now deliver mucus instead of wasting it.
        if (on_named) agents[1].position.x = 41.0f;
        else f.chaff.pos_x[1] = 41.0f;
        f.step(1, nullptr, &agents);
        REQUIRE(f.system.effects().splashes.size() == 1);
        REQUIRE(f.system.last_stats().chase_timeouts == 1);
        REQUIRE(f.swarm.count() == 0);
    }
}

TEST_CASE("an expiring goblet checks mucus coverage at a named enemy's center",
          "[swarm][sim][mucus][named]") {
    for (const bool center_in_range : {false, true}) {
        INFO("center in splash range " << center_in_range);
        Fixture f;
        SwarmerProfile p = f.swarm.profile_at(kMucus);
        p.lifetime = kDt * 0.5f;
        f.swarm.set_profile(kMucus, p);
        std::vector<NamedTarget> agents{
            f.make_named(11u, Vec2{center_in_range ? 41.0f : 42.0f, 40.0f}, 1000.0f)};
        agents[0].radius = 2.0f;
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, kMucus);
        f.step(1, nullptr, &agents);
        REQUIRE(f.system.effects().splashes.size() == (center_in_range ? 1u : 0u));
        REQUIRE(f.swarm.count() == 0);
    }
}

TEST_CASE("mucus coating does not hide prey from other swarmer kinds",
          "[swarm][sim][mucus]") {
    for (const u16 profile : {kLatch, kShooter, kBomber}) {
        INFO("profile " << profile);
        Fixture f;
        f.add_chaff(Vec2{40.2f, 40.0f}, 1000.0f, chaff_flags::kSlowed);
        f.chaff.slow_remaining[0] = 3.0f;
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{}, profile);
        f.step(1);
        if (profile == kBomber) {
            REQUIRE(f.system.effects().bursts.size() == 1);
            REQUIRE(f.swarm.count() == 0);
        } else {
            REQUIRE(f.swarm.count() == 1);
            REQUIRE(f.swarm.target(0).valid());
            REQUIRE(f.swarm.target_index[0] == 0u);
        }
    }
}

TEST_CASE("a bomber gives a chase only so long, then detonates where it is",
          "[swarm][sim][bomber][chase]") {
    Fixture f;
    SwarmerProfile short_fuse = f.swarm.profile_at(kBomber);
    short_fuse.chase_seconds = 0.5f;
    f.swarm.set_profile(kBomber, short_fuse);
    // A target inside the aggro radius that keeps exactly the bomber's pace,
    // so the chase never closes.
    const usize a = f.add_chaff(Vec2{48.0f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kBomber);
    f.step(1);
    REQUIRE(f.swarm.target_index[0] == a);

    bool detonated = false;
    int ticks = 0;
    for (; ticks < 120 && !detonated; ++ticks) {
        f.chaff.pos_x[a] += 14.0f * kDt;
        f.step(1);
        detonated = !f.system.effects().bursts.empty();
    }
    REQUIRE(detonated);
    // Half a second of chase, give or take the tick it acquired on.
    REQUIRE(ticks >= 28);
    REQUIRE(ticks <= 34);
    REQUIRE(f.system.last_stats().chase_timeouts == 1);
    // ...and it went off short of the target, not on it.
    const SwarmerBurst& b = f.system.effects().bursts[0];
    REQUIRE(math::length(b.origin - Vec2{f.chaff.pos_x[a], 40.0f}) > 2.0f);
    REQUIRE(f.swarm.count() == 0);

    // Off (<= 0), the same bomber chases for as long as it lives.
    Fixture g;
    SwarmerProfile hound = g.swarm.profile_at(kBomber);
    hound.chase_seconds = 0.0f;
    g.swarm.set_profile(kBomber, hound);
    const usize t = g.add_chaff(Vec2{48.0f, 40.0f}, 1.0e6f);
    g.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kBomber);
    for (int i = 0; i < 120; ++i) {
        g.chaff.pos_x[t] += 14.0f * kDt;
        g.step(1);
        REQUIRE(g.system.effects().bursts.empty());
    }
    REQUIRE(g.swarm.count() == 1);
}

TEST_CASE("a bomber that runs out of lifetime detonates where it stands", "[swarm][sim][bomber]") {
    Fixture f;   // no chaff at all
    f.add_swarmer(Vec2{20.0f, 20.0f}, Vec2{0.0f, 0.0f}, kBomber);
    bool detonated = false;
    for (int i = 0; i < 200 && !detonated; ++i) {
        f.step(1);
        if (!f.system.effects().bursts.empty()) {
            detonated = true;
            const SwarmerBurst& b = f.system.effects().bursts[0];
            REQUIRE(math::length(b.origin - Vec2{20.0f, 20.0f}) < 0.5f);
            REQUIRE(b.radius == Catch::Approx(3.0f));
            REQUIRE(b.damage == Catch::Approx(25.0f));
        }
    }
    REQUIRE(detonated);
    REQUIRE(f.swarm.count() == 0);
    REQUIRE(f.system.last_stats().expired == 0);
}

TEST_CASE("a bomber pressing against the world edge survives without detonating", "[swarm][sim][bomber][bounds]") {
    Fixture f;
    f.add_swarmer(Vec2{127.5f, 64.0f}, Vec2{60.0f, 0.0f}, kBomber);
    CombatEventSink sink;
    sink.reserve(64);
    f.step(3, &sink);
    REQUIRE(f.swarm.count() == 1);
    check_body_inside(f.swarm, 0, kBounds);
    REQUIRE(f.swarm.health[0] > 0.0f);
    REQUIRE(f.system.last_stats().expired == 0);
    REQUIRE(f.system.effects().bursts.empty());
    // Reaching the edge is a movement constraint rather than a death.
    for (const CombatEvent& e : sink.events()) REQUIRE(e.type != CombatEventType::SwarmerDeath);
}

TEST_CASE("friendly cells ignore off-board prey when choosing their nearest target",
          "[swarm][sim][bounds][targeting]") {
    // kArbor is the exception: its arms reach past the edge (tested below).
    for (const u16 profile : {kLatch, kShooter, kBomber, kMucus}) {
        for (const bool on_named : {false, true}) {
            for (const bool inside_prey : {false, true}) {
                INFO("profile " << profile << ", named " << on_named << ", inside prey " << inside_prey);
                Fixture f;
                f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
                SwarmerCollisionTuning off;
                off.enabled = false;
                f.system.set_collision(off);
                SwarmerProfile p = f.swarm.profile_at(profile);
                p.speed = 0.0f;
                p.search_radius = 12.0f;
                p.attach_radius = 0.1f;
                f.swarm.set_profile(profile, p);
                std::vector<NamedTarget> agents;
                // Both enemies remain in the spatial hash / named snapshot.
                // The nearer one is beyond the playable area's left edge.
                if (on_named) {
                    agents.push_back(f.make_named(11u, Vec2{31, 64}, 1000.0f));
                    if (inside_prey) agents.push_back(f.make_named(12u, Vec2{40, 64}, 1000.0f));
                } else {
                    f.add_chaff(Vec2{31, 64}, 1000.0f);
                    if (inside_prey) f.add_chaff(Vec2{40, 64}, 1000.0f);
                }
                f.add_swarmer(Vec2{33, 64}, Vec2{}, profile);
                f.step(1, nullptr, &agents);
                REQUIRE(f.swarm.count() == 1);
                if (!inside_prey) {
                    CHECK_FALSE(f.swarm.target(0).valid());
                    CHECK_FALSE(f.swarm.target_named[0].valid());
                } else if (on_named) {
                    CHECK(f.swarm.target_named[0] == agents[1].id);
                } else {
                    CHECK(f.swarm.target(0).valid());
                    CHECK(f.swarm.target_index[0] == 1u);
                }
                CHECK(f.system.effects().bursts.empty());
                CHECK(f.system.effects().splashes.empty());
            }
        }
    }
}

TEST_CASE("a held enemy crossing the playable edge is dropped for an on-board target",
          "[swarm][sim][bounds][targeting]") {
    for (const u16 profile : {kLatch, kShooter, kBomber, kMucus}) {
        for (const bool on_named : {false, true}) {
            INFO("profile " << profile << ", named " << on_named);
            Fixture f;
            f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
            SwarmerCollisionTuning off;
            off.enabled = false;
            f.system.set_collision(off);
            SwarmerProfile p = f.swarm.profile_at(profile);
            p.speed = 0.0f;
            p.attach_radius = 0.1f;
            f.swarm.set_profile(profile, p);
            std::vector<NamedTarget> agents;
            if (on_named) {
                agents = {f.make_named(11u, Vec2{36, 64}, 1000.0f),
                          f.make_named(12u, Vec2{42, 64}, 1000.0f)};
            } else {
                f.add_chaff(Vec2{36, 64}, 1000.0f);
                f.add_chaff(Vec2{42, 64}, 1000.0f);
            }
            f.add_swarmer(Vec2{33, 64}, Vec2{}, profile);
            f.step(1, nullptr, &agents);
            REQUIRE(f.swarm.count() == 1);
            if (on_named) REQUIRE(f.swarm.target_named[0] == agents[0].id);
            else REQUIRE(f.swarm.target_index[0] == 0u);

            if (on_named) agents[0].position.x = 31.0f;
            else f.chaff.pos_x[0] = 31.0f;
            f.step(1, nullptr, &agents);
            REQUIRE(f.swarm.count() == 1);
            if (on_named) CHECK(f.swarm.target_named[0] == agents[1].id);
            else CHECK(f.swarm.target_index[0] == 1u);
            CHECK(f.system.last_stats().hosts_finished == 0);
            CHECK(f.system.last_stats().kill_pulses == 0);
            check_body_inside(f.swarm, 0, f.playable_bounds);
        }
    }
}

TEST_CASE("an arbor arm reaches off-board prey while its body stays on the board",
          "[swarm][sim][bounds][arbor_grabber]") {
    for (const bool on_named : {false, true}) {
        INFO("named " << on_named);
        Fixture f;
        f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
        SwarmerCollisionTuning off;
        off.enabled = false;
        f.system.set_collision(off);
        SwarmerProfile p = f.swarm.profile_at(kArbor);
        p.speed = 0.0f;
        p.attach_radius = 6.0f;
        p.arbor_arm_count = 1;
        p.arbor_max_captives = 1;
        p.arbor_extend_seconds = 0.001f;
        p.arbor_latch_seconds = 0.001f;
        p.arbor_pull_seconds = 0.05f;
        f.swarm.set_profile(kArbor, p);
        // The only prey sits past the left edge, inside the arm's reach.
        std::vector<NamedTarget> agents;
        if (on_named) agents = {f.make_named(11u, Vec2{30, 64}, 50.0f)};
        else f.add_chaff(Vec2{30, 64}, 50.0f);
        f.add_swarmer(Vec2{34, 64}, Vec2{}, kArbor);
        f.step(3, nullptr, &agents);
        REQUIRE(f.swarm.count() == 1);
        const ArborArmState& arm = f.swarm.arbor_grabber[0].arms[0];
        REQUIRE(arm.phase == ArborArmPhase::Pulling);
        if (on_named) CHECK(arm.captive.named == agents[0].id);
        else CHECK(arm.captive.chaff_count == 1u);

        u32 swallowed = 0;
        for (int tick = 0; tick < 20; ++tick) {
            f.step(1, nullptr, &agents);
            REQUIRE(f.swarm.count() == 1);
            swallowed += f.system.last_stats().hosts_finished;
            check_body_inside(f.swarm, 0, f.playable_bounds);
        }
        CHECK(swallowed == 1u);
        if (on_named) CHECK(agents[0].health <= 0.0f);
        else CHECK(f.chaff.count() == 0);
    }
}

TEST_CASE("an arbor cluster swallows a follower near the edge",
          "[swarm][sim][bounds][arbor_grabber][cluster]") {
    Fixture f;
    f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
    SwarmerCollisionTuning off;
    off.enabled = false;
    f.system.set_collision(off);
    SwarmerProfile p = f.swarm.profile_at(kArbor);
    p.size = 2.6f;
    p.speed = 0.0f;
    p.attach_radius = 6.0f;
    p.arbor_arm_count = 1;
    p.arbor_max_captives = 2;
    p.arbor_cluster_radius = 2.4f;
    p.arbor_extend_seconds = 0.001f;
    p.arbor_latch_seconds = 0.001f;
    p.arbor_pull_seconds = 0.1f;
    p.heal_per_kill = 7.0f;
    f.swarm.set_profile(kArbor, p);
    const usize lead_slot = f.add_chaff(Vec2{36.35f, 64}, 100.0f);
    const ChaffHandle lead{static_cast<u32>(lead_slot), f.chaff.generation[lead_slot]};
    f.add_swarmer(Vec2{33.95f, 64}, Vec2{}, kArbor);
    f.swarm.size_scale[0] = 0.75f;  // Full radius 1.95, touching the playable edge.
    f.step(1);
    REQUIRE(f.swarm.arbor_grabber[0].arms[0].phase == ArborArmPhase::Extending);

    // The follower is close to the edge and remains captured while its own
    // side branch pulls it toward the macrophage.
    const usize follower_slot = f.add_chaff(Vec2{34.0f, 64}, 100.0f);
    const ChaffHandle follower{static_cast<u32>(follower_slot), f.chaff.generation[follower_slot]};
    f.step(2);
    REQUIRE(f.swarm.arbor_grabber[0].arms[0].phase == ArborArmPhase::Pulling);
    REQUIRE(f.swarm.arbor_grabber[0].arms[0].captive.chaff_count == 2u);
    u32 swallowed = 0;
    f32 healed = 0.0f;
    f32 density_removed = 0.0f;
    for (int tick = 0; tick < 20; ++tick) {
        f.step(1);
        REQUIRE(f.swarm.count() == 1);
        swallowed += f.system.last_stats().hosts_finished;
        healed += f.system.last_stats().healed;
        density_removed += f.system.last_stats().density_removed;
        check_body_inside(f.swarm, 0, f.playable_bounds);
    }
    CHECK(f.chaff.resolve(lead) == ChaffBuffers::npos);
    CHECK(f.chaff.resolve(follower) == ChaffBuffers::npos);
    CHECK(f.chaff.count() == 0);
    CHECK(swallowed == 2u);
    CHECK(healed == Catch::Approx(2.0f * p.heal_per_kill));
    CHECK(density_removed == Catch::Approx(200.0f));
}

TEST_CASE("an attached latcher releases a live host that leaves the board without a kill pulse",
          "[swarm][sim][bounds][latch]") {
    for (const bool on_named : {false, true}) {
        INFO("named " << on_named);
        Fixture f;
        f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
        SwarmerProfile p = f.swarm.profile_at(kLatch);
        p.kill_pulse_radius = 4.0f;
        p.kill_pulse_damage = 100.0f;
        f.swarm.set_profile(kLatch, p);
        std::vector<NamedTarget> agents;
        if (on_named) {
            agents = {f.make_named(11u, Vec2{34.1f, 64}, 1000.0f),
                      f.make_named(12u, Vec2{40, 64}, 1000.0f)};
        } else {
            f.add_chaff(Vec2{34.1f, 64}, 1000.0f);
            f.add_chaff(Vec2{40, 64}, 1000.0f);
        }
        f.add_swarmer(Vec2{34, 64}, Vec2{}, kLatch);
        f.swarm.flags[0] |= swarmer_flags::kPersistent;
        f.step(1, nullptr, &agents);
        REQUIRE(f.any_attached());
        const f32 host_health = on_named ? agents[0].health : f.chaff.density[0];
        REQUIRE(host_health < 1000.0f);
        if (on_named) agents[0].position.x = 31.0f;
        else f.chaff.pos_x[0] = 31.0f;
        f.step(1, nullptr, &agents);
        REQUIRE(f.swarm.count() == 1);
        CHECK_FALSE(f.any_attached());
        if (on_named) {
            CHECK(f.swarm.target_named[0] == agents[1].id);
            CHECK(agents[0].health == Catch::Approx(host_health));
        } else {
            CHECK(f.swarm.target_index[0] == 1u);
            CHECK(f.chaff.density[0] == Catch::Approx(host_health));
        }
        CHECK(f.system.last_stats().hosts_finished == 0);
        CHECK(f.system.last_stats().kill_pulses == 0);
        CHECK(f.system.effects().bursts.empty());
        CHECK((f.swarm.flags[0] & swarmer_flags::kPersistent) != 0);
        check_body_inside(f.swarm, 0, f.playable_bounds);
    }
}

TEST_CASE("all cell kinds keep their complete body inside each arena edge and recover alive",
          "[swarm][sim][bounds]") {
    constexpr u16 builder_slot = 6;
    const Vec2 directions[] = {Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}};
    for (const u16 profile : {kLatch, kShooter, kBomber, kMucus, kArbor, builder_slot}) {
        for (Vec2 outward : directions) {
            for (const bool already_outside : {false, true}) {
                INFO("profile " << profile << ", outward " << outward.x << "," << outward.y
                     << ", initially outside " << already_outside);
                Fixture f;
                f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
                SwarmerProfile p = f.swarm.profile_at(profile == builder_slot ? kLatch : profile);
                if (profile == builder_slot) p.kind = SwarmerKind::Builder;
                p.size = 2.0f;
                p.speed = 0.0f;
                f.swarm.set_profile(profile, p);
                const Vec2 position = f.playable_bounds.center() + outward * (already_outside ? 35.0f : 29.0f);
                f.add_swarmer(position, outward * 300.0f, profile);
                f.swarm.flags[0] |= swarmer_flags::kPersistent;
                f.swarm.size_scale[0] = 1.5f;
                const u32 generation = f.swarm.generation[0];
                const f32 health = f.swarm.health[0];
                CombatEventSink sink;
                sink.reserve(64);
                for (int tick = 0; tick < 8; ++tick) {
                    f.step(1, &sink);
                    REQUIRE(f.swarm.count() == 1);
                    check_body_inside(f.swarm, 0, f.playable_bounds);
                    CHECK(f.swarm.vel_x[0] * outward.x + f.swarm.vel_y[0] * outward.y <= 1.0e-4f);
                }
                CHECK(f.swarm.generation[0] == generation);
                CHECK(f.swarm.health[0] == health);
                CHECK((f.swarm.flags[0] & swarmer_flags::kPersistent) != 0);
                CHECK(f.system.last_stats().expired == 0);
                CHECK(f.system.effects().bursts.empty());
                CHECK(f.system.effects().splashes.empty());
                for (const CombatEvent& event : sink.events()) CHECK(event.type != CombatEventType::SwarmerDeath);
            }
        }
    }
}

TEST_CASE("kiting and body separation cannot push cells through the arena boundary",
          "[swarm][sim][bounds][bodies]") {
    const Vec2 directions[] = {Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}};
    for (Vec2 outward : directions) {
        for (const bool kiting : {false, true}) {
            INFO("outward " << outward.x << "," << outward.y << ", kiting " << kiting);
            Fixture f;
            f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
            SwarmerProfile p = f.swarm.profile_at(kShooter);
            p.size = 2.0f;
            p.speed = 30.0f;
            p.attach_radius = 8.0f;
            p.kite_fraction = 1.0f;
            f.swarm.set_profile(kShooter, p);
            const Vec2 position = f.playable_bounds.center() + outward * 30.0f;
            if (kiting) {
                f.add_chaff(position - outward * 3.0f, 1000.0f);
                f.add_swarmer(position, Vec2{}, kShooter);
                f.swarm.flags[0] |= swarmer_flags::kPersistent;
                u32 kite_ticks = 0;
                for (int tick = 0; tick < 30; ++tick) {
                    f.step(1);
                    REQUIRE(f.swarm.count() == 1);
                    kite_ticks += f.system.last_stats().kiting;
                    check_body_inside(f.swarm, 0, f.playable_bounds);
                }
                CHECK(kite_ticks > 0);
            } else {
                f.add_swarmer(position, Vec2{}, kShooter);
                f.add_swarmer(position - outward * 0.2f, Vec2{}, kShooter);
                f.step(1);
                REQUIRE(f.swarm.count() == 2);
                CHECK(f.system.last_stats().friendly_contacts > 0);
                check_body_inside(f.swarm, 0, f.playable_bounds);
                check_body_inside(f.swarm, 1, f.playable_bounds);
            }
        }
    }
}

TEST_CASE("the final arena correction also contains a cell projected outward by a tissue wall",
          "[swarm][sim][bounds][walls]") {
    const Vec2 directions[] = {Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}};
    for (Vec2 outward : directions) {
        INFO("outward " << outward.x << "," << outward.y);
        Fixture f;
        f.playable_bounds = Rect{Vec2{32, 32}, Vec2{64, 64}};
        TissueMask mask;
        mask.resize(64, 64, 1.0f, Vec2{16, 16});
        for (i32 y = 0; y < mask.height(); ++y) {
            for (i32 x = 0; x < mask.width(); ++x) {
                const Vec2 cell = mask.cell_to_world(x, y);
                const bool walkable = outward.x < 0 ? cell.x < 32.0f : outward.x > 0 ? cell.x > 64.0f :
                                      outward.y < 0 ? cell.y < 32.0f : cell.y > 64.0f;
                mask.set_walkable(x, y, walkable);
            }
        }
        DistanceField sdf;
        sdf.bake(mask);
        f.sdf = &sdf;
        SwarmerProfile p = f.swarm.profile_at(kShooter);
        p.size = 1.0f;
        p.speed = 0.0f;
        f.swarm.set_profile(kShooter, p);
        f.add_swarmer(f.playable_bounds.center() + outward * 15.0f, Vec2{}, kShooter);
        f.step(1);
        REQUIRE(f.swarm.count() == 1);
        CHECK(f.system.last_stats().wall_contacts > 0);
        check_body_inside(f.swarm, 0, f.playable_bounds);
    }
}

TEST_CASE("an oversized body or degenerate arena produces a finite live recovery position",
          "[swarm][sim][bounds]") {
    Fixture f;
    SECTION("the body is larger than the entire arena") {
        f.playable_bounds = Rect{Vec2{44, 44}, Vec2{48, 48}};
    }
    SECTION("the arena has zero extent") {
        f.playable_bounds = Rect{Vec2{46, 46}, Vec2{46, 46}};
    }
    SwarmerProfile p = f.swarm.profile_at(kShooter);
    p.size = 20.0f;
    p.speed = 0.0f;
    f.swarm.set_profile(kShooter, p);
    f.add_swarmer(Vec2{20, 20}, Vec2{100, -100}, kShooter);
    f.swarm.flags[0] |= swarmer_flags::kPersistent;
    f.step(1);
    REQUIRE(f.swarm.count() == 1);
    check_body_inside(f.swarm, 0, f.playable_bounds);
    CHECK(f.swarm.pos_x[0] == Catch::Approx(46.0f));
    CHECK(f.swarm.pos_y[0] == Catch::Approx(46.0f));
    CHECK(f.swarm.health[0] > 0.0f);
}

TEST_CASE("a persistent builder reaches an edge site without moving its body outside the arena",
          "[swarm][sim][bounds][builder]") {
    constexpr u16 builder_slot = 6;
    const Vec2 directions[] = {Vec2{-1, 0}, Vec2{1, 0}, Vec2{0, -1}, Vec2{0, 1}};
    for (Vec2 outward : directions) {
        INFO("outward " << outward.x << "," << outward.y);
        Fixture f;
        f.playable_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
        SwarmerProfile p = f.swarm.profile_at(kLatch);
        p.kind = SwarmerKind::Builder;
        p.source = TowerType::Fibroblast;
        p.size = 1.6f;
        p.attach_radius = 0.8f;
        f.swarm.set_profile(builder_slot, p);
        // The authored site is valid, but the full body cannot put its centre
        // within attach_radius of it. Arrival must use a reachable approach.
        const Vec2 goal = f.playable_bounds.center() + outward * 31.8f;
        SwarmerSpawnParams builder;
        builder.position = goal - outward * 8.0f;
        builder.profile = builder_slot;
        builder.owner = EntityId{55u};
        builder.goal = goal;
        builder.has_goal = true;
        builder.persistent = true;
        REQUIRE(f.swarm.spawn(builder));
        bool built = false;
        for (int tick = 0; tick < 600 && !built; ++tick) {
            f.step(1);
            REQUIRE(f.swarm.count() == 1);
            check_body_inside(f.swarm, 0, f.playable_bounds);
            if (f.system.effects().builds.empty()) continue;
            REQUIRE(f.system.effects().builds.size() == 1);
            const SwarmerBuild& request = f.system.effects().builds[0];
            CHECK(request.origin.x == Catch::Approx(goal.x));
            CHECK(request.origin.y == Catch::Approx(goal.y));
            CHECK(request.owner == builder.owner);
            CHECK(request.source == TowerType::Fibroblast);
            CHECK(request.profile == builder_slot);
            CHECK(request.slot == 0u);
            const Vec2 body{f.swarm.pos_x[0], f.swarm.pos_y[0]};
            CHECK(math::length(body - goal) > p.attach_radius);
            built = true;
        }
        REQUIRE(built);
        CHECK((f.swarm.flags[0] & swarmer_flags::kPersistent) != 0);
        CHECK((f.swarm.flags[0] & swarmer_flags::kHasGoal) == 0);
        CHECK(f.swarm.health[0] > 0.0f);
    }
}

TEST_CASE("SimWorld retains off-board spawns in sim bounds while friendlies obey the playable arena",
          "[swarm][sim][bounds][world]") {
    SimDesc desc;
    desc.world_bounds = Rect{Vec2{32, 32}, Vec2{96, 96}};
    desc.sim_bounds = kBounds;
    desc.max_chaff = 32;
    desc.max_swarmers = 8;
    desc.swarmer_collision.enabled = false;
    for (auto& family : desc.chaff_tuning.family) {
        family.max_speed = 0.0f;
        family.acceleration = 0.0f;
        family.jitter = 0.0f;
    }
    SimWorld world;
    world.init(desc, nullptr);
    world.tissue().resize(128, 128, 1.0f, Vec2{});
    for (i32 y = 0; y < 128; ++y)
        for (i32 x = 0; x < 128; ++x) world.tissue().set_walkable(x, y, true);
    world.sdf().bake(world.tissue());
    ChaffSpawnParams prey;
    prey.position = Vec2{31, 64};
    prey.density = 1000.0f;
    REQUIRE(world.chaff().spawn(prey).valid());
    prey.position = Vec2{40, 64};
    REQUIRE(world.chaff().spawn(prey).valid());
    SwarmerProfile profile;
    profile.kind = SwarmerKind::Shooter;
    profile.size = 2.0f;
    profile.speed = 0.0f;
    profile.search_radius = 12.0f;
    profile.attach_radius = 4.0f;
    world.swarmers().set_profile(kShooter, profile);
    SwarmerSpawnParams cell;
    cell.profile = kShooter;
    cell.position = Vec2{33, 64};
    cell.persistent = true;
    REQUIRE(world.swarmers().spawn(cell));
    cell.position = Vec2{12, 80};
    REQUIRE(world.swarmers().spawn(cell));
    for (int tick = 0; tick < 8; ++tick) {
        world.tick();
        REQUIRE(world.swarmers().count() == 2);
        REQUIRE(world.chaff().count() == 2);
        CHECK(world.swarmers().target_index[0] == 1u);
        for (usize i = 0; i < world.swarmers().count(); ++i)
            check_body_inside(world.swarmers(), i, desc.world_bounds);
    }
    CHECK(world.chaff().pos_x[0] < desc.world_bounds.min.x);
    CHECK(world.chaff().density[0] == Catch::Approx(1000.0f));
    CHECK(world.snapshot().swarmers_killed_total == 0);
}

// ---------------------------------------------------------------------------
// Death bursts
// ---------------------------------------------------------------------------

TEST_CASE("every kind of swarmer raises one death burst when it dies, however it dies",
          "[swarm][sim][death]") {
    // The VFX layer draws a unit bursting apart off SwarmerDeath
    // (CombatEvents.h), so a kind or a cause that forgot to raise it would
    // simply vanish.
    constexpr u16 kBuilder = 6;
    struct Case {
        const char* name;
        u16 profile;
        TowerType source;
        bool target;   ///< Put an unkillable pathogen in reach (bomber contact).
    };
    const Case cases[] = {
        {"latch expiry", kLatch, TowerType::CytotoxicT, false},
        {"shooter expiry", kShooter, TowerType::Neutrophil, false},
        {"arbor expiry", kArbor, TowerType::Macrophage, false},
        {"bomber expiry", kBomber, TowerType::Count, false},
        {"bomber contact", kBomber, TowerType::Count, true},
        {"mucus contact", kMucus, TowerType::GobletCell, true},
        {"builder spent on its scar", kBuilder, TowerType::Fibroblast, false},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        Fixture f;
        SwarmerProfile builder = f.swarm.profile_at(kLatch);
        builder.kind = SwarmerKind::Builder;
        builder.source = TowerType::Fibroblast;
        builder.attach_radius = 0.8f;
        f.swarm.set_profile(kBuilder, builder);
        SwarmerProfile p = f.swarm.profile_at(c.profile);
        p.lifetime = 0.5f;
        f.swarm.set_profile(c.profile, p);

        if (c.target) f.add_chaff(Vec2{44.0f, 40.0f}, 1.0e6f);
        if (c.profile == kBuilder) {
            SwarmerSpawnParams s;
            s.position = Vec2{40.0f, 40.0f};
            s.profile = kBuilder;
            s.owner = EntityId{7u};
            s.goal = Vec2{44.0f, 40.0f};
            s.has_goal = true;
            f.swarm.spawn(s);
        } else {
            f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{6.0f, 0.0f}, c.profile);
        }

        CombatEventSink sink;
        sink.reserve(4096);
        u32 built = 0;
        for (int t = 0; t < 60; ++t) {   // one second: twice the lifetime
            f.step(1, &sink);
            built += f.system.last_stats().built;
        }
        REQUIRE(f.swarm.count() == 0);
        // The builder case must actually exercise the spent-on-arrival path.
        if (c.profile == kBuilder) REQUIRE(built == 1);

        u32 deaths = 0;
        for (const CombatEvent& e : sink.events()) {
            REQUIRE(e.type != CombatEventType::ProjectileExpired);   // no more fizzles
            if (e.type != CombatEventType::SwarmerDeath) continue;
            ++deaths;
            CHECK(e.source == c.source);
            CHECK((e.visual_id & kSwarmerEventBit) != 0);
            CHECK(e.radius == Catch::Approx(f.swarm.profile_at(c.profile).size));
            CHECK(e.magnitude >= 0.0f);
            CHECK(math::length(e.direction) == Catch::Approx(1.0f));
            CHECK(math::length(e.origin - Vec2{40.0f, 40.0f}) < 8.0f);
        }
        REQUIRE(deaths == 1);
    }
}

// ---------------------------------------------------------------------------
// Named agents and hidden targets
// ---------------------------------------------------------------------------

TEST_CASE("every kind goes at a named agent when it is the nearest thing", "[swarm][sim][named]") {
    // Latch drains, shooter hits directly (its round is only the tracer),
    // bomber asks for a burst on it. No chaff anywhere.
    for (const u16 profile : {kLatch, kShooter, kBomber}) {
        INFO("profile " << profile);
        Fixture f;
        std::vector<NamedTarget> agents{f.make_named(42u, Vec2{40.0f, 40.0f}, 500.0f, /*armor=*/1.0f)};
        f.add_swarmer(Vec2{32.0f, 40.0f}, Vec2{14.0f, 0.0f}, profile);

        bool hit = false;
        for (int i = 0; i < 240 && !hit; ++i) {
            f.step(1, nullptr, &agents);
            if (profile == kBomber) hit = !f.system.effects().bursts.empty();
            else hit = agents[0].health < 500.0f;
        }
        REQUIRE(hit);
        if (profile == kBomber) {
            REQUIRE(f.system.effects().bursts[0].named_damage == Catch::Approx(30.0f));
        } else {
            REQUIRE(f.system.last_stats().named_damage > 0.0f);
        }
    }
}

TEST_CASE("a named target that dies releases its swarmer to look again", "[swarm][sim][named]") {
    Fixture f;
    std::vector<NamedTarget> agents{f.make_named(42u, Vec2{40.0f, 40.0f}, 2.0f)};
    f.add_chaff(Vec2{44.0f, 40.0f}, 4000.0f);
    const f32 chaff_before = f.chaff.density[0];
    SwarmerProfile p = f.swarm.profile_at(kLatch);
    p.lifetime = 8.0f;
    f.swarm.set_profile(kLatch, p);
    f.add_swarmer(Vec2{36.0f, 40.0f}, Vec2{14.0f, 0.0f});

    f.step(300, nullptr, &agents);
    REQUIRE(agents[0].health <= 0.0f);
    REQUIRE(f.chaff.density[0] < chaff_before);   // moved on to the chaff
}

TEST_CASE("hidden chaff is invisible to a swarmer", "[swarm][sim][hidden]") {
    Fixture f;
    const usize hidden = f.add_chaff(Vec2{40.0f, 40.0f}, 500.0f, chaff_flags::kHidden);
    f.add_swarmer(Vec2{38.0f, 40.0f}, Vec2{4.0f, 0.0f});
    f.step(120);
    REQUIRE_FALSE(f.any_attached());
    REQUIRE(f.chaff.density[hidden] == 500.0f);
    // Every tick it was searching, and never found anything.
    REQUIRE(f.system.last_stats().searching == 1);
}

// ---------------------------------------------------------------------------
// Kiting
// ---------------------------------------------------------------------------

/// An open field whose flow runs everywhere toward `goal`.
FlowField make_flow_toward(Vec2 goal) {
    TissueMask mask;
    const i32 w = static_cast<i32>(kBounds.size().x);
    const i32 h = static_cast<i32>(kBounds.size().y);
    mask.resize(w, h, 1.0f, kBounds.min);
    for (i32 y = 0; y < h; ++y)
        for (i32 x = 0; x < w; ++x) mask.set_walkable(x, y, true);
    FlowField flow;
    FlowFieldBakeDesc desc;
    desc.goals = {FlowGoal{mask.world_to_cell(goal)}};
    flow.bake(mask, desc);
    return flow;
}

TEST_CASE("a shooter backs away from a target that closes inside its kite radius, still firing",
          "[swarm][sim][shooter][kite]") {
    Fixture f;
    // Standoff 4, kite radius 3. The target sits 3.6 out: in range, not too
    // close. The shooter holds there and shoots.
    const usize host = f.add_chaff(Vec2{43.6f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    u32 shots_holding = 0;
    for (int t = 0; t < 30; ++t) {
        f.step(1);
        REQUIRE(f.system.last_stats().kiting == 0);
        shots_holding += f.system.last_stats().shots_fired;
    }
    REQUIRE(f.any_attached());
    REQUIRE(shots_holding > 0);

    // Walk the target onto it. The shooter gives ground, keeps firing, and
    // settles with the target no closer than the kite circle and no further
    // than the standoff.
    u32 shots_while_kiting = 0;
    u32 kite_ticks = 0;
    for (int t = 0; t < 90; ++t) {
        f.chaff.pos_x[host] -= 6.0f * kDt;
        f.step(1);
        if (f.system.last_stats().kiting > 0) {
            ++kite_ticks;
            shots_while_kiting += f.system.last_stats().shots_fired;
        }
    }
    REQUIRE(kite_ticks > 0);
    REQUIRE(shots_while_kiting > 0);
    const f32 gap = f.chaff.pos_x[host] - f.swarm.pos_x[0];
    // Gave ground. The retreat ramps to full speed two thirds of the way in,
    // so against a target closing at 6 u/s it settles a little inside the
    // 3.0 circle -- matching the pace, not fleeing.
    REQUIRE(gap > 2.4f);
    REQUIRE(gap <= 4.0f);    // and never lost it out of the standoff
    REQUIRE(f.swarm.pos_x[0] < 40.0f);   // retreated straight back, away from it
}

TEST_CASE("a stranger walking through the rank also drives a shooter back", "[swarm][sim][shooter][kite]") {
    Fixture f;
    const usize host = f.add_chaff(Vec2{43.7f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(30);
    REQUIRE(f.swarm.target_index[0] == host);
    REQUIRE(f.system.last_stats().kiting == 0);
    // Not its target, but inside the kite radius, off to the side.
    f.add_chaff(Vec2{40.0f, 41.5f}, 1.0e6f);
    f.step(1);
    REQUIRE(f.system.last_stats().kiting == 1);
    f.step(20);
    // Pushed away from the stranger (downward), and still on its target.
    REQUIRE(f.swarm.pos_y[0] < 39.5f);
}

TEST_CASE("the flow field bends a retreat toward where the horde is going", "[swarm][sim][shooter][kite][flow]") {
    // Two identical set-ups: the threat is directly to the shooter's left, so
    // "away" is +x. With a flow that runs +y everywhere, the retreat is the
    // diagonal; without, it is straight +x.
    auto run = [](const FlowField* flow) {
        Fixture f;
        f.flow = flow;
        f.add_chaff(Vec2{38.5f, 40.0f}, 1.0e6f);
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
        f.step(12);
        return Vec2{f.swarm.pos_x[0], f.swarm.pos_y[0]};
    };
    const FlowField flow = make_flow_toward(Vec2{40.0f, 120.0f});
    const Vec2 straight = run(nullptr);
    const Vec2 bent = run(&flow);
    REQUIRE(straight.x > 40.5f);
    REQUIRE(std::abs(straight.y - 40.0f) < 0.4f);
    REQUIRE(bent.x > 40.3f);
    REQUIRE(bent.y > 40.5f);
    // Same speed budget, split between the two directions.
    REQUIRE(bent.y > bent.x - 40.0f - 0.6f);
}

TEST_CASE("kite_speed_mult makes a shooter give ground faster than it takes it", "[swarm][sim][shooter][kite]") {
    // Threat parked well inside the kite radius: full urgency from tick one.
    // The retreat stops at the kite edge by design, so what the multiplier
    // buys is TIME to get there, not distance.
    auto ticks_to_clear = [](f32 mult) {
        Fixture f;
        SwarmerProfile pr = f.swarm.profile_at(kShooter);
        pr.kite_speed_mult = mult;
        f.swarm.set_profile(kShooter, pr);
        f.add_chaff(Vec2{39.5f, 40.0f}, 1.0e6f);
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
        for (int t = 1; t <= 120; ++t) {
            f.step(1);
            if (f.swarm.pos_x[0] - 39.5f > 2.5f) return t;
        }
        return 999;
    };
    const int plain = ticks_to_clear(1.0f);
    const int bolt = ticks_to_clear(1.5f);
    REQUIRE(plain < 999);
    REQUIRE(bolt < plain);
}

TEST_CASE("the flow may bend a retreat but never point it back at the threat", "[swarm][sim][shooter][kite][flow]") {
    // The shooter is UPSTREAM of the threat: the flow runs from it straight
    // into the thing it is fleeing. It must still back off along -x, not
    // shuffle sideways.
    Fixture f;
    const FlowField flow = make_flow_toward(Vec2{120.0f, 40.0f});
    f.flow = &flow;
    f.add_chaff(Vec2{41.5f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(12);
    REQUIRE(f.swarm.pos_x[0] < 39.0f);
    REQUIRE(std::abs(f.swarm.pos_y[0] - 40.0f) < 0.5f);
}

TEST_CASE("kite_fraction 0 disables kiting: the shooter holds its slot however close the target gets",
          "[swarm][sim][shooter][kite]") {
    Fixture f;
    SwarmerProfile brave = f.swarm.profile_at(kShooter);
    brave.kite_fraction = 0.0f;
    f.swarm.set_profile(kShooter, brave);
    const usize host = f.add_chaff(Vec2{43.0f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(30);
    for (int t = 0; t < 60; ++t) {
        f.chaff.pos_x[host] -= 6.0f * kDt;
        f.step(1);
        REQUIRE(f.system.last_stats().kiting == 0);
    }
    // The slot logic alone: it re-forms the rank at 80% of standoff on the
    // approach side, which is a slower, arrive-limited backpedal.
    REQUIRE(f.system.last_stats().attached == 1);
}

// ---------------------------------------------------------------------------
// Bodies (SwarmerCollisionTuning)
// ---------------------------------------------------------------------------

TEST_CASE("two overlapping swarmers push apart, half the overlap each", "[swarm][sim][bodies]") {
    Fixture f;
    // Shooters with nothing to shoot: no target, no steering, only bodies.
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.add_swarmer(Vec2{40.4f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(1);
    // size 0.5 -> body 0.4 each (kWallContactFraction): contact at 0.8, so
    // 0.4 of overlap, 0.2 a side.
    REQUIRE(f.swarm.pos_x[0] == Catch::Approx(39.8f).margin(1e-4f));
    REQUIRE(f.swarm.pos_x[1] == Catch::Approx(40.6f).margin(1e-4f));
    REQUIRE(f.swarm.pos_y[0] == Catch::Approx(40.0f).margin(1e-4f));
    REQUIRE(f.system.last_stats().friendly_contacts == 2);
    // Settled: nothing left to resolve next tick.
    f.step(1);
    REQUIRE(f.swarm.pos_x[0] == Catch::Approx(39.8f).margin(1e-4f));
    REQUIRE(f.system.last_stats().friendly_contacts == 0);
}

TEST_CASE("cells placed at identical coordinates push equally in opposite directions", "[swarm][sim][bodies]") {
    Fixture f;
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(1);
    const Vec2 a{f.swarm.pos_x[0], f.swarm.pos_y[0]};
    const Vec2 b{f.swarm.pos_x[1], f.swarm.pos_y[1]};
    REQUIRE(math::length(a - b) == Catch::Approx(0.8f).margin(1e-4f));
    REQUIRE((a.x + b.x) * 0.5f == Catch::Approx(40.0f));
    REQUIRE((a.y + b.y) * 0.5f == Catch::Approx(40.0f));
    REQUIRE(f.system.last_stats().friendly_contacts == 2);
}

TEST_CASE("directly deployed Cytotoxic T cells push apart while free", "[swarm][sim][bodies][latch]") {
    Fixture f;
    SwarmerProfile latch = f.swarm.profile_at(kLatch);
    latch.speed = 0.0f;
    f.swarm.set_profile(kLatch, latch);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kLatch);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kLatch);
    f.swarm.flags[0] |= swarmer_flags::kPersistent;
    f.swarm.flags[1] |= swarmer_flags::kPersistent;
    f.step(1);
    const Vec2 a{f.swarm.pos_x[0], f.swarm.pos_y[0]};
    const Vec2 b{f.swarm.pos_x[1], f.swarm.pos_y[1]};
    REQUIRE(math::length(a - b) == Catch::Approx(0.8f).margin(1e-4f));
    REQUIRE(f.system.last_stats().friendly_contacts == 2);
}

TEST_CASE("a stack of directly placed cells fans out", "[swarm][sim][bodies]") {
    Fixture f;
    for (int i = 0; i < 8; ++i)
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(20);
    for (usize i = 0; i < f.swarm.count(); ++i) {
        const Vec2 a{f.swarm.pos_x[i], f.swarm.pos_y[i]};
        REQUIRE(math::length(a - Vec2{40.0f, 40.0f}) > 0.05f);
        for (usize j = i + 1; j < f.swarm.count(); ++j) {
            const Vec2 b{f.swarm.pos_x[j], f.swarm.pos_y[j]};
            REQUIRE(math::length(a - b) > 0.1f);
        }
    }
}

TEST_CASE("the master switch reproduces the pre-collision swarm", "[swarm][sim][bodies]") {
    Fixture f;
    SwarmerCollisionTuning off;
    off.enabled = false;
    f.system.set_collision(off);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.add_swarmer(Vec2{40.4f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(5);
    REQUIRE(f.swarm.pos_x[0] == 40.0f);
    REQUIRE(f.swarm.pos_x[1] == 40.4f);
    REQUIRE(f.system.last_stats().friendly_contacts == 0);
}

TEST_CASE("latchers have no body: they stack, and nothing pushes off them", "[swarm][sim][bodies][latch]") {
    Fixture f;
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kLatch);
    f.add_swarmer(Vec2{40.4f, 40.0f}, Vec2{0.0f, 0.0f}, kLatch);
    f.add_swarmer(Vec2{40.2f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.step(1);
    REQUIRE(f.swarm.pos_x[0] == 40.0f);
    REQUIRE(f.swarm.pos_x[1] == 40.4f);
    REQUIRE(f.swarm.pos_x[2] == 40.2f);
    REQUIRE(f.system.last_stats().friendly_contacts == 0);
}

TEST_CASE("a swarmer is pushed out of a pathogen, and the pathogen does not move", "[swarm][sim][bodies]") {
    Fixture f;
    const usize c = f.add_chaff(Vec2{40.0f, 40.0f}, 100.0f);
    f.add_swarmer(Vec2{40.3f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    // Not a target for it, so the only thing moving the unit is the contact.
    f.swarm.family_mask[0] = 0;
    f.step(1);
    // Contact at body 0.4 + radius 0.5 = 0.9; overlap 0.6 at stiffness 0.6
    // is 0.36, within the 0.4 cap. The pathogen's slot is untouched.
    REQUIRE(f.swarm.pos_x[0] == Catch::Approx(40.66f).margin(1e-4f));
    REQUIRE(f.chaff.pos_x[c] == 40.0f);
    REQUIRE(f.system.last_stats().enemy_contacts == 1);
    REQUIRE(f.system.last_stats().friendly_contacts == 0);
    // Keeps squeezing out, converging on the rim: at 0.6 a tick the overlap
    // decays geometrically, so it is clear for every purpose but never
    // exactly zero.
    f.step(10);
    REQUIRE(f.swarm.pos_x[0] >= 40.0f + 0.9f - 1e-2f);
    REQUIRE(f.swarm.pos_x[0] <= 40.0f + 0.9f);
}

TEST_CASE("the family radius table sets the pathogen contact distance", "[swarm][sim][bodies]") {
    Fixture f;
    const f32 radii[kFamilyCount] = {2.0f, 2.0f};
    f.system.set_chaff_radii(radii, kFamilyCount);
    f.add_chaff(Vec2{40.0f, 40.0f}, 100.0f);
    f.add_swarmer(Vec2{41.5f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.swarm.family_mask[0] = 0;
    f.step(1);
    // Contact is now 0.4 + 2.0 = 2.4; the unit at 1.5 is inside and gets pushed.
    REQUIRE(f.swarm.pos_x[0] > 41.5f);
    REQUIRE(f.system.last_stats().enemy_contacts == 1);
}

TEST_CASE("a bomber that touches an enemy it was not chasing goes off there", "[swarm][sim][bodies][bomber]") {
    Fixture f;
    // Adopt a far target first, alone in the world.
    f.add_chaff(Vec2{50.0f, 40.0f}, 1.0e6f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{14.0f, 0.0f}, kBomber);
    f.step(1);
    REQUIRE(f.swarm.target(0).valid());
    REQUIRE(f.swarm.target_index[0] == 0);
    // Now drop a stranger straight in its path. It is nearer than the target
    // but the bomber keeps its target (the leash is the search radius) -- and
    // still goes off the moment its body meets the stranger's.
    const usize stranger = f.add_chaff(Vec2{43.0f, 40.0f}, 1.0e6f);
    bool detonated = false;
    Vec2 where{};
    for (int i = 0; i < 60 && !detonated; ++i) {
        f.step(1);
        if (!f.system.effects().bursts.empty()) {
            detonated = true;
            where = f.system.effects().bursts[0].origin;
        }
    }
    REQUIRE(detonated);
    REQUIRE(f.system.last_stats().contact_detonations == 1);
    REQUIRE(f.system.last_stats().detonated == 1);
    REQUIRE(f.swarm.count() == 0);
    // At the point of contact -- on the stranger's rim, not at the target
    // and not at the bomber's own centre.
    REQUIRE(math::length(where - f_pos(f, stranger)) <= 0.5f + 0.15f);
    REQUIRE(where.x < 45.0f);
}

TEST_CASE("a contact detonation lands on the membrane, toward the enemy", "[swarm][sim][bodies][bomber]") {
    Fixture f;
    // A fat bomber (body 1.6) parked with a pathogen just inside its fuse,
    // off to its upper right. No target adopted yet, so nothing steers it.
    SwarmerProfile fat = f.swarm.profile_at(kBomber);
    fat.size = 2.0f;
    fat.search_radius = 0.0f;
    f.swarm.set_profile(kBomber, fat);
    f.add_chaff(Vec2{41.2f, 41.2f}, 100.0f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kBomber);
    f.step(1);
    REQUIRE(f.system.last_stats().contact_detonations == 1);
    REQUIRE(f.system.effects().bursts.size() == 1);
    const Vec2 at = f.system.effects().bursts[0].origin;
    // 1.6 along the diagonal from the centre: (40 + 1.131, 40 + 1.131).
    REQUIRE(at.x == Catch::Approx(41.1314f).margin(1e-3f));
    REQUIRE(at.y == Catch::Approx(41.1314f).margin(1e-3f));
}

TEST_CASE("a bomber ignores the bodies of pathogens outside its family mask", "[swarm][sim][bodies][bomber]") {
    Fixture f;
    f.add_chaff(Vec2{40.3f, 40.0f}, 100.0f);
    f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kBomber);
    f.swarm.family_mask[0] = 0;
    f.step(1);
    // Pushed, not popped: it cannot hurt this family so it does not spend
    // itself on it.
    REQUIRE(f.swarm.count() == 1);
    REQUIRE(f.system.last_stats().contact_detonations == 0);
    REQUIRE(f.system.last_stats().enemy_contacts == 1);
    REQUIRE(f.swarm.pos_x[0] < 40.0f);
}

TEST_CASE("a swarmer is pushed off a named agent's rim", "[swarm][sim][bodies][named]") {
    Fixture f;
    std::vector<NamedTarget> agents{f.make_named(11u, Vec2{40.0f, 40.0f}, 1000.0f)};
    f.add_swarmer(Vec2{40.8f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
    f.swarm.family_mask[0] = 0;
    f.step(1, nullptr, &agents);
    // Contact at body 0.4 + radius 1.0 = 1.4; overlap 0.6 at 0.6 is 0.36.
    REQUIRE(f.swarm.pos_x[0] == Catch::Approx(41.16f).margin(1e-4f));
    REQUIRE(f.system.last_stats().enemy_contacts == 1);
}

TEST_CASE("the bodies pass is linear in swarmer count and bounded per unit", "[swarm][sim][bodies][perf]") {
    // The cost claim from the file header, measured: one capped walk of each
    // hash per non-Latch unit. A pessimistic layout -- shooters packed at
    // their own contact spacing, on top of a chaff crowd packed the same way,
    // so every walk is full and every unit is in contact on both sides.
    auto measure = [](usize n_swarm, int n_chaff, bool enabled) {
        Fixture f;
        SwarmerCollisionTuning t;
        t.enabled = enabled;
        f.system.set_collision(t);
        Rng r(4242);
        for (int i = 0; i < n_chaff; ++i) {
            f.add_chaff(Vec2{r.range_f(20.0f, 100.0f), r.range_f(20.0f, 60.0f)}, 1.0e9f);
        }
        for (usize i = 0; i < n_swarm; ++i) {
            f.add_swarmer(Vec2{r.range_f(20.0f, 100.0f), r.range_f(20.0f, 60.0f)}, Vec2{0.0f, 0.0f},
                          kShooter);
            // Standing off, not steering: a shooter with no valid family has
            // no target, so the pass is the only thing moving it.
            f.swarm.family_mask[i] = 0;
        }
        f64 best = 1.0e30;
        for (int k = 0; k < 30; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            f.step(1);
            const auto t1 = std::chrono::steady_clock::now();
            best = math::min(best, std::chrono::duration<f64, std::milli>(t1 - t0).count());
        }
        REQUIRE(f.swarm.count() == n_swarm);
        return best;
    };

    // The fixture's stores hold 4096 of each; the ratios are what matter.
    // Every unit here is targetless and so pays the search every tick as
    // well, which is why each size is measured with the pass off too: the
    // difference is the pass.
    struct Row { const char* label; usize units; int chaff; f64 off; f64 on; };
    Row rows[] = {
        {"1000 units / 1000 chaff", 1000, 1000, 0.0, 0.0},
        {"1000 units / 4000 chaff", 1000, 4000, 0.0, 0.0},
        {"3000 units / 1000 chaff", 3000, 1000, 0.0, 0.0},
        {"4000 units / 4000 chaff", 4000, 4000, 0.0, 0.0},
    };
    for (Row& r : rows) {
        r.off = measure(r.units, r.chaff, false);
        r.on = measure(r.units, r.chaff, true);
        std::printf("[swarm bodies] %s : off %.4f  on %.4f  pass %.4f ms/tick\n", r.label, r.off,
                    r.on, r.on - r.off);
    }
    const f64 a = math::max(rows[0].on - rows[0].off, 0.0);
    const f64 b = math::max(rows[1].on - rows[1].off, 0.0);
    const f64 c = math::max(rows[2].on - rows[2].off, 0.0);
    // Three times the units is at most ~three times the cost: the walk per
    // unit is capped, so the pass is linear in units. Wide margins, the same
    // as the fluid test: what is being ruled out is a pair sweep.
    REQUIRE(c < a * 6.0 + 0.1);
    // Four times the chaff makes each unit's chaff walk denser but no longer
    // than its cap, so it must not cost a multiple either.
    REQUIRE(b < a * 3.0 + 0.1);
}

// ---------------------------------------------------------------------------
// Store and determinism
// ---------------------------------------------------------------------------

TEST_CASE("the store is a hard wall and never grows", "[swarm][sim]") {
    Fixture f;
    for (int i = 0; i < 64; ++i) f.add_swarmer(Vec2{10.0f, 10.0f}, Vec2{1.0f, 0.0f});
    const usize cap = f.swarm.capacity();
    SwarmerBuffers small;
    small.reserve(4);
    for (int i = 0; i < 50; ++i) {
        SwarmerSpawnParams s;
        s.position = Vec2{1.0f, 1.0f};
        small.spawn(s);
    }
    REQUIRE(small.count() == 4);
    REQUIRE(small.capacity() == 4);
    REQUIRE(cap == 4096);
}

TEST_CASE("a swarmer takes its lifetime from its profile at spawn", "[swarm][sim]") {
    SwarmerBuffers b;
    b.reserve(8);
    SwarmerProfile p;
    p.lifetime = 2.5f;
    b.set_profile(3, p);
    SwarmerSpawnParams s;
    s.profile = 3;
    REQUIRE(b.spawn(s));
    REQUIRE(b.life[0] == Catch::Approx(2.5f));
    REQUIRE(b.profile_of(0).lifetime == Catch::Approx(2.5f));
    // An out-of-range slot falls back to slot 0 rather than reading garbage.
    s.profile = 999;
    REQUIRE(b.spawn(s));
    REQUIRE(b.profile[1] == 0);
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
            f.add_swarmer(Vec2{30.0f, 39.0f + static_cast<f32>(i) * 0.1f}, Vec2{14.0f, 0.5f},
                          static_cast<u16>(i % 5));
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
            f.add_swarmer(Vec2{30.0f, 38.0f + static_cast<f32>(i) * 0.12f}, Vec2{13.0f, 0.3f},
                          static_cast<u16>(i % 5));
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

TEST_CASE("swarmer spawn size jitter: each unit rolls its own body within +-jitter",
          "[swarmers][size]") {
    SwarmerBuffers sw;
    sw.reserve(1000);
    SwarmerProfile jittered;
    jittered.size = 0.5f;
    jittered.size_jitter = 0.1f;
    sw.set_profile(1, jittered);
    SwarmerProfile exact;
    exact.size = 0.5f;
    sw.set_profile(2, exact);   // size_jitter defaults to 0 in the sim
    f32 lo = 1.0f, hi = 0.0f;
    for (u32 k = 0; k < 1000; ++k) {
        SwarmerSpawnParams p;
        p.profile = (k % 2 == 0) ? 1 : 2;
        p.seed = k * 7919u;
        REQUIRE(sw.spawn(p));
        if (k % 2 == 0) {
            const f32 b = sw.body_size(k);
            REQUIRE(b >= 0.45f - 1e-6f);
            REQUIRE(b <= 0.55f + 1e-6f);
            lo = std::min(lo, b);
            hi = std::max(hi, b);
        } else {
            REQUIRE(sw.body_size(k) == 0.5f);
        }
    }
    REQUIRE(lo < 0.46f);
    REQUIRE(hi > 0.54f);

    // Carried through compaction with the unit.
    const u32 gen = sw.generation[998];
    const f32 body = sw.body_size(998);
    sw.kill(0);
    sw.compact();
    for (usize i = 0; i < sw.count(); ++i) {
        if (sw.generation[i] == gen) REQUIRE(sw.body_size(i) == body);
    }
}

TEST_CASE("Inflammation boosts shooter hits on named enemies", "[swarm][sim][shooter][inflammation]") {
    const auto damage_dealt = [](bool inside_zone) {
        Fixture f;
        SwarmerProfile shooter = f.swarm.profile_at(kShooter);
        shooter.speed = 0.0f;
        shooter.round_spread = 0.0f;
        shooter.volley_cone = 0.0f;
        f.swarm.set_profile(kShooter, shooter);
        f.add_swarmer(Vec2{40.0f, 40.0f}, Vec2{0.0f, 0.0f}, kShooter);
        std::vector<NamedTarget> agents{f.make_named(11u, Vec2{43.0f, 40.0f}, 1000.0f, 1.0f)};
        const std::vector<InflamedZone> zones{{inside_zone ? Vec2{40.0f, 40.0f} : Vec2{80.0f, 80.0f}, 5.0f}};
        f.system.set_inflamed_zones(&zones, 1.5f, 1.0f);
        f.step(60, nullptr, &agents);
        return 1000.0f - agents[0].health;
    };
    const f32 outside = damage_dealt(false);
    const f32 inside = damage_dealt(true);
    REQUIRE(outside > 0.0f);
    // Each shot does 3 damage, or 4.5 in the aura, before the target's 1 armor.
    CHECK(inside == Catch::Approx(outside * 3.5f / 2.0f).margin(0.001f));
}
