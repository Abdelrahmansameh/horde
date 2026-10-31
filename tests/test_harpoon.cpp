// Coverage for the Spore's harpoon (sim/hostile/Harpoon.h).
//
// The properties the design rests on, not balance numbers: a spore picks the
// BIGGEST friendly in range and the smallest last (a Cytotoxic T beside it is
// still a legal target when it is all there is); it plants while it fights;
// damage lands only when a slug reaches the end of the hose, never before,
// and it keeps pumping until the target is dead; a dead spore's hose does no more harm; nothing out of range is shot; and
// the pass switches off cleanly and is deterministic.
#include "sim/hostile/HostileAttacks.h"

#include "core/Math.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const Rect kBounds{Vec2{0.0f, 0.0f}, Vec2{160.0f, 160.0f}};

constexpr u16 kBig = 0;     // a Macrophage-sized body
constexpr u16 kMid = 1;     // a Neutrophil-sized one
constexpr u16 kSmall = 2;   // a Cytotoxic T

constexpr f32 kSporeRadius = 2.2f;

struct Fixture {
    ChaffBuffers chaff;
    SwarmerBuffers swarm;
    SpatialHash hash;
    HostileSystem hostile;
    FriendlyTowerList towers;
    CombatEventSink events;
    HostileTuning tuning;
    TissueMask mask;

    Fixture() {
        chaff.reserve(256);
        swarm.reserve(64);
        SpatialHashDesc hd;
        hd.bounds = kBounds;
        hd.cell_size = 4.0f;
        hash.configure(hd);
        events.reserve(1024);

        SwarmerProfile big;
        big.kind = SwarmerKind::ArborGrabber;
        big.source = TowerType::Macrophage;
        big.size = 2.6f;
        big.max_health = 1000.0f;
        swarm.set_profile(kBig, big);
        SwarmerProfile mid = big;
        mid.kind = SwarmerKind::Shooter;
        mid.source = TowerType::Neutrophil;
        mid.size = 2.0f;
        swarm.set_profile(kMid, mid);
        SwarmerProfile small = big;
        small.kind = SwarmerKind::Latch;
        small.source = TowerType::CytotoxicT;
        small.size = 0.705f;
        swarm.set_profile(kSmall, small);

        f32 radii[kFamilyCount] = {};
        radii[static_cast<u32>(PathogenFamily::Spore)] = kSporeRadius;
        hostile.set_chaff_radii(radii, kFamilyCount);

        tuning.enabled = true;
        HarpoonParams& hp = tuning.harpoon[static_cast<u32>(PathogenFamily::Spore)];
        hp = HarpoonParams{};
        hp.enabled = true;
        hostile.set_tuning(tuning);
    }

    const HarpoonParams& params() const {
        return tuning.harpoon[static_cast<u32>(PathogenFamily::Spore)];
    }

    usize add_spore(Vec2 p) {
        ChaffSpawnParams c;
        c.position = p;
        c.density = 3.0f;
        c.family = PathogenFamily::Spore;
        c.size_scale = 1.0f;
        chaff.spawn(c);
        return chaff.count() - 1;
    }

    usize add_swarmer(Vec2 p, u16 profile) {
        SwarmerSpawnParams s;
        s.position = p;
        s.profile = profile;
        s.owner = EntityId{7u};
        s.seed = 0x1234u + static_cast<u32>(swarm.count()) * 7919u;
        swarm.spawn(s);
        const usize i = swarm.count() - 1;
        swarm.size_scale[i] = 1.0f;
        return i;
    }

    HostileStats step() {
        hash.rebuild(chaff.pos_x.data(), chaff.pos_y.data(), chaff.count(), nullptr);
        return hostile.update(chaff, hash, swarm, towers, mask, kDt, &events);
    }

