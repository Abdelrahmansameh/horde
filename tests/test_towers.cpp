// Tests for game/towers/TowerSystem.cpp: placement validation, the
// towers-are-not-obstacles guarantee (a placement never edits the tissue mask
// or the flow field), upgrade/sell economics, find_target(), and the combat
// behaviour of the roster. Every tower releases its own small cells; the
// Macrophage's released units attack with one synchronized pincer pair.
//
// Scene layout used by the placement/geometry tests (see make_world()):
//
//   y
//   15 #########################........##########################
//      #     left room (0..24)   .  corridor (25..34, rows 9-10)  .     right room (35..59)     #
//   4  #########################........##########################
//      0                        25       35                                                    60 (x, world units == cells, cell_size=1)
//
// The corridor is the narrowest ground a tower still fits on (clearance >=
// footprint_radius). Towers are not obstacles, so a tower dropped dead centre
// in it is a legal placement and the corridor stays walkable and reachable
// underneath it -- which is exactly what the placement tests below assert.
//
// COMBAT TESTS
// Every tower gets a test that proves its CHARACTERISTIC BEHAVIOUR, not merely
// that it did damage: latchers ride and drain, shooters hold a standoff and put
// real rounds into world.projectiles(), the Macrophage grabs and absorbs,
// slow bombers leave circles that slow and then wear off, and mucus bombers
// splash into real fluid. Each also asserts the CombatEvent the VFX layer keys
// on -- the whole particle layer is downstream of those events and renders
// nothing without them.
#include "game/towers/TowerSystem.h"
#include "game/towers/TowerMechanics.h"

#include "game/economy/Economy.h"
#include "game/session/LevelSession.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Profiler.h"
#include "core/Rng.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/NamedAgents.h"
#include "sim/swarm/Swarmers.h"
#include "sim/zone/SlowZones.h"
#include "vfx/Particles.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::sim;
using namespace immune::game;

namespace {

constexpr i32 kW = 60;
constexpr i32 kH = 20;

Vec2 kRoomCenterLeft{12.0f, 10.0f};
Vec2 kRoomCenterRight{47.0f, 10.0f};
Vec2 kCorridorCenter{30.0f, 10.5f};   // centre of the 5-cell corridor (y 8..13)
Vec2 kGoal{55.0f, 10.0f};

/// Initialises `world` with the scene above. Split from make_world() so a test
/// can start a second "level" in the SAME SimWorld, which is what App does.
void build_scene(SimWorld& world) {
    SimDesc desc;
    desc.seed = 12345;
    desc.max_chaff = 4096;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{static_cast<f32>(kW), static_cast<f32>(kH)}};
    desc.spatial_cell_size = 2.0f;
    world.init(desc, nullptr);

    TissueMask& mask = world.tissue();
    mask.resize(kW, kH, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 4; y < 16; ++y)
        for (i32 x = 0; x < 25; ++x) mask.set_walkable(x, y, true); // left room
    for (i32 y = 4; y < 16; ++y)
        for (i32 x = 35; x < 60; ++x) mask.set_walkable(x, y, true); // right room
    // 5-cell-tall corridor. Sized to the tower footprints, not chosen freely:
    // the corridor has to be wide enough that the tower FITS (clearance >=
    // footprint_radius, or validate() rejects it for InsufficientClearance)
    // while still being narrow enough that the footprint spans every corridor
    // cell -- so that "the corridor is still walkable under the tower" is a
    // real claim and not one about cells the footprint never covered.
    //
    // The SDF measures centre-to-wall less half a cell, so a corridor H cells
    // tall has centre clearance H/2 - 0.5. Footprint 1.6 therefore needs
    // H >= 4.2, i.e. 5, giving clearance 2.0. The 3.2-wide footprint then spans
    // y 8.9..12.1, which intersects all five corridor cells (8..12).
    for (i32 y = 8; y < 13; ++y)
        for (i32 x = 25; x < 35; ++x) mask.set_walkable(x, y, true);

    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goals = {sim::FlowGoal{mask.world_to_cell(kGoal)}};
    world.flow().bake(mask, fdesc);
}

SimWorld make_world() {
    SimWorld world;
    build_scene(world);
    return world;
}

SystemContext make_ctx(SimWorld& world) {
    return SystemContext{world, world.ecs().registry(), world.rng(), kFixedDt, world.tick_index()};
}

void rebuild_spatial(SimWorld& world) {
    world.spatial().rebuild(world.chaff().pos_x.data(), world.chaff().pos_y.data(), world.chaff().count(), nullptr);
}

/// One combat step with the chaff held still.
///
/// SimWorld::tick() would also flow-move and compact the horde, which makes a
/// geometry assertion ("this cluster took damage and that one did not") race
/// the movement kernel. This runs exactly the damage-relevant half of the
/// canonical tick order (SimWorld.h): hash rebuild -> ECS systems -> aggregate
/// damage -> projectiles -> swarmers and what they asked for -> slow zones ->
/// fluid -> transient-field expiry. No movement, no compaction, so chaff
/// indices and positions are stable across the whole test.
void step_combat(SimWorld& world) {
    rebuild_spatial(world);
    SystemContext ctx = make_ctx(world);
    world.ecs().tick(ctx);
    world.damage().apply(world.chaff(), world.spatial(), world.rng(), kFixedDt);
    world.projectile_system().update(world.projectiles(), world.chaff(), world.spatial(),
                                     world.tissue(), world.desc().world_bounds, world.rng(), kFixedDt,
                                     &world.combat_events());
    world.build_named_targets();
    world.swarmer_system().update(world.swarmers(), world.chaff(), world.spatial(),
                                  world.named_targets(), &world.sdf(), &world.flow(),
                                  world.desc().world_bounds,
                                  world.rng(), kFixedDt, &world.combat_events());
    world.apply_swarmer_effects();
    world.slow_zones().update(world.chaff(), world.spatial(), kFixedDt);
    world.fluid_system().update(world.fluid(), world.chaff(), world.spatial(), world.sdf(),
                                world.desc().world_bounds, kFixedDt, &world.combat_events());
    world.damage().clear_transient(kFixedDt);
}

/// Just the submission half of a step. Needed because step_combat() ends with
/// clear_transient(), which by contract drops every `lifetime <= 0` field --
/// so a test that wants to prove no tower publishes a persistent field has to
/// look between submission and expiry, which is the same window sim/damage
/// itself evaluates them in.
void submit_only(SimWorld& world) {
    rebuild_spatial(world);
    SystemContext ctx = make_ctx(world);
    world.ecs().tick(ctx);
}

void spawn_chaff_cluster(SimWorld& world, Vec2 center, u32 count, f32 density, f32 spread = 0.3f) {
    Rng r(999);
    for (u32 i = 0; i < count; ++i) {
        ChaffSpawnParams p;
        p.position = center + Vec2{r.range_f(-spread, spread), r.range_f(-spread, spread)};
        p.density = density;
        p.family = PathogenFamily::Virus;
        world.chaff().spawn(p);
    }
}

void spawn_one(SimWorld& world, Vec2 pos, f32 density = 4.0f) {
    ChaffSpawnParams p;
    p.position = pos;
    p.density = density;
    p.family = PathogenFamily::Virus;
    world.chaff().spawn(p);
}

/// Total remaining chaff density inside a circle. Test-side only; the sim never
/// walks the store like this.
f32 density_in(const SimWorld& world, Vec2 center, f32 radius) {
    const ChaffBuffers& c = world.chaff();
    f32 sum = 0.0f;
    for (usize i = 0; i < c.count(); ++i) {
        const Vec2 p{c.pos_x[i], c.pos_y[i]};
        if (math::length_sq(p - center) <= radius * radius) sum += c.density[i];
    }
    return sum;
}

usize count_events(const SimWorld& world, CombatEventType type, TowerType source) {
    usize n = 0;
    for (const CombatEvent& e : world.combat_events().events()) {
        if (e.type == type && e.source == source) ++n;
    }
    return n;
}

const CombatEvent* first_event(const SimWorld& world, CombatEventType type, TowerType source) {
    for (const CombatEvent& e : world.combat_events().events()) {
        if (e.type == type && e.source == source) return &e;
    }
    return nullptr;
}

/// Clears a tower's cooldown so a test does not have to spend its spin-up
/// (2 s for the Macrophage) in ticks before the interesting thing happens.
void ready_now(SimWorld& world, EntityId tower) {
    world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower)).cooldown = 0.0f;
}

EntityId spawn_named(SimWorld& world, Vec2 pos, f32 health_scale = 1.0f) {
    const u16 archetype = named::placeholder_elite(world.ecs().registry());
    named::SpawnParams p;
    p.archetype = archetype;
    p.position = pos;
    p.health_scale = health_scale;
    return named::spawn(world, p);
}

} // namespace

// ---------------------------------------------------------------------------
// Stats table
// ---------------------------------------------------------------------------
// Stats table
// ---------------------------------------------------------------------------

TEST_CASE("default stats table covers every type/tier with real, distinguishing numbers",
          "[towers][stats]") {
    TowerSystem ts;
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        const TowerStats& s1 = ts.stats(type, 1);
        const TowerStats& s2 = ts.stats(type, 2);
        const TowerStats& s3 = ts.stats(type, 3);
        INFO("tower type " << tower_type_name(type));
        REQUIRE(s1.build_cost > 0);
        REQUIRE(s1.footprint_radius > 0.0f);
        REQUIRE(s1.fire_interval > 0.0f);
        // Rate goes UP with tier for every tower (fire_interval goes down).
        REQUIRE(s3.fire_interval <= s1.fire_interval);
        // And the swarmer table behind it is populated for every tier, with
        // the aggro radius (the tower's only reach) growing with tier.
        for (u8 tier = 1; tier <= 3; ++tier) {
            const TowerMechanics& m = tower_mechanics(type, tier);
            REQUIRE(m.swarm.release_per_shot > 0);
            if (type == TowerType::Macrophage) {
                REQUIRE(m.arbor_grabber.arm_count > 0u);
                REQUIRE(m.arbor_grabber.arm_count <= kArborMaxArms);
                REQUIRE(m.arbor_grabber.max_captives > 0u);
                REQUIRE(m.arbor_grabber.max_captives <= kArborMaxCaptives);
                REQUIRE(m.arbor_grabber.cluster_radius > 0.0f);
                REQUIRE(m.arbor_grabber.extend_seconds > 0.0f);
                REQUIRE(m.arbor_grabber.latch_seconds > 0.0f);
                REQUIRE(m.arbor_grabber.pull_seconds > 0.0f);
                REQUIRE(m.arbor_grabber.recover_seconds > 0.0f);
            }
            REQUIRE(m.swarm.lifetime > 0.0f);
            REQUIRE(m.swarm.speed > 0.0f);
            REQUIRE(m.swarm.search_radius > 0.0f);
            REQUIRE(m.swarm.attach_radius > 0.0f);
            REQUIRE(m.swarm.size > 0.0f);
            if (tier > 1) {
                REQUIRE(m.swarm.search_radius >= tower_mechanics(type, tier - 1).swarm.search_radius);
            }
            // Towers spawn continuously inside a round, so the standing cloud
            // per tower is bounded by this and must stay modest.
            const f32 standing = static_cast<f32>(m.swarm.release_per_shot) * m.swarm.lifetime /
                                 ts.stats(type, tier).fire_interval;
            INFO("standing swarmers at tier " << static_cast<int>(tier) << ": " << standing);
            REQUIRE(standing < 200.0f);
        }
    }
}

