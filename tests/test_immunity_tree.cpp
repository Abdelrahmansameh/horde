// Coverage for the Strengthen Immunity tree (game/meta/ImmunityTree.h) and the
// sim hooks its purchases drive (sim/Immunity.h, SwarmerProfile's capstones,
// the unlock masks on TowerSystem / ActiveAbilitySystem).
//
// Properties, not balance numbers: the catalog is well-formed and is one
// tree; applying the tree only ever changes what was bought; an unbought
// tree leaves a run's numbers alone; locked towers and abilities cannot be
// used by any path; and each capstone does its one new thing and nothing
// when it is off.
#include "game/meta/ImmunityTree.h"

#include "game/abilities/AbilityConfigApply.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/config/GameConfig.h"
#include "game/towers/TowerSystem.h"

#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/Components.h"
#include "sim/projectile/Projectiles.h"
#include "sim/scar/Scars.h"
#include "sim/swarm/Swarmers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>

using namespace immune;
using namespace immune::game;

namespace {

constexpr i32 kW = 60;
constexpr i32 kH = 20;
const Vec2 kGoal{58.0f, 10.0f};

/// An open 60x20 field flowing left to right. Hostile pass off: nothing here
/// is about the horde fighting back.
struct Field {
    sim::SimWorld world;

    explicit Field(u64 seed = 1) {
        sim::SimDesc desc;
        desc.seed = seed;
        desc.max_chaff = 4096;
        desc.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{static_cast<f32>(kW), static_cast<f32>(kH)}};
        desc.spatial_cell_size = 4.0f;
        desc.chaff_tuning.family[0].radius = 0.5f;
        desc.chaff_tuning.family[0].replication_rate = 0.0f;
        world.init(desc, nullptr);

        sim::TissueMask& mask = world.tissue();
        mask.resize(kW, kH, 1.0f, Vec2{0.0f, 0.0f});
        for (i32 y = 0; y < kH; ++y)
            for (i32 x = 0; x < kW; ++x) mask.set_walkable(x, y, true);
        world.sdf().bake(mask);
        sim::FlowFieldBakeDesc fdesc;
        fdesc.goals = {sim::FlowGoal{mask.world_to_cell(kGoal)}};
        world.flow().bake(mask, fdesc);
    }

    usize spawn(Vec2 p, f32 density = 1.0f) {
        sim::ChaffSpawnParams c;
        c.position = p;
        c.density = density;
        world.chaff().spawn(c);
        return world.chaff().count() - 1;
    }

    u32 slowed() const {
        u32 n = 0;
        const sim::ChaffBuffers& c = world.chaff();
        for (usize i = 0; i < c.count(); ++i) n += (c.flags[i] & sim::chaff_flags::kSlowed) ? 1u : 0u;
        return n;
    }
};

TreeLevels bought(std::initializer_list<std::pair<TreeNode, u8>> nodes) {
    TreeLevels lv{};
    for (const auto& [n, l] : nodes) lv[n] = l;
    return lv;
}

} // namespace

// ---------------------------------------------------------------------------
// The catalog.
// ---------------------------------------------------------------------------

TEST_CASE("every tree node has a unique key that finds it again", "[meta][tree]") {
    std::set<std::string> keys;
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const TreeNode n = static_cast<TreeNode>(i);
        const TreeNodeDef& d = tree_node(n);
        REQUIRE(std::string(d.key).size() > 0);
        REQUIRE(std::string(d.name).size() > 0);
        REQUIRE(d.max_level >= 1);
        REQUIRE(keys.insert(d.key).second);
        TreeNode back{};
        REQUIRE(find_tree_node(d.key, back));
        REQUIRE(back == n);
    }
    TreeNode none{};
    REQUIRE_FALSE(find_tree_node("not.a.node", none));
    REQUIRE_FALSE(find_tree_node("hub.rapid_metabolism", none));
    REQUIRE_FALSE(find_tree_node("neutrophil.unlock", none));
}

TEST_CASE("cell branches have their own lines and capstone; only new cells need unlock nodes", "[meta][tree]") {
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const TowerType type = static_cast<TowerType>(t);
        const TreeBranch b = tower_branch(type);
        REQUIRE(branch_tower(b) == type);
        u32 roots = 0, caps = 0, stats = 0;
        for (u32 i = 0; i < kTreeNodeCount; ++i) {
            const TreeNodeDef& d = tree_node(static_cast<TreeNode>(i));
            if (d.branch != b) continue;
            roots += d.kind == TreeNodeKind::TowerRoot;
            caps += d.kind == TreeNodeKind::Capstone;
            stats += d.kind == TreeNodeKind::Stat;
        }
        REQUIRE(roots == (type == TowerType::Neutrophil ? 0u : 1u));
        REQUIRE(caps == 1);
        REQUIRE(stats >= 5);
        if (type == TowerType::Neutrophil) {
            REQUIRE(tower_root(type) == TreeNode::Count);
        } else {
            REQUIRE(tree_node(tower_root(type)).kind == TreeNodeKind::TowerRoot);
            REQUIRE(tree_node(tower_root(type)).branch == b);
            REQUIRE(tree_node(tower_root(type)).max_level == 1);
        }
        REQUIRE(tree_node(branch_capstone(b)).kind == TreeNodeKind::Capstone);
        REQUIRE(tree_node(branch_capstone(b)).max_level == 1);
    }
    for (u32 a = 0; a < kAbilityCount; ++a) {
        const TreeNodeDef& d = tree_node(ability_root(static_cast<AbilityId>(a)));
        REQUIRE(d.kind == TreeNodeKind::AbilityRoot);
        REQUIRE(d.ability == static_cast<AbilityId>(a));
    }
}

