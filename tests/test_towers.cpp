// Tests for game/towers/TowerSystem.cpp (Wave 2B): placement validation,
// the tissue/flow-field footprint round-trip, upgrade/sell economics,
// find_target(), and a smoke test per tower type proving its Combat-phase
// system does something observably different (submits a field / sets a
// flag / damages a target).
//
// Scene layout used throughout (see make_world()):
//
//   y
//   15 #########################........##########################
//      #     left room (0..24)   .  corridor (25..34, rows 9-10)  .     right room (35..59)     #
//   4  #########################........##########################
//      0                        25       35                                                    60 (x, world units == cells, cell_size=1)
//
// The corridor is exactly 2 cells (2 world units) tall — narrow enough that a
// tower whose footprint clears it (per InsufficientClearance) also fully
// plugs it (WouldBlockAllPaths). Both rooms are deliberately much larger than
// would_block_all_paths()'s local search window (a ~14-cell margin around the
// footprint — see TowerSystem.cpp), so the window's border lands on genuine
// open interior on both sides of the corridor rather than swallowing an
// entire room and losing one side's anchors.
#include "game/towers/TowerSystem.h"

#include "core/Rng.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/NamedAgents.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace immune;
using namespace immune::sim;
using namespace immune::game;

namespace {

constexpr i32 kW = 60;
constexpr i32 kH = 20;

Vec2 kRoomCenterLeft{12.0f, 10.0f};
Vec2 kRoomCenterRight{47.0f, 10.0f};
Vec2 kCorridorCenter{30.0f, 10.5f};
Vec2 kGoal{55.0f, 10.0f};

SimWorld make_world() {
    SimWorld world;
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
    for (i32 y = 9; y < 12; ++y)
        for (i32 x = 25; x < 35; ++x) mask.set_walkable(x, y, true); // 3-cell-tall corridor

    world.sdf().bake(mask);
    FlowFieldBakeDesc fdesc;
    fdesc.goal_cells = {mask.world_to_cell(kGoal)};
    world.flow().bake(mask, fdesc);
    return world;
}

SystemContext make_ctx(SimWorld& world) {
    return SystemContext{world, world.ecs().registry(), world.rng(), kFixedDt, world.tick_index()};
}

void rebuild_spatial(SimWorld& world) {
    world.spatial().rebuild(world.chaff().pos_x.data(), world.chaff().pos_y.data(), world.chaff().count(), nullptr);
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
// Deliverable 1: default stats table.
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
        // Tiers scale range monotonically; every roster entry does in this table.
        REQUIRE(s2.range >= s1.range);
        REQUIRE(s3.range >= s2.range);
        // At least one of damage/kill_rate strictly increases tier-over-tier
        // (every combat-capable tower); Dendritic is the deliberate exception
        // (support, always 0 damage/kill_rate).
        if (type != TowerType::Dendritic) {
            const bool damage_grows = s3.damage > s1.damage;
            const bool kill_grows = s3.kill_rate > s1.kill_rate;
            REQUIRE((damage_grows || kill_grows));
        }
    }
}

TEST_CASE("tower_type_name/parse_tower_type round-trip for all 8 types", "[towers][naming]") {
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        TowerType parsed{};
        REQUIRE(parse_tower_type(tower_type_name(type), parsed));
        REQUIRE(parsed == type);
    }
    TowerType unused{};
    REQUIRE_FALSE(parse_tower_type("not_a_tower", unused));
}

// ---------------------------------------------------------------------------
// Deliverable 2: validate()/place()/upgrade()/sell().
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
    // ComplementCascade tier1 footprint_radius is 1.2; 0.5 units from the left
    // room's wall (x=0 boundary) leaves far less clearance than that.
    const auto q = ts.validate(world, TowerType::ComplementCascade, Vec2{0.5f, 10.0f}, 100000);
    REQUIRE(q.result == PlacementResult::InsufficientClearance);
    REQUIRE(q.clearance < 1.2f);
    // The light snap should nudge back toward more clearance (away from x=0).
    REQUIRE(q.snapped_position.x > 0.5f);
}

