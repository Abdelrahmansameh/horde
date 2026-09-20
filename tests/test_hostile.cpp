// Coverage for the horde fighting back (sim/hostile/HostileAttacks.h) and for
// what the game layer does with it (towers that can be destroyed).
//
// As with the swarmer tests, these check the properties the design rests on
// rather than exact balance numbers: a virus that touches a friendly grabs
// on, rides it, feeds on it and lets go when the host dies; a host carries
// only so many; a bacterium burns what stands in its aura and nothing
// outside it; a Latch granule cannot be climbed; a tower emptied by the horde
// is torn down and leaves the placed list; the whole thing is deterministic
// and switches off cleanly.
#include "sim/hostile/HostileAttacks.h"

#include "game/towers/TowerSystem.h"
#include "game/towers/TowerMechanics.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/ecs/Components.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const Rect kBounds{Vec2{0.0f, 0.0f}, Vec2{128.0f, 128.0f}};

constexpr u16 kShooter = 0;
constexpr u16 kLatch = 1;
constexpr u16 kBomber = 2;

constexpr f32 kVirusRadius = 0.9f;
constexpr f32 kBacteriaRadius = 2.0f;
constexpr f32 kSwarmerSize = 1.5f;
constexpr f32 kSwarmerBody = kSwarmerSize * kWallContactFraction;

/// A bare hostile pass over a chaff store, a swarmer store and a hash --
/// nothing else, which is what the layer promises to be testable with.
struct Fixture {
    ChaffBuffers chaff;
    SwarmerBuffers swarm;
    SpatialHash hash;
    HostileSystem hostile;
    FriendlyTowerList towers;
    CombatEventSink events;
    HostileTuning tuning;
    TissueMask mask; // default-constructed (width 0): containment check disabled

    Fixture() {
        chaff.reserve(1024);
        swarm.reserve(256);
        SpatialHashDesc hd;
        hd.bounds = kBounds;
        hd.cell_size = 4.0f;
        hash.configure(hd);
        events.reserve(1024);

        SwarmerProfile shooter;
        shooter.kind = SwarmerKind::Shooter;
        shooter.source = TowerType::Neutrophil;
        shooter.size = kSwarmerSize;
        shooter.max_health = 10.0f;
        swarm.set_profile(kShooter, shooter);

        SwarmerProfile latch = shooter;
        latch.kind = SwarmerKind::Latch;
        latch.source = TowerType::CytotoxicT;
        swarm.set_profile(kLatch, latch);

        SwarmerProfile bomber = shooter;
        bomber.kind = SwarmerKind::Bomber;
        bomber.source = TowerType::Count;
        swarm.set_profile(kBomber, bomber);

        const f32 radii[kFamilyCount] = {kVirusRadius, kBacteriaRadius};
        hostile.set_chaff_radii(radii, kFamilyCount);

        tuning.enabled = true;
        HostileFamilyParams& virus = tuning.family[static_cast<u32>(PathogenFamily::Virus)];
        virus.latch_dps = 6.0f;
        virus.latch_reach = 0.4f;
        virus.latch_cap_swarmer = 3;
        virus.latch_cap_tower = 8;
        virus.latch_speed = 30.0f;
        virus.latch_ease_distance = 1.2f;
        virus.latch_ease_power = 3.0f;
        HostileFamilyParams& bacteria = tuning.family[static_cast<u32>(PathogenFamily::Bacteria)];
        bacteria.aura_dps = 12.0f;
        bacteria.aura_radius = 4.0f;
        hostile.set_tuning(tuning);
    }

    usize add_chaff(Vec2 p, PathogenFamily family = PathogenFamily::Virus, u8 flags = 0) {
        ChaffSpawnParams c;
        c.position = p;
        c.density = 1.0f;
        c.family = family;
        c.flags = flags;
        chaff.spawn(c);
        return chaff.count() - 1;
    }

    usize add_swarmer(Vec2 p, u16 profile = kShooter) {
        SwarmerSpawnParams s;
        s.position = p;
        s.profile = profile;
        s.owner = EntityId{7u};
        s.seed = 0x1234u + static_cast<u32>(swarm.count()) * 7919u;
        swarm.spawn(s);
        return swarm.count() - 1;
    }