TEST_CASE("the catalog is one tree with small branches around the replenishment hub", "[meta][tree]") {
    u32 children[kTreeNodeCount] = {};
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const TreeNode n = static_cast<TreeNode>(i);
        const TreeNode parent = tree_node(n).parent;
        CAPTURE(tree_node(n).key);
        if (n == kTreeRoot) {
            REQUIRE(parent == TreeNode::Count);
            continue;
        }
        REQUIRE(parent != TreeNode::Count);
        ++children[static_cast<u32>(parent)];
        // Every parent chain reaches the root (no cycles, no islands).
        TreeNode a = n;
        u32 steps = 0;
        while (a != kTreeRoot && steps++ <= kTreeNodeCount) a = tree_node(a).parent;
        REQUIRE(a == kTreeRoot);
    }
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        CAPTURE(tree_node(static_cast<TreeNode>(i)).key);
        const u32 limit = static_cast<TreeNode>(i) == kTreeHub ? kTreeHubMaxChildren : kTreeMaxChildren;
        REQUIRE(children[i] <= limit);
    }
    // The legacy stable identifier is both the first purchase and the hub.
    REQUIRE(kTreeRoot == TreeNode::NeutrophilRoundDamage);
    REQUIRE(kTreeRoot == kTreeHub);
    REQUIRE(children[static_cast<u32>(kTreeHub)] == kTreeHubMaxChildren);
}

TEST_CASE("all economy upgrades share one branch without cell or ability unlocks", "[meta][tree]") {
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const TreeNode n = static_cast<TreeNode>(i);
        if (n == kTreeRoot || n == kTreeHub) continue;
        CAPTURE(tree_node(n).key);
        TreeNode spoke = n;
        while (tree_node(spoke).parent != kTreeHub) spoke = tree_node(spoke).parent;
        REQUIRE((spoke == TreeNode::BoneMarrowReserve) == (tree_node(n).kind == TreeNodeKind::Economy));
    }
}

TEST_CASE("cell unlocks remain independent and capstones end their branch", "[meta][tree]") {
    // Cells remain independent choices; related discoveries grow directly
    // from that cell's unlock without requiring Memory Cell stat purchases.
    auto antibody_priced = [](TreeNode n) {
        const TreeNodeKind k = tree_node(n).kind;
        return k == TreeNodeKind::TowerRoot || k == TreeNodeKind::AbilityRoot || k == TreeNodeKind::Capstone;
    };
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const TreeNode n = static_cast<TreeNode>(i);
        const TreeNodeKind kind = tree_node(n).kind;
        CAPTURE(tree_node(n).key);
        if (kind == TreeNodeKind::TowerRoot) {
            for (TreeNode a = tree_node(n).parent; a != kTreeRoot; a = tree_node(a).parent) {
                CAPTURE(tree_node(a).key);
                REQUIRE_FALSE(antibody_priced(a));
            }
        }
        if (kind == TreeNodeKind::Capstone) {
            for (u32 j = 0; j < kTreeNodeCount; ++j) REQUIRE(tree_node(static_cast<TreeNode>(j)).parent != n);
        }
    }
    // A branch's lines grow from its unlock; the innate Neutrophil's lines
    // grow from Round Damage itself.
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const TreeNode n = static_cast<TreeNode>(i);
        if (n == kTreeRoot) continue;
        const TreeNodeDef& d = tree_node(static_cast<TreeNode>(i));
        if (d.kind != TreeNodeKind::Stat && d.kind != TreeNodeKind::Capstone && d.kind != TreeNodeKind::AbilityStat) {
            continue;
        }
        CAPTURE(d.key);
        const TreeNode root = d.kind == TreeNodeKind::AbilityStat ? ability_root(d.ability)
                            : d.branch == TreeBranch::Neutrophil ? kTreeRoot
                                                               : tower_root(branch_tower(d.branch));
        TreeNode a = d.parent;
        while (a != root && a != kTreeRoot) a = tree_node(a).parent;
        REQUIRE(a == root);
    }
}

TEST_CASE("active abilities grow directly from their four related cell unlocks", "[meta][tree]") {
    struct Placement { TreeNode ability, parent, branch; };
    const Placement placements[] = {
        {TreeNode::ComplementUnlock, TreeNode::CytotoxicRoot, TreeNode::CytotoxicRoot},
        {TreeNode::HistamineUnlock, TreeNode::GobletRoot, TreeNode::GobletRoot},
        {TreeNode::FeverUnlock, TreeNode::MacrophageRoot, TreeNode::MacrophageRoot},
        {TreeNode::ClotUnlock, TreeNode::FibroblastRoot, TreeNode::FibroblastRoot},
    };
    std::set<TreeNode> branches;
    for (const Placement& p : placements) {
        CAPTURE(tree_node(p.ability).key);
        REQUIRE(tree_node(p.ability).parent == p.parent);
        REQUIRE(tree_node(p.parent).kind == TreeNodeKind::TowerRoot);
        TreeNode spoke = p.ability;
        while (tree_node(spoke).parent != kTreeRoot) spoke = tree_node(spoke).parent;
        REQUIRE(spoke == p.branch);
        REQUIRE(branches.insert(spoke).second);
    }
}