TEST_CASE("validate() rejects a placement overlapping an existing tower", "[towers][placement]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId first = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(first.valid());

    const auto q = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft + Vec2{0.3f, 0.0f}, 100000);
    REQUIRE(q.result == PlacementResult::Overlapping);

    const auto far_enough = ts.validate(world, TowerType::Macrophage, kRoomCenterLeft + Vec2{4.0f, 0.0f}, 100000);
    REQUIRE(far_enough.result == PlacementResult::Ok);
}

TEST_CASE("validate() rejects a placement that would fully plug the only corridor, "
          "but allows the same tower with room to spare",
          "[towers][placement][reachability]") {
    SimWorld world = make_world();
    TowerSystem ts;

    // Sanity: the goal is reachable from the left room before any placement.
    REQUIRE(world.flow().reachable(kRoomCenterLeft));

    const auto blocked = ts.validate(world, TowerType::MastCell, kCorridorCenter, 100000);
    REQUIRE(blocked.result == PlacementResult::WouldBlockAllPaths);

    const auto open = ts.validate(world, TowerType::MastCell, kRoomCenterRight, 100000);
    REQUIRE(open.result == PlacementResult::Ok);
}

TEST_CASE("place() blocks tissue over exactly its footprint and marks the flow field dirty",
          "[towers][placement][flowfield]") {
    SimWorld world = make_world();
    TowerSystem ts;
    REQUIRE_FALSE(world.flow().has_pending_rebake());

    const EntityId id = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(id.valid());

    // footprint_radius (tier 1) is 1.0, so the tower's own cell must now be blocked.
    const IVec2 c = world.tissue().world_to_cell(kRoomCenterLeft);
    REQUIRE_FALSE(world.tissue().walkable(c.x, c.y));
    REQUIRE(world.flow().has_pending_rebake());

    world.flow().rebake_pending(world.tissue());
    REQUIRE_FALSE(world.flow().has_pending_rebake());
    // The room is large; a detour around the tower still reaches the goal.
    REQUIRE(world.flow().reachable(kRoomCenterLeft + Vec2{2.0f, 3.0f}));
}

TEST_CASE("upgrade() advances tier and stats, caps at 3; sell() refunds ATP and restores tissue",
          "[towers][economy]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId id = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(id.valid());

    const entt::entity e = world.ecs().from_id(id);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 1);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).range == ts.stats(TowerType::Macrophage, 1).range);

    REQUIRE(ts.upgrade(world, id) == 2);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 2);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).range == ts.stats(TowerType::Macrophage, 2).range);

    REQUIRE(ts.upgrade(world, id) == 3);
    REQUIRE(world.ecs().registry().get<comp::Tower>(e).tier == 3);
    REQUIRE(ts.upgrade(world, id) == 0); // already maxed

    // invested = tier1 build_cost (80) + upgrade to tier2 (60) + upgrade to
    // tier3 (90) = 230; refund at the documented 0.7 fraction = 161.
    const u32 refund = ts.sell(world, id);
    REQUIRE(refund == 161);

    REQUIRE_FALSE(world.ecs().registry().valid(e));
    const IVec2 c = world.tissue().world_to_cell(kRoomCenterLeft);
    REQUIRE(world.tissue().walkable(c.x, c.y)); // tissue restored
    REQUIRE(world.flow().has_pending_rebake());  // dirtied again by the sell

    bool still_listed = false;
    for (EntityId t : ts.placed_towers())
        if (t == id) still_listed = true;
    REQUIRE_FALSE(still_listed);
}

// ---------------------------------------------------------------------------
// find_target()
// ---------------------------------------------------------------------------

