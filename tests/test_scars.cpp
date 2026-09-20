// Coverage for the Fibroblast and its collagen scars: the builder swarmer
// kind (sim/swarm), the scar system (sim/scar), the hostile pass's bar hosts
// (sim/hostile), and what the game layer does with all three (game/towers).
//
// The properties the design rests on, not balance numbers: a builder walks
// to the site it was given and lays a bar ACROSS the flow there; the bar is
// solid ground to the horde and nothing at all to the player's own units;
// viruses latch along its faces and bacteria burn it, and at zero integrity
// it hands the tissue back; a builder arriving on a standing scar reinforces
// it rather than stacking a second; a bar that would seal the lane is never
// laid; the tower releases builders that actually build; and the whole thing
// is deterministic.
#include "sim/scar/Scars.h"

#include "game/towers/TowerSystem.h"
#include "game/towers/TowerMechanics.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/NamedAgents.h"
#include "sim/flowfield/RuntimeBlock.h"
#include "sim/hostile/HostileAttacks.h"
#include "sim/projectile/Projectiles.h"
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
constexpr i32 kW = 60;
constexpr i32 kH = 20;
constexpr f32 kVirusRadius = 0.9f;
constexpr f32 kBacteriaRadius = 2.0f;
constexpr u16 kBuilderSlot = 3;
const Vec2 kGoal{58.0f, 10.0f};

/// An open 60x20 field flowing left to right, hostile pass on, and a builder
/// profile registered. Everything the real game wires up, minus a level.
struct Field {
    SimWorld world;
    SwarmerProfile builder;

    explicit Field(u64 seed, i32 lane_height = kH, bool hostile_on = true) {
        SimDesc desc;
        desc.seed = seed;
        desc.max_chaff = 4096;
        desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{static_cast<f32>(kW), static_cast<f32>(kH)}};
        desc.spatial_cell_size = 4.0f;
        desc.chaff_tuning.family[0].radius = kVirusRadius;
        desc.chaff_tuning.family[1].radius = kBacteriaRadius;
        desc.chaff_tuning.family[0].replication_rate = 0.0f;
        desc.hostile_tuning.enabled = hostile_on;
        desc.hostile_tuning.family[0].latch_dps = 20.0f;
        desc.hostile_tuning.family[0].latch_reach = 0.4f;
        desc.hostile_tuning.family[0].latch_cap_swarmer = 3;
        desc.hostile_tuning.family[0].latch_cap_tower = 12;
        desc.hostile_tuning.family[0].latch_cap_scar = 24;
        desc.hostile_tuning.family[1].aura_dps = 30.0f;
        desc.hostile_tuning.family[1].aura_radius = 4.0f;
        world.init(desc, nullptr);

        TissueMask& mask = world.tissue();
        mask.resize(kW, kH, 1.0f, Vec2{0.0f, 0.0f});
        const i32 y0 = kH / 2 - lane_height / 2;
        for (i32 y = y0; y < y0 + lane_height; ++y)
            for (i32 x = 0; x < kW; ++x) mask.set_walkable(x, y, true);
        world.sdf().bake(mask);
        FlowFieldBakeDesc fdesc;
        fdesc.goals = {sim::FlowGoal{mask.world_to_cell(kGoal)}};
        world.flow().bake(mask, fdesc);