TEST_CASE("milestones cost Antibodies, lines cost Memory Cells that rise per level", "[meta][tree]") {
    const MetaConfig cfg;
    REQUIRE(tree_node_cost(cfg, TreeNode::MacrophageRoot, 0).antibodies == 1);
    REQUIRE(tree_node_cost(cfg, TreeNode::MacrophageRoot, 0).memory_cells == 0);
    REQUIRE(tree_node_cost(cfg, TreeNode::FeverUnlock, 0).antibodies == 1);
    const TreeCost cap = tree_node_cost(cfg, TreeNode::GobletCapstone, 0);
    REQUIRE(cap.antibodies == 1);
    REQUIRE(cap.memory_cells == 0);
    const TreeCost l0 = tree_node_cost(cfg, TreeNode::BoneMarrowReserve, 0);
    const TreeCost l1 = tree_node_cost(cfg, TreeNode::BoneMarrowReserve, 1);
    REQUIRE(l0.antibodies == 0);
    REQUIRE(l0.memory_cells == cfg.stat_base_cost);
    REQUIRE(l1.memory_cells == cfg.stat_base_cost + cfg.stat_cost_step);
}

// ---------------------------------------------------------------------------
// Folding the tree into a run's config.
// ---------------------------------------------------------------------------

TEST_CASE("an unbought tree leaves the baseline alone", "[meta][tree]") {
    const GameConfig base = default_game_config();
    GameConfig cfg = base;
    const TreeEffects fx = apply_immunity_tree(bought({}), cfg);

    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const TowerStats& s0 = cfg.towers.stats[t];
        REQUIRE(s0.build_cost == base.towers.stats[t].build_cost);
        REQUIRE(s0.max_health == Catch::Approx(base.towers.stats[t].max_health));
        REQUIRE(s0.fire_interval == Catch::Approx(base.towers.stats[t].fire_interval));
        REQUIRE(cfg.towers.mechanics[t].shooter.round_damage ==
                Catch::Approx(base.towers.mechanics[t].shooter.round_damage));
        REQUIRE(cfg.towers.mechanics[t].capstone.heal_per_kill == 0.0f);
    }
    REQUIRE(cfg.economy.starting_atp == base.economy.starting_atp);
    REQUIRE(cfg.abilities.ability[0].cooldown_seconds ==
            Catch::Approx(base.abilities.ability[0].cooldown_seconds));
    REQUIRE(fx.tower_mask == (1u << static_cast<u32>(TowerType::Neutrophil)));
    REQUIRE(fx.ability_mask == 0u);
    REQUIRE_FALSE(fx.immunity.any_active());
    REQUIRE(fx.immunity.named_damage_mult == 1.0f);
    REQUIRE(fx.immunity.leak_damage == Catch::Approx(base.sim.globals.objective_damage_per_leak));
    REQUIRE(fx.hostile_damage_taken_mult == 1.0f);
}

TEST_CASE("stat lines change their tower and nothing else", "[meta][tree]") {
    const GameConfig base = default_game_config();
    GameConfig cfg = base;
    apply_immunity_tree(bought({{TreeNode::NeutrophilRoundDamage, 2},
                                {TreeNode::NeutrophilSquadSize, 1}}),
                        cfg);
    const u32 n = static_cast<u32>(TowerType::Neutrophil);
    const u32 c = static_cast<u32>(TowerType::CytotoxicT);
    REQUIRE(cfg.towers.mechanics[n].shooter.round_damage ==
            Catch::Approx(base.towers.mechanics[n].shooter.round_damage));
    REQUIRE(cfg.towers.mechanics[n].shooter.reload_seconds ==
            Catch::Approx(base.towers.mechanics[n].shooter.reload_seconds * 0.72f));
    REQUIRE(cfg.towers.mechanics[n].shooter.fire_interval ==
            Catch::Approx(base.towers.mechanics[n].shooter.fire_interval * 0.88f));
    REQUIRE(cfg.towers.mechanics[n].shooter.gather_seconds ==
            Catch::Approx(base.towers.mechanics[n].shooter.gather_seconds * 0.88f));
    REQUIRE(cfg.towers.mechanics[n].shooter.magazine_size ==
            base.towers.mechanics[n].shooter.magazine_size + 2);
    REQUIRE(cfg.towers.mechanics[c].latch.dps == Catch::Approx(base.towers.mechanics[c].latch.dps));
}

