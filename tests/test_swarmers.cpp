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
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const Rect kBounds{Vec2{0.0f, 0.0f}, Vec2{128.0f, 128.0f}};

constexpr u16 kLatch = 0;
constexpr u16 kShooter = 1;
constexpr u16 kBomber = 2;
constexpr u16 kSlow = 3;
constexpr u16 kMucus = 4;

struct Fixture {
    ChaffBuffers chaff;
    SwarmerBuffers swarm;
    SwarmerSystem system;
    SpatialHash hash;
    NamedTargetList named;
    Rng rng{4242};

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

        SwarmerProfile latch = base;
        latch.kind = SwarmerKind::Latch;
        latch.source = TowerType::CytotoxicT;
        swarm.set_profile(kLatch, latch);

        SwarmerProfile shooter = base;
        shooter.kind = SwarmerKind::Shooter;
        shooter.source = TowerType::Neutrophil;
        shooter.attach_radius = 4.0f;   // the standoff
        shooter.fire_interval = 0.2f;
        shooter.round_damage = 3.0f;
        shooter.round_speed = 40.0f;
        swarm.set_profile(kShooter, shooter);

        SwarmerProfile bomber = base;
        bomber.kind = SwarmerKind::Bomber;
        bomber.source = TowerType::Macrophage;
        bomber.burst_radius = 3.0f;
        bomber.burst_damage = 25.0f;
        bomber.burst_named_damage = 30.0f;
        swarm.set_profile(kBomber, bomber);

        SwarmerProfile slow = base;
        slow.kind = SwarmerKind::SlowBomber;
        slow.source = TowerType::Interferon;
        swarm.set_profile(kSlow, slow);

        SwarmerProfile mucus = base;
        mucus.kind = SwarmerKind::MucusBomber;
        mucus.source = TowerType::GobletCell;
        swarm.set_profile(kMucus, mucus);
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
            system.update(swarm, chaff, hash, named, nullptr, kBounds, rng, kDt, sink);
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

// ---------------------------------------------------------------------------
// Bombers
// ---------------------------------------------------------------------------

TEST_CASE("a bomber detonates on contact and asks for its effect, killing itself",
          "[swarm][sim][bomber]") {
    struct Case { u16 profile; TowerType source; };
    const Case cases[] = {{kBomber, TowerType::Macrophage}, {kSlow, TowerType::Interferon},
                          {kMucus, TowerType::GobletCell}};
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
            if (c.profile == kSlow && !fx.zones.empty()) { detonated = true; where = fx.zones[0].origin; }
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

TEST_CASE("leaving the world is a plain dissolve even for a bomber", "[swarm][sim][bomber]") {
    Fixture f;
    f.add_swarmer(Vec2{127.5f, 64.0f}, Vec2{60.0f, 0.0f}, kBomber);
    f.step(3);
    REQUIRE(f.swarm.count() == 0);
    REQUIRE(f.system.effects().bursts.empty());
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