    /// Steps until the shooter is in `state` (or `max_ticks` pass).
    bool step_until(usize spore, u8 state, int max_ticks = 600) {
        for (int t = 0; t < max_ticks; ++t) {
            step();
            if (chaff.harpoon_state[spore] == state) return true;
        }
        return false;
    }
};

u32 count_events(const CombatEventSink& sink, CombatEventType type) {
    u32 n = 0;
    for (const CombatEvent& e : sink.events()) n += e.type == type ? 1u : 0u;
    return n;
}

} // namespace

TEST_CASE("a spore shoots the biggest friendly in range, the smallest last", "[sim][hostile][harpoon]") {
    Fixture f;
    const usize spore = f.add_spore(Vec2{40.0f, 80.0f});
    // A Cytotoxic T right beside it, a Neutrophil nearer than the Macrophage.
    f.add_swarmer(Vec2{43.0f, 80.0f}, kSmall);
    f.add_swarmer(Vec2{55.0f, 80.0f}, kMid);
    const usize big = f.add_swarmer(Vec2{70.0f, 80.0f}, kBig);

    REQUIRE(f.step_until(spore, harpoon_state::kDraw));
    REQUIRE(f.hostile.harpoons().size() == 1);
    const HarpoonLine& line = f.hostile.harpoons().front();
    REQUIRE_FALSE(line.target_tower);
    REQUIRE(line.target_index == big);
    REQUIRE(line.target_generation == f.swarm.generation[big]);
}

TEST_CASE("a cytotoxic T beside the spore is shot when it is all there is", "[sim][hostile][harpoon]") {
    Fixture f;
    const usize spore = f.add_spore(Vec2{40.0f, 80.0f});
    const usize t_cell = f.add_swarmer(Vec2{42.5f, 80.0f}, kSmall);

    REQUIRE(f.step_until(spore, harpoon_state::kDraw));
    REQUIRE(f.hostile.harpoons().front().target_index == t_cell);
    REQUIRE(f.step_until(spore, harpoon_state::kRecover));
    REQUIRE(f.swarm.health[t_cell] < 1000.0f);
}

TEST_CASE("nothing out of range is shot", "[sim][hostile][harpoon]") {
    Fixture f;
    const usize spore = f.add_spore(Vec2{20.0f, 80.0f});
    const f32 beyond = f.params().range + 2.6f + 3.0f;
    f.add_swarmer(Vec2{20.0f + kSporeRadius + beyond, 80.0f}, kBig);
    for (int t = 0; t < 120; ++t) f.step();
    REQUIRE(f.hostile.harpoons().empty());
    REQUIRE(f.chaff.harpoon_state[spore] == harpoon_state::kReady);
    REQUIRE(f.chaff.hold[spore] == 0.0f);
}

TEST_CASE("a slug hurts only when it reaches the end of the hose", "[sim][hostile][harpoon]") {
    Fixture f;
    const usize spore = f.add_spore(Vec2{40.0f, 80.0f});
    const usize big = f.add_swarmer(Vec2{70.0f, 80.0f}, kBig);
    const f32 start = f.swarm.health[big];

    // Drawing, the hose in flight, and the moment it anchors: no damage yet.
    REQUIRE(f.step_until(spore, harpoon_state::kPump));
    REQUIRE(f.swarm.health[big] == start);
    REQUIRE(count_events(f.events, CombatEventType::HarpoonPulse) >= 1);   // the bite

    // The first slug has left; it has the hose's length still to go.
    const HostileStats first = f.step();
    REQUIRE(first.harpoon_hits == 0);
    REQUIRE(f.swarm.health[big] == start);

    // No fixed volley: it pumps until the target is dead, then lets go.
    const HarpoonParams& hp = f.params();
    REQUIRE(hp.pulses == 0u);
    u32 hits = 0;
    for (int t = 0; t < 3000 && f.chaff.harpoon_state[spore] == harpoon_state::kPump; ++t)
        hits += f.step().harpoon_hits;
    const u32 lethal = static_cast<u32>(std::ceil(start / hp.pulse_damage));
    REQUIRE(hits == lethal);
    REQUIRE(f.swarm.health[big] == Catch::Approx(start - hp.pulse_damage * static_cast<f32>(lethal)));
    REQUIRE(f.swarm.health[big] <= 0.0f);
    REQUIRE(f.chaff.harpoon_state[spore] == harpoon_state::kRecover);
}