TEST_CASE("every tower type maps to its own swarmer kind, and the profile says so",
          "[towers][stats][kind]") {
    // The kind IS the tower. Five towers, five kinds, no two the same -- a
    // tuning pass that made two towers release the same thing would collapse
    // the roster into fewer towers than the build menu shows.
    bool seen[static_cast<u32>(SwarmerKind::Count)] = {};
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        const SwarmerKind kind = tower_kind(type);
        REQUIRE(static_cast<u32>(kind) < static_cast<u32>(SwarmerKind::Count));
        REQUIRE_FALSE(seen[static_cast<u32>(kind)]);
        seen[static_cast<u32>(kind)] = true;
        for (u8 tier = 1; tier <= 3; ++tier) {
            const SwarmerProfile p = swarmer_profile(type, tier);
            REQUIRE(p.kind == kind);
            REQUIRE(p.source == type);
            REQUIRE(p.lifetime == tower_mechanics(type, tier).swarm.lifetime);
        }
    }
    REQUIRE(tower_kind(TowerType::CytotoxicT) == SwarmerKind::Latch);
    REQUIRE(tower_kind(TowerType::Neutrophil) == SwarmerKind::Shooter);
    REQUIRE(tower_kind(TowerType::Macrophage) == SwarmerKind::ArborGrabber);
    REQUIRE(tower_kind(TowerType::Interferon) == SwarmerKind::SlowBomber);
    REQUIRE(tower_kind(TowerType::GobletCell) == SwarmerKind::MucusBomber);
}

TEST_CASE("the five towers occupy genuinely different niches in the table", "[towers][stats]") {
    // The point of the roster: a player should be able to describe each tower
    // in one phrase. These are the numeric shapes behind those phrases, so a
    // future tuning pass that accidentally flattens the roster fails here
    // rather than silently making five towers feel like one.
    TowerSystem ts;
    const TowerStats shooter = ts.stats(TowerType::Neutrophil, 1);
    const TowerStats grabber = ts.stats(TowerType::Macrophage, 1);
    const TowerStats slow    = ts.stats(TowerType::Interferon, 1);
    const TowerStats latch   = ts.stats(TowerType::CytotoxicT, 1);
    const TowerStats mucus   = ts.stats(TowerType::GobletCell, 1);

    const SwarmParams& shooter_sw = tower_mechanics(TowerType::Neutrophil, 1).swarm;
    const SwarmParams& latch_sw   = tower_mechanics(TowerType::CytotoxicT, 1).swarm;

    // LATCH is the cloud: the biggest standing population in the roster by a
    // wide margin, on the fastest cadence.
    const auto standing = [&](TowerType t) {
        const SwarmParams& sw = tower_mechanics(t, 1).swarm;
        return static_cast<f32>(sw.release_per_shot) * sw.lifetime / ts.stats(t, 1).fire_interval;
    };
    REQUIRE(standing(TowerType::CytotoxicT) > 2.0f * standing(TowerType::Neutrophil));
    REQUIRE(standing(TowerType::CytotoxicT) > 4.0f * standing(TowerType::Macrophage));
    REQUIRE(latch.fire_interval < shooter.fire_interval);
    REQUIRE(latch.fire_interval < grabber.fire_interval);

    // SHOOTER swarmers keep a real standoff; everything else goes to contact.
    REQUIRE(shooter_sw.attach_radius > 3.0f * latch_sw.attach_radius);
    // ARBOR GRABBER is a slow, far-reaching tank. Its value is several
    // branching pseudopods, reach that keeps its body out of the horde's
    // bite, and the amount of punishment its large body can take.
    const ArborGrabberParams& grab = tower_mechanics(TowerType::Macrophage, 1).arbor_grabber;
    REQUIRE(grabber.fire_interval > shooter.fire_interval);
    REQUIRE(grabber.fire_interval > latch.fire_interval);
    const SwarmParams& grab_sw = tower_mechanics(TowerType::Macrophage, 1).swarm;
    REQUIRE(grab.arm_count >= 2u);
    REQUIRE(grab.max_captives >= 2u);
    REQUIRE(grab.cluster_radius > 0.0f);
    REQUIRE(grab_sw.attach_radius > 2.0f * grab_sw.size);
    REQUIRE(grab_sw.size > shooter_sw.size);
    REQUIRE(swarmer_profile(TowerType::Macrophage, 1).max_health >
            swarmer_profile(TowerType::Neutrophil, 1).max_health);
    REQUIRE(grabber.max_health > shooter.max_health);
    REQUIRE(grabber.footprint_radius > shooter.footprint_radius);
    REQUIRE(slow.fire_interval > shooter.fire_interval);
    REQUIRE(mucus.fire_interval > shooter.fire_interval);

    // SLOW BOMBER does no damage at all: its whole value is the circles.
    REQUIRE(tower_mechanics(TowerType::Interferon, 1).slow_bomber.slow_factor < 1.0f);
    REQUIRE(tower_mechanics(TowerType::Interferon, 1).slow_bomber.zone_duration > 0.0f);

    // Cost ordering: the cheap workhorse, then the specialists, then the
    // widest/tankiest one in the roster.
    REQUIRE(shooter.build_cost < slow.build_cost);
    REQUIRE(slow.build_cost < latch.build_cost);
    REQUIRE(grabber.build_cost > mucus.build_cost);
}

TEST_CASE("tower_type_name/parse_tower_type round-trip for every roster type", "[towers][naming]") {
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        TowerType parsed{};
        REQUIRE(parse_tower_type(tower_type_name(type), parsed));
        REQUIRE(parsed == type);
    }
    TowerType unused{};
    REQUIRE_FALSE(parse_tower_type("not_a_tower", unused));
    REQUIRE_FALSE(parse_tower_type("nk_cell", unused));   // retired with the swarmer roster
}

// ---------------------------------------------------------------------------
// Upgrade-over-expand cost curve (DESIGN.md §5.3/§7.1).
//
// tower_output() mirrors what each kind actually does with each activation:
//
//   LATCH         standing granules x dps        release/interval x life x dps
//   SHOOTER       standing shooters x round dps  release/interval x life x dmg/fire
//   BOMBER        bursts per second x damage     release/interval x burst_damage
//   ARBOR GRABBER arm count x catch area          arms x r^2 / cycle
//   SLOW BOMBER   circle-area-seconds of slow    release/interval x r^2 x dur x (1-factor)
//   MUCUS BOMBER  slow coverage                   release/interval x droplets x life x slow strength
//   BUILDER       wall laid per second           release/interval x scar_health x length
// ---------------------------------------------------------------------------

namespace {

f32 tower_output(TowerType type, u8 tier, const TowerStats& s) {
    const f32 rate = s.fire_interval > 0.0f ? 1.0f / s.fire_interval : 0.0f;
    const TowerMechanics& m = tower_mechanics(type, tier);
    const f32 per_sec = static_cast<f32>(m.swarm.release_per_shot) * rate;
    switch (tower_kind(type)) {
    case SwarmerKind::Latch:
        return per_sec * m.swarm.lifetime * m.latch.dps;
    case SwarmerKind::Shooter:
        return per_sec * m.swarm.lifetime * m.shooter.round_damage /
               math::max(m.shooter.fire_interval, 0.001f);
    case SwarmerKind::Bomber:
        return per_sec * m.bomber.burst_damage;
    case SwarmerKind::ArborGrabber:
        return per_sec * m.swarm.lifetime * static_cast<f32>(m.arbor_grabber.arm_count) /
               math::max(m.arbor_grabber.extend_seconds + m.arbor_grabber.latch_seconds +
                             m.arbor_grabber.pull_seconds + m.arbor_grabber.recover_seconds,
                         0.001f);
    case SwarmerKind::SlowBomber:
        return per_sec * m.slow_bomber.zone_radius * m.slow_bomber.zone_radius *
               m.slow_bomber.zone_duration * (1.0f - m.slow_bomber.slow_factor);
    case SwarmerKind::MucusBomber:
        return per_sec * static_cast<f32>(m.mucus_bomber.droplets) *
               m.mucus_bomber.droplet_lifetime * (1.0f - m.mucus_bomber.slow_factor);
    case SwarmerKind::Builder:
        // Integrity of wall laid per second: hit points the horde has to chew
        // through, times how much lane each wall closes.
        return per_sec * m.builder.scar_health * m.builder.scar_half_length * 2.0f;
    case SwarmerKind::Count:
        break;
    }
    return 0.0f;
}

} // namespace