    void add_tower(u32 id, Vec2 p, f32 radius = 2.0f) {
        FriendlyTower t;
        t.id = EntityId{id};
        t.position = p;
        t.radius = radius;
        t.health = 100.0f;
        t.type = TowerType::Macrophage;
        towers.add(t);
        towers.sort();
    }

    HostileStats step() {
        hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr);
        for (usize k = 0; k < towers.items.size(); ++k) towers.damage[k] = 0.0f;
        return hostile.update(chaff, hash, swarm, towers, mask, kDt, &events);
    }

    bool latched(usize i) const { return (chaff.flags[i] & chaff_flags::kLatched) != 0; }
};

/// Distance at which a virus touching a swarmer of the fixture's size latches.
f32 virus_grab_distance() { return kVirusRadius + kSwarmerBody + 0.4f; }

u32 count_events(const CombatEventSink& sink, CombatEventType type) {
    u32 n = 0;
    for (const CombatEvent& e : sink.events()) n += e.type == type ? 1u : 0u;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// Latching
// ---------------------------------------------------------------------------

TEST_CASE("a virus touching a swarmer latches on, rides it and feeds on it", "[sim][hostile]") {
    Fixture f;
    const usize host = f.add_swarmer(Vec2{20.0f, 20.0f});
    f.swarm.health[host] = 1.0e6f;   // this test is about the ride, not the kill
    const usize v = f.add_chaff(Vec2{20.0f + virus_grab_distance() - 0.05f, 20.0f});
    const usize far = f.add_chaff(Vec2{20.0f + virus_grab_distance() + 1.0f, 20.0f});

    const HostileStats s = f.step();
    REQUIRE(s.latches_new == 1);
    REQUIRE(f.latched(v));
    REQUIRE_FALSE(f.latched(far));
    REQUIRE(f.chaff.host_kind[v] == host_kind::kSwarmer);
    REQUIRE(f.chaff.host_index[v] == host);
    REQUIRE(f.chaff.host_generation[v] == f.swarm.generation[host]);
    REQUIRE(count_events(f.events, CombatEventType::PathogenLatch) == 1);
    // Not teleported: the grab tick leaves it where it touched.
    REQUIRE(f.chaff.pos_x[v] == Catch::Approx(20.0f + virus_grab_distance() - 0.05f));
    REQUIRE(f.chaff.pos_y[v] == Catch::Approx(20.0f));

    // Then it LUNGES to its spot on the membrane: a straight run at the
    // spot (the distance to where it ends up only ever shrinks), settled on
    // the ring within a second.
    const f32 ring = kSwarmerBody + kVirusRadius * 0.35f;
    auto gap = [&](Vec2 host_pos) {
        return std::fabs(math::length(Vec2{f.chaff.pos_x[v], f.chaff.pos_y[v]} - host_pos) - ring);
    };
    std::vector<Vec2> path;
    for (int k = 0; k < 60; ++k) {
        f.step();
        path.push_back(Vec2{f.chaff.pos_x[v], f.chaff.pos_y[v]});
    }
    const Vec2 spot = path.back();
    f32 last = 1.0e9f;
    for (const Vec2& q : path) {
        const f32 d = math::length(q - spot);
        REQUIRE(d <= last + 1e-4f);
        last = d;
    }
    REQUIRE(gap(Vec2{20.0f, 20.0f}) < 1e-2f);
    // Jerky: full lunge speed (the fixture's 30 u/s) from the very first
    // tick, no ease-in -- three ticks cover three ticks' worth of the way --
    // and the tail is the ease-out: the last ten ticks together move it less
    // than one full-speed tick would.
    const f32 whole = math::length(Vec2{20.0f + virus_grab_distance() - 0.05f, 20.0f} - spot);
    REQUIRE(math::length(path[2] - spot) <= whole - 30.0f * kDt * 3.0f + 1e-3f);
    REQUIRE(math::length(path[49] - spot) < 30.0f * kDt);

    // The host jumps; the passenger lunges after it, re-settles on the
    // membrane, and keeps feeding the whole way.
    f.swarm.pos_x[host] = 40.0f;
    f.swarm.pos_y[host] = 33.0f;
    const f32 hp_before = f.swarm.health[host];
    const HostileStats s2 = f.step();
    REQUIRE(s2.latched == 1);
    REQUIRE(s2.latches_new == 0);
    REQUIRE(f.swarm.health[host] == Catch::Approx(hp_before - 6.0f * kDt));
    // 24 units at 30 u/s is ~50 ticks of lunge before the ease-out even
    // starts; give it two seconds to settle.
    for (int k = 0; k < 120; ++k) f.step();
    REQUIRE(gap(Vec2{40.0f, 33.0f}) < 1e-2f);

    // The host WALKS: the passenger is carried on its skin, not towed behind
    // it. The fixture moves the host by hand, the way the swarmer kernel
    // would, and the passenger's stored velocity is the host's.
    // The one tick of lag is the first: the carry runs on LAST tick's host
    // velocity, which was zero, so the first step of the walk opens a gap of
    // one step (0.2) that the settle then closes.
    f.swarm.vel_x[host] = 12.0f;
    for (int k = 0; k < 60; ++k) {
        f.swarm.pos_x[host] += 12.0f * kDt;
        f.step();
        REQUIRE(f.chaff.vel_x[v] == Catch::Approx(12.0f));
        REQUIRE(gap(Vec2{f.swarm.pos_x[host], 33.0f}) < 12.0f * kDt + 1e-3f);
    }
    REQUIRE(gap(Vec2{f.swarm.pos_x[host], 33.0f}) < 0.02f);
}

TEST_CASE("a drained swarmer dissolves and its passengers are let go", "[sim][hostile]") {
    Fixture f;
    const usize host = f.add_swarmer(Vec2{20.0f, 20.0f});
    const usize v = f.add_chaff(Vec2{20.0f + kSwarmerBody, 20.0f});
    f.swarm.health[host] = 6.0f * kDt * 1.5f;   // grabs, bites once, dies on the second

    f.step();
    f.step();
    REQUIRE((f.swarm.flags[host] & swarmer_flags::kPendingKill) == 0);
    const HostileStats s = f.step();
    REQUIRE(s.swarmers_killed == 1);
    REQUIRE((f.swarm.flags[host] & swarmer_flags::kPendingKill) != 0);
    REQUIRE(count_events(f.events, CombatEventType::SwarmerDeath) == 1);

    // Still flagged latched on the death tick; the next pass finds the host
    // dead and releases it where it stood, with no host-velocity carry-over.
    REQUIRE(f.latched(v));
    const HostileStats s2 = f.step();
    REQUIRE(s2.released == 1);
    REQUIRE_FALSE(f.latched(v));
    REQUIRE(f.chaff.host_kind[v] == host_kind::kNone);
    REQUIRE(f.chaff.vel_x[v] == 0.0f);

    // And a compacted-away host reads as gone through the generation check
    // even when a new unit moves into its slot.
    f.swarm.compact();
    REQUIRE(f.swarm.count() == 0);
    const usize fresh = f.add_swarmer(Vec2{90.0f, 90.0f});
    REQUIRE(fresh == host);
    REQUIRE_FALSE(f.swarm.alive_at(static_cast<u32>(host), f.chaff.host_generation[v]));
}

TEST_CASE("a host carries only as many passengers as its cap allows", "[sim][hostile]") {
    Fixture f;
    f.add_swarmer(Vec2{20.0f, 20.0f});
    const f32 d = kSwarmerBody;
    for (u32 k = 0; k < 6; ++k) {
        const f32 a = static_cast<f32>(k) * (math::kTwoPi / 6.0f);
        f.add_chaff(Vec2{20.0f + std::cos(a) * d, 20.0f + std::sin(a) * d});
    }
    const HostileStats s = f.step();
    REQUIRE(s.latches_new == 3);
    u32 latched = 0;
    for (usize i = 0; i < f.chaff.count(); ++i) latched += f.latched(i) ? 1u : 0u;
    REQUIRE(latched == 3);
    // The cap holds across ticks too: the three unlatched do not pile on later.
    const HostileStats s2 = f.step();
    REQUIRE(s2.latched == 3);
    REQUIRE(s2.latches_new == 0);
}

TEST_CASE("a Latch granule cannot be climbed, but still burns in an aura", "[sim][hostile]") {
    Fixture f;
    const usize granule = f.add_swarmer(Vec2{20.0f, 20.0f}, kLatch);
    const usize v = f.add_chaff(Vec2{20.0f + kSwarmerBody, 20.0f});
    f.add_chaff(Vec2{22.0f, 20.0f}, PathogenFamily::Bacteria);
    const HostileStats s = f.step();
    REQUIRE(s.latches_new == 0);
    REQUIRE_FALSE(f.latched(v));
    REQUIRE(s.aura_hits == 1);
    REQUIRE(f.swarm.health[granule] == Catch::Approx(10.0f - 12.0f * kDt));
}

TEST_CASE("burrowed and dying chaff neither latch nor burn", "[sim][hostile]") {
    Fixture f;
    const usize host = f.add_swarmer(Vec2{20.0f, 20.0f});
    f.add_chaff(Vec2{20.0f + kSwarmerBody, 20.0f}, PathogenFamily::Virus, chaff_flags::kHidden);
    const usize dying = f.add_chaff(Vec2{20.0f - kSwarmerBody, 20.0f});
    f.chaff.kill(dying);
    f.add_chaff(Vec2{21.0f, 21.0f}, PathogenFamily::Bacteria, chaff_flags::kHidden);
    const HostileStats s = f.step();
    REQUIRE(s.latches_new == 0);
    REQUIRE(s.aura_hits == 0);
    REQUIRE(f.swarm.health[host] == Catch::Approx(10.0f));
}

// ---------------------------------------------------------------------------
// Aura
// ---------------------------------------------------------------------------

TEST_CASE("a bacterium burns a swarmer inside its aura and nothing outside it", "[sim][hostile]") {
    Fixture f;
    const usize inside = f.add_swarmer(Vec2{20.0f, 20.0f});
    const usize edge = f.add_swarmer(Vec2{20.0f + 4.0f + kSwarmerBody + 0.05f, 20.0f});
    const usize outside = f.add_swarmer(Vec2{40.0f, 40.0f});
    f.add_chaff(Vec2{20.0f, 20.0f}, PathogenFamily::Bacteria);
    const HostileStats s = f.step();
    REQUIRE(s.aura_hits == 1);
    REQUIRE(f.swarm.health[inside] == Catch::Approx(10.0f - 12.0f * kDt));
    REQUIRE(f.swarm.health[edge] == Catch::Approx(10.0f));
    REQUIRE(f.swarm.health[outside] == Catch::Approx(10.0f));
    // Two bacteria, twice the burn: the aura stacks per agent.
    f.add_chaff(Vec2{21.0f, 20.0f}, PathogenFamily::Bacteria);
    const f32 before = f.swarm.health[inside];
    f.step();
    REQUIRE(f.swarm.health[inside] == Catch::Approx(before - 2.0f * 12.0f * kDt));
}

TEST_CASE("a bacterium never latches and a virus has no aura", "[sim][hostile]") {
    Fixture f;
    const usize host = f.add_swarmer(Vec2{20.0f, 20.0f});
    const usize b = f.add_chaff(Vec2{20.0f + kSwarmerBody, 20.0f}, PathogenFamily::Bacteria);
    f.add_chaff(Vec2{20.0f + kSwarmerBody + 3.0f, 20.0f}, PathogenFamily::Virus);
    const HostileStats s = f.step();
    REQUIRE(s.latches_new == 0);
    REQUIRE_FALSE(f.latched(b));
    // Only the bacterium's aura landed; the virus at 3 units is out of grab
    // reach and has no aura to contribute.
    REQUIRE(s.aura_hits == 1);
    REQUIRE(f.swarm.health[host] == Catch::Approx(10.0f - 12.0f * kDt));
}

// ---------------------------------------------------------------------------
// Towers (the flat list; the ECS half is below)
// ---------------------------------------------------------------------------

TEST_CASE("a virus latches onto a tower and the damage is queued for the ECS", "[sim][hostile]") {
    Fixture f;
    f.add_tower(42u, Vec2{30.0f, 30.0f}, 2.0f);
    const usize v = f.add_chaff(Vec2{32.0f, 30.0f});
    f.add_chaff(Vec2{29.0f, 31.0f}, PathogenFamily::Bacteria);
    const HostileStats s = f.step();
    REQUIRE(s.latches_new == 1);
    REQUIRE(f.chaff.host_kind[v] == host_kind::kTower);
    REQUIRE(f.chaff.host_index[v] == 42u);
    REQUIRE(f.towers.passengers[0] == 1);
    REQUIRE(f.towers.damage[0] == Catch::Approx(12.0f * kDt));   // aura only: the bite starts next tick
    const HostileStats s2 = f.step();
    REQUIRE(s2.tower_damage == Catch::Approx(12.0f * kDt + 6.0f * kDt));
    REQUIRE(f.towers.damage[0] == Catch::Approx(12.0f * kDt + 6.0f * kDt));
    for (int k = 0; k < 60; ++k) f.step();
    const f32 d = math::length(Vec2{f.chaff.pos_x[v], f.chaff.pos_y[v]} - Vec2{30.0f, 30.0f});
    REQUIRE(d == Catch::Approx(2.0f + kVirusRadius * 0.35f).margin(1e-2f));

    // The tower is gone from the list (destroyed): the passenger is dropped.
    f.towers.clear();
    const HostileStats s3 = f.step();
    REQUIRE(s3.released == 1);
    REQUIRE_FALSE(f.latched(v));
}

TEST_CASE("switching the pass off lets every passenger go and does nothing else", "[sim][hostile]") {
    Fixture f;
    const usize host = f.add_swarmer(Vec2{20.0f, 20.0f});
    const usize v = f.add_chaff(Vec2{20.0f + kSwarmerBody, 20.0f});
    f.add_chaff(Vec2{21.0f, 20.0f}, PathogenFamily::Bacteria);
    f.step();
    REQUIRE(f.latched(v));
    HostileTuning off = f.tuning;
    off.enabled = false;
    f.hostile.set_tuning(off);
    const f32 hp = f.swarm.health[host];
    const HostileStats s = f.step();
    REQUIRE(s.released == 1);
    REQUIRE(s.aura_hits == 0);
    REQUIRE_FALSE(f.latched(v));
    REQUIRE(f.swarm.health[host] == Catch::Approx(hp));
}

// ---------------------------------------------------------------------------
// Cost
// ---------------------------------------------------------------------------

TEST_CASE("the hostile pass is linear in friendlies and bounded per unit", "[sim][hostile][perf]") {
    // The cost claim from the file header, measured: one capped walk per
    // friendly, independent of total chaff. A pessimistic layout -- a dense
    // half-virus, half-bacteria crowd with the units and the towers dropped
    // straight into it, so every walk is full, every unit is in an aura and
    // every latchable unit fills its cap.
    auto measure = [](usize n_swarm, int n_chaff, usize n_towers) {
        Fixture f;
        f.chaff.reserve(16384);
        f.swarm.reserve(8192);
        Rng r(4242);
        for (int i = 0; i < n_chaff; ++i) {
            f.add_chaff(Vec2{r.range_f(20.0f, 100.0f), r.range_f(20.0f, 60.0f)},
                        (i & 1) ? PathogenFamily::Bacteria : PathogenFamily::Virus);
        }
        for (usize i = 0; i < n_swarm; ++i) {
            f.add_swarmer(Vec2{r.range_f(20.0f, 100.0f), r.range_f(20.0f, 60.0f)}, kShooter);
            f.swarm.health[i] = 1.0e9f;   // never dies: the walk is what is being timed
        }
        for (usize k = 0; k < n_towers; ++k) {
            f.add_tower(static_cast<u32>(k + 1), Vec2{r.range_f(20.0f, 100.0f), r.range_f(20.0f, 60.0f)}, 2.8f);
        }
        f64 best = 1.0e30;
        for (int k = 0; k < 30; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            f.step();
            const auto t1 = std::chrono::steady_clock::now();
            best = math::min(best, std::chrono::duration<f64, std::milli>(t1 - t0).count());
        }
        return best;
    };

    struct Row { const char* label; usize units; int chaff; usize towers; f64 ms; };
    Row rows[] = {
        {"1000 units / 4000 chaff / 16 towers", 1000, 4000, 16, 0.0},
        {"1000 units / 12000 chaff / 16 towers", 1000, 12000, 16, 0.0},
        {"3000 units / 4000 chaff / 16 towers", 3000, 4000, 16, 0.0},
        {"3000 units / 12000 chaff / 64 towers", 3000, 12000, 64, 0.0},
    };
    for (Row& r : rows) {
        r.ms = measure(r.units, r.chaff, r.towers);
        std::printf("[hostile] %s : %.4f ms/tick\n", r.label, r.ms);
    }
    // Three times the units is at most ~three times the cost (the walk per
    // unit is capped, so the pass is linear in units), and three times the
    // chaff costs no multiple at all (the passenger walk is a flag test per
    // agent). Wide margins, as in the swarmer bodies test: what is being
    // ruled out is a pair sweep.
    REQUIRE(rows[2].ms < rows[0].ms * 6.0 + 0.1);
    REQUIRE(rows[1].ms < rows[0].ms * 3.0 + 0.1);
    // And a full board buried in the densest crowd the fixture can build --
    // every walk at its cap -- stays a fraction of the frame (DESIGN.md 8.6).
    // In play the cloud is a few hundred units and most of them are not
    // inside a jam, so this is the ceiling, not the typical cost.
    REQUIRE(rows[3].ms < 6.0);
}

// ---------------------------------------------------------------------------
// Through the whole tick: the ECS tower, the game layer, determinism.
// ---------------------------------------------------------------------------

namespace {

constexpr i32 kW = 60;
constexpr i32 kH = 20;
const Vec2 kTowerPos{30.0f, 10.0f};

/// An open 60x20 field with the goal at the far right, hostile pass on, and
/// one tower dead centre. Everything the real game wires up, minus a level.
struct WorldFixture {
    SimWorld world;
    game::TowerSystem towers;
    EntityId tower{};

    explicit WorldFixture(u64 seed, bool hostile_on = true) {
        SimDesc desc;
        desc.seed = seed;
        desc.max_chaff = 4096;
        desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{static_cast<f32>(kW), static_cast<f32>(kH)}};
        desc.spatial_cell_size = 4.0f;
        desc.chaff_tuning.family[0].radius = kVirusRadius;
        desc.chaff_tuning.family[1].radius = kBacteriaRadius;
        // Viruses that never replicate, so the agent count is the one the
        // test spawned and nothing else.
        desc.chaff_tuning.family[0].replication_rate = 0.0f;
        desc.hostile_tuning.enabled = hostile_on;
        desc.hostile_tuning.family[0].latch_dps = 20.0f;
        desc.hostile_tuning.family[0].latch_reach = 0.4f;
        desc.hostile_tuning.family[0].latch_cap_swarmer = 3;
        desc.hostile_tuning.family[0].latch_cap_tower = 12;
        desc.hostile_tuning.family[1].aura_dps = 30.0f;
        desc.hostile_tuning.family[1].aura_radius = 4.0f;
        world.init(desc, nullptr);

        TissueMask& mask = world.tissue();
        mask.resize(kW, kH, 1.0f, Vec2{0.0f, 0.0f});
        for (i32 y = 0; y < kH; ++y)
            for (i32 x = 0; x < kW; ++x) mask.set_walkable(x, y, true);
        world.sdf().bake(mask);
        FlowFieldBakeDesc fdesc;
        fdesc.goals = {sim::FlowGoal{mask.world_to_cell(Vec2{58.0f, 10.0f})}};
        world.flow().bake(mask, fdesc);

        towers.register_systems(world);
        // Nothing released: this is about the horde biting, not the tower
        // biting back, and a volley of swarmers scraping the passengers off
        // would make the integrity numbers below a race.
        towers.set_releasing(false);
        tower = towers.place(world, TowerType::Macrophage, kTowerPos);
        REQUIRE(tower.valid());
    }

    comp::Health& health() {
        return world.ecs().registry().get<comp::Health>(world.ecs().from_id(tower));
    }

    void spawn_ring(PathogenFamily family, u32 n, f32 radius) {
        for (u32 k = 0; k < n; ++k) {
            const f32 a = static_cast<f32>(k) * (math::kTwoPi / static_cast<f32>(n));
            ChaffSpawnParams c;
            c.position = kTowerPos + Vec2{std::cos(a), std::sin(a)} * radius;
            c.density = 1.0f;
            c.family = family;
            world.chaff().spawn(c);
        }
    }
};

} // namespace