TEST_CASE("hub lines reach the economy, every tower and the sim", "[meta][tree]") {
    const GameConfig base = default_game_config();
    GameConfig cfg = base;
    const TreeEffects fx = apply_immunity_tree(
        bought({{TreeNode::BoneMarrowReserve, 2}, {TreeNode::EfficientClearance, 2},
                {TreeNode::FieldRequisition, 2},
                {TreeNode::SystemicPotency, 1}, {TreeNode::EliteResponse, 3},
                {TreeNode::Homeostasis, 2}, {TreeNode::MembraneResilience, 1}}),
        cfg);
    REQUIRE(cfg.economy.starting_atp == base.economy.starting_atp + 80);
    REQUIRE(cfg.economy.atp_per_density == Catch::Approx(base.economy.atp_per_density * 1.20f));
    REQUIRE(cfg.towers.globals.refund_fraction == Catch::Approx(cfg.economy.refund_fraction));
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        REQUIRE(cfg.towers.stats[t].build_cost < base.towers.stats[t].build_cost);
        REQUIRE(cfg.towers.mechanics[t].shooter.round_damage ==
                Catch::Approx(base.towers.mechanics[t].shooter.round_damage));
        REQUIRE(cfg.towers.mechanics[t].swarm.max_health ==
                Catch::Approx(base.towers.mechanics[t].swarm.max_health * 1.45f));
    }
    REQUIRE(fx.immunity.named_damage_mult == 1.0f);
    REQUIRE(fx.immunity.leak_damage == Catch::Approx(base.sim.globals.objective_damage_per_leak * 0.8f));
    REQUIRE(fx.hostile_damage_taken_mult == Catch::Approx(0.92f));
}

TEST_CASE("coordinated response reaches each role's actual work cycle and preserves payloads", "[meta][tree]") {
    const GameConfig base = default_game_config();
    GameConfig cfg = base;
    const TreeEffects fx = apply_immunity_tree(bought({{TreeNode::SystemicPotency, 3}}), cfg);
    constexpr f32 cycle = 0.82f;
    const auto& neutro = cfg.towers.mechanics[static_cast<u32>(TowerType::Neutrophil)];
    const auto& n0 = base.towers.mechanics[static_cast<u32>(TowerType::Neutrophil)];
    REQUIRE(neutro.shooter.reload_seconds == Catch::Approx(n0.shooter.reload_seconds * cycle));
    REQUIRE(neutro.shooter.fire_interval == Catch::Approx(n0.shooter.fire_interval * cycle));
    REQUIRE(neutro.shooter.gather_seconds == Catch::Approx(n0.shooter.gather_seconds * cycle));
    const auto& cyto = cfg.towers.mechanics[static_cast<u32>(TowerType::CytotoxicT)];
    const auto& c0 = base.towers.mechanics[static_cast<u32>(TowerType::CytotoxicT)];
    REQUIRE(cyto.latch.dps == Catch::Approx(c0.latch.dps / cycle));
    REQUIRE(cyto.latch.attach_seconds == Catch::Approx(c0.latch.attach_seconds * cycle));
    const auto& macro = cfg.towers.mechanics[static_cast<u32>(TowerType::Macrophage)];
    const auto& m0 = base.towers.mechanics[static_cast<u32>(TowerType::Macrophage)];
    REQUIRE(macro.arbor_grabber.extend_seconds == Catch::Approx(m0.arbor_grabber.extend_seconds * cycle));
    REQUIRE(macro.arbor_grabber.pull_seconds == Catch::Approx(m0.arbor_grabber.pull_seconds * cycle));
    const auto& goblet = cfg.towers.mechanics[static_cast<u32>(TowerType::GobletCell)];
    const auto& g0 = base.towers.mechanics[static_cast<u32>(TowerType::GobletCell)];
    REQUIRE(goblet.swarm.speed == Catch::Approx(g0.swarm.speed / cycle));
    REQUIRE(goblet.mucus_bomber.chase_seconds == g0.mucus_bomber.chase_seconds);
    REQUIRE(goblet.mucus_bomber.droplets == g0.mucus_bomber.droplets);
    const auto& fibro = cfg.towers.mechanics[static_cast<u32>(TowerType::Fibroblast)];
    const auto& f0 = base.towers.mechanics[static_cast<u32>(TowerType::Fibroblast)];
    REQUIRE(fibro.builder.build_cooldown == Catch::Approx(f0.builder.build_cooldown * cycle));
    REQUIRE(fibro.builder.secrete_seconds == Catch::Approx(f0.builder.secrete_seconds * cycle));
    REQUIRE(fibro.builder.scar_health == f0.builder.scar_health);
    REQUIRE(fx.immunity.scar_contact_rate == 0);
}

TEST_CASE("survival lines add base health before percentage reserves and preserve mobility", "[meta][tree]") {
    const GameConfig base = default_game_config();
    GameConfig cfg = base;
    apply_immunity_tree(bought({{TreeNode::NeutrophilVitality, 2},
                                {TreeNode::CytotoxicHealth, 2}, {TreeNode::CytotoxicStamina, 1},
                                {TreeNode::GobletHealth, 3}, {TreeNode::EliteResponse, 1}}), cfg);
    const u32 n = static_cast<u32>(TowerType::Neutrophil);
    const u32 c = static_cast<u32>(TowerType::CytotoxicT);
    const u32 g = static_cast<u32>(TowerType::GobletCell);
    REQUIRE(cfg.towers.mechanics[n].swarm.max_health ==
            Catch::Approx((base.towers.mechanics[n].swarm.max_health + 18.0f) * 1.15f));
    REQUIRE(cfg.towers.mechanics[n].swarm.speed ==
            Catch::Approx(base.towers.mechanics[n].swarm.speed * 1.2f));
    REQUIRE(cfg.towers.mechanics[c].swarm.max_health ==
            Catch::Approx((base.towers.mechanics[c].swarm.max_health + 20.0f + 9.0f) * 1.15f));
    REQUIRE(cfg.towers.mechanics[c].swarm.speed ==
            Catch::Approx(base.towers.mechanics[c].swarm.speed * 1.1f));
    REQUIRE(cfg.towers.mechanics[g].swarm.max_health ==
            Catch::Approx((base.towers.mechanics[g].swarm.max_health + 30.0f) * 1.15f));
}