TEST_CASE("upgrading is a better ATP-per-output deal than a fresh tower, for every tower type",
          "[towers][economy][cost_curve]") {
    TowerSystem ts;
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        INFO("tower type " << tower_type_name(type));
        const TowerStats& s1 = ts.stats(type, 1);
        const TowerStats& s2 = ts.stats(type, 2);
        const TowerStats& s3 = ts.stats(type, 3);

        // Structural cost-curve shape (DESIGN.md §5.3): each upgrade step
        // costs noticeably less than a fresh tier-1 build, and the curve
        // decelerates further up the tree.
        REQUIRE(s1.upgrade_cost < s1.build_cost);
        REQUIRE(s2.upgrade_cost < s1.upgrade_cost);
        REQUIRE(s2.upgrade_cost > 0);

        const f32 o1 = tower_output(type, 1, s1);
        const f32 o2 = tower_output(type, 2, s2);
        const f32 o3 = tower_output(type, 3, s3);
        REQUIRE(o1 > 0.0f);

        // Accelerating value: tier 2 pushes output well above 2x tier 1's,
        // and tier 3 pulls further ahead still (the absolute gap grows).
        REQUIRE(o2 > 2.0f * o1);
        REQUIRE((o3 - o2) > (o2 - o1));

        // The actual economic claim: ATP spent all the way up the upgrade
        // tree buys strictly more output-per-ATP than stopping at tier 1 (and
        // therefore than spending the same total ATP on N fresh tier-1
        // towers, which nets exactly tier 1's own output-per-ATP -- spreading
        // thin is a visible tax, not the efficient move).
        const f32 cost_to_t1 = static_cast<f32>(s1.build_cost);
        const f32 cost_to_t2 = cost_to_t1 + static_cast<f32>(s1.upgrade_cost);
        const f32 cost_to_t3 = cost_to_t2 + static_cast<f32>(s2.upgrade_cost);
        const f32 eff1 = o1 / cost_to_t1;
        const f32 eff2 = o2 / cost_to_t2;
        const f32 eff3 = o3 / cost_to_t3;
        REQUIRE(eff2 > eff1);
        REQUIRE(eff3 > eff2);
    }
}

// ---------------------------------------------------------------------------
// validate()/place()/upgrade()/sell()
// ---------------------------------------------------------------------------

TEST_CASE("validate() rejects off-tissue and unaffordable placements, accepts an open interior spot",
          "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;

    const auto off_tissue = ts.validate(world, TowerType::Macrophage, Vec2{13.0f, 2.0f}, 100000);
    REQUIRE(off_tissue.result == PlacementResult::NotOnTissue);

    const auto ok = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft, 100000);
    REQUIRE(ok.result == PlacementResult::Ok);

    const auto broke = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft, /*available_atp=*/0);
    REQUIRE(broke.result == PlacementResult::CannotAfford);
}

TEST_CASE("validate() rejects insufficient clearance near a wall", "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;
    // Interferon (CRYO) has the roster's largest tier-1 footprint_radius (1.2);
    // 0.5 units from the left room's wall (x=0 boundary) leaves far less than that.
    REQUIRE(ts.stats(TowerType::Interferon, 1).footprint_radius > 0.5f);
    const auto q = ts.validate(world, TowerType::Interferon, Vec2{0.5f, 10.0f}, 100000);
    REQUIRE(q.result == PlacementResult::InsufficientClearance);
    REQUIRE(q.clearance < ts.stats(TowerType::Interferon, 1).footprint_radius);
    // The light snap should nudge back toward more clearance (away from x=0).
    REQUIRE(q.snapped_position.x > 0.5f);
}

TEST_CASE("validate() rejects a placement overlapping an existing tower", "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const Vec2 first_pos{7.0f, 10.0f};
    const EntityId first = ts.place(world, TowerType::Macrophage, first_pos);
    REQUIRE(first.valid());

    const auto q = ts.validate(world, TowerType::Macrophage, first_pos + Vec2{0.3f, 0.0f}, 100000);
    REQUIRE(q.result == PlacementResult::Overlapping);

    const auto far_enough = ts.validate(world, TowerType::Macrophage,
                                        first_pos + Vec2{10.0f, 0.0f}, 100000);
    REQUIRE(far_enough.result == PlacementResult::Ok);
}

TEST_CASE("validate() allows a tower that spans the only corridor: towers are not obstacles",
          "[towers][placement][reachability]") {
    SimWorld world = make_world();
    TowerSystem ts;

    // Sanity: the goal is reachable from the left room before any placement,
    // and the Cytotoxic T's footprint genuinely spans the whole corridor (see
    // build_scene), so this is the placement that used to be refused for
    // sealing the lane.
    REQUIRE(world.flow().reachable(kRoomCenterLeft));
    REQUIRE(ts.stats(TowerType::CytotoxicT, 1).footprint_radius * 2.0f >= 3.0f);

    const auto mid = ts.validate(world, TowerType::CytotoxicT, kCorridorCenter, 100000);
    REQUIRE(mid.result == PlacementResult::Ok);

    const auto open = ts.validate(world, TowerType::CytotoxicT, kRoomCenterRight, 100000);
    REQUIRE(open.result == PlacementResult::Ok);
}

TEST_CASE("place() and sell() never touch the tissue mask or the flow field",
          "[towers][placement][flowfield]") {
    SimWorld world = make_world();
    TowerSystem ts;
    REQUIRE_FALSE(world.flow().has_pending_rebake());

    // Snapshot the mask so the assertion is "unchanged", not merely "walkable":
    // a tower that flipped cost or re-marked cells walkable would pass a
    // walkable() check and still be editing ground it has no business editing.
    const TissueMask& mask = world.tissue();
    std::vector<u8> before_walkable;
    std::vector<f32> before_cost;
    for (i32 y = 0; y < mask.height(); ++y) {
        for (i32 x = 0; x < mask.width(); ++x) {
            before_walkable.push_back(mask.walkable(x, y) ? 1u : 0u);
            before_cost.push_back(mask.cost(x, y));
        }
    }
    const auto mask_unchanged = [&]() {
        usize i = 0;
        for (i32 y = 0; y < mask.height(); ++y) {
            for (i32 x = 0; x < mask.width(); ++x, ++i) {
                if ((mask.walkable(x, y) ? 1u : 0u) != before_walkable[i]) return false;
                if (mask.cost(x, y) != before_cost[i]) return false;
            }
        }
        return true;
    };

    // Dead centre of the corridor, where the footprint covers every cell of
    // the lane: the strongest case for "nothing was blocked".
    const EntityId id = ts.place(world, TowerType::CytotoxicT, kCorridorCenter);
    REQUIRE(id.valid());

    const IVec2 c = mask.world_to_cell(kCorridorCenter);
    REQUIRE(mask.walkable(c.x, c.y));
    REQUIRE(mask_unchanged());
    REQUIRE_FALSE(world.flow().has_pending_rebake());
    // The (untouched) field still routes the left room through the corridor.
    REQUIRE(world.flow().reachable(kRoomCenterLeft));
    REQUIRE(world.flow().reachable(kCorridorCenter));

    REQUIRE(ts.sell(world, id) > 0);
    REQUIRE(mask_unchanged());
    REQUIRE_FALSE(world.flow().has_pending_rebake());
}

TEST_CASE("a freshly placed tower spins up rather than discharging on the placement tick",
          "[towers][placement][combat]") {
    // tests/scripts/tower_thins_horde.json asserts chaff_killed_total == 0 on
    // the tick the tower lands. With a 2s Macrophage cycle this is the
    // difference between placing a tower and instantly answering the wave.
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    const comp::Tower& tw = world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower));
    REQUIRE(tw.cooldown == ts.stats(TowerType::Macrophage, 1).fire_interval);

    spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{6.0f, 0.0f}, 20, 1.0f);
    const f32 before = world.chaff().total_density();
    step_combat(world);
    REQUIRE(world.chaff().total_density() == before);
    REQUIRE(world.combat_events().size() == 0);
}

TEST_CASE("upgrade() advances tier and stats, caps at 3; sell() refunds ATP",
          "[towers][economy]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId id = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(id.valid());

    const entt::entity e = world.ecs().from_id(id);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 1);
    // comp::Tower::range is the swarmers' aggro radius (the tower has none).
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).range ==
            tower_mechanics(TowerType::Macrophage, 1).swarm.search_radius);

    REQUIRE(ts.upgrade(world, id) == 2);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 2);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).range ==
            tower_mechanics(TowerType::Macrophage, 2).swarm.search_radius);

    REQUIRE(ts.upgrade(world, id) == 3);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 3);
    REQUIRE(ts.upgrade(world, id) == 0); // already maxed

    // invested = tier1 build_cost (180) + upgrade to tier2 (115) + upgrade to
    // tier3 (95) = 390; refund at the documented 0.7 fraction = 273 (truncated).
    const u32 expected = static_cast<u32>(
        (static_cast<f32>(ts.stats(TowerType::Macrophage, 1).build_cost) +
         static_cast<f32>(ts.stats(TowerType::Macrophage, 1).upgrade_cost) +
         static_cast<f32>(ts.stats(TowerType::Macrophage, 2).upgrade_cost)) * 0.7f);
    const u32 refund = ts.sell(world, id);
    REQUIRE(refund == expected);
    REQUIRE(refund == 273);

    REQUIRE_FALSE(world.ecs().registry().valid(e));

    bool still_listed = false;
    for (EntityId t : ts.placed_towers())
        if (t == id) still_listed = true;
    REQUIRE_FALSE(still_listed);
}

// ---------------------------------------------------------------------------
// find_target()
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// find_target()
// ---------------------------------------------------------------------------

TEST_CASE("find_target respects range and family_mask, and never returns a Burrowed agent",
          "[towers][targeting]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId agent = spawn_named(world, kRoomCenterLeft);
    REQUIRE(agent.valid());
    const PathogenFamily family = world.ecs().registry().get<comp::NamedAgent>(world.ecs().from_id(agent)).family;
    const u8 fam_bit = static_cast<u8>(1u << static_cast<u8>(family));

    REQUIRE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF) == agent);
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft + Vec2{3.0f, 0.0f}, 1.0f, 0xFF).valid()); // out of range
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft + Vec2{20.0f, 0.0f}, 2.0f, 0xFF).valid());
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft, 5.0f, static_cast<u8>(~fam_bit)).valid());

    // Burrowed is invisible to the whole roster now that the NK Cell is gone.
    world.ecs().registry().get<comp::AiBrain>(world.ecs().from_id(agent)).state = comp::AiState::Burrowed;
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF).valid());
}

// ---------------------------------------------------------------------------
// THE SPAWNER. Every tower releases swarmers and nothing else.
// ---------------------------------------------------------------------------