TEST_CASE("a spore plants while it fights and walks when nothing is in range", "[sim][hostile][harpoon]") {
    Fixture f;
    const usize spore = f.add_spore(Vec2{40.0f, 80.0f});
    const usize big = f.add_swarmer(Vec2{70.0f, 80.0f}, kBig);
    REQUIRE(f.step_until(spore, harpoon_state::kPump));
    for (int t = 0; t < 30; ++t) f.step();
    // Planted dead still, not easing toward still for seconds.
    REQUIRE(f.chaff.hold[spore] == f.params().hold);
    REQUIRE(f.params().hold == 1.0f);

    // The target leaves; the spore uproots.
    f.swarm.kill(big, CombatRetirement::Damage);
    f.swarm.compact();
    for (int t = 0; t < 90; ++t) f.step();
    REQUIRE(f.chaff.hold[spore] == 0.0f);
}

TEST_CASE("a dead spore's hose is severed and does no more harm", "[sim][hostile][harpoon]") {
    Fixture f;
    const usize spore = f.add_spore(Vec2{40.0f, 80.0f});
    const usize big = f.add_swarmer(Vec2{70.0f, 80.0f}, kBig);
    REQUIRE(f.step_until(spore, harpoon_state::kPump));
    f.step();   // a slug is now in the hose
    const f32 before = f.swarm.health[big];

    f.chaff.flags[spore] |= chaff_flags::kPendingKill;
    f.chaff.compact();
    REQUIRE(f.chaff.count() == 0);
    for (int t = 0; t < 60; ++t) f.step();
    REQUIRE(f.swarm.health[big] == before);
    REQUIRE(f.hostile.harpoons().empty());   // severed, faded, gone
}

TEST_CASE("switching hostile attacks off drops every hose and uproots every spore", "[sim][hostile][harpoon]") {
    Fixture f;
    const usize spore = f.add_spore(Vec2{40.0f, 80.0f});
    f.add_swarmer(Vec2{70.0f, 80.0f}, kBig);
    REQUIRE(f.step_until(spore, harpoon_state::kPump));
    f.tuning.enabled = false;
    f.hostile.set_tuning(f.tuning);
    f.step();
    REQUIRE(f.hostile.harpoons().empty());
    REQUIRE(f.chaff.harpoon_state[spore] == harpoon_state::kReady);
    REQUIRE(f.chaff.hold[spore] == 0.0f);
}

TEST_CASE("the harpoon pass is deterministic", "[sim][hostile][harpoon]") {
    const auto run = [] {
        Fixture f;
        for (u32 k = 0; k < 6; ++k) f.add_spore(Vec2{20.0f + 4.0f * static_cast<f32>(k), 60.0f + 9.0f * static_cast<f32>(k % 3)});
        for (u32 k = 0; k < 8; ++k) {
            f.add_swarmer(Vec2{50.0f + 3.0f * static_cast<f32>(k), 70.0f + 2.0f * static_cast<f32>(k % 4)},
                          static_cast<u16>(k % 3));
        }
        std::vector<f32> trace;
        for (int t = 0; t < 400; ++t) {
            f.step();
            for (const HarpoonLine& line : f.hostile.harpoons()) {
                trace.push_back(line.reach);
                trace.push_back(line.pulse[0]);
                trace.push_back(static_cast<f32>(line.target_index));
            }
            for (usize s = 0; s < f.swarm.count(); ++s) trace.push_back(f.swarm.health[s]);
        }
        return trace;
    };
    REQUIRE(run() == run());
}