TEST_CASE("the Fibroblast lines reach its builder's rest, territory and wall count", "[meta][tree]") {
    const GameConfig base = default_game_config();
    GameConfig cfg = base;
    apply_immunity_tree(bought({{TreeNode::FibroblastRoot, 1}, {TreeNode::FibroblastBuildRadius, 2},
                                {TreeNode::FibroblastHealth, 1}, {TreeNode::FibroblastBuildRate, 3},
                                {TreeNode::FibroblastScarSize, 1}, {TreeNode::FibroblastScarNetwork, 2}}),
                        cfg);
    const u32 t = static_cast<u32>(TowerType::Fibroblast);
    const BuilderParams& b0 = base.towers.mechanics[t].builder;
    const BuilderParams& b = cfg.towers.mechanics[t].builder;
    REQUIRE(b.build_radius == Catch::Approx(b0.build_radius * 1.24f));
    REQUIRE(b.leash_radius == Catch::Approx(b0.leash_radius * 1.24f));
    REQUIRE(b.build_cooldown == Catch::Approx(b0.build_cooldown * 0.64f));
    REQUIRE(b.max_scars == b0.max_scars + 2);
    REQUIRE(b.roam_speed_mult == Catch::Approx(b0.roam_speed_mult));
    // The other cells are untouched.
    const u32 n = static_cast<u32>(TowerType::Neutrophil);
    REQUIRE(cfg.towers.mechanics[n].builder.build_cooldown == Catch::Approx(base.towers.mechanics[n].builder.build_cooldown));
    REQUIRE(cfg.towers.mechanics[n].builder.max_scars == base.towers.mechanics[n].builder.max_scars);
}

TEST_CASE("capstones turn on their mechanics", "[meta][tree]") {
    GameConfig cfg = default_game_config();
    const TreeEffects fx = apply_immunity_tree(
        bought({{TreeNode::CytotoxicRoot, 1}, {TreeNode::CytotoxicCapstone, 1},
                {TreeNode::MacrophageRoot, 1}, {TreeNode::MacrophageCapstone, 1},
                {TreeNode::NeutrophilCapstone, 1}, {TreeNode::GobletRoot, 1},
                {TreeNode::GobletCapstone, 1}, {TreeNode::FibroblastRoot, 1},
                {TreeNode::FibroblastCapstone, 1}}),
        cfg);
    const CapstoneParams& cyto = cfg.towers.mechanics[static_cast<u32>(TowerType::CytotoxicT)].capstone;
    REQUIRE(cyto.kill_pulse_radius == Catch::Approx(10.0f));
    REQUIRE(cyto.kill_pulse_damage == Catch::Approx(1.2f));
    REQUIRE(cyto.named_damage_mult > 1.0f);
    REQUIRE(cfg.towers.mechanics[static_cast<u32>(TowerType::Macrophage)].capstone.heal_per_kill > 0.0f);
    REQUIRE(cfg.towers.mechanics[static_cast<u32>(TowerType::Neutrophil)].shooter.round_damage ==
            Catch::Approx(default_game_config().towers.mechanics[static_cast<u32>(TowerType::Neutrophil)].shooter.round_damage * 1.25f));
    REQUIRE(fx.immunity.contagion_radius > 0.0f);
    REQUIRE(fx.immunity.scar_contact_rate > 0.0f);
    REQUIRE(fx.tower_mask == TowerSystem::kAllTowersMask);
}

TEST_CASE("ability lines reach the ability tuning", "[meta][tree]") {
    const GameConfig base = default_game_config();
    GameConfig cfg = base;
    apply_immunity_tree(bought({{TreeNode::ComplementUnlock, 1}, {TreeNode::ComplementChain, 2},
                                {TreeNode::FeverUnlock, 1}, {TreeNode::FeverMagnitude, 1},
                                {TreeNode::ClotUnlock, 1}, {TreeNode::ClotCooldown, 3}}),
                        cfg);
    const auto& cascade = cfg.abilities.ability[static_cast<u32>(AbilityId::ComplementCascadeBurst)];
    const auto& fever = cfg.abilities.ability[static_cast<u32>(AbilityId::FeverResponse)];
    const auto& clot = cfg.abilities.ability[static_cast<u32>(AbilityId::FibrinClot)];
    REQUIRE(cascade.chain_links == 12u);
    REQUIRE(fever.fever_cooldown_relief ==
            Catch::Approx(base.abilities.ability[static_cast<u32>(AbilityId::FeverResponse)].fever_cooldown_relief *
                          1.25f));
    REQUIRE(clot.cooldown_seconds ==
            Catch::Approx(base.abilities.ability[static_cast<u32>(AbilityId::FibrinClot)].cooldown_seconds * 0.7f));
}

// ---------------------------------------------------------------------------
// Unlocks are enforced by the systems, not the HUD.
// ---------------------------------------------------------------------------

TEST_CASE("a locked tower cannot be placed by any path", "[meta][tree][towers]") {
    Field f;
    TowerSystem towers;
    towers.register_systems(f.world);
    const Vec2 at{20.0f, 10.0f};
    REQUIRE(towers.validate(f.world, TowerType::Macrophage, at, 100000).valid());
    towers.set_unlocked_towers(1u << static_cast<u32>(TowerType::Neutrophil));
    REQUIRE(towers.validate(f.world, TowerType::Macrophage, at, 100000).result ==
            PlacementResult::TowerLocked);
    REQUIRE_FALSE(towers.place(f.world, TowerType::Macrophage, at).valid());
    REQUIRE(towers.validate(f.world, TowerType::Neutrophil, at, 100000).valid());
}