namespace {

usize engaged_count(const SimWorld& world) {
    usize n = 0;
    for (usize k = 0; k < world.swarmers().count(); ++k) {
        if ((world.swarmers().flags[k] & swarmer_flags::kAttached) != 0) ++n;
    }
    return n;
}

/// Distance from swarmer `k` to the nearest live chaff agent.
f32 nearest_chaff_distance(const SimWorld& world, usize k) {
    const ChaffBuffers& c = world.chaff();
    const Vec2 p{world.swarmers().pos_x[k], world.swarmers().pos_y[k]};
    f32 best = 1e9f;
    for (usize i = 0; i < c.count(); ++i) {
        best = math::min(best, math::length(Vec2{c.pos_x[i], c.pos_y[i]} - p));
    }
    return best;
}

usize slowed_count(const SimWorld& world) {
    usize n = 0;
    for (usize i = 0; i < world.chaff().count(); ++i) {
        if ((world.chaff().flags[i] & chaff_flags::kSlowed) != 0) ++n;
    }
    return n;
}

usize marked_count(const SimWorld& world) {
    usize n = 0;
    for (usize i = 0; i < world.chaff().count(); ++i) {
        if ((world.chaff().flags[i] & chaff_flags::kMarked) != 0) ++n;
    }
    return n;
}

f32 named_health(const SimWorld& world, EntityId id) {
    return world.ecs().registry().get<comp::Health>(world.ecs().from_id(id)).current;
}

} // namespace

TEST_CASE("every swarmer tower releases a volley of its own kind and publishes no damage field",
          "[towers][combat][spawner]") {
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        INFO("tower " << tower_type_name(type));
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        const EntityId tower = ts.place(world, type, kRoomCenterLeft);
        REQUIRE(tower.valid());
        ready_now(world, tower);

        spawn_one(world, kRoomCenterLeft + Vec2{5.0f, 0.0f}, /*density=*/40.0f);
        REQUIRE(world.swarmers().count() == 0);

        // Look between submission and expiry: if any tower still published a
        // persistent field this is the window it would be visible in.
        submit_only(world);
        for (const DamageField& f : world.damage().fields()) {
            INFO("tower published a field of shape " << static_cast<int>(f.shape));
            REQUIRE(f.owner != tower);
        }

        // A volley, not a shot: exactly the configured count, all of this
        // tower's kind, all owned by it.
        const u32 expected = tower_mechanics(type, 1).swarm.release_per_shot;
        REQUIRE(world.swarmers().count() == expected);
        for (usize k = 0; k < world.swarmers().count(); ++k) {
            REQUIRE(world.swarmers().profile_of(k).kind == tower_kind(type));
            REQUIRE(world.swarmers().profile_of(k).source == type);
            REQUIRE(world.swarmers().owner[k] == tower);
            // Released from the cell's FACE, on the target's side of centre.
            const Vec2 p{world.swarmers().pos_x[k], world.swarmers().pos_y[k]};
            REQUIRE(p.x > kRoomCenterLeft.x);
        }

        // The release is announced once per volley, by the TOWER (no swarmer
        // bit on the visual id).
        REQUIRE(count_events(world, CombatEventType::MuzzleFlash, type) == 1);
        const CombatEvent* e = first_event(world, CombatEventType::MuzzleFlash, type);
        REQUIRE((e->visual_id & kSwarmerEventBit) == 0);
        REQUIRE(e->magnitude == static_cast<f32>(expected));
    }
}

TEST_CASE("a tower spawns continuously with nothing in sight, and holds only between rounds",
          "[towers][combat][spawner]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::CytotoxicT, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);
    // The only pathogen is in the other room, far outside any aggro radius.
    spawn_one(world, kRoomCenterRight, 40.0f);

    // In a round (the default): volleys keep coming on the cadence with no
    // target at all -- and go out all round, not down a cone.
    const TowerStats& st = ts.stats(TowerType::CytotoxicT, 1);
    const u32 per_volley = tower_mechanics(TowerType::CytotoxicT, 1).swarm.release_per_shot;
    const int ticks = static_cast<int>(2.5f * st.fire_interval / kFixedDt);
    for (int i = 0; i < ticks; ++i) step_combat(world);
    REQUIRE(count_events(world, CombatEventType::MuzzleFlash, TowerType::CytotoxicT) == 3);
    // (A ring volley near the room's edge can lose the odd unit off the map.)
    REQUIRE(world.swarmers().count() >= 2 * per_volley);
    bool left = false, right = false;
    for (usize k = 0; k < world.swarmers().count(); ++k) {
        if (world.swarmers().pos_x[k] < kRoomCenterLeft.x - 0.5f) left = true;
        if (world.swarmers().pos_x[k] > kRoomCenterLeft.x + 0.5f) right = true;
    }
    REQUIRE((left && right));

    // Between rounds: nothing leaves, however long the cooldown has been up.
    ts.set_releasing(false);
    world.combat_events().clear();
    world.swarmers().clear();
    ready_now(world, tower);
    for (int i = 0; i < ticks; ++i) step_combat(world);
    REQUIRE(world.swarmers().count() == 0);
    REQUIRE(count_events(world, CombatEventType::MuzzleFlash, TowerType::CytotoxicT) == 0);

    // And the round resuming picks straight back up.
    ts.set_releasing(true);
    step_combat(world);
    REQUIRE(world.swarmers().count() == per_volley);
}

TEST_CASE("the Macrophage tower only releases macrophage units and never attacks itself",
          "[towers][combat][arbor_grabber]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);
    // Inside unit aggro, off to the right. The release tick may launch a unit,
    // but it cannot damage from the factory's position.
    spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{8.0f, 0.0f}, 10, 40.0f, 0.3f);
    const f32 before = world.chaff().total_density();
    step_combat(world);
    REQUIRE(world.swarmers().count() ==
            tower_mechanics(TowerType::Macrophage, 1).swarm.release_per_shot);
    REQUIRE(world.swarmers().profile_of(0).kind == SwarmerKind::ArborGrabber);
    REQUIRE(world.chaff().total_density() == before);
}

// ---------------------------------------------------------------------------
// LATCH -- Cytotoxic T.
// ---------------------------------------------------------------------------

TEST_CASE("LATCH granules fly to a pathogen, latch on, and drain it",
          "[towers][combat][latch]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::CytotoxicT, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    spawn_one(world, Vec2{16.0f, 10.0f}, /*density=*/400.0f);
    const f32 before = world.chaff().density[0];

    // Nothing should have been touched on the release tick: the granules have
    // to cross the gap first.
    step_combat(world);
    REQUIRE(world.chaff().count() == 1);
    REQUIRE(world.chaff().density[0] == before);

    bool ever_attached = false;
    for (int i = 0; i < 60 && !ever_attached; ++i) {
        step_combat(world);
        if (engaged_count(world) > 0) ever_attached = true;
    }
    REQUIRE(ever_attached);
    // Latching is not yet feeding: the granule spends attach_seconds
    // entering its host first (Swarmers.h, SwarmerProfile::attach_seconds).
    REQUIRE(world.chaff().density[0] == before);
    const f32 entry = tower_mechanics(TowerType::CytotoxicT, 1).latch.attach_seconds;
    for (int i = 0; i < static_cast<int>(std::ceil(entry / kFixedDt)) + 1; ++i) step_combat(world);
    REQUIRE(world.chaff().density[0] < before);
    // Latching is announced, by the SWARMER.
    const CombatEvent* e = first_event(world, CombatEventType::ProjectileImpact, TowerType::CytotoxicT);
    REQUIRE(e != nullptr);
    REQUIRE((e->visual_id & kSwarmerEventBit) != 0);
}

TEST_CASE("LATCH granules move on to another pathogen once their host dies",
          "[towers][combat][latch]") {
    // The serial-killing property, and the reason a granule holds a
    // ChaffHandle rather than an index: it has to notice its host is gone and
    // pick again, across a compaction that moves every survivor's slot.
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::CytotoxicT, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    spawn_one(world, Vec2{15.0f, 10.0f}, /*density=*/1.0f);
    spawn_one(world, Vec2{18.0f, 10.0f}, /*density=*/1.0e6f);
    const f32 tough_before = world.chaff().density[1];

    bool frail_gone = false;
    for (int i = 0; i < 400; ++i) {
        step_combat(world);
        world.chaff().compact();
        if (!frail_gone && world.chaff().count() == 1) frail_gone = true;
    }

    REQUIRE(frail_gone);
    REQUIRE(world.chaff().count() == 1);
    INFO("tough agent density " << world.chaff().density[0] << " (was " << tough_before << ")");
    REQUIRE(world.chaff().density[0] < tough_before);
}

TEST_CASE("latchers and shooters dissolve on their own lifetime, so the cloud stays bounded",
          "[towers][combat][latch][shooter]") {
    for (const TowerType type : {TowerType::CytotoxicT, TowerType::Neutrophil}) {
        INFO("tower " << tower_type_name(type));
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        const EntityId tower = ts.place(world, type, kRoomCenterLeft);
        REQUIRE(tower.valid());
        ready_now(world, tower);

        spawn_one(world, Vec2{16.0f, 10.0f}, /*density=*/4.0f);
        step_combat(world);
        REQUIRE(world.swarmers().count() > 0);

        // Clear the lane and end the round, so no further volley is released
        // and the standing cloud has to drain to nothing on its own.
        for (usize i = 0; i < world.chaff().count(); ++i) world.chaff().kill(i);
        world.chaff().compact();
        REQUIRE(world.chaff().count() == 0);
        ts.set_releasing(false);

        for (int i = 0; i < 600; ++i) step_combat(world);
        REQUIRE(world.swarmers().count() == 0);
        // A dissolve is a fizzle, not a detonation.
        REQUIRE(count_events(world, CombatEventType::Explosion, type) == 0);
    }
}

// ---------------------------------------------------------------------------
// SHOOTER -- Neutrophil.
// ---------------------------------------------------------------------------