TEST_CASE("a tower is placed with its integrity and an upgrade restores it", "[game][towers][hostile]") {
    WorldFixture w(1);
    const game::TowerStats& t1 = w.towers.stats(TowerType::Macrophage, 1);
    REQUIRE(w.health().max == Catch::Approx(t1.max_health));
    REQUIRE(w.health().current == Catch::Approx(t1.max_health));
    w.health().current = 10.0f;
    REQUIRE(w.towers.upgrade(w.world, w.tower) == 2);
    const game::TowerStats& t2 = w.towers.stats(TowerType::Macrophage, 2);
    REQUIRE(w.health().max == Catch::Approx(t2.max_health));
    REQUIRE(w.health().current == Catch::Approx(t2.max_health));
    REQUIRE(t2.max_health > t1.max_health);
}

TEST_CASE("viruses on a tower's footprint latch on, hold still, and eat it down", "[game][towers][hostile]") {
    WorldFixture w(7);
    const f32 footprint = w.towers.stats(TowerType::Macrophage, 1).footprint_radius;
    w.spawn_ring(PathogenFamily::Virus, 8, footprint);

    w.world.tick();
    SimSnapshot s = w.world.snapshot();
    REQUIRE(s.chaff_latched == 8);
    REQUIRE(w.world.friendly_towers().items.size() == 1);
    REQUIRE(w.world.friendly_towers().passengers[0] == 8);

    // Latched agents are frozen by the chaff kernel: after a second of flow
    // toward the goal they are all still on the tower's membrane.
    for (int i = 0; i < 60; ++i) w.world.tick();
    const ChaffBuffers& chaff = w.world.chaff();
    for (usize i = 0; i < chaff.count(); ++i) {
        REQUIRE((chaff.flags[i] & chaff_flags::kLatched) != 0);
        const f32 d = math::length(Vec2{chaff.pos_x[i], chaff.pos_y[i]} - kTowerPos);
        REQUIRE(d == Catch::Approx(footprint + kVirusRadius * 0.35f).margin(1e-2f));
    }
    // 8 passengers x 20 hp/s x 61 ticks, minus the grab tick (no bite yet).
    const f32 expected = w.towers.stats(TowerType::Macrophage, 1).max_health - 8.0f * 20.0f * kFixedDt * 60.0f;
    REQUIRE(w.health().current == Catch::Approx(expected).margin(0.5f));
    REQUIRE(w.world.snapshot().towers_lost_total == 0);
}