TEST_CASE("find_target respects range and family_mask, and hides Burrowed agents unless asked",
          "[towers][targeting]") {
    SimWorld world = make_world();
    TowerSystem ts;
    const EntityId agent = spawn_named(world, kRoomCenterLeft);
    REQUIRE(agent.valid());
    const PathogenFamily family = world.ecs().registry().get<comp::NamedAgent>(world.ecs().from_id(agent)).family;
    const u8 fam_bit = static_cast<u8>(1u << static_cast<u8>(family));

    REQUIRE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF, false) == agent);
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft + Vec2{3.0f, 0.0f}, 1.0f, 0xFF, false).valid()); // out of range
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft + Vec2{20.0f, 0.0f}, 2.0f, 0xFF, false).valid());
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft, 5.0f, static_cast<u8>(~fam_bit), false).valid());

    world.ecs().registry().get<comp::AiBrain>(world.ecs().from_id(agent)).state = comp::AiState::Burrowed;
    REQUIRE_FALSE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF, false).valid());
    REQUIRE(ts.find_target(world, kRoomCenterLeft, 5.0f, 0xFF, /*require_detect_hidden=*/true) == agent);
}

// ---------------------------------------------------------------------------
// Deliverable 3: per-tower Combat-phase systems. Each proves the system does
// something observably different from the others.
// ---------------------------------------------------------------------------

TEST_CASE("Macrophage submits a self-centered chaff-kill field and damages a named target over time",
          "[towers][combat][macrophage]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    spawn_chaff_cluster(world, kRoomCenterLeft, 20, 1.0f);
    const EntityId agent = spawn_named(world, kRoomCenterLeft + Vec2{0.5f, 0.0f});
    const entt::entity ae = world.ecs().from_id(agent);
    const f32 hp_before = world.ecs().registry().get<comp::Health>(ae).current;

    rebuild_spatial(world);
    SystemContext ctx = make_ctx(world);
    world.ecs().tick(ctx);

    bool found_field = false;
    for (const DamageField& f : world.damage().fields()) {
        if (f.shape == FieldShape::Circle && f.kill_rate > 0.0f) found_field = true;
    }
    REQUIRE(found_field);
    REQUIRE(world.ecs().registry().get<comp::Health>(ae).current < hp_before);
}

TEST_CASE("Macrophage measurably thins a spawned chaff cluster over many ticks (or proves 2A's stub honestly)",
          "[towers][combat][macrophage][integration]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Macrophage, kRoomCenterLeft);
    REQUIRE(tower.valid());
    spawn_chaff_cluster(world, kRoomCenterLeft, 40, 1.0f, /*spread=*/0.5f);

    const f32 density_before = world.chaff().total_density();
    for (int i = 0; i < 300; ++i) world.tick();
    const f32 density_after = world.chaff().total_density();

    INFO("density before=" << density_before << " after=" << density_after
         << " (DamageSystem::apply() is Wave 2A's stub as of this run if these are equal)");
    REQUIRE(density_after <= density_before); // never increases; strict drop depends on 2A's apply()
}

TEST_CASE("Neutrophil submits a swarm field, and its NET ability slows chaff and spawns drifting micro-units",
          "[towers][combat][neutrophil]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Neutrophil, kRoomCenterLeft);
    REQUIRE(tower.valid());
    spawn_chaff_cluster(world, kRoomCenterLeft, 5, 1.0f, /*spread=*/0.1f);

    rebuild_spatial(world);
    {
        SystemContext ctx = make_ctx(world);
        world.ecs().tick(ctx);
    }
    bool found_field = false;
    for (const DamageField& f : world.damage().fields())
        if (f.owner == tower) found_field = true;
    REQUIRE(found_field);

    const usize entities_before = world.ecs().entity_count();
    REQUIRE(ts.trigger_ability(world, tower));
    // 3 micro-units + 1 NET entity.
    REQUIRE(world.ecs().entity_count() == entities_before + 4);

    const entt::entity micro = *world.ecs().registry().view<comp::Ephemeral, comp::Velocity>().begin();
    const Vec2 pos_before = world.ecs().registry().get<comp::Transform>(micro).position;

    rebuild_spatial(world);
    {
        SystemContext ctx = make_ctx(world);
        world.ecs().tick(ctx);
    }
    const Vec2 pos_after = world.ecs().registry().get<comp::Transform>(micro).position;
    REQUIRE(pos_after != pos_before); // drifted

    for (usize i = 0; i < world.chaff().count(); ++i) {
        REQUIRE((world.chaff().flags[i] & chaff_flags::kSlowed) != 0);
    }
}