TEST_CASE("SHOOTER swarmers hold a standoff and put real rounds into world.projectiles()",
          "[towers][combat][shooter][projectiles]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Neutrophil, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{9.0f, 0.0f}, 30, 40.0f, 0.4f);
    const f32 before = density_in(world, kRoomCenterLeft + Vec2{9.0f, 0.0f}, 2.0f);
    REQUIRE(world.projectiles().count() == 0);

    bool fired = false;
    usize peak_rounds = 0;
    for (int i = 0; i < 180; ++i) {
        step_combat(world);
        peak_rounds = math::max(peak_rounds, world.projectiles().count());
        if (engaged_count(world) > 0 && world.projectiles().count() > 0) fired = true;
    }
    REQUIRE(fired);
    INFO("peak live rounds " << peak_rounds);
    REQUIRE(peak_rounds > 0);

    // Engaged shooters sit at their standoff, not on the target: every one
    // that is firing is well clear of the cluster.
    const f32 standoff = tower_mechanics(TowerType::Neutrophil, 1).swarm.attach_radius;
    usize checked = 0;
    for (usize k = 0; k < world.swarmers().count(); ++k) {
        if ((world.swarmers().flags[k] & swarmer_flags::kAttached) == 0) continue;
        ++checked;
        REQUIRE(nearest_chaff_distance(world, k) > standoff * 0.35f);
    }
    REQUIRE(checked > 0);

    // And the rounds land: the cluster is thinner.
    REQUIRE(density_in(world, kRoomCenterLeft + Vec2{9.0f, 0.0f}, 2.0f) < before);
    // Each round is announced by the SWARMER that fired it.
    const CombatEvent* shot = nullptr;
    for (const CombatEvent& e : world.combat_events().events()) {
        if (e.type == CombatEventType::MuzzleFlash && e.source == TowerType::Neutrophil &&
            (e.visual_id & kSwarmerEventBit) != 0) { shot = &e; break; }
    }
    REQUIRE(shot != nullptr);
}

// ---------------------------------------------------------------------------
// ARBOR GRABBER -- Macrophage.
// ---------------------------------------------------------------------------

TEST_CASE("Macrophage grows several independent trees and pulls targets concurrently",
          "[towers][combat][arbor_grabber]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    // Three targets around the release side force separate radial arms. The
    // unit must not collapse them into a single tree.
    spawn_one(world, kRoomCenterLeft + Vec2{7.0f, 0.0f}, 4.0f);
    spawn_one(world, kRoomCenterLeft + Vec2{5.0f, 4.0f}, 4.0f);
    spawn_one(world, kRoomCenterLeft + Vec2{5.0f, -4.0f}, 4.0f);
    const f32 before = world.chaff().total_density();

    step_combat(world);
    REQUIRE(world.swarmers().count() == 1);
    const SwarmerProfile& profile = world.swarmers().profile_of(0);
    REQUIRE(profile.kind == SwarmerKind::ArborGrabber);
    REQUIRE(profile.arbor_arm_count == 2u);
    REQUIRE(profile.arbor_fake_mass == Catch::Approx(0.60f));

    u32 peak_active = 0;
    u32 peak_gripping = 0;
    bool saw_divergent_headings = false;
    u32 kills = 0;
    for (int tick = 0; tick < 300; ++tick) {
        step_combat(world);
        kills += world.swarmer_system().last_stats().hosts_finished;
        for (usize i = 0; i < world.swarmers().count(); ++i) {
            if (world.swarmers().profile_of(i).kind != SwarmerKind::ArborGrabber) continue;
            const ArborGrabberState& arbor = world.swarmers().arbor_grabber[i];
            u32 active = 0;
            u32 gripping = 0;
            Vec2 first{};
            bool have_first = false;
            for (const ArborArmState& arm : arbor.arms) {
                if (arm.phase == ArborArmPhase::Idle) continue;
                ++active;
                if (arm.phase == ArborArmPhase::Latching ||
                    arm.phase == ArborArmPhase::Pulling) ++gripping;
                if (!have_first) {
                    first = arm.heading;
                    have_first = true;
                } else if (first.x * arm.heading.x + first.y * arm.heading.y < 0.90f) {
                    saw_divergent_headings = true;
                }
            }
            peak_active = math::max(peak_active, active);
            peak_gripping = math::max(peak_gripping, gripping);
        }
        if (kills >= 2u && peak_gripping >= 2u && saw_divergent_headings) break;
    }

    REQUIRE(peak_active >= 2u);
    REQUIRE(peak_gripping >= 2u);
    REQUIRE(saw_divergent_headings);
    REQUIRE(kills >= 2u);
    REQUIRE(world.chaff().total_density() < before);
    REQUIRE(count_events(world, CombatEventType::ProjectileImpact,
                         TowerType::Macrophage) > 0);
    REQUIRE(count_events(world, CombatEventType::Explosion,
                         TowerType::Macrophage) == 0);
}

TEST_CASE("a distant Macrophage factory cannot damage before its unit arrives",
          "[towers][combat][arbor_grabber][range]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{20.0f, 0.0f}, 12, 25.0f, 0.2f);
    const f32 before = world.chaff().total_density();
    step_combat(world);

    REQUIRE(world.chaff().total_density() == before);
    REQUIRE(world.swarmers().count() > 0);
    REQUIRE(count_events(world, CombatEventType::MuzzleFlash,
                         TowerType::Macrophage) == 1);
}

TEST_CASE("a bomber whose lifetime runs out detonates where it stands", "[towers][combat][bomber]") {
    // Every detonating kind. Spawned straight into the store with nothing to
    // go at, so the only way it can end is by expiring -- and expiring must
    // leave the effect behind, not fizzle.
    for (const TowerType type : {TowerType::Interferon, TowerType::GobletCell}) {
        INFO("tower " << tower_type_name(type));
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);

        const u16 slot = swarmer_profile_slot(type, 1);
        world.swarmers().set_profile(slot, swarmer_profile(type, 1));
        SwarmerSpawnParams p;
        p.position = kRoomCenterLeft;
        p.profile = slot;
        p.seed = 77u;
        REQUIRE(world.swarmers().spawn(p));

        const f32 lifetime = swarmer_profile(type, 1).lifetime;
        const int ticks = static_cast<int>(lifetime / kFixedDt) + 5;
        for (int i = 0; i < ticks; ++i) step_combat(world);
        REQUIRE(world.swarmers().count() == 0);
        REQUIRE(count_events(world, CombatEventType::Explosion, type) == 1);
        const CombatEvent* boom = first_event(world, CombatEventType::Explosion, type);
        REQUIRE(math::length(boom->origin - kRoomCenterLeft) < 2.0f);

        // ...and the effect is real, in whichever store it belongs to.
        switch (tower_kind(type)) {
        case SwarmerKind::Bomber: {
            bool found = false;
            for (const DamageField& f : world.damage().fields()) found = found || f.shape == FieldShape::Circle;
            REQUIRE(found);
            break;
        }
        case SwarmerKind::SlowBomber:  REQUIRE(world.slow_zones().count() == 1); break;
        case SwarmerKind::MucusBomber: REQUIRE(world.fluid().count() > 0); break;
        default: FAIL("not a detonating kind"); break;
        }
    }
}

// ---------------------------------------------------------------------------
// SLOW BOMBER -- Interferon.
// ---------------------------------------------------------------------------

TEST_CASE("SLOW BOMBER swarmers leave a circle that slows what stands in it, and the slow wears off",
          "[towers][combat][slow]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Interferon, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    const Vec2 at = kRoomCenterLeft + Vec2{7.0f, 0.0f};
    spawn_chaff_cluster(world, at, 30, 30.0f, 0.4f);
    const f32 before = world.chaff().total_density();
    REQUIRE(slowed_count(world) == 0);

    for (int i = 0; i < 240 && world.slow_zones().count() == 0; ++i) step_combat(world);
    REQUIRE(world.slow_zones().count() >= 1);
    const CombatEvent* pop = first_event(world, CombatEventType::Explosion, TowerType::Interferon);
    REQUIRE(pop != nullptr);
    REQUIRE((pop->visual_id & kSwarmerEventBit) != 0);

    // The circle slows what it covers -- with the tier's factor, not a global
    // constant -- and damages nothing.
    step_combat(world);
    REQUIRE(slowed_count(world) > 0);
    const SlowBomberParams& sb = tower_mechanics(TowerType::Interferon, 1).slow_bomber;
    for (usize i = 0; i < world.chaff().count(); ++i) {
        if ((world.chaff().flags[i] & chaff_flags::kSlowed) == 0) continue;
        REQUIRE(world.chaff().slow_factor[i] == Catch::Approx(sb.slow_factor));
        REQUIRE(world.chaff().slow_remaining[i] > 0.0f);
    }
    REQUIRE(world.chaff().total_density() == before);

    // Now stop the supply: no tower, no swarmers in flight. The circle runs
    // out, then the slow it left on the agents runs out after it.
    ts.sell(world, tower);
    world.swarmers().clear();
    const int settle = static_cast<int>((sb.zone_duration + sb.slow_duration) / kFixedDt) + 30;
    for (int i = 0; i < settle; ++i) step_combat(world);
    REQUIRE(world.slow_zones().count() == 0);
    REQUIRE(slowed_count(world) == 0);
    for (usize i = 0; i < world.chaff().count(); ++i) REQUIRE(world.chaff().slow_remaining[i] == 0.0f);
}

TEST_CASE("a slowed chaff agent actually moves slower, by its own factor", "[towers][combat][slow]") {
    // The chaff half of the debuff end to end: kSlowed plus the per-agent
    // factor stream, read by the movement kernel on a real tick.
    SimWorld world = make_world();
    // Both in the left room, a few units apart so separation never couples
    // them, both walking the flow toward the goal on the right.
    spawn_one(world, Vec2{6.0f, 8.0f}, 4.0f);
    spawn_one(world, Vec2{6.0f, 12.0f}, 4.0f);
    world.chaff().flags[1] |= chaff_flags::kSlowed;
    world.chaff().slow_remaining[1] = 100.0f;
    world.chaff().slow_factor[1] = 0.4f;

    const Vec2 a0{world.chaff().pos_x[0], world.chaff().pos_y[0]};
    const Vec2 b0{world.chaff().pos_x[1], world.chaff().pos_y[1]};
    for (int i = 0; i < 120; ++i) world.tick();
    REQUIRE(world.chaff().count() == 2);
    const f32 moved_free = math::length(Vec2{world.chaff().pos_x[0], world.chaff().pos_y[0]} - a0);
    const f32 moved_slow = math::length(Vec2{world.chaff().pos_x[1], world.chaff().pos_y[1]} - b0);
    INFO("free " << moved_free << " slowed " << moved_slow);
    REQUIRE(moved_free > 1.0f);
    REQUIRE(moved_slow < 0.7f * moved_free);
    // Still slowed: nothing has expired it.
    REQUIRE((world.chaff().flags[1] & chaff_flags::kSlowed) != 0);
}