        builder.kind = SwarmerKind::Builder;
        builder.source = TowerType::Fibroblast;
        builder.lifetime = 20.0f;
        builder.speed = 12.0f;
        builder.attach_radius = 0.8f;
        builder.size = 1.6f;
        builder.max_health = 10.0f;
        builder.scar_half_length = 5.0f;
        builder.scar_half_width = 0.9f;
        builder.scar_health = 100.0f;
        builder.scar_reinforce = 40.0f;
        builder.scar_spacing = 6.0f;
        builder.max_scars = 3;
        builder.scar_lifetime = 0.0f;
        world.swarmers().set_profile(kBuilderSlot, builder);
    }

    usize release_builder(Vec2 at, Vec2 goal, bool has_goal = true) {
        SwarmerSpawnParams p;
        p.position = at;
        p.profile = kBuilderSlot;
        p.owner = EntityId{7u};
        p.seed = 0x51u + static_cast<u32>(world.swarmers().count()) * 7919u;
        p.goal = goal;
        p.has_goal = has_goal;
        world.swarmers().spawn(p);
        return world.swarmers().count() - 1;
    }

    ScarDesc desc_at(Vec2 p) {
        ScarDesc d = scar_desc_from_profile(builder);
        d.center = p;
        d.owner = EntityId{7u};
        return d;
    }

    void spawn_chaff(Vec2 p, PathogenFamily family = PathogenFamily::Virus) {
        ChaffSpawnParams c;
        c.position = p;
        c.density = 1.0f;
        c.family = family;
        world.chaff().spawn(c);
    }

    u32 scar_count() {
        u32 n = 0;
        for ([[maybe_unused]] auto e : world.ecs().registry().view<comp::Scar>()) ++n;
        return n;
    }

    entt::entity first_scar() {
        for (auto e : world.ecs().registry().view<comp::Scar>()) return e;
        return entt::null;
    }

    Bar bar_of(entt::entity e) {
        const auto& sc = world.ecs().registry().get<comp::Scar>(e);
        const auto& tf = world.ecs().registry().get<comp::Transform>(e);
        Bar b;
        b.center = tf.position;
        b.half_extents = sc.half_extents;
        b.rotation = tf.rotation;
        return b;
    }

    u32 walkable_count() {
        u32 n = 0;
        const TissueMask& mask = world.tissue();
        for (i32 y = 0; y < mask.height(); ++y)
            for (i32 x = 0; x < mask.width(); ++x) n += mask.walkable(x, y) ? 1u : 0u;
        return n;
    }
};