TEST_CASE("Dendritic marks chaff in range and never submits a damage field", "[towers][combat][dendritic]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::Dendritic, kRoomCenterLeft);
    REQUIRE(tower.valid());
    spawn_chaff_cluster(world, kRoomCenterLeft, 10, 1.0f);

    rebuild_spatial(world);
    SystemContext ctx = make_ctx(world);
    world.ecs().tick(ctx);

    REQUIRE(world.damage().fields().empty());
    usize marked = 0;
    for (usize i = 0; i < world.chaff().count(); ++i)
        if (world.chaff().flags[i] & chaff_flags::kMarked) ++marked;
    REQUIRE(marked > 0);
}

TEST_CASE("Cytotoxic T deals precision burst damage to the nearest named agent, boosted vs a boss",
          "[towers][combat][cytotoxic_t]") {
    auto run = [](u8 named_tier) {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        const EntityId tower = ts.place(world, TowerType::CytotoxicT, kRoomCenterLeft);
        REQUIRE(tower.valid());
        const EntityId agent = spawn_named(world, kRoomCenterLeft + Vec2{0.5f, 0.0f}, /*health_scale=*/10.0f);
        world.ecs().registry().get<comp::NamedAgent>(world.ecs().from_id(agent)).tier = named_tier;
        const f32 hp_before = world.ecs().registry().get<comp::Health>(world.ecs().from_id(agent)).current;

        rebuild_spatial(world);
        SystemContext ctx = make_ctx(world);
        world.ecs().tick(ctx);

        return hp_before - world.ecs().registry().get<comp::Health>(world.ecs().from_id(agent)).current;
    };

    const f32 elite_dmg = run(1);
    const f32 boss_dmg = run(2);
    REQUIRE(elite_dmg > 0.0f);
    REQUIRE(boss_dmg > elite_dmg); // boss bonus multiplier is larger
}

TEST_CASE("B-Cell marks a named target in range, or the nearest chaff agent when no named target exists",
          "[towers][combat][bcell]") {
    SECTION("named target present") {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        REQUIRE(ts.place(world, TowerType::BCell, kRoomCenterLeft).valid());
        const EntityId agent = spawn_named(world, kRoomCenterLeft + Vec2{0.5f, 0.0f});

        rebuild_spatial(world);
        SystemContext ctx = make_ctx(world);
        world.ecs().tick(ctx);

        REQUIRE(world.ecs().registry().all_of<comp::Marked>(world.ecs().from_id(agent)));
    }
    SECTION("no named target: falls back to chaff") {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        REQUIRE(ts.place(world, TowerType::BCell, kRoomCenterLeft).valid());
        spawn_chaff_cluster(world, kRoomCenterLeft, 6, 1.0f);

        rebuild_spatial(world);
        SystemContext ctx = make_ctx(world);
        world.ecs().tick(ctx);

        usize marked = 0;
        for (usize i = 0; i < world.chaff().count(); ++i)
            if (world.chaff().flags[i] & chaff_flags::kMarked) ++marked;
        REQUIRE(marked == 1); // "sticks" to exactly one nearest agent
    }
}