// ---------------------------------------------------------------------------
// MUCUS BOMBER -- Goblet Cell.
// ---------------------------------------------------------------------------

TEST_CASE("MUCUS BOMBER swarmers splash into real fluid that strongly slows without damage",
          "[towers][combat][mucus]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::GobletCell, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    const Vec2 at = kRoomCenterLeft + Vec2{7.0f, 0.0f};
    spawn_chaff_cluster(world, at, 40, 30.0f, 0.5f);
    REQUIRE(world.fluid().count() == 0);
    const f32 original_density = world.chaff().density[0];

    for (int i = 0; i < 240 && world.fluid().count() == 0; ++i) step_combat(world);
    REQUIRE(world.fluid().count() > 0);
    const CombatEvent* pop = first_event(world, CombatEventType::Explosion, TowerType::GobletCell);
    REQUIRE(pop != nullptr);
    REQUIRE((pop->visual_id & kSwarmerEventBit) != 0);

    // Mucus is crowd control: covered enemies retain density and take a timed slow.
    for (int i = 0; i < 120; ++i) step_combat(world);
    REQUIRE(slowed_count(world) > 0);
    REQUIRE(marked_count(world) == 0);
    REQUIRE(world.chaff().density[0] == original_density);
    const auto& mucus = tower_mechanics(TowerType::GobletCell, 1).mucus_bomber;
    for (usize i = 0; i < world.chaff().count(); ++i) {
        if ((world.chaff().flags[i] & chaff_flags::kSlowed) == 0) continue;
        REQUIRE(world.chaff().slow_factor[i] == Catch::Approx(mucus.slow_factor));
        REQUIRE(world.chaff().slow_remaining[i] > 0.0f);
    }

    // And it is a moment, not terrain: with the supply cut it evaporates.
    ts.sell(world, tower);
    world.swarmers().clear();
    const f32 life = tower_mechanics(TowerType::GobletCell, 1).mucus_bomber.droplet_lifetime;
    for (int i = 0; i < static_cast<int>(life / kFixedDt) + 30; ++i) step_combat(world);
    REQUIRE(world.fluid().count() == 0);
    for (int i = 0; i < static_cast<int>(mucus.slow_duration / kFixedDt) + 30; ++i) step_combat(world);
    REQUIRE(slowed_count(world) == 0);
}

TEST_CASE("mucus coverage slows named enemies without reducing health",
          "[towers][combat][mucus][named]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const Vec2 at{18.0f, 10.0f};
    const EntityId target = spawn_named(world, at);
    REQUIRE(target.valid());
    const f32 before = named_health(world, target);

    FluidJetParams jet;
    jet.lifetime = 2.0f;
    jet.damage_per_second = 0.0f;
    jet.slow_duration = 3.0f;
    jet.slow_factor = 0.08f;
    REQUIRE(world.fluid_system().splash(world.fluid(), jet, at, 0.5f, 0.0f, 100) > 0);
    world.tick();

    const entt::entity entity = world.ecs().from_id(target);
    REQUIRE(named_health(world, target) == before);
    REQUIRE(world.ecs().registry().all_of<comp::Slowed>(entity));
    const comp::Slowed& slow = world.ecs().registry().get<comp::Slowed>(entity);
    REQUIRE(slow.remaining > 0.0f);
    REQUIRE(slow.factor == Catch::Approx(0.08f));
    REQUIRE_FALSE(world.ecs().registry().all_of<comp::Marked>(entity));
}

// ---------------------------------------------------------------------------
// Named agents. Every kind can hunt an elite or a boss, not just chaff.
// ---------------------------------------------------------------------------

TEST_CASE("swarmers hunt named agents: a latch drains one, a shooter shoots one, a burst hits one",
          "[towers][combat][named]") {
    struct Case { TowerType type; f32 offset; };
    const Case cases[] = {
        {TowerType::CytotoxicT, 5.0f},
        {TowerType::Neutrophil, 8.0f},
        {TowerType::Macrophage, 6.0f},
    };
    for (const Case& c : cases) {
        INFO("tower " << tower_type_name(c.type));
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        const EntityId tower = ts.place(world, c.type, kRoomCenterLeft);
        REQUIRE(tower.valid());
        // Tier 3: the placeholder elite carries 2 armor, which a tier-1 round
        // (1.9) cannot get through -- the same flat-reduction rule the old
        // per-shot strike had, and not what this test is about.
        REQUIRE(ts.upgrade(world, tower) == 2);
        REQUIRE(ts.upgrade(world, tower) == 3);
        ready_now(world, tower);

        // No chaff anywhere: the named agent is the only thing to go at.
        const EntityId boss = spawn_named(world, kRoomCenterLeft + Vec2{c.offset, 0.0f});
        REQUIRE(boss.valid());
        const f32 before = named_health(world, boss);

        for (int i = 0; i < 300; ++i) step_combat(world);
        INFO("health " << named_health(world, boss) << " (was " << before << ")");
        REQUIRE(named_health(world, boss) < before);
        if (c.type == TowerType::Macrophage) {
            // The pincer pair encloses and ingests it inside the window.
            REQUIRE(named_health(world, boss) <= 0.0f);
            REQUIRE(world.swarmers().count() > 0);
            REQUIRE(count_events(world, CombatEventType::ProjectileImpact,
                                 TowerType::Macrophage) > 0);
        } else {
            REQUIRE(world.swarmers().count() > 0);   // the tower kept releasing at it
        }
    }
}

TEST_CASE("burrowed targets are invisible to every tower and every swarmer",
          "[towers][combat][targeting][hidden]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::CytotoxicT, kRoomCenterLeft);
    REQUIRE(tower.valid());
    ready_now(world, tower);

    // A burrowed elite and a burrowed chaff agent, both well inside range.
    const EntityId boss = spawn_named(world, kRoomCenterLeft + Vec2{4.0f, 0.0f});
    world.ecs().registry().get<comp::AiBrain>(world.ecs().from_id(boss)).state = comp::AiState::Burrowed;
    {
        ChaffSpawnParams p;
        p.position = kRoomCenterLeft + Vec2{4.0f, 2.0f};
        p.density = 40.0f;
        p.family = PathogenFamily::Virus;
        p.flags = chaff_flags::kHidden;
        world.chaff().spawn(p);
    }
    const f32 boss_before = named_health(world, boss);
    const f32 chaff_before = world.chaff().density[0];

    // The tower sees nothing: it faces up the lane rather than at either.
    world.ecs().registry().get<comp::Transform>(world.ecs().from_id(tower)).rotation = 1.0f;
    for (int i = 0; i < 5; ++i) step_combat(world);
    REQUIRE(world.ecs().registry().get<comp::Transform>(world.ecs().from_id(tower)).rotation != 1.0f);

    // Nothing its volleys released, or a volley spawned by hand right next to
    // the two, can pick either up over a whole lifetime.
    const u16 slot = swarmer_profile_slot(TowerType::CytotoxicT, 1);
    world.swarmers().set_profile(slot, swarmer_profile(TowerType::CytotoxicT, 1));
    for (u32 k = 0; k < 12; ++k) {
        SwarmerSpawnParams p;
        p.position = kRoomCenterLeft + Vec2{3.0f, 1.0f};
        p.velocity = Vec2{2.0f, 0.0f};
        p.profile = slot;
        p.seed = 1000u + k * 7919u;
        REQUIRE(world.swarmers().spawn(p));
    }
    for (int i = 0; i < 200; ++i) {
        step_combat(world);
        REQUIRE(engaged_count(world) == 0);
    }
    REQUIRE(named_health(world, boss) == boss_before);
    REQUIRE(world.chaff().density[0] == chaff_before);
}

TEST_CASE("swarmers respect the vessel: a volley fired at a wall stays on the tissue",
          "[towers][combat][walls]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    // No chaff anywhere: these units drift on their launch velocity, which is
    // aimed straight at the left room's bottom wall (y = 4).
    const u16 slot = swarmer_profile_slot(TowerType::CytotoxicT, 1);
    world.swarmers().set_profile(slot, swarmer_profile(TowerType::CytotoxicT, 1));
    const f32 contact = swarmer_profile(TowerType::CytotoxicT, 1).size * kWallContactFraction;
    for (u32 k = 0; k < 12; ++k) {
        SwarmerSpawnParams p;
        p.position = kRoomCenterLeft + Vec2{static_cast<f32>(k) * 0.4f - 2.4f, 0.0f};
        p.velocity = Vec2{0.0f, -40.0f};
        p.profile = slot;
        p.seed = 500u + k * 7919u;
        REQUIRE(world.swarmers().spawn(p));
    }
    u32 contacts = 0;
    for (int i = 0; i < 90; ++i) {
        step_combat(world);
        contacts += world.swarmer_system().last_stats().wall_contacts;
        for (usize s = 0; s < world.swarmers().count(); ++s) {
            const Vec2 p{world.swarmers().pos_x[s], world.swarmers().pos_y[s]};
            INFO("swarmer " << s << " at " << p.x << "," << p.y << " clearance " << world.sdf().sample(p));
            REQUIRE(world.sdf().sample(p) >= contact - 0.05f);
        }
    }
    REQUIRE(world.swarmers().count() == 12);
    REQUIRE(contacts > 0);   // they did actually reach the wall
}

// ---------------------------------------------------------------------------
// Cross-cutting: events, determinism, budget.
// ---------------------------------------------------------------------------