TEST_CASE("a locked ability refuses to cast", "[meta][tree][abilities]") {
    Field f;
    ActiveAbilitySystem abilities;
    abilities.load_defaults();
    abilities.register_systems(f.world);
    abilities.set_unlocked(0u);
    REQUIRE_FALSE(abilities.cast(f.world, AbilityId::HistamineFlare, Vec2{20.0f, 10.0f}));
    REQUIRE(abilities.ready(AbilityId::HistamineFlare)); // no cooldown spent
    abilities.set_unlocked(1u << static_cast<u32>(AbilityId::HistamineFlare));
    REQUIRE(abilities.cast(f.world, AbilityId::HistamineFlare, Vec2{20.0f, 10.0f}));
}

TEST_CASE("Fever's Buff Duration keeps every tower reloading faster after the burst", "[meta][tree][abilities]") {
    Field f;
    AbilityConfig ac = ability_config();
    AbilityTuning& fever = ac.ability[static_cast<u32>(AbilityId::FeverResponse)];
    fever.fever_cooldown_relief = 1.0f;
    fever.fever_linger_seconds = 1.0f;
    fever.fever_linger_rate = 0.5f;
    ActiveAbilitySystem abilities;
    apply_ability_config(abilities, ac);
    abilities.register_systems(f.world);

    entt::registry& reg = f.world.ecs().registry();
    const entt::entity tower = reg.create();
    reg.emplace<sim::comp::Tower>(tower, sim::comp::Tower{TowerType::Neutrophil, 8.0f, 10.0f, 1.0f, {}});
    REQUIRE(abilities.cast(f.world, AbilityId::FeverResponse, Vec2{}));
    REQUIRE(reg.get<sim::comp::Tower>(tower).cooldown == Catch::Approx(9.0f));
    for (int i = 0; i < 90; ++i) f.world.tick();
    // One second of linger at 0.5 extra seconds per second, and nothing after.
    REQUIRE(reg.get<sim::comp::Tower>(tower).cooldown == Catch::Approx(8.5f).margin(0.02f));

    // The shipped defaults have no linger: restore them for later tests.
    apply_ability_config(abilities, ability_config());
}

TEST_CASE("Chain Links extends the cascade's hop count", "[meta][tree][damage]") {
    for (const u32 links : {0u, 12u}) {
        Field f;
        for (int i = 0; i < 16; ++i) f.spawn(Vec2{10.0f + static_cast<f32>(i), 10.0f});
        f.world.tick(); // index the hash
        sim::DamageField field;
        field.shape = sim::FieldShape::Chain;
        field.origin = Vec2{10.0f, 10.0f};
        field.radius = 1.6f;
        field.kill_rate = 1.0e5f;
        field.lifetime = 0.1f;
        field.chain_links = links;
        f.world.damage().submit(field);
        const u64 before = f.world.snapshot().chaff_killed_total;
        f.world.tick();
        const u64 killed = f.world.snapshot().chaff_killed_total - before;
        REQUIRE(killed == (links == 0 ? 8u : 12u));
    }
}

// ---------------------------------------------------------------------------
// Capstones in the sim.
// ---------------------------------------------------------------------------

TEST_CASE("Anaphylactic Shock: a slowed agent's death slows its neighbours", "[meta][tree][sim]") {
    for (const bool on : {false, true}) {
        Field f;
        sim::ImmunityTuning im;
        if (on) {
            im.contagion_radius = 3.0f;
            im.contagion_seconds = 2.0f;
        }
        f.world.set_immunity(im);
        const usize dying = f.spawn(Vec2{30.0f, 10.0f});
        f.spawn(Vec2{31.5f, 10.0f});   // inside the radius
        f.spawn(Vec2{45.0f, 10.0f});   // well outside it
        sim::ChaffBuffers& c = f.world.chaff();
        c.flags[dying] |= sim::chaff_flags::kSlowed;
        c.slow_remaining[dying] = 5.0f;
        c.slow_factor[dying] = 0.2f;
        f.world.tick(); // index the hash
        c.apply_density_loss(dying, 10.0f);
        f.world.tick();
        REQUIRE(f.world.chaff().count() == 2);
        REQUIRE(f.slowed() == (on ? 1u : 0u));
    }
}

TEST_CASE("Inflammatory Scarring: a scar burns what presses against it", "[meta][tree][sim][scar]") {
    for (const bool on : {false, true}) {
        Field f;
        sim::ImmunityTuning im;
        if (on) {
            im.scar_contact_rate = 120.0f;
            im.scar_contact_reach = 0.8f;
        }
        f.world.set_immunity(im);
        sim::ScarDesc d;
        d.center = Vec2{30.0f, 10.0f};
        d.half_extents = Vec2{4.0f, 0.9f};
        d.max_health = 500.0f;
        d.owner = EntityId{7u};
        REQUIRE(f.world.scars().build(f.world, d, nullptr, nullptr).valid());
        f.spawn(Vec2{28.6f, 10.0f});   // against the upstream face
        f.spawn(Vec2{10.0f, 10.0f});   // nowhere near it
        for (int i = 0; i < 3; ++i) f.world.tick();
        REQUIRE(f.world.chaff().count() == (on ? 1u : 2u));
    }
}