TEST_CASE("a tower the horde empties is torn down, leaves the placed list and announces itself",
          "[game][towers][hostile]") {
    WorldFixture w(11);
    const f32 footprint = w.towers.stats(TowerType::Macrophage, 1).footprint_radius;
    w.spawn_ring(PathogenFamily::Virus, 12, footprint);
    w.health().current = 12.0f * 20.0f * kFixedDt * 3.5f;   // dead on the fourth bite

    u32 ticks = 0;
    while (w.world.snapshot().towers_lost_total == 0 && ticks < 30) {
        w.world.tick();
        ++ticks;
    }
    REQUIRE(w.world.snapshot().towers_lost_total == 1);
    // Torn down by the next tick's PreUpdate sweep, before it could fire.
    w.world.tick();
    REQUIRE_FALSE(w.world.ecs().registry().valid(w.world.ecs().from_id(w.tower)));
    REQUIRE(w.towers.placed_towers().empty());
    REQUIRE(w.towers.towers_destroyed() == 1);
    REQUIRE(count_events(w.world.combat_events(), CombatEventType::TowerDestroyed) == 1);
    // Its passengers are dropped and walk on: nobody is latched a tick later.
    w.world.tick();
    REQUIRE(w.world.snapshot().chaff_latched == 0);
    // The ground it stood on is buildable again.
    REQUIRE(w.towers.validate(w.world, TowerType::Macrophage, kTowerPos, 100000u).valid());
}