TEST_CASE("every tower stamps its own TowerType and tier onto the events it raises",
          "[towers][combat][events]") {
    // vfx/Particles.cpp keys the ENTIRE per-tower look on (type, source,
    // visual_id). A tower that raises events with source == Count renders as
    // the generic grey fallback, which is indistinguishable from "broken".
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        for (u8 tier = 1; tier <= 3; ++tier) {
            INFO("tower " << tower_type_name(type) << " tier " << static_cast<int>(tier));
            SimWorld world = make_world();
            TowerSystem ts;
            ts.register_systems(world);
            const EntityId tower = ts.place(world, type, kRoomCenterLeft);
            REQUIRE(tower.valid());
            for (u8 k = 1; k < tier; ++k) REQUIRE(ts.upgrade(world, tower) == k + 1);
            ready_now(world, tower);
            spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{5.0f, 0.0f}, 30, 20.0f, 0.3f);

            // Every tower stamps the tier on both its release event and units.
            u16 swarmer_visual = 0;
            bool saw_swarmer = false;
            for (int i = 0; i < 5; ++i) {
                step_combat(world);
                if (!saw_swarmer && world.swarmers().count() > 0) {
                    saw_swarmer = true;
                    swarmer_visual = world.swarmers().visual_id[0];
                }
            }

            const CombatEvent* e = first_event(world, CombatEventType::MuzzleFlash, type);
            REQUIRE(e != nullptr);
            REQUIRE(e->source == type);
            // 3 data tiers spread across the VFX layer's 1..5 escalation axis.
            const u16 expected_visual = tier >= 3 ? 5 : (tier == 2 ? 3 : 1);
            REQUIRE(e->visual_id == expected_visual);
            REQUIRE(saw_swarmer);
            REQUIRE(swarmer_visual == expected_visual);
        }
    }
}

TEST_CASE("attaching a combat-event sink cannot change one bit of state_hash",
          "[towers][combat][determinism]") {
    // CombatEvents.h's central promise, tested from the producer side: events
    // are an OUTPUT of the tick. Same seed, same towers, same result -- with
    // the sink drained every tick or never drained at all.
    auto run = [](bool drain_every_tick) {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        REQUIRE(ts.place(world, TowerType::Neutrophil, kRoomCenterLeft).valid());
        REQUIRE(ts.place(world, TowerType::Interferon, Vec2{6.0f, 7.0f}).valid());
        REQUIRE(ts.place(world, TowerType::GobletCell, Vec2{6.0f, 13.0f}).valid());
        spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{5.0f, 1.0f}, 120, 3.0f, 1.5f);
        for (int i = 0; i < 240; ++i) {
            world.tick();
            if (drain_every_tick) world.combat_events().clear();
        }
        return world.state_hash();
    };
    REQUIRE(run(true) == run(false));
}

TEST_CASE("the same seed and the same towers produce the same tick-by-tick hashes",
          "[towers][combat][determinism]") {
    // Every kind at once: the swarmer kernel draws nothing from the sim Rng,
    // so this is really a test that its private-seed rule holds across every
    // payload, including the effects SimWorld resolves out of it.
    auto run = [] {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        // One of each, spread around the left room (x 0..24, y 4..15) with
        // enough clearance for every footprint.
        const Vec2 spots[] = {Vec2{21.0f, 6.5f}, Vec2{12.0f, 10.0f}, Vec2{21.0f, 13.5f},
                              Vec2{3.0f, 6.5f}, Vec2{3.0f, 13.5f}, Vec2{30.0f, 10.0f}};
        static_assert(sizeof(spots) / sizeof(spots[0]) == kTowerTypeCount, "one spot per roster type");
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            INFO("placing " << tower_type_name(static_cast<TowerType>(t)) << " at "
                            << spots[t].x << "," << spots[t].y);
            REQUIRE(ts.place(world, static_cast<TowerType>(t), spots[t]).valid());
        }
        spawn_chaff_cluster(world, kRoomCenterLeft + Vec2{6.0f, 0.0f}, 150, 2.0f, 1.2f);
        std::vector<u64> hashes;
        for (int i = 0; i < 180; ++i) {
            world.tick();
            hashes.push_back(world.state_hash());
        }
        return hashes;
    };
    const std::vector<u64> a = run();
    const std::vector<u64> b = run();
    REQUIRE(a == b);
    // ...and it isn't trivially constant.
    REQUIRE(a.front() != a.back());
}

TEST_CASE("a full roster of towers against a 10k horde stays inside the ecs_tick budget",
          "[towers][combat][perf]") {
    // AGENT_BRIEF §5 gives the ECS phase (<=200 named agents) a 2 ms budget, and
    // the Combat phase is where the spawner runs. The swarmer kernel itself is
    // step 4c of the tick and has its own cost model (sim/swarm/Swarmers.h);
    // what is measured here is that releasing volleys from 25 towers -- the
    // aim search plus the spawns -- stays cheap against a full horde.
    SimWorld world;
    SimDesc desc;
    desc.seed = 6060;
    desc.max_chaff = 16384;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    world.init(desc, nullptr);

    TissueMask& mask = world.tissue();
    mask.resize(256, 144, 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 4; y < 140; ++y)
        for (i32 x = 4; x < 252; ++x) mask.set_walkable(x, y, true);
    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goals = {sim::FlowGoal{mask.world_to_cell(Vec2{250.0f, 72.0f})}};
    world.flow().bake(mask, fdesc);

    // 200 named agents too: the aim fallback walks the named view, and the
    // named-target snapshot is rebuilt every tick. With no elites present
    // both are free, which would make this a lie about the real worst case.
    REQUIRE(named::setup_bench_scenario(world, 200) == 200);

    TowerSystem ts;
    ts.register_systems(world);
    u32 placed = 0;
    for (u32 i = 0; i < 25; ++i) {
        const auto type = static_cast<TowerType>(i % kTowerTypeCount);
        const Vec2 at{20.0f + 28.0f * static_cast<f32>(i % 8), 40.0f + 30.0f * static_cast<f32>(i / 8)};
        if (ts.place(world, type, at).valid()) ++placed;
    }
    REQUIRE(placed == 25);

    Rng r(31337);
    for (u32 i = 0; i < 10000; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{r.range_f(10.0f, 246.0f), r.range_f(10.0f, 134.0f)};
        p.density = 1000.0f;   // survives the whole run so the load never thins
        p.family = static_cast<PathogenFamily>(i % kFamilyCount);
        world.chaff().spawn(p);
    }
    REQUIRE(world.chaff().count() == 10000);

    Profiler profiler;
    profiler.reserve(400);
    for (int i = 0; i < 30; ++i) world.tick(nullptr);   // warm up caches / scratch
    for (int i = 0; i < 240; ++i) world.tick(&profiler);

    const auto stats = profiler.summarize();
    const TimingStats ecs = stats.at(prof_key::kEcsTick);
    const TimingStats chaff = stats.at(prof_key::kChaffUpdate);
    std::fprintf(stderr,
                 "[swarm perf] 25 towers @ 10k chaff -- ecs_tick avg=%.3fms p99=%.3fms max=%.3fms | "
                 "chaff_update (SERIAL, not the gate) avg=%.3fms p99=%.3fms | live swarmers=%zu rounds=%zu\n",
                 ecs.avg_ms, ecs.p99_ms, ecs.max_ms, chaff.avg_ms, chaff.p99_ms,
                 world.swarmers().count(), world.projectiles().count());

    CHECK(ecs.p99_ms < 2.0);
    CHECK(ecs.avg_ms < 1.0);
}

// ---------------------------------------------------------------------------
// VISUAL PROOF
//
// These drive the headless-GL Renderer path through the public APIs directly
// -- the fallback tests/test_render_vfx.cpp established -- and write PNGs to
// $TEMP for manual read-back.
// ---------------------------------------------------------------------------

namespace {

struct HeadlessGl {
    platform::Window window;
    bool ok = false;
    HeadlessGl(i32 w, i32 h) { ok = platform::create_headless_gl(window, w, h); }
};

std::string scratch_path(const std::string& filename) {
    const char* t = std::getenv("TEMP");
    if (t == nullptr) t = std::getenv("TMP");
    const std::string dir = t != nullptr ? std::string(t) : std::string(".");
    return dir + "/" + filename;
}

constexpr f32 kShowW = 120.0f;
constexpr f32 kShowH = 68.0f;

/// Open arena with a two-cell wall margin, so every tower has clearance and the
/// horde has somewhere to be.
SimWorld make_showcase_world(u64 seed) {
    SimWorld world;
    SimDesc desc;
    desc.seed = seed;
    desc.max_chaff = 16384;
    desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{kShowW, kShowH}};
    world.init(desc, nullptr);

    TissueMask& mask = world.tissue();
    mask.resize(static_cast<i32>(kShowW), static_cast<i32>(kShowH), 1.0f, Vec2{0.0f, 0.0f});
    for (i32 y = 3; y < static_cast<i32>(kShowH) - 3; ++y)
        for (i32 x = 3; x < static_cast<i32>(kShowW) - 3; ++x) mask.set_walkable(x, y, true);
    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goals = {sim::FlowGoal{mask.world_to_cell(Vec2{kShowW - 5.0f, kShowH * 0.5f})}};
    world.flow().bake(mask, fdesc);
    return world;
}

/// One frame of the real App loop's VFX half: drain the tick's combat events
/// into particles, clear the sink, advance the particles on the render clock.
void pump_vfx(SimWorld& world, vfx::ParticleSystem& particles, f32 dt) {
    const auto& events = world.combat_events().events();
    particles.emit_for_events(events.data(), events.size());
    world.combat_events().clear();
    particles.update(dt, nullptr);
}

/// Same submission order App::render_frame() uses, then a PNG.
bool capture(SimWorld& world, vfx::ParticleSystem& particles, render::Renderer& renderer,
             const render::Camera& camera, const std::string& out, render::FrameStats& out_stats) {
    static std::vector<vfx::ParticleInstance> scratch;
    renderer.begin_frame(camera, 0.0f);
    renderer.submit_tissue(world.tissue(), world.sdf(), 0.0f);
    renderer.submit_chaff(world.chaff(), world.spatial());
    renderer.submit_entities(world.ecs());
    renderer.submit_fields(world.damage().fields().data(), world.damage().fields().size(),
                           world.slow_zones().zones().data(), world.slow_zones().zones().size());
    renderer.submit_projectiles(world.projectiles());
    renderer.submit_swarmers(world.swarmers());
    renderer.submit_fluid(world.fluid(), world.fluid_system().draw_radius());
    particles.build_instances(vfx::BlendMode::Additive, scratch);
    renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::Additive);
    particles.build_instances(vfx::BlendMode::AlphaBlend, scratch);
    renderer.submit_particles(scratch.data(), scratch.size(), vfx::BlendMode::AlphaBlend);
    renderer.end_frame();
    out_stats = renderer.stats();

    std::vector<u8> pixels;
    i32 w = 0, h = 0;
    if (!renderer.read_pixels(pixels, w, h)) return false;
    return render::write_png_rgba(out, pixels.data(), w, h);
}

} // namespace