TEST_CASE("Apoptosis Trigger: a latcher that finishes its host bursts it", "[meta][tree][sim][swarm]") {
    for (const bool on : {false, true}) {
        Field f;
        sim::SwarmerProfile latch;
        latch.kind = sim::SwarmerKind::Latch;
        latch.source = TowerType::CytotoxicT;
        latch.lifetime = 5.0f;
        latch.speed = 20.0f;
        latch.search_radius = 10.0f;
        latch.attach_radius = 0.6f;
        latch.attach_seconds = 0.0f;
        latch.dps = 30.0f;
        if (on) {
            latch.kill_pulse_radius = 2.5f;
            latch.kill_pulse_damage = 3.0f;
        }
        f.world.swarmers().set_profile(2, latch);
        f.spawn(Vec2{30.0f, 10.0f}, 0.5f);
        f.world.tick();
        sim::SwarmerSpawnParams sp;
        sp.position = Vec2{f.world.chaff().pos_x[0], f.world.chaff().pos_y[0]};
        sp.profile = 2;
        sp.owner = EntityId{9u};
        sp.seed = 0x77u;
        f.world.swarmers().spawn(sp);
        u32 pulses = 0;
        for (int i = 0; i < 120 && f.world.chaff().count() > 0; ++i) {
            f.world.tick();
            pulses += f.world.swarmer_system().last_stats().kill_pulses;
        }
        REQUIRE(f.world.chaff().count() == 0);
        REQUIRE(pulses == (on ? 1u : 0u));
    }
}

TEST_CASE("Phagocytic Sustain: a heal lands on the tower, capped at its max", "[meta][tree][sim]") {
    Field f;
    entt::registry& reg = f.world.ecs().registry();
    const entt::entity tower = reg.create();
    reg.emplace<sim::comp::Tower>(tower);
    reg.emplace<sim::comp::Health>(tower, sim::comp::Health{50.0f, 100.0f, 0.0f});
    const EntityId id = f.world.ecs().to_id(tower);

    f.world.swarmer_system().effects().heals.push_back(sim::SwarmerHeal{id, 30.0f});
    f.world.apply_swarmer_effects();
    REQUIRE(reg.get<sim::comp::Health>(tower).current == Catch::Approx(80.0f));
    f.world.swarmer_system().effects().heals.push_back(sim::SwarmerHeal{id, 30.0f});
    f.world.apply_swarmer_effects();
    REQUIRE(reg.get<sim::comp::Health>(tower).current == Catch::Approx(100.0f));
}

TEST_CASE("Homeostasis scales what a leak costs the objective", "[meta][tree][sim]") {
    f32 lost[2] = {};
    for (const bool on : {false, true}) {
        Field f;
        sim::ImmunityTuning im;
        im.leak_damage = on ? 0.5f : 1.0f;
        f.world.set_immunity(im);
        f.world.chaff_system().set_goal(kGoal, Vec2{3.0f, 3.0f});
        for (int i = 0; i < 6; ++i) f.spawn(Vec2{kGoal.x - 0.2f, kGoal.y - 1.0f + 0.4f * i});
        for (int i = 0; i < 240 && f.world.chaff().count() > 0; ++i) f.world.tick();
        REQUIRE(f.world.snapshot().chaff_leaked_total == 6);
        lost[on ? 1 : 0] = 100.0f - f.world.snapshot().objective_integrity;
    }
    REQUIRE(lost[0] == Catch::Approx(6.0f));
    REQUIRE(lost[1] == Catch::Approx(3.0f));
}

// ---------------------------------------------------------------------------
// Weakening Mucus and Inflammation.
// ---------------------------------------------------------------------------

TEST_CASE("the Weakening Mucus and Inflammation lines reach the sim tuning", "[meta][tree]") {
    GameConfig cfg = default_game_config();
    const TreeEffects off = apply_immunity_tree(bought({}), cfg);
    REQUIRE(off.immunity.slowed_damage_mult == 1.0f);
    REQUIRE(off.immunity.inflammation_radius == 0.0f);

    cfg = default_game_config();
    const TreeEffects on = apply_immunity_tree(
        bought({{TreeNode::GobletRoot, 1}, {TreeNode::GobletWeakness, 2},
                {TreeNode::FibroblastRoot, 1}, {TreeNode::FibroblastInflammation, 3}}),
        cfg);
    REQUIRE(on.immunity.slowed_damage_mult == Catch::Approx(1.2f));
    REQUIRE(on.immunity.inflammation_radius > 0.0f);
    REQUIRE(on.immunity.inflammation_damage_mult == Catch::Approx(1.3f));
    REQUIRE(on.immunity.inflammation_reload_mult == Catch::Approx(1.3f));
}