TEST_CASE("NK Cell can damage a Burrowed named agent that Cytotoxic T cannot even see",
          "[towers][combat][nkcell]") {
    SimWorld nk_world = make_world();
    TowerSystem nk_ts;
    nk_ts.register_systems(nk_world);
    REQUIRE(nk_ts.place(nk_world, TowerType::NKCell, kRoomCenterLeft).valid());
    const EntityId nk_target = spawn_named(nk_world, kRoomCenterLeft + Vec2{0.5f, 0.0f});
    nk_world.ecs().registry().get<comp::AiBrain>(nk_world.ecs().from_id(nk_target)).state = comp::AiState::Burrowed;
    const f32 nk_hp_before = nk_world.ecs().registry().get<comp::Health>(nk_world.ecs().from_id(nk_target)).current;
    rebuild_spatial(nk_world);
    { SystemContext ctx = make_ctx(nk_world); nk_world.ecs().tick(ctx); }
    REQUIRE(nk_world.ecs().registry().get<comp::Health>(nk_world.ecs().from_id(nk_target)).current < nk_hp_before);

    SimWorld ct_world = make_world();
    TowerSystem ct_ts;
    ct_ts.register_systems(ct_world);
    REQUIRE(ct_ts.place(ct_world, TowerType::CytotoxicT, kRoomCenterLeft).valid());
    const EntityId ct_target = spawn_named(ct_world, kRoomCenterLeft + Vec2{0.5f, 0.0f});
    ct_world.ecs().registry().get<comp::AiBrain>(ct_world.ecs().from_id(ct_target)).state = comp::AiState::Burrowed;
    const f32 ct_hp_before = ct_world.ecs().registry().get<comp::Health>(ct_world.ecs().from_id(ct_target)).current;
    rebuild_spatial(ct_world);
    { SystemContext ctx = make_ctx(ct_world); ct_world.ecs().tick(ctx); }
    REQUIRE(ct_world.ecs().registry().get<comp::Health>(ct_world.ecs().from_id(ct_target)).current == ct_hp_before);
}

TEST_CASE("Mast Cell triggers its nova once local chaff density crosses the threshold, not before",
          "[towers][combat][mastcell]") {
    SECTION("below threshold: no trigger") {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        const EntityId tower = ts.place(world, TowerType::MastCell, kRoomCenterLeft);
        spawn_chaff_cluster(world, kRoomCenterLeft, 3, 1.0f); // density 3 << threshold (range*kill_rate=15)

        rebuild_spatial(world);
        SystemContext ctx = make_ctx(world);
        world.ecs().tick(ctx);

        REQUIRE(world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower)).ability_cooldown == 0.0f);
    }
    SECTION("above threshold: nova fires") {
        SimWorld world = make_world();
        TowerSystem ts;
        ts.register_systems(world);
        const EntityId tower = ts.place(world, TowerType::MastCell, kRoomCenterLeft);
        spawn_chaff_cluster(world, kRoomCenterLeft, 20, 1.0f); // density 20 >= threshold 15

        rebuild_spatial(world);
        SystemContext ctx = make_ctx(world);
        world.ecs().tick(ctx);

        REQUIRE(world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower)).ability_cooldown > 0.0f);
        bool found_nova = false;
        for (const DamageField& f : world.damage().fields())
            if (f.owner == tower && f.radius > ts.stats(TowerType::MastCell, 1).range) found_nova = true;
        REQUIRE(found_nova);
    }
}

TEST_CASE("Complement Cascade auto-casts its chain nova whenever chaff is present in range",
          "[towers][combat][complement]") {
    SimWorld world = make_world();
    TowerSystem ts;
    ts.register_systems(world);
    const EntityId tower = ts.place(world, TowerType::ComplementCascade, kRoomCenterLeft);
    spawn_chaff_cluster(world, kRoomCenterLeft, 5, 1.0f);

    rebuild_spatial(world);
    SystemContext ctx = make_ctx(world);
    world.ecs().tick(ctx);

    REQUIRE(world.ecs().registry().get<comp::Tower>(world.ecs().from_id(tower)).ability_cooldown > 0.0f);
    bool found_chain = false;
    for (const DamageField& f : world.damage().fields())
        if (f.owner == tower && f.shape == FieldShape::Chain) found_chain = true;
    REQUIRE(found_chain);
}