u32 count_events(const CombatEventSink& sink, CombatEventType type) {
    u32 n = 0;
    for (const CombatEvent& e : sink.events()) n += e.type == type ? 1u : 0u;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// The builder swarmer.
// ---------------------------------------------------------------------------

TEST_CASE("a builder walks to its site and lays a scar across the flow there", "[sim][scar][swarm]") {
    Field f(1);
    const Vec2 site{30.0f, 10.0f};
    f.release_builder(Vec2{18.0f, 10.0f}, site);
    const u32 open_before = f.walkable_count();

    u32 ticks = 0;
    while (f.scar_count() == 0 && ticks < 240) {
        f.world.tick();
        ++ticks;
    }
    REQUIRE(f.scar_count() == 1);
    // At 12 u/s over 12 units it is a one-second walk, give or take the arrive.
    REQUIRE(ticks > 30);
    REQUIRE(ticks < 150);
    // The builder is spent.
    REQUIRE(f.world.swarmers().count() == 0);
    REQUIRE(f.world.scars().stats().built_total == 1);
    REQUIRE(f.world.snapshot().scars_live == 1);

    // Laid at the SITE, across the flow: the field runs +x, so the bar runs
    // along +-y, and it carries the profile's numbers.
    const entt::entity e = f.first_scar();
    const Bar bar = f.bar_of(e);
    REQUIRE(bar.center.x == Catch::Approx(site.x));
    REQUIRE(bar.center.y == Catch::Approx(site.y));
    REQUIRE(std::fabs(std::sin(bar.rotation)) == Catch::Approx(1.0f).margin(0.05f));
    REQUIRE(bar.half_extents.x == Catch::Approx(5.0f));
    REQUIRE(bar.half_extents.y == Catch::Approx(0.9f));
    const auto& hp = f.world.ecs().registry().get<comp::Health>(e);
    REQUIRE(hp.max == Catch::Approx(100.0f));
    REQUIRE(hp.current == Catch::Approx(100.0f));
    REQUIRE(f.world.ecs().registry().get<comp::Scar>(e).owner == EntityId{7u});

    // Carved: every cell whose centre is inside the bar is now solid, and
    // nothing outside it moved.
    const TissueMask& mask = f.world.tissue();
    u32 inside = 0;
    for (i32 y = 0; y < mask.height(); ++y) {
        for (i32 x = 0; x < mask.width(); ++x) {
            const bool in = bar.contains(mask.cell_to_world(x, y));
            if (in) ++inside;
            REQUIRE(mask.walkable(x, y) == !in);
        }
    }
    REQUIRE(inside > 0);
    REQUIRE(f.walkable_count() == open_before - inside);
    // The flow was told (the pump drains it within a few ticks).
    for (int k = 0; k < 60 && f.world.flow().has_pending_rebake(); ++k) f.world.tick();
    REQUIRE_FALSE(f.world.flow().has_pending_rebake());
    // Upstream of the bar the field now steers round it rather than into it:
    // below the centreline it heads for the gap under the bar, above it for
    // the gap over it. (Dead on the centreline the two cancel by symmetry.)
    REQUIRE(f.world.flow().sample(Vec2{28.5f, 7.0f}).y < -0.3f);
    REQUIRE(f.world.flow().sample(Vec2{28.5f, 13.0f}).y > 0.3f);
}

TEST_CASE("a builder released with no site drifts and dissolves without building", "[sim][scar][swarm]") {
    Field f(2);
    f.builder.lifetime = 1.0f;
    f.world.swarmers().set_profile(kBuilderSlot, f.builder);
    f.release_builder(Vec2{18.0f, 10.0f}, Vec2{}, /*has_goal=*/false);
    for (int k = 0; k < 90; ++k) f.world.tick();
    REQUIRE(f.world.swarmers().count() == 0);
    REQUIRE(f.scar_count() == 0);
    REQUIRE(f.world.scars().stats().built_total == 0);
}

// ---------------------------------------------------------------------------
// The wall.
// ---------------------------------------------------------------------------

TEST_CASE("a scar is solid ground to the horde and nothing to the player's units", "[sim][scar]") {
    Field f(3, kH, /*hostile_on=*/false);
    ScarBuildResult r;
    const EntityId id = f.world.scars().build(f.world, f.desc_at(Vec2{30.0f, 10.0f}), &r, nullptr);
    REQUIRE(r == ScarBuildResult::Built);
    REQUIRE(id.valid());
    const Bar bar = f.bar_of(f.world.ecs().from_id(id));
    f.world.flow().rebake_pending(f.world.tissue());

    // A column of pathogens upstream, walking the flow.
    for (int k = 0; k < 40; ++k) {
        f.spawn_chaff(Vec2{18.0f + static_cast<f32>(k % 5), 6.0f + static_cast<f32>(k % 9)});
    }
    // A builder sent straight through the wall to a site beyond it.
    const usize unit = f.release_builder(Vec2{20.0f, 10.0f}, Vec2{42.0f, 10.0f});
    f.builder.scar_spacing = 0.0f;   // let it build on the far side
    f.world.swarmers().set_profile(kBuilderSlot, f.builder);
    (void)unit;

    bool crossed = false;
    u32 chaff_inside = 0;
    u32 chaff_past = 0;
    for (int t = 0; t < 420; ++t) {
        f.world.tick();
        const ChaffBuffers& chaff = f.world.chaff();
        const TissueMask& mask = f.world.tissue();
        for (usize i = 0; i < chaff.count(); ++i) {
            const Vec2 p{chaff.pos_x[i], chaff.pos_y[i]};
            const IVec2 c = mask.world_to_cell(p);
            if (bar.contains(p) && !mask.walkable(c.x, c.y)) ++chaff_inside;
            if (p.x > bar.center.x + 3.0f) ++chaff_past;
        }
        const SwarmerBuffers& sw = f.world.swarmers();
        for (usize i = 0; i < sw.count(); ++i) {
            if (sw.pos_x[i] > bar.center.x + 2.0f) crossed = true;
        }
    }
    // Never an agent standing in the collagen; plenty got round it.
    REQUIRE(chaff_inside == 0);
    REQUIRE(chaff_past > 0);
    // The builder walked through the wall as if it were not there and built
    // on the far side.
    REQUIRE(crossed);
    REQUIRE(f.scar_count() == 2);

    // The mask knows the difference: the wall's cells are runtime blocks, not
    // authored tissue, and the level's real edge is still a wall to all.
    const TissueMask& mask = f.world.tissue();
    {
        const IVec2 c = mask.world_to_cell(bar.center);
        REQUIRE_FALSE(mask.walkable(c.x, c.y));
        REQUIRE(mask.runtime_block(c.x, c.y));
        REQUIRE_FALSE(mask.authored_wall(c.x, c.y));
        REQUIRE(mask.authored_wall(c.x, -1));
    }
}

TEST_CASE("the shared containment holds a walker at a block's face and keeps its slide",
          "[sim][scar]") {
    Field f(15, kH, false);
    ScarBuildResult r;
    REQUIRE(f.world.scars().build(f.world, f.desc_at(Vec2{30.0f, 10.0f}), &r, nullptr).valid());
    const TissueMask& mask = f.world.tissue();
    // The wall's upstream face: the last walkable column before x = 30 - 0.9.
    // Stepping straight into it is stopped at the face; stepping diagonally
    // keeps the along-wall part of the step.
    Vec2 p{29.5f, 10.0f};
    contain_to_walkable(mask, p, Vec2{28.5f, 10.0f});
    {
        const IVec2 c = mask.world_to_cell(p);
        REQUIRE(mask.walkable(c.x, c.y));
        REQUIRE(p.x < 29.5f);
        REQUIRE(p.x > 28.5f);
        REQUIRE(p.y == Catch::Approx(10.0f));
    }
    Vec2 q{29.5f, 11.0f};
    contain_to_walkable(mask, q, Vec2{28.5f, 10.0f});
    {
        const IVec2 c = mask.world_to_cell(q);
        REQUIRE(mask.walkable(c.x, c.y));
        REQUIRE(q.y == Catch::Approx(11.0f));   // the slide survived
        REQUIRE(q.x > 28.5f);
    }
    // A step that ends on walkable ground is untouched, and one that STARTED
    // inside is left to the SDF response.
    Vec2 ok{27.0f, 12.0f};
    contain_to_walkable(mask, ok, Vec2{26.0f, 12.0f});
    REQUIRE(ok.x == 27.0f);
    Vec2 buried{30.5f, 10.0f};
    contain_to_walkable(mask, buried, Vec2{30.0f, 10.0f});
    REQUIRE(buried.x == 30.5f);
}

TEST_CASE("a Neutrophil's rounds fly through a scar; an elite does not walk through one",
          "[sim][scar][projectile][named]") {
    Field f(14, kH, /*hostile_on=*/false);
    ScarBuildResult r;
    const EntityId id = f.world.scars().build(f.world, f.desc_at(Vec2{30.0f, 10.0f}), &r, nullptr);
    REQUIRE(r == ScarBuildResult::Built);
    const Bar bar = f.bar_of(f.world.ecs().from_id(id));
    f.world.flow().rebake_pending(f.world.tissue());

    // ROUNDS. A target sits behind the wall; rounds fired from in front of it
    // cross the collagen and land, with no wall impact counted. The same
    // rounds fired at the lane's authored edge are stopped, so the test
    // proves the distinction and not merely that walls are off.
    f.spawn_chaff(Vec2{33.0f, 10.0f});
    u32 wall_impacts = 0;
    u32 impacts = 0;
    for (int k = 0; k < 6; ++k) {
        ProjectileSpawnParams p;
        p.position = Vec2{26.0f, 9.0f + static_cast<f32>(k) * 0.4f};
        p.velocity = Vec2{40.0f, 0.0f};
        p.damage = 0.01f;   // a graze: the target must survive all six
        p.lifetime = 1.0f;
        p.hit_radius = 0.6f;
        REQUIRE(f.world.projectiles().spawn(p));
    }
    for (int t = 0; t < 30; ++t) {
        f.world.tick();
        wall_impacts += f.world.projectile_system().last_stats().wall_impacts;
        impacts += f.world.projectile_system().last_stats().impacts;
    }
    REQUIRE(wall_impacts == 0);
    REQUIRE(impacts > 0);
    // Let the strays run out (they would count against the grid's far edge,
    // which IS a wall) before the control shot.
    for (int t = 0; t < 90 && f.world.projectiles().count() > 0; ++t) f.world.tick();
    REQUIRE(f.world.projectiles().count() == 0);
    {
        ProjectileSpawnParams p;
        p.position = Vec2{10.0f, 10.0f};
        p.velocity = Vec2{0.0f, 40.0f};   // straight at the lane's top edge
        p.lifetime = 1.0f;
        REQUIRE(f.world.projectiles().spawn(p));
        u32 edge_impacts = 0;
        for (int t = 0; t < 30; ++t) {
            f.world.tick();
            edge_impacts += f.world.projectile_system().last_stats().wall_impacts;
        }
        REQUIRE(edge_impacts == 1);
    }

    // ELITE. A named agent released upstream follows the flow into the wall
    // and is held at its face like the chaff are: its centre never enters a
    // carved cell, and it still gets round to the far side. Released a little
    // off the wall's centreline, where the rerouted field already leans
    // toward the gap; dead on it the two ways round cancel and it would sit
    // pressed against the collagen, which is correct but proves nothing.
    named::install(f.world);
    const u16 archetype = named::placeholder_elite(f.world.ecs().registry());
    named::SpawnParams sp;
    sp.archetype = archetype;
    sp.position = Vec2{24.0f, 7.0f};
    const EntityId elite = named::spawn(f.world, sp);
    REQUIRE(elite.valid());
    const entt::entity ee = f.world.ecs().from_id(elite);
    bool inside = false;
    bool past = false;
    for (int t = 0; t < 60 * 12 && f.world.ecs().registry().valid(ee); ++t) {
        f.world.tick();
        const Vec2 p = f.world.ecs().registry().get<comp::Transform>(ee).position;
        const IVec2 c = f.world.tissue().world_to_cell(p);
        if (bar.contains(p) && !f.world.tissue().walkable(c.x, c.y)) inside = true;
        if (p.x > bar.center.x + 2.0f) past = true;
    }
    REQUIRE_FALSE(inside);
    REQUIRE(past);
}

TEST_CASE("viruses latch along a scar's faces and bacteria burn it, and at zero it hands the tissue back",
          "[sim][scar][hostile]") {
    Field f(4);
    const u32 open_before = f.walkable_count();
    ScarBuildResult r;
    // Tough enough to be fed on for a few seconds: eight viruses at 20 hp/s
    // each would empty the fixture's 100-hp wall in under a second.
    ScarDesc tough = f.desc_at(Vec2{30.0f, 10.0f});
    tough.max_health = 600.0f;
    const EntityId id = f.world.scars().build(f.world, tough, &r, nullptr);
    REQUIRE(r == ScarBuildResult::Built);
    const entt::entity e = f.world.ecs().from_id(id);
    const Bar bar = f.bar_of(e);
    REQUIRE(f.walkable_count() < open_before);

    // Viruses touching the upstream face, spread along its length; one at
    // each end of the bar too, where a centre-only walk would miss. Two
    // ticks, because the crowd kernel's separation nudges a fresh clump
    // apart before the hostile pass sees it.
    for (int k = 0; k < 8; ++k) {
        const f32 along = -4.5f + static_cast<f32>(k) * (9.0f / 7.0f);
        f.spawn_chaff(bar.center + Vec2{-(bar.half_extents.y + kVirusRadius), along});
    }
    f.world.tick();
    f.world.tick();
    REQUIRE(f.world.snapshot().chaff_latched == 8);
    REQUIRE(count_events(f.world.combat_events(), CombatEventType::PathogenLatch) == 8);
    {
        const usize k = f.world.friendly_towers().find(id);
        REQUIRE(k != FriendlyTowerList::npos);
        REQUIRE(f.world.friendly_towers().items[k].is_bar());
        REQUIRE(f.world.friendly_towers().passengers[k] == 8);
    }
    const f32 hp_after_one = f.world.ecs().registry().get<comp::Health>(e).current;
    REQUIRE(hp_after_one < 600.0f);

    // They settle ON the face -- sunk in by a fraction of their body -- and
    // stay on the side they grabbed rather than crossing the wall.
    for (int k = 0; k < 60; ++k) f.world.tick();
    const ChaffBuffers& chaff = f.world.chaff();
    REQUIRE(chaff.count() == 8);
    for (usize i = 0; i < chaff.count(); ++i) {
        REQUIRE((chaff.flags[i] & chaff_flags::kLatched) != 0);
        const Vec2 p{chaff.pos_x[i], chaff.pos_y[i]};
        const f32 d = std::sqrt(bar.distance_sq(p));
        REQUIRE(d == Catch::Approx(kVirusRadius * 0.35f).margin(0.05f));
        REQUIRE(p.x < bar.center.x);
    }
    // Feeding at latch_dps each: 8 x 20 hp/s.
    const f32 hp_a = f.world.ecs().registry().get<comp::Health>(e).current;
    for (int k = 0; k < 30; ++k) f.world.tick();
    const f32 hp_b = f.world.ecs().registry().get<comp::Health>(e).current;
    REQUIRE((hp_a - hp_b) == Catch::Approx(8.0f * 20.0f * 0.5f).margin(1.0f));

    // A bacterium at the far END of the bar burns it: the aura reaches the
    // wall and the segmented walk reaches the bacterium.
    Field g(5);
    const EntityId id2 = g.world.scars().build(g.world, g.desc_at(Vec2{30.0f, 10.0f}), &r, nullptr);
    REQUIRE(r == ScarBuildResult::Built);
    const entt::entity e2 = g.world.ecs().from_id(id2);
    g.spawn_chaff(Vec2{30.0f, 10.0f + 5.0f + 2.5f}, PathogenFamily::Bacteria);   // 2.5 past the end
    g.spawn_chaff(Vec2{30.0f + 8.0f, 10.0f}, PathogenFamily::Bacteria);          // out of reach
    g.world.tick();
    REQUIRE(g.world.hostile().last_stats().aura_hits == 1);
    REQUIRE(g.world.ecs().registry().get<comp::Health>(e2).current == Catch::Approx(100.0f - 30.0f * kDt));

    // Chewed to nothing: the entity goes, the cells come back, the flow is
    // told, the passengers are let go, and it is counted as lost.
    for (int k = 0; k < 600 && f.world.ecs().registry().valid(e); ++k) f.world.tick();
    REQUIRE_FALSE(f.world.ecs().registry().valid(e));
    REQUIRE(f.scar_count() == 0);
    REQUIRE(f.walkable_count() == open_before);
    REQUIRE(f.world.scars().stats().lost_total == 1);
    REQUIRE(f.world.snapshot().scars_lost_total == 1);
    REQUIRE(f.world.snapshot().towers_lost_total == 0);
    f.world.tick();
    REQUIRE(f.world.snapshot().chaff_latched == 0);
    for (int k = 0; k < 60 && f.world.flow().has_pending_rebake(); ++k) f.world.tick();
    REQUIRE_FALSE(f.world.flow().has_pending_rebake());
}

TEST_CASE("a builder arriving on a standing scar reinforces it instead of stacking a second",
          "[sim][scar]") {
    Field f(6, kH, false);
    ScarBuildResult r;
    const EntityId id = f.world.scars().build(f.world, f.desc_at(Vec2{30.0f, 10.0f}), &r, nullptr);
    REQUIRE(r == ScarBuildResult::Built);
    comp::Health& hp = f.world.ecs().registry().get<comp::Health>(f.world.ecs().from_id(id));
    hp.current = 30.0f;

    // Inside the spacing: reinforce, capped at the max.
    const EntityId again = f.world.scars().build(f.world, f.desc_at(Vec2{33.0f, 11.0f}), &r, nullptr);
    REQUIRE(r == ScarBuildResult::Reinforced);
    REQUIRE(again == id);
    REQUIRE(hp.current == Catch::Approx(70.0f));
    REQUIRE(f.scar_count() == 1);
    f.world.scars().build(f.world, f.desc_at(Vec2{33.0f, 11.0f}), &r, nullptr);
    REQUIRE(hp.current == Catch::Approx(100.0f));
    REQUIRE(f.world.scars().stats().reinforced_total == 2);

    // Outside it: a second scar.
    const EntityId second = f.world.scars().build(f.world, f.desc_at(Vec2{44.0f, 10.0f}), &r, nullptr);
    REQUIRE(r == ScarBuildResult::Built);
    REQUIRE(second.valid());
    REQUIRE(second != id);
    REQUIRE(f.scar_count() == 2);

    // At the owner's cap a fresh site still reinforces (the nearest of its own).
    ScarDesc capped = f.desc_at(Vec2{16.0f, 10.0f});
    capped.max_scars = 2;
    hp.current = 10.0f;
    f.world.scars().build(f.world, capped, &r, nullptr);
    REQUIRE(r == ScarBuildResult::Reinforced);
    REQUIRE(f.scar_count() == 2);
    REQUIRE(hp.current == Catch::Approx(50.0f));

    // With nothing to reinforce with, a blocked builder is simply dropped.
    ScarDesc dropped = f.desc_at(Vec2{33.0f, 11.0f});
    dropped.reinforce = 0.0f;
    REQUIRE_FALSE(f.world.scars().build(f.world, dropped, &r, nullptr).valid());
    REQUIRE(r == ScarBuildResult::Dropped);
}

TEST_CASE("a scar that would seal the lane, or lie off the tissue, is refused", "[sim][scar]") {
    // An 8-tall lane cannot take a 10-long bar across it.
    Field f(7, /*lane_height=*/8, false);
    const u32 open_before = f.walkable_count();
    ScarBuildResult r;
    REQUIRE_FALSE(f.world.scars().build(f.world, f.desc_at(Vec2{30.0f, 10.0f}), &r, nullptr).valid());
    REQUIRE(r == ScarBuildResult::WouldSever);
    REQUIRE(f.walkable_count() == open_before);
    REQUIRE(f.scar_count() == 0);
    REQUIRE(f.world.scars().stats().refused_total == 1);
    REQUIRE_FALSE(f.world.flow().has_pending_rebake());

    // Off the tissue: nothing to carve.
    REQUIRE_FALSE(f.world.scars().build(f.world, f.desc_at(Vec2{30.0f, 2.0f}), &r, nullptr).valid());
    REQUIRE(r == ScarBuildResult::OffTissue);

    // A shorter bar fits.
    ScarDesc small = f.desc_at(Vec2{30.0f, 10.0f});
    small.half_extents = Vec2{2.0f, 0.9f};
    REQUIRE(f.world.scars().build(f.world, small, &r, nullptr).valid());
    REQUIRE(r == ScarBuildResult::Built);
}

TEST_CASE("a scar lies across the local flow, give or take its configured tilt", "[sim][scar]") {
    // Square to the arrows with no tilt: the field runs +x, so every bar
    // runs along +-y wherever it is laid.
    Field f(12, kH, false);
    ScarBuildResult r;
    for (int k = 0; k < 4; ++k) {
        ScarDesc d = f.desc_at(Vec2{14.0f + 10.0f * static_cast<f32>(k), 8.0f + static_cast<f32>(k % 2) * 4.0f});
        d.spacing = 0.0f;
        d.max_scars = 0;
        d.tilt = 0.0f;
        const EntityId id = f.world.scars().build(f.world, d, &r, nullptr);
        REQUIRE(r == ScarBuildResult::Built);
        const Bar bar = f.bar_of(f.world.ecs().from_id(id));
        // Measured against the LOCAL arrow, not the world x axis: even an
        // open field bends a little toward a single goal cell.
        const Vec2 arrow = math::normalize_safe(f.world.flow().sample(bar.center));
        const Vec2 along{std::cos(bar.rotation), std::sin(bar.rotation)};
        REQUIRE(std::fabs(arrow.x * along.x + arrow.y * along.y) < 1e-3f);
        REQUIRE(std::fabs(std::sin(bar.rotation)) == Catch::Approx(1.0f).margin(0.1f));
    }

    // With a tilt: every bar stays within the tilt of square, they do not
    // all share one angle, and the same site with the same owner always
    // draws the same one -- it is a hash, not a stream, so the site picker
    // at release and the build on arrival cannot disagree.
    Field g(13, kH, false);
    std::vector<f32> offs;
    for (int k = 0; k < 5; ++k) {
        const Vec2 site{10.0f + 9.0f * static_cast<f32>(k), 8.0f + static_cast<f32>(k % 3) * 2.0f};
        ScarDesc d = g.desc_at(site);
        d.spacing = 0.0f;
        d.max_scars = 0;
        d.tilt = 0.3f;
        const f32 expected = scar_rotation(g.world.flow(), site, d.owner, d.tilt);
        REQUIRE(scar_rotation(g.world.flow(), site, d.owner, d.tilt) == expected);
        const EntityId id = g.world.scars().build(g.world, d, &r, nullptr);
        REQUIRE(r == ScarBuildResult::Built);
        const Bar bar = g.bar_of(g.world.ecs().from_id(id));
        REQUIRE(bar.rotation == Catch::Approx(expected));
        // Angle off square to the local arrow, folded into [-pi/2, pi/2].
        f32 off = bar.rotation - across_flow_rotation(g.world.flow(), site);
        while (off > math::kPi * 0.5f) off -= math::kPi;
        while (off < -math::kPi * 0.5f) off += math::kPi;
        REQUIRE(std::fabs(off) <= 0.3f + 1e-4f);
        offs.push_back(off);
    }
    bool varied = false;
    for (usize i = 1; i < offs.size(); ++i) varied = varied || std::fabs(offs[i] - offs[0]) > 0.02f;
    REQUIRE(varied);
    // A different owner at the same site draws a different tilt.
    REQUIRE(scar_rotation(g.world.flow(), Vec2{10.0f, 8.0f}, EntityId{7u}, 0.3f) !=
            scar_rotation(g.world.flow(), Vec2{10.0f, 8.0f}, EntityId{8u}, 0.3f));
}

TEST_CASE("a scar on a lifetime dissolves on its own and is not counted as lost", "[sim][scar]") {
    Field f(8, kH, false);
    ScarDesc d = f.desc_at(Vec2{30.0f, 10.0f});
    d.lifetime = 0.5f;
    const u32 open_before = f.walkable_count();
    ScarBuildResult r;
    REQUIRE(f.world.scars().build(f.world, d, &r, nullptr).valid());
    REQUIRE(f.walkable_count() < open_before);
    for (int k = 0; k < 20; ++k) f.world.tick();
    REQUIRE(f.scar_count() == 1);
    for (int k = 0; k < 15; ++k) f.world.tick();
    REQUIRE(f.scar_count() == 0);
    REQUIRE(f.walkable_count() == open_before);
    REQUIRE(f.world.scars().stats().dissolved_total == 1);
    REQUIRE(f.world.scars().stats().lost_total == 0);
}

// ---------------------------------------------------------------------------
// The tower.
// ---------------------------------------------------------------------------

TEST_CASE("a Fibroblast releases builders that lay scars in its reach, up to its cap",
          "[game][towers][scar]") {
    Field f(9, kH, false);
    game::TowerSystem towers;
    towers.register_systems(f.world);
    const Vec2 at{24.0f, 10.0f};
    const EntityId tower = towers.place(f.world, TowerType::Fibroblast, at);
    REQUIRE(tower.valid());
    const sim::SwarmerProfile pr = game::swarmer_profile(TowerType::Fibroblast, 1);
    REQUIRE(pr.kind == SwarmerKind::Builder);
    REQUIRE(pr.max_scars > 0);

    for (int k = 0; k < 60 * 40; ++k) f.world.tick();
    const ScarStats& st = f.world.scars().stats();
    REQUIRE(st.built_total >= pr.max_scars);
    REQUIRE(f.scar_count() == pr.max_scars);
    // Past the cap the builders reinforce rather than build.
    REQUIRE(st.reinforced_total > 0);
    // Every wall stands inside the tower's reach and off the tower itself,
    // owned by it, across the flow.
    for (auto e : f.world.ecs().registry().view<comp::Scar>()) {
        const Bar bar = f.bar_of(e);
        const f32 d = math::length(bar.center - at);
        REQUIRE(d <= pr.build_radius + 0.01f);
        REQUIRE(d >= pr.build_min_radius - 0.01f);
        REQUIRE(f.world.ecs().registry().get<comp::Scar>(e).owner == tower);
        REQUIRE(std::fabs(std::sin(bar.rotation)) == Catch::Approx(1.0f).margin(0.1f));
    }
    // And the horde never sealed itself out: the goal stays reachable.
    f.world.flow().rebake_pending(f.world.tissue());
    REQUIRE(f.world.flow().reachable(Vec2{4.0f, 10.0f}));

    // A tower cannot be built on a scar.
    const entt::entity e = f.first_scar();
    const Bar bar = f.bar_of(e);
    const game::PlacementQuery q = towers.validate(f.world, TowerType::Neutrophil, bar.center, 100000u);
    REQUIRE(q.result == game::PlacementResult::Overlapping);
}

TEST_CASE("a Fibroblast with nowhere to build releases nothing", "[game][towers][scar]") {
    // A lane too narrow for its walls: every candidate would sever it. (Six
    // tall would not do -- a tier-1 wall is eight long, and one laid against
    // one edge of a six-tall lane leaves a cell open at the other.)
    Field f(10, /*lane_height=*/4, false);
    game::TowerSystem towers;
    towers.register_systems(f.world);
    REQUIRE(towers.place(f.world, TowerType::Fibroblast, Vec2{24.0f, 10.0f}).valid());
    for (int k = 0; k < 60 * 10; ++k) f.world.tick();
    REQUIRE(f.world.scars().stats().built_total == 0);
    REQUIRE(f.world.swarmers().count() == 0);
    REQUIRE(count_events(f.world.combat_events(), CombatEventType::MuzzleFlash) == 0);
}

TEST_CASE("scars are deterministic and part of what the state hash sees", "[sim][scar][determinism]") {
    auto run = [](bool with_tower) {
        Field f(11);
        game::TowerSystem towers;
        towers.register_systems(f.world);
        if (with_tower) REQUIRE(towers.place(f.world, TowerType::Fibroblast, Vec2{24.0f, 10.0f}).valid());
        for (int k = 0; k < 40; ++k) {
            f.spawn_chaff(Vec2{8.0f + static_cast<f32>(k % 6), 5.0f + static_cast<f32>(k % 10)},
                          (k % 3 == 0) ? PathogenFamily::Bacteria : PathogenFamily::Virus);
        }
        std::vector<u64> hashes;
        for (int k = 0; k < 600; ++k) {
            f.world.tick();
            hashes.push_back(f.world.state_hash());
        }
        return hashes;
    };
    const std::vector<u64> a = run(true);
    const std::vector<u64> b = run(true);
    REQUIRE(a == b);
    // The walls change where the horde goes, so a world with them diverges
    // from one without.
    const std::vector<u64> c = run(false);
    REQUIRE(a.back() != c.back());
}