TEST_CASE("bacteria burn a tower from inside their aura without touching it", "[game][towers][hostile]") {
    WorldFixture w(3);
    const f32 footprint = w.towers.stats(TowerType::Macrophage, 1).footprint_radius;
    // Just inside the aura's reach to the membrane, and well outside it.
    w.spawn_ring(PathogenFamily::Bacteria, 4, footprint + 4.0f - 0.3f);
    w.spawn_ring(PathogenFamily::Bacteria, 4, footprint + 4.0f + 3.0f);
    const f32 before = w.health().current;
    w.world.tick();
    REQUIRE(w.world.snapshot().chaff_latched == 0);
    REQUIRE(w.health().current == Catch::Approx(before - 4.0f * 30.0f * kFixedDt).margin(1e-3f));
}

TEST_CASE("a released swarmer that the horde kills is compacted and never fires again",
          "[game][towers][hostile]") {
    WorldFixture w(5);
    // Drop one shooter by hand on top of a bacterial cluster; releasing is off
    // so this is the only unit in the world.
    const u16 slot = swarmer_profile_slot(TowerType::Neutrophil, 1);
    SwarmerProfile pr = game::swarmer_profile(TowerType::Neutrophil, 1);
    pr.max_health = 30.0f * kFixedDt * 2.5f;   // three bacteria: dead on the first tick
    w.world.swarmers().set_profile(slot, pr);
    SwarmerSpawnParams sp;
    sp.position = Vec2{45.0f, 10.0f};
    sp.profile = slot;
    sp.owner = w.tower;
    sp.seed = 99u;
    REQUIRE(w.world.swarmers().spawn(sp));
    for (u32 k = 0; k < 3; ++k) {
        ChaffSpawnParams c;
        c.position = Vec2{45.0f + static_cast<f32>(k) * 0.5f, 10.5f};
        c.density = 1.0f;
        c.family = PathogenFamily::Bacteria;
        w.world.chaff().spawn(c);
    }
    w.world.tick();
    REQUIRE(w.world.snapshot().swarmers_killed_total == 1);
    REQUIRE(w.world.swarmers().count() == 1);   // flagged, compacted next update
    REQUIRE((w.world.swarmers().flags[0] & swarmer_flags::kPendingKill) != 0);
    w.world.tick();
    REQUIRE(w.world.swarmers().count() == 0);
    REQUIRE(w.world.swarmer_system().last_stats().shots_fired == 0);
    REQUIRE(count_events(w.world.combat_events(), CombatEventType::SwarmerDeath) == 1);
}

TEST_CASE("the hostile pass is deterministic and is part of the state hash", "[sim][hostile][determinism]") {
    WorldFixture a(21), b(21), off(21, /*hostile_on=*/false);
    for (WorldFixture* w : {&a, &b, &off}) {
        const f32 footprint = w->towers.stats(TowerType::Macrophage, 1).footprint_radius;
        w->spawn_ring(PathogenFamily::Virus, 6, footprint);
        w->spawn_ring(PathogenFamily::Bacteria, 6, footprint + 2.0f);
    }
    for (int i = 0; i < 60; ++i) {
        a.world.tick();
        b.world.tick();
        off.world.tick();
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }
    REQUIRE(a.world.snapshot().chaff_latched == 6);
    REQUIRE(off.world.snapshot().chaff_latched == 0);
    // With the pass off the viruses walked away down the lane; with it on
    // they are still on the tower. Different worlds, different hashes.
    REQUIRE(a.world.state_hash() != off.world.state_hash());
}