TEST_CASE("Weakening Mucus: a slowed agent takes more from the same hit", "[meta][tree][sim]") {
    Field f;
    sim::ImmunityTuning im;
    im.slowed_damage_mult = 1.5f;
    f.world.set_immunity(im);
    const usize plain = f.spawn(Vec2{20.0f, 5.0f}, 10.0f);
    const usize slowed = f.spawn(Vec2{20.0f, 15.0f}, 10.0f);
    sim::ChaffBuffers& c = f.world.chaff();
    c.flags[slowed] |= sim::chaff_flags::kSlowed;
    c.slow_remaining[slowed] = 5.0f;
    c.apply_density_loss(plain, 2.0f);
    c.apply_density_loss(slowed, 2.0f);
    REQUIRE(c.density[plain] == Catch::Approx(8.0f));
    REQUIRE(c.density[slowed] == Catch::Approx(7.0f));

    // Off again, the slow no longer matters.
    f.world.set_immunity(sim::ImmunityTuning{});
    c.apply_density_loss(slowed, 2.0f);
    REQUIRE(c.density[slowed] == Catch::Approx(5.0f));
}

TEST_CASE("Inflammation: a unit near a scar drains harder, a tower near one reloads faster", "[meta][tree][sim][scar]") {
    f32 drained[2] = {};
    f32 cooldown[2] = {};
    for (const bool on : {false, true}) {
        Field f;
        sim::ImmunityTuning im;
        if (on) {
            im.inflammation_radius = 8.0f;
            im.inflammation_damage_mult = 1.5f;
            im.inflammation_reload_mult = 1.5f;
        }
        f.world.set_immunity(im);
        sim::ScarDesc d;
        d.center = Vec2{20.0f, 10.0f};
        d.half_extents = Vec2{4.0f, 0.9f};
        d.max_health = 500.0f;
        d.owner = EntityId{7u};
        REQUIRE(f.world.scars().build(f.world, d, nullptr, nullptr).valid());

        // A tower 5 units from the scar, with no tower system of its own:
        // only Inflammation moves its cooldown.
        entt::registry& reg = f.world.ecs().registry();
        const entt::entity tower = reg.create();
        reg.emplace<sim::comp::Tower>(tower, sim::comp::Tower{TowerType::Neutrophil, 8.0f, 10.0f, 1.0f, {}});
        reg.emplace<sim::comp::Transform>(tower, sim::comp::Transform{Vec2{25.0f, 10.0f}, 0.0f, 1.0f});

        sim::SwarmerProfile latch;
        latch.kind = sim::SwarmerKind::Latch;
        latch.source = TowerType::CytotoxicT;
        latch.lifetime = 5.0f;
        latch.speed = 20.0f;
        latch.search_radius = 10.0f;
        latch.attach_radius = 0.6f;
        latch.attach_seconds = 0.0f;
        latch.dps = 6.0f;
        f.world.swarmers().set_profile(2, latch);
        f.spawn(Vec2{26.0f, 10.0f}, 100.0f);
        f.world.tick();
        sim::SwarmerSpawnParams sp;
        sp.position = Vec2{f.world.chaff().pos_x[0], f.world.chaff().pos_y[0]};
        sp.profile = 2;
        sp.owner = EntityId{9u};
        sp.seed = 0x77u;
        f.world.swarmers().spawn(sp);
        for (int i = 0; i < 30; ++i) f.world.tick();
        REQUIRE(f.world.chaff().count() == 1);
        drained[on ? 1 : 0] = 100.0f - f.world.chaff().density[0];
        cooldown[on ? 1 : 0] = reg.get<sim::comp::Tower>(tower).cooldown;
    }
    REQUIRE(drained[0] > 0.5f);
    REQUIRE(drained[1] == Catch::Approx(drained[0] * 1.5f).epsilon(0.05));
    REQUIRE(cooldown[0] == Catch::Approx(10.0f));
    // 31 ticks at half a tick's extra relief each.
    REQUIRE(cooldown[1] == Catch::Approx(10.0f - 31.0f * 0.5f / 60.0f).margin(0.01f));
}

TEST_CASE("Inflammation speeds the reload of a deployed shooter beside a scar", "[meta][tree][sim][scar]") {
    f32 reload_progress[2] = {};
    for (const bool on : {false, true}) {
        Field f;
        sim::ImmunityTuning im;
        if (on) {
            im.inflammation_radius = 8.0f;
            im.inflammation_reload_mult = 1.5f;
        }
        f.world.set_immunity(im);
        sim::ScarDesc scar;
        scar.center = Vec2{20.0f, 10.0f};
        scar.half_extents = Vec2{4.0f, 0.9f};
        scar.max_health = 500.0f;
        REQUIRE(f.world.scars().build(f.world, scar, nullptr, nullptr).valid());

        sim::SwarmerProfile shooter;
        shooter.kind = sim::SwarmerKind::Shooter;
        shooter.source = TowerType::Neutrophil;
        shooter.speed = 0.0f;
        shooter.search_radius = 0.0f;
        shooter.reload_seconds = 2.0f;
        f.world.swarmers().set_profile(1, shooter);
        sim::SwarmerSpawnParams sp;
        sp.position = Vec2{25.0f, 10.0f};
        sp.profile = 1;
        sp.persistent = true;
        REQUIRE(f.world.swarmers().spawn(sp));
        f.world.swarmers().magazine[0].phase = sim::MagazinePhase::Reloading;

        for (int i = 0; i < 30; ++i) f.world.tick();
        reload_progress[on ? 1 : 0] = f.world.swarmers().magazine[0].timer;
    }
    REQUIRE(reload_progress[0] == Catch::Approx(0.5f).margin(0.01f));
    REQUIRE(reload_progress[1] == Catch::Approx(0.75f).margin(0.01f));
}