TEST_CASE("VISUAL: all seven towers release at once, with live swarmers and particles",
          "[towers][combat][visual][gl]") {
    HeadlessGl gl(1600, 900);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    SimWorld world = make_showcase_world(4242);
    TowerSystem ts;
    ts.register_systems(world);

    struct Site { TowerType type; Vec2 tower; Vec2 horde; };
    const Site sites[] = {
        {TowerType::Neutrophil, {22.0f, 50.0f}, {32.0f, 50.0f}},   // SHOOTER
        {TowerType::Macrophage, {22.0f, 18.0f}, {32.0f, 18.0f}},   // ARBOR GRABBER
        {TowerType::Interferon, {62.0f, 50.0f}, {71.0f, 50.0f}},   // SLOW BOMBER
        {TowerType::CytotoxicT, {62.0f, 18.0f}, {68.0f, 18.0f}},   // LATCH
        {TowerType::GobletCell, {98.0f, 34.0f}, {107.0f, 34.0f}},  // MUCUS BOMBER
        {TowerType::Fibroblast, {98.0f, 58.0f}, {110.0f, 58.0f}},  // BUILDER
    };
    static_assert(sizeof(sites) / sizeof(sites[0]) == kTowerTypeCount, "one site per roster type");

    std::vector<EntityId> towers;
    for (const Site& s : sites) {
        const EntityId id = ts.place(world, s.type, s.tower);
        INFO("placing " << tower_type_name(s.type));
        REQUIRE(id.valid());
        // Tier 3 everywhere: this is the escalated look.
        REQUIRE(ts.upgrade(world, id) == 2);
        REQUIRE(ts.upgrade(world, id) == 3);
        towers.push_back(id);
    }

    vfx::ParticleSystem particles;
    particles.init(vfx::ParticleSystem::kDefaultCapacity, 0xBEEF'CAFEull);

    auto reseed_hordes = [&] {
        for (const Site& s : sites) spawn_chaff_cluster(world, s.horde, 60, 20.0f, /*spread=*/1.8f);
    };
    reseed_hordes();

    // Warm up: swarmers get into the field, shooters reach their standoff,
    // bombers land their first circles. Top the hordes back up so nothing is
    // starved at capture.
    for (int t = 0; t < 120; ++t) {
        world.tick();
        pump_vfx(world, particles, kFixedDt);
        if (t % 30 == 29) reseed_hordes();
    }

    // Showtime: every tower releases on the same tick, so one still frame
    // carries all six vocabularies at once.
    for (EntityId id : towers) ready_now(world, id);
    for (int t = 0; t < 4; ++t) {
        world.tick();
        pump_vfx(world, particles, kFixedDt);
    }

    INFO("live particles: " << particles.live_count() << ", live swarmers: " << world.swarmers().count());
    REQUIRE(particles.live_count() > 100);
    REQUIRE(world.swarmers().count() > 0);

    render::RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    render::Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_bounds(world.desc().world_bounds);
    camera.set_center(world.desc().world_bounds.center());
    camera.set_view_height(kShowH);
    camera.clamp_to_bounds();

    const std::string out = scratch_path("swarm_roster.png");
    render::FrameStats stats{};
    REQUIRE(capture(world, particles, renderer, camera, out, stats));
    std::fprintf(stderr,
                 "[swarm] %s  particles_drawn=%u swarmers_drawn=%u projectiles_drawn=%u fields=%u chaff=%u draws=%u\n",
                 out.c_str(), stats.particle_instances_drawn, stats.swarmer_instances_drawn,
                 stats.projectile_instances_drawn, stats.vfx_fields_drawn,
                 stats.chaff_instances_drawn, stats.draw_calls);

    REQUIRE(stats.particle_instances_drawn > 100);
    REQUIRE(stats.swarmer_instances_drawn > 0);

    // Per-tower close-ups of the SAME instant.
    for (const Site& s : sites) {
        camera.set_view_height(26.0f);
        camera.set_center(math::lerp(s.tower, s.horde, 0.55f));
        render::FrameStats close{};
        const std::string path = scratch_path(std::string("swarm_") + tower_type_name(s.type) + ".png");
        REQUIRE(capture(world, particles, renderer, camera, path, close));
        std::fprintf(stderr, "[swarm] %s particles=%u swarmers=%u\n", path.c_str(),
                     close.particle_instances_drawn, close.swarmer_instances_drawn);
    }

    renderer.shutdown();
}

TEST_CASE("VISUAL: a Fibroblast walls the lane and the horde chews on the collagen",
          "[towers][combat][visual][gl][scar]") {
    HeadlessGl gl(1600, 900);
    if (!gl.ok) { WARN("headless GL unavailable; skipping"); return; }

    // The horde has to bite for the walls to show wear: the showcase world
    // ships the hostile pass off, so it is switched on here with the shipped
    // virus numbers.
    SimWorld world = make_showcase_world(777);
    {
        HostileTuning ht;
        ht.enabled = true;
        ht.family[0].latch_dps = 2.0f;
        ht.family[0].latch_reach = 0.4f;
        ht.family[0].latch_cap_scar = 40;
        ht.family[0].latch_speed = 30.0f;
        ht.family[0].latch_ease_distance = 1.2f;
        ht.family[0].latch_ease_power = 3.0f;
        world.hostile().set_tuning(ht);
    }
    TowerSystem ts;
    ts.register_systems(world);
    // Off to one side of the horde's band, so the tower itself is not eaten
    // before its walls are; its reach still spans the lane.
    const Vec2 at{54.0f, 50.0f};
    const EntityId id = ts.place(world, TowerType::Fibroblast, at);
    REQUIRE(id.valid());
    REQUIRE(ts.upgrade(world, id) == 2);
    REQUIRE(ts.upgrade(world, id) == 3);

    vfx::ParticleSystem particles;
    particles.init(vfx::ParticleSystem::kDefaultCapacity, 0xC0FFEEull);

    // Twenty seconds: enough for the builders to lay the tier's worth of
    // walls, and for a horde walking in from upstream to pile up against
    // them and start eating.
    for (int t = 0; t < 60 * 20; ++t) {
        if (t % 30 == 0 && t < 60 * 16) spawn_chaff_cluster(world, Vec2{14.0f, 34.0f}, 10, 1.0f, 5.0f);
        world.tick();
        pump_vfx(world, particles, kFixedDt);
    }
    const SimSnapshot snap = world.snapshot();
    INFO("scars live " << snap.scars_live << " built " << snap.scars_built_total << " latched "
                       << snap.chaff_latched);
    REQUIRE(snap.scars_built_total > 0);
    REQUIRE(snap.scars_live > 0);

    render::RendererDesc rd;
    rd.framebuffer_width = gl.window.width();
    rd.framebuffer_height = gl.window.height();
    rd.max_chaff_instances = static_cast<u32>(world.desc().max_chaff);
    render::Renderer renderer;
    REQUIRE(renderer.init(rd));

    render::Camera camera;
    camera.set_viewport(gl.window.width(), gl.window.height());
    camera.set_bounds(world.desc().world_bounds);
    camera.set_center(at + Vec2{-2.0f, -10.0f});
    camera.set_view_height(46.0f);
    camera.clamp_to_bounds();

    const std::string out = scratch_path("scar_fibroblast.png");
    render::FrameStats stats{};
    REQUIRE(capture(world, particles, renderer, camera, out, stats));
    std::fprintf(stderr, "[scar] %s scars=%u latched=%u chaff=%u draws=%u\n", out.c_str(),
                 snap.scars_live, snap.chaff_latched, stats.chaff_instances_drawn, stats.draw_calls);

    camera.set_view_height(18.0f);
    render::FrameStats close{};
    const std::string near = scratch_path("scar_fibroblast_close.png");
    REQUIRE(capture(world, particles, renderer, camera, near, close));
    std::fprintf(stderr, "[scar] %s\n", near.c_str());

    renderer.shutdown();
}

// ---------------------------------------------------------------------------
// Level starts are fresh: App keeps ONE TowerSystem for the whole session and
// re-binds it to a new SimWorld on every level load.
// ---------------------------------------------------------------------------

TEST_CASE("register_systems() drops the previous level's placed towers",
          "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    REQUIRE(ts.place(world, TowerType::Macrophage, kRoomCenterLeft).valid());
    REQUIRE(ts.place(world, TowerType::Macrophage, kRoomCenterRight).valid());
    REQUIRE(ts.placed_towers().size() == 2);

    // Next level, same TowerSystem. Those two ids belong to a world that no
    // longer exists; carrying them over leaves the new level reporting towers
    // that were never built, and hands them to the HUD and the balance bot.
    SimWorld next = make_world();
    ts.register_systems(next);
    REQUIRE(ts.placed_towers().empty());

    REQUIRE(ts.place(next, TowerType::Macrophage, kRoomCenterLeft).valid());
    REQUIRE(ts.placed_towers().size() == 1);
}

TEST_CASE("the second level of a session does the same damage as the first",
          "[towers][combat]") {
    // The player-visible symptom of a world that carried its systems across a
    // level load: App re-registers the tower systems on every load, so level 2
    // ran two copies of tower_spawner and friends, level 3 ran three, and
    // how hard a tower hit depended on how many levels you had started
    // this session.
    SimWorld world = make_world();
    TowerSystem ts;

    auto play_a_level = [&ts](SimWorld& w) {
        ts.register_systems(w);
        REQUIRE(ts.place(w, TowerType::Macrophage, kRoomCenterLeft).valid());
        // Deliberately more horde than the tower can chew through in the window
        // below: a cluster that dies either way measures nothing, since "wiped
        // out" and "wiped out twice as fast" leave the same final density.
        spawn_chaff_cluster(w, kRoomCenterLeft + Vec2{4.0f, 0.0f}, 500, 40.0f);
        const f32 before = w.chaff().total_density();
        for (int i = 0; i < 400; ++i) step_combat(w);
        REQUIRE(w.chaff().total_density() > 0.0f);
        return before - w.chaff().total_density();
    };

    const f32 first = play_a_level(world);
    REQUIRE(first > 0.0f);

    // Next level, same App: one SimWorld re-initialised, the same TowerSystem
    // re-bound to it. Identical scene, identical seed, so identical damage.
    build_scene(world);
    const f32 second = play_a_level(world);
    REQUIRE(second == Catch::Approx(first));
}
