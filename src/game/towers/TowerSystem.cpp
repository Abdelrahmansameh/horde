// game/towers/TowerSystem.cpp — placement, targeting, upgrades. Wave 2B.
//
// TowerSystem.h is a FROZEN CONTRACT: no signature, member, or friend may be
// added to it. Two consequences shape everything below:
//
//  1. Any state that must outlive a single call but isn't already one of the
//     header's declared members (`stats_`, `towers_`) cannot live on
//     TowerSystem itself. Per-tower bookkeeping needed for sell() refunds and
//     tissue restoration, and Neutrophil's NET slow-zone state, are instead
//     stored as ordinary EnTT components private to this translation unit
//     (`priv::TowerRecord`, `priv::ActiveNet`) — attached to entities this
//     file creates, cleaned up automatically by registry.destroy().
//  2. Helper logic that needs `stats_`/`towers_` can only live inside the
//     bodies of the methods the header actually declares (validate, place,
//     upgrade, sell, trigger_ability, find_target, stats, set_stats,
//     register_systems) — those have normal private access because they are
//     genuine member-function definitions. Free functions in the anonymous
//     namespace below (the default-stats loader, the eight per-tower Combat
//     systems, the WouldBlockAllPaths heuristic) only ever go through the
//     class's *public* API (`stats()`, `find_target()`, `trigger_ability()`).
#include "game/towers/TowerSystem.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/TissueRaster.h"
#include "sim/spatial/SpatialHash.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace immune::game {

// TowerSystem.h (this class's own header) refers to sim::SimWorld etc. with an
// explicit `sim::` qualifier throughout, which this file keeps for anything
// declared in the header's own signatures. Everything below additionally
// needs `sim::comp::*`, `sim::EcsWorld`, `sim::SystemContext`, `sim::block_rect`,
// `sim::chaff_flags`, and friends constantly enough that an unqualified
// `comp::Tower` etc. is far more readable — hence this using-directive,
// exactly like the sim-side test files (e.g. tests/test_named_agents.cpp) do.
using namespace immune::sim;

namespace {

constexpr const char* kTowerNames[kTowerTypeCount] = {
    "macrophage", "neutrophil", "dendritic",  "cytotoxic_t",
    "b_cell",     "nk_cell",    "mast_cell",  "complement_cascade"};

// ---------------------------------------------------------------------------
// Private auxiliary ECS components (see file header comment).
// ---------------------------------------------------------------------------
namespace priv {

/// Attached to every tower entity at place() time. Carries what sell() needs
/// to refund ATP and restore the exact tissue cells that were blocked — not
/// just "set walkable = true", since a footprint's square corners can extend
/// slightly past the disc the clearance check guaranteed, and may have been
/// unwalkable (or a non-default cost) *before* this tower ever stood there.
struct TowerRecord {
    Rect footprint{};
    IVec2 cell_min{0, 0};
    IVec2 cell_dims{0, 0};
    std::vector<u8> saved_walkable;
    std::vector<f32> saved_cost;
    u32 invested_atp = 0;
};

/// A live Neutrophil NET: a timed slow zone. Its own entity rather than tower
/// state, so it keeps ticking down (and applying kSlowed) independently of
/// the tower that dropped it, including across an upgrade or sell.
struct ActiveNet {
    Vec2 origin{0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 remaining = 0.0f;
    EntityId owner{};
};

} // namespace priv

Rect footprint_rect(Vec2 pos, f32 radius) { return Rect{pos - Vec2{radius, radius}, pos + Vec2{radius, radius}}; }

// ---------------------------------------------------------------------------
// Default stats table (deliverable 1).
// ---------------------------------------------------------------------------

/// TowerStats{}'s default-member-initializer build_cost. `stats_[8][3]{}`
/// value-initializes every slot identically to this, so
/// `stats(Macrophage, 1).build_cost == kUnpopulatedBuildCost` is a reliable
/// "load_default_stats() has never populated this instance" sentinel — see
/// ensure_default_stats() below for why a sentinel is needed at all instead
/// of a constructor or a loaded-flag member.
constexpr u32 kUnpopulatedBuildCost = 100;

TowerStats make_stats(f32 range, f32 fire_interval, f32 damage, f32 kill_rate, f32 footprint_radius,
                      u32 build_cost, u32 upgrade_cost, f32 ability_cooldown, bool blocks_flow = true) {
    TowerStats s;
    s.range = range;
    s.fire_interval = fire_interval;
    s.damage = damage;
    s.kill_rate = kill_rate;
    s.footprint_radius = footprint_radius;
    s.build_cost = build_cost;
    s.upgrade_cost = upgrade_cost;
    s.ability_cooldown = ability_cooldown;
    s.family_mask = 0xFF; // every tower can affect every family; see report re: tuning scope.
    s.blocks_flow = blocks_flow;
    return s;
}

// ---------------------------------------------------------------------------
// Cost-curve design goal (DESIGN.md §5.3/§7.1, deliverable 2): upgrading one
// tier must be a reliably better ATP-per-output deal than placing a fresh
// tower of equivalent total output, so reinforcing a concentrated position
// beats spreading thin. Concretely, for every tower type below: if tier 1
// costs `C` and produces output `O` (its per-second damage/kill_rate,
// whichever the type actually uses — see tests/test_towers.cpp's
// tower_output() for the exact per-type metric), tier 2's upgrade_cost is
// noticeably less than `C` again while tier 2's output pushes well past `2O`,
// and tier 3's upgrade_cost is smaller still while its output pulls further
// ahead — an accelerating-value, decelerating-cost curve up the tree. Tier 1
// numbers are deliberately left exactly as they were (several existing tests
// hardcode them, e.g. Mast Cell's density-threshold test relies on
// range*kill_rate == 15); only tier 2/3 damage-or-kill_rate and every tier's
// upgrade_cost move. See tests/test_towers.cpp's
// "upgrading is a better ATP-per-output deal than a fresh tower" test for the
// numeric proof, across all 8 types.
// ---------------------------------------------------------------------------

void load_default_stats(TowerSystem& self) {
    // Macrophage: melee sink. Big kill_rate self-field + slow single-target DPS.
    self.set_stats(TowerType::Macrophage, 1, make_stats(4.0f, 1.0f, 6.0f, 3.0f, 1.0f, 80, 45, 0.0f));
    self.set_stats(TowerType::Macrophage, 2, make_stats(4.5f, 0.9f, 16.0f, 7.0f, 1.0f, 80, 30, 0.0f));
    self.set_stats(TowerType::Macrophage, 3, make_stats(5.0f, 0.75f, 30.0f, 12.0f, 1.0f, 80, 0, 0.0f));

    // Neutrophil: swarm. Modest self-field; NET + micro-units is the ability.
    self.set_stats(TowerType::Neutrophil, 1, make_stats(5.0f, 0.6f, 2.0f, 2.0f, 0.8f, 90, 50, 6.0f));
    self.set_stats(TowerType::Neutrophil, 2, make_stats(5.5f, 0.5f, 2.0f, 8.5f, 0.8f, 90, 32, 5.0f));
    self.set_stats(TowerType::Neutrophil, 3, make_stats(6.0f, 0.4f, 2.0f, 15.5f, 0.8f, 90, 0, 4.0f));

    // Dendritic: support, no damage — kill_rate stays 0 for every tier. Its
    // only power lever is coverage (range), so tier growth pushes range hard.
    self.set_stats(TowerType::Dendritic, 1, make_stats(5.0f, 0.5f, 0.0f, 0.0f, 0.75f, 70, 40, 0.0f));
    self.set_stats(TowerType::Dendritic, 2, make_stats(7.5f, 0.5f, 0.0f, 0.0f, 0.75f, 70, 25, 0.0f));
    self.set_stats(TowerType::Dendritic, 3, make_stats(10.0f, 0.5f, 0.0f, 0.0f, 0.75f, 70, 0, 0.0f));

    // Cytotoxic T: precision named-agent burst, bonus vs elite/boss applied in system_cytotoxic_t.
    self.set_stats(TowerType::CytotoxicT, 1, make_stats(6.0f, 1.2f, 22.0f, 0.0f, 0.8f, 110, 65, 0.0f));
    self.set_stats(TowerType::CytotoxicT, 2, make_stats(6.5f, 0.9f, 46.0f, 0.0f, 0.8f, 110, 42, 0.0f));
    self.set_stats(TowerType::CytotoxicT, 3, make_stats(7.0f, 0.65f, 80.0f, 0.0f, 0.8f, 110, 0, 0.0f));

    // B-Cell: homing tag & chase (see system_bcell for the Marked-application simplification).
    self.set_stats(TowerType::BCell, 1, make_stats(7.0f, 1.0f, 8.0f, 0.0f, 0.7f, 100, 55, 0.0f));
    self.set_stats(TowerType::BCell, 2, make_stats(8.0f, 0.75f, 18.0f, 0.0f, 0.7f, 100, 35, 0.0f));
    self.set_stats(TowerType::BCell, 3, make_stats(9.0f, 0.55f, 34.0f, 0.0f, 0.7f, 100, 0, 0.0f));

    // NK Cell: anti-stealth precision (find_target with require_detect_hidden=true).
    self.set_stats(TowerType::NKCell, 1, make_stats(6.0f, 1.0f, 20.0f, 0.0f, 0.8f, 120, 68, 0.0f));
    self.set_stats(TowerType::NKCell, 2, make_stats(6.5f, 0.85f, 44.0f, 0.0f, 0.8f, 120, 44, 0.0f));
    self.set_stats(TowerType::NKCell, 3, make_stats(7.0f, 0.6f, 85.0f, 0.0f, 0.8f, 120, 0, 0.0f));

    // Mast Cell: reactive nova. TowerStats has no dedicated "trigger threshold"
    // field (frozen struct); system_mastcell() reuses range * kill_rate as a
    // per-tier-tunable density threshold — see that function's comment. Tier 1
    // range/kill_rate are untouched (threshold stays 15, matching the existing
    // combat test), only tiers 2/3 move.
    self.set_stats(TowerType::MastCell, 1, make_stats(5.0f, 1.0f, 0.0f, 3.0f, 0.9f, 130, 75, 8.0f));
    self.set_stats(TowerType::MastCell, 2, make_stats(5.5f, 1.0f, 0.0f, 7.0f, 0.9f, 130, 48, 6.0f));
    self.set_stats(TowerType::MastCell, 3, make_stats(6.0f, 1.0f, 0.0f, 15.0f, 0.9f, 130, 0, 4.5f));

    // Complement Cascade: ultimate chain nova.
    self.set_stats(TowerType::ComplementCascade, 1, make_stats(6.0f, 1.0f, 0.0f, 6.0f, 1.2f, 220, 130, 10.0f));
    self.set_stats(TowerType::ComplementCascade, 2, make_stats(6.5f, 1.0f, 0.0f, 14.0f, 1.2f, 220, 85, 8.0f));
    self.set_stats(TowerType::ComplementCascade, 3, make_stats(7.0f, 1.0f, 0.0f, 30.0f, 1.2f, 220, 0, 6.0f));
}

/// TowerSystem.h forbids adding a constructor, so there is no natural hook to
/// populate `stats_` with real per-type/tier values once per instance. This
/// checks the sentinel described at kUnpopulatedBuildCost and lazily
/// populates the whole table the first time any public entry point observes
/// it still holds the header's uniform default. Idempotent; safe to call
/// from const methods via const_cast because the underlying TowerSystem
/// object is never actually const in practice (App owns a plain member; the
/// tests below construct plain locals).
void ensure_default_stats(const TowerSystem& self) {
    if (self.stats(TowerType::Macrophage, 1).build_cost != kUnpopulatedBuildCost) return;
    load_default_stats(const_cast<TowerSystem&>(self));
}

// ---------------------------------------------------------------------------
// WouldBlockAllPaths heuristic.
// ---------------------------------------------------------------------------

/// Local, bounded-cost stand-in for a full re-route check. TowerSystem.h
/// explicitly leaves the exact approach up to the implementer (it points at
/// FlowField::reachable() and mentions either probing from a portal or
/// test-blocking a scratch copy). A *global* scratch copy would need the
/// level's portal list, which SimWorld does not store (Level.h's
/// SpawnPortal/placement_zones never get threaded onto SimWorld — see the
/// report for this gap) — so this tests connectivity in a *local* window
/// around the footprint instead, on the theory that a placement can only
/// wall off "every lane" by fully closing the local vessel cross-section it
/// sits in; if a walkable detour exists within a modest margin, the (real,
/// unedited) flow field already had a path through there and will keep
/// routing through it after the rebake. Anchors are seeded only from points
/// currently `reachable()` per the live FlowField, so a placement inside an
/// already-dead-end pocket is never wrongly rejected.
bool would_block_all_paths(const sim::SimWorld& world, const Rect& footprint) {
    const sim::TissueMask& mask = world.tissue();
    const sim::FlowField& flow = world.flow();
    if (mask.width() <= 0 || mask.height() <= 0) return false;

    const f32 cs = math::max(mask.cell_size(), 1e-3f);
    const f32 pad = cs * 14.0f;
    const IVec2 c0raw = mask.world_to_cell(footprint.min - Vec2{pad, pad});
    const IVec2 c1raw = mask.world_to_cell(footprint.max + Vec2{pad, pad});
    const i32 x0 = math::clamp(c0raw.x, 0, mask.width() - 1);
    const i32 y0 = math::clamp(c0raw.y, 0, mask.height() - 1);
    const i32 x1 = math::clamp(c1raw.x, 0, mask.width() - 1);
    const i32 y1 = math::clamp(c1raw.y, 0, mask.height() - 1);
    const i32 w = x1 - x0 + 1;
    const i32 h = y1 - y0 + 1;
    if (w <= 0 || h <= 0) return false;

    const IVec2 f0 = mask.world_to_cell(footprint.min);
    const IVec2 f1 = mask.world_to_cell(footprint.max);
    auto blocked = [&](i32 x, i32 y) { return x >= f0.x && x <= f1.x && y >= f0.y && y <= f1.y; };
    auto passable = [&](i32 x, i32 y) { return mask.in_range(x, y) && !blocked(x, y) && mask.walkable(x, y); };

    // Transient BFS scratch only — never holds anything meaningful across
    // calls, so reusing capacity via function-local statics is safe even
    // though TowerSystem.h forbids adding persistent per-instance members.
    static thread_local std::vector<u32> visited_gen;
    static thread_local u32 gen = 0;
    static thread_local std::vector<IVec2> stack;
    static thread_local std::vector<IVec2> anchors;
    const usize cell_count = static_cast<usize>(w) * static_cast<usize>(h);
    if (visited_gen.size() < cell_count) visited_gen.assign(cell_count, 0);
    ++gen;
    stack.clear();
    anchors.clear();

    auto local_index = [&](i32 x, i32 y) {
        return static_cast<usize>(y - y0) * static_cast<usize>(w) + static_cast<usize>(x - x0);
    };

    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const bool on_border = (x == x0 || x == x1 || y == y0 || y == y1);
            if (!on_border || !mask.walkable(x, y)) continue;
            if (!flow.reachable(mask.cell_to_world(x, y))) continue;
            anchors.push_back(IVec2{x, y});
        }
    }
    if (anchors.size() < 2) return false; // nothing that could be disconnected

    stack.push_back(anchors[0]);
    visited_gen[local_index(anchors[0].x, anchors[0].y)] = gen;
    const IVec2 offsets[4] = {IVec2{1, 0}, IVec2{-1, 0}, IVec2{0, 1}, IVec2{0, -1}};
    while (!stack.empty()) {
        const IVec2 cur = stack.back();
        stack.pop_back();
        for (const IVec2& o : offsets) {
            const i32 nx = cur.x + o.x;
            const i32 ny = cur.y + o.y;
            if (nx < x0 || nx > x1 || ny < y0 || ny > y1 || !passable(nx, ny)) continue;
            const usize idx = local_index(nx, ny);
            if (visited_gen[idx] == gen) continue;
            visited_gen[idx] = gen;
            stack.push_back(IVec2{nx, ny});
        }
    }

    for (usize i = 1; i < anchors.size(); ++i) {
        if (visited_gen[local_index(anchors[i].x, anchors[i].y)] != gen) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tissue footprint snapshot/restore (place()/sell()).
// ---------------------------------------------------------------------------

void snapshot_and_block(sim::TissueMask& mask, const Rect& footprint, priv::TowerRecord& rec) {
    const IVec2 c0 = mask.world_to_cell(footprint.min);
    const IVec2 c1 = mask.world_to_cell(footprint.max);
    rec.footprint = footprint;
    rec.cell_min = c0;
    rec.cell_dims = IVec2{c1.x - c0.x + 1, c1.y - c0.y + 1};
    const usize n = static_cast<usize>(rec.cell_dims.x) * static_cast<usize>(rec.cell_dims.y);
    rec.saved_walkable.assign(n, 0);
    rec.saved_cost.assign(n, 1.0f);
    for (i32 y = c0.y; y <= c1.y; ++y) {
        for (i32 x = c0.x; x <= c1.x; ++x) {
            const usize idx =
                static_cast<usize>(y - c0.y) * static_cast<usize>(rec.cell_dims.x) + static_cast<usize>(x - c0.x);
            rec.saved_walkable[idx] = mask.walkable(x, y) ? 1u : 0u;
            rec.saved_cost[idx] = mask.cost(x, y);
        }
    }
    sim::block_rect(mask, footprint);
}

void restore_footprint(sim::TissueMask& mask, const priv::TowerRecord& rec) {
    for (i32 y = 0; y < rec.cell_dims.y; ++y) {
        for (i32 x = 0; x < rec.cell_dims.x; ++x) {
            const i32 wx = rec.cell_min.x + x;
            const i32 wy = rec.cell_min.y + y;
            const usize idx = static_cast<usize>(y) * static_cast<usize>(rec.cell_dims.x) + static_cast<usize>(x);
            mask.set_walkable(wx, wy, rec.saved_walkable[idx] != 0);
            mask.set_cost(wx, wy, rec.saved_cost[idx]);
        }
    }
}

// ---------------------------------------------------------------------------
// Per-tower Combat-phase systems (deliverable 3). Free functions taking
// `TowerSystem&` so they can reach stats()/find_target()/trigger_ability()
// through the public API only (see file header comment).
// ---------------------------------------------------------------------------

/// Macrophage: melee sink. Submits a small high-kill-rate self field against
/// chaff every tick, and separately runs continuous single-target DPS against
/// the nearest named agent in range ("eating" an elite over time).
void system_macrophage(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::Macrophage) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);

        if (st.kill_rate > 0.0f) {
            sim::DamageField field;
            field.shape = sim::FieldShape::Circle;
            field.origin = tf.position;
            field.radius = st.range;
            field.kill_rate = st.kill_rate;
            field.family_mask = st.family_mask;
            field.marked_multiplier = 1.5f;
            field.lifetime = 0.0f; // persistent: resubmitted every tick
            field.owner = ctx.world.ecs().to_id(e);
            ctx.world.damage().submit(field);
        }

        if (st.damage <= 0.0f) continue;
        entt::entity te = ctx.world.ecs().from_id(tw.current_target);
        bool have_target = tw.current_target.valid() && ctx.registry.valid(te) &&
                           ctx.registry.all_of<comp::Health, comp::Transform>(te);
        if (have_target) {
            const f32 d2 = math::length_sq(ctx.registry.get<comp::Transform>(te).position - tf.position);
            have_target = d2 <= st.range * st.range && !ctx.registry.get<comp::Health>(te).dead();
        }
        if (!have_target) {
            tw.current_target = self.find_target(ctx.world, tf.position, st.range, st.family_mask, false);
            te = ctx.world.ecs().from_id(tw.current_target);
            have_target = tw.current_target.valid() && ctx.registry.valid(te) && ctx.registry.all_of<comp::Health>(te);
        }
        if (have_target) {
            comp::Health& hp = ctx.registry.get<comp::Health>(te);
            // Armor is a flat reduction "per damage event" (Components.h), not
            // per tick — subtracting it from the tiny per-tick fraction
            // (st.damage * dt, often << 1) would let any armor value >= that
            // fraction fully nullify continuous DPS. Apply it once to the
            // full per-second rate instead, then prorate the net by dt.
            hp.current -= math::max(0.0f, st.damage - hp.armor) * ctx.dt;
        }
    }
}

/// Neutrophil: swarm. Submits a modest self field every tick; the NET / micro
/// -unit burst is the active ability (trigger_ability), not per-tick.
void system_neutrophil(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::Neutrophil) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        sim::DamageField field;
        field.shape = sim::FieldShape::Circle;
        field.origin = tf.position;
        field.radius = st.range;
        field.kill_rate = st.kill_rate;
        field.family_mask = st.family_mask;
        field.lifetime = 0.0f;
        field.owner = ctx.world.ecs().to_id(e);
        ctx.world.damage().submit(field);
    }
}

/// Dendritic: no direct damage. Marks chaff in range every tick via a direct
/// spatial-hash query + exact test — no DamageField submitted (kill_rate 0).
void system_dendritic(sim::SystemContext& ctx) {
    static thread_local std::vector<u32> scratch;
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        const comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::Dendritic) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        scratch.clear();
        ctx.world.spatial().query_circle(tf.position, tw.range, scratch);
        sim::ChaffBuffers& chaff = ctx.world.chaff();
        const f32 r2 = tw.range * tw.range;
        for (u32 idx : scratch) {
            if (idx >= chaff.count()) continue;
            const f32 dx = chaff.pos_x[idx] - tf.position.x;
            const f32 dy = chaff.pos_y[idx] - tf.position.y;
            if (dx * dx + dy * dy > r2) continue;
            chaff.flags[idx] |= sim::chaff_flags::kMarked;
        }
    }
}

/// Cytotoxic T: precision. Named agents only, high burst on cooldown, bonus
/// multiplier vs elite (tier 1) / boss (tier >= 2).
void system_cytotoxic_t(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::CytotoxicT || tw.cooldown > 0.0f) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        const EntityId target = self.find_target(ctx.world, tf.position, tw.range, st.family_mask, false);
        if (!target.valid()) continue;
        const entt::entity te = ctx.world.ecs().from_id(target);
        if (!ctx.registry.valid(te) || !ctx.registry.all_of<comp::Health, comp::NamedAgent>(te)) continue;
        comp::Health& hp = ctx.registry.get<comp::Health>(te);
        const comp::NamedAgent& agent = ctx.registry.get<comp::NamedAgent>(te);
        const f32 tier_bonus = agent.tier >= 2 ? 2.5f : 1.75f; // named agents are always tier>=1 (elite/boss)
        hp.current -= math::max(0.0f, st.damage * tier_bonus - hp.armor);
        tw.current_target = target;
        tw.cooldown = tw.fire_interval;
    }
}

/// B-Cell: tag & chase. Simplification (documented in the report): rather
/// than simulating a homing projectile in flight, the antibody "sticks"
/// immediately by applying comp::Marked to the nearest named target. With no
/// named target in range it falls back to flagging the nearest chaff agent
/// with chaff_flags::kMarked, so the tower isn't dead weight on chaff-only
/// waves.
void system_bcell(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::BCell || tw.cooldown > 0.0f) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        const EntityId target = self.find_target(ctx.world, tf.position, tw.range, st.family_mask, false);
        bool fired = false;

        if (target.valid()) {
            const entt::entity te = ctx.world.ecs().from_id(target);
            if (ctx.registry.valid(te)) {
                comp::Marked mk;
                mk.remaining = 4.0f;
                mk.damage_multiplier = 1.5f;
                mk.source = ctx.world.ecs().to_id(e);
                ctx.registry.emplace_or_replace<comp::Marked>(te, mk);
                if (st.damage > 0.0f && ctx.registry.all_of<comp::Health>(te)) {
                    comp::Health& hp = ctx.registry.get<comp::Health>(te);
                    hp.current -= math::max(0.0f, st.damage - hp.armor);
                }
                fired = true;
            }
        }

        if (!fired) {
            static thread_local std::vector<u32> scratch;
            scratch.clear();
            ctx.world.spatial().query_circle(tf.position, tw.range, scratch);
            sim::ChaffBuffers& chaff = ctx.world.chaff();
            f32 best_d2 = tw.range * tw.range;
            i64 best_idx = -1;
            for (u32 idx : scratch) {
                if (idx >= chaff.count()) continue;
                const f32 dx = chaff.pos_x[idx] - tf.position.x;
                const f32 dy = chaff.pos_y[idx] - tf.position.y;
                const f32 d2 = dx * dx + dy * dy;
                if (d2 <= best_d2) {
                    best_d2 = d2;
                    best_idx = static_cast<i64>(idx);
                }
            }
            if (best_idx >= 0) {
                chaff.flags[static_cast<usize>(best_idx)] |= sim::chaff_flags::kMarked;
                fired = true;
            }
        }
        if (fired) tw.cooldown = tw.fire_interval;
    }
}

/// NK Cell: anti-stealth. find_target with require_detect_hidden=true is the
/// only path that may return a Burrowed named agent.
void system_nkcell(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::NKCell || tw.cooldown > 0.0f) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);
        const EntityId target =
            self.find_target(ctx.world, tf.position, tw.range, st.family_mask, /*require_detect_hidden=*/true);
        if (!target.valid()) continue;
        const entt::entity te = ctx.world.ecs().from_id(target);
        if (!ctx.registry.valid(te) || !ctx.registry.all_of<comp::Health>(te)) continue;
        comp::Health& hp = ctx.registry.get<comp::Health>(te);
        hp.current -= math::max(0.0f, st.damage - hp.armor);
        tw.current_target = target;
        tw.cooldown = tw.fire_interval;
    }
}

/// Mast Cell: reactive trap. Measures local density every tick (no damage);
/// once it crosses a threshold, triggers the nova ability.
void system_mastcell(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::MastCell) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);

        sim::DamageField region;
        region.shape = sim::FieldShape::Circle;
        region.origin = tf.position;
        region.radius = st.range;
        region.family_mask = st.family_mask;
        const f32 density = ctx.world.damage().measure_density(ctx.world.chaff(), ctx.world.spatial(), region);

        // Judgment call: TowerStats has no dedicated trigger-threshold field
        // (frozen struct); range * kill_rate gives a per-tier-tunable number
        // reusing existing fields instead.
        const f32 threshold = st.range * st.kill_rate;
        if (threshold > 0.0f && density >= threshold && tw.ability_cooldown <= 0.0f) {
            self.trigger_ability(ctx.world, ctx.world.ecs().to_id(e));
        }
    }
}

/// Complement Cascade: ultimate. Auto-casts the chain nova whenever there is
/// any chaff in range and the ability is off cooldown — it has no other
/// per-tick attack.
void system_complement(TowerSystem& self, sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower, comp::Transform>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        if (tw.type != TowerType::ComplementCascade || tw.ability_cooldown > 0.0f) continue;
        const comp::Transform& tf = view.get<comp::Transform>(e);
        const TowerStats& st = self.stats(tw.type, tw.tier);

        sim::DamageField probe;
        probe.shape = sim::FieldShape::Circle;
        probe.origin = tf.position;
        probe.radius = st.range;
        probe.family_mask = st.family_mask;
        const f32 density = ctx.world.damage().measure_density(ctx.world.chaff(), ctx.world.spatial(), probe);
        if (density > 0.0f) self.trigger_ability(ctx.world, ctx.world.ecs().to_id(e));
    }
}

/// Decrements every live Neutrophil NET, applies chaff_flags::kSlowed to
/// whatever it overlaps this tick, and destroys expired NETs. ActiveNet is
/// exclusive to this file, so this fully owns its lifecycle (unlike
/// comp::Ephemeral — see system_ephemeral_drift below).
void system_net_upkeep(sim::SystemContext& ctx) {
    static thread_local std::vector<u32> scratch;
    static thread_local std::vector<entt::entity> expired;
    expired.clear();
    auto view = ctx.registry.view<priv::ActiveNet>();
    for (auto e : view) {
        priv::ActiveNet& net = view.get<priv::ActiveNet>(e);
        net.remaining -= ctx.dt;
        scratch.clear();
        ctx.world.spatial().query_circle(net.origin, net.radius, scratch);
        sim::ChaffBuffers& chaff = ctx.world.chaff();
        const f32 r2 = net.radius * net.radius;
        for (u32 idx : scratch) {
            if (idx >= chaff.count()) continue;
            const f32 dx = chaff.pos_x[idx] - net.origin.x;
            const f32 dy = chaff.pos_y[idx] - net.origin.y;
            if (dx * dx + dy * dy > r2) continue;
            chaff.flags[idx] |= sim::chaff_flags::kSlowed;
        }
        if (net.remaining <= 0.0f) expired.push_back(e);
    }
    for (entt::entity e : expired) ctx.registry.destroy(e);
}

/// Integrates position for Ephemeral entities that also carry Velocity —
/// currently only Neutrophil's micro-units, spawned by trigger_ability.
/// Deliberately does NOT touch comp::Ephemeral::lifetime or destroy anything:
/// Wave 1D's `named::system_named_cleanup` (registered by `named::install`)
/// already owns comp::Ephemeral's decay/expiry for the whole sim. Duplicating
/// that here would double-decrement lifetime and risk destroy()ing an entity
/// twice in the same tick.
void system_ephemeral_drift(sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Ephemeral, comp::Transform, comp::Velocity>();
    for (auto e : view) {
        comp::Transform& tf = view.get<comp::Transform>(e);
        const comp::Velocity& vel = view.get<comp::Velocity>(e);
        tf.position += vel.value * ctx.dt;
    }
}

/// PreUpdate: decays comp::Tower's own cooldown timers. This one legitimately
/// needs no TowerSystem access at all.
void system_tower_cooldowns(sim::SystemContext& ctx) {
    auto view = ctx.registry.view<comp::Tower>();
    for (auto e : view) {
        comp::Tower& tw = view.get<comp::Tower>(e);
        tw.cooldown = math::max(0.0f, tw.cooldown - ctx.dt);
        tw.ability_cooldown = math::max(0.0f, tw.ability_cooldown - ctx.dt);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API (deliverables 2-4).
// ---------------------------------------------------------------------------

const char* tower_type_name(TowerType type) {
    const u32 i = static_cast<u32>(type);
    return i < kTowerTypeCount ? kTowerNames[i] : "unknown";
}

bool parse_tower_type(std::string_view name, TowerType& out) {
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        if (name == kTowerNames[i]) {
            out = static_cast<TowerType>(i);
            return true;
        }
    }
    return false;
}

const TowerStats& TowerSystem::stats(TowerType type, u8 tier) const {
    // NOTE: deliberately does NOT call the free ensure_default_stats() helper
    // here — that helper itself calls stats(Macrophage, 1) to read the
    // sentinel, and stats() calling back into ensure_default_stats() would be
    // infinite mutual recursion (this bit a first draft: SIGSEGV stack
    // overflow on the very first stats() call). Since stats() is a genuine
    // member function it has direct private access to stats_ and can do the
    // same sentinel check inline instead.
    if (stats_[0][0].build_cost == kUnpopulatedBuildCost) {
        load_default_stats(const_cast<TowerSystem&>(*this));
    }
    const u32 t = static_cast<u32>(type) < kTowerTypeCount ? static_cast<u32>(type) : 0u;
    const u32 k = (tier >= 1 && tier <= 3) ? static_cast<u32>(tier - 1) : 0u;
    return stats_[t][k];
}

void TowerSystem::set_stats(TowerType type, u8 tier, const TowerStats& s) {
    if (static_cast<u32>(type) >= kTowerTypeCount || tier < 1 || tier > 3) return;
    stats_[static_cast<u32>(type)][tier - 1] = s;
}

PlacementQuery TowerSystem::validate(const sim::SimWorld& world, TowerType type, Vec2 pos, u32 available_atp) const {
    ensure_default_stats(*this);
    PlacementQuery q;
    q.snapped_position = pos;

    const u32 t = static_cast<u32>(type);
    if (t >= kTowerTypeCount) {
        q.result = PlacementResult::NotOnTissue;
        return q;
    }
    const TowerStats& st = stats_[t][0]; // new placements are always tier 1

    const f32 clearance = world.sdf().sample(pos);
    q.clearance = clearance;
    if (clearance <= 0.0f) {
        q.result = PlacementResult::NotOnTissue;
        return q;
    }
    if (clearance < st.footprint_radius) {
        const Vec2 gn = math::normalize_safe(world.sdf().gradient(pos));
        if (gn.x != 0.0f || gn.y != 0.0f) q.snapped_position = pos + gn * (st.footprint_radius - clearance);
        q.result = PlacementResult::InsufficientClearance;
        return q;
    }

    for (const EntityId existing : towers_) {
        const entt::entity e = world.ecs().from_id(existing);
        if (!world.ecs().registry().valid(e) || !world.ecs().registry().all_of<comp::Tower, comp::Transform>(e))
            continue;
        const comp::Tower& etw = world.ecs().registry().get<comp::Tower>(e);
        const comp::Transform& etf = world.ecs().registry().get<comp::Transform>(e);
        const TowerStats& est = stats_[static_cast<u32>(etw.type)][etw.tier - 1];
        const f32 min_dist = st.footprint_radius + est.footprint_radius;
        if (math::length_sq(etf.position - pos) < min_dist * min_dist) {
            q.result = PlacementResult::Overlapping;
            return q;
        }
    }

    if (available_atp < st.build_cost) {
        q.result = PlacementResult::CannotAfford;
        return q;
    }

    // NOTE: PlacementResult::OutsidePlacementZone is never returned.
    // LevelDef::placement_zones (game/level/Level.h) is never threaded onto
    // sim::SimWorld, so TowerSystem has no data source for it. See the report.

    if (st.blocks_flow && would_block_all_paths(world, footprint_rect(pos, st.footprint_radius))) {
        q.result = PlacementResult::WouldBlockAllPaths;
        return q;
    }

    q.result = PlacementResult::Ok;
    return q;
}

EntityId TowerSystem::place(sim::SimWorld& world, TowerType type, Vec2 world_pos) {
    ensure_default_stats(*this);
    // No ATP is threaded through this signature (per the frozen header):
    // affordability is the caller's responsibility via validate(), same as
    // for upgrade()/sell(). Re-validate everything else here so place() is
    // never the caller's only correctness check.
    const PlacementQuery q = validate(world, type, world_pos, /*available_atp=*/0xFFFFFFFFu);
    if (!q.valid()) return EntityId{};

    const u32 t = static_cast<u32>(type);
    const TowerStats& st = stats_[t][0];
    const Rect footprint = footprint_rect(world_pos, st.footprint_radius);

    priv::TowerRecord rec;
    rec.invested_atp = st.build_cost;
    if (st.blocks_flow) {
        snapshot_and_block(world.tissue(), footprint, rec);
        world.flow().mark_dirty(footprint);
    } else {
        rec.footprint = footprint;
    }

    entt::registry& registry = world.ecs().registry();
    const entt::entity e = registry.create();
    registry.emplace<comp::Transform>(e, comp::Transform{world_pos, 0.0f, 1.0f});

    comp::Tower tw;
    tw.type = type;
    tw.tier = 1;
    tw.range = st.range;
    tw.cooldown = 0.0f;
    tw.fire_interval = st.fire_interval;
    tw.ability_cooldown = 0.0f;
    registry.emplace<comp::Tower>(e, tw);
    registry.emplace<comp::Sprite>(e, comp::Sprite{Vec4{1.0f, 1.0f, 1.0f, 1.0f}, st.footprint_radius * 2.0f,
                                                    static_cast<u16>(t), /*layer=*/1});
    registry.emplace<priv::TowerRecord>(e, std::move(rec));

    const EntityId id = world.ecs().to_id(e);
    towers_.push_back(id);
    return id;
}

u8 TowerSystem::upgrade(sim::SimWorld& world, EntityId tower) {
    ensure_default_stats(*this);
    entt::registry& registry = world.ecs().registry();
    const entt::entity e = world.ecs().from_id(tower);
    if (!registry.valid(e) || !registry.all_of<comp::Tower>(e)) return 0;

    comp::Tower& tw = registry.get<comp::Tower>(e);
    if (tw.tier >= 3) return 0;

    const TowerStats& cur = stats_[static_cast<u32>(tw.type)][tw.tier - 1];
    const u8 next_tier = static_cast<u8>(tw.tier + 1);
    const TowerStats& next = stats_[static_cast<u32>(tw.type)][next_tier - 1];

    tw.tier = next_tier;
    tw.range = next.range;
    tw.fire_interval = next.fire_interval;

    if (auto* rec = registry.try_get<priv::TowerRecord>(e)) rec->invested_atp += cur.upgrade_cost;
    return next_tier;
}

u32 TowerSystem::sell(sim::SimWorld& world, EntityId tower) {
    ensure_default_stats(*this);
    entt::registry& registry = world.ecs().registry();
    const entt::entity e = world.ecs().from_id(tower);
    if (!registry.valid(e)) return 0;

    // Refund fraction: Economy.h (Wave 3A) owns EconomyConfig::refund_fraction
    // but TowerSystem has no reference to an Economy instance (the frozen
    // header gives sell() none), so this mirrors EconomyConfig's own default
    // (0.7) rather than inventing an unrelated number. The caller is expected
    // to actually credit the returned amount to its Economy.
    constexpr f32 kRefundFraction = 0.7f;

    u32 refund = 0;
    if (auto* rec = registry.try_get<priv::TowerRecord>(e)) {
        refund = static_cast<u32>(static_cast<f32>(rec->invested_atp) * kRefundFraction);
        if (registry.all_of<comp::Tower>(e)) {
            const comp::Tower& tw = registry.get<comp::Tower>(e);
            if (stats_[static_cast<u32>(tw.type)][0].blocks_flow && !rec->saved_walkable.empty()) {
                restore_footprint(world.tissue(), *rec);
                world.flow().mark_dirty(rec->footprint);
            }
        }
    }

    registry.destroy(e);
    towers_.erase(std::remove(towers_.begin(), towers_.end(), tower), towers_.end());
    return refund;
}

bool TowerSystem::trigger_ability(sim::SimWorld& world, EntityId tower) {
    ensure_default_stats(*this);
    entt::registry& registry = world.ecs().registry();
    const entt::entity e = world.ecs().from_id(tower);
    if (!registry.valid(e) || !registry.all_of<comp::Tower, comp::Transform>(e)) return false;

    comp::Tower& tw = registry.get<comp::Tower>(e);
    if (tw.ability_cooldown > 0.0f) return false;
    const TowerStats& st = stats_[static_cast<u32>(tw.type)][tw.tier - 1];
    if (st.ability_cooldown <= 0.0f) return false; // this type has no active ability

    const comp::Transform& tf = registry.get<comp::Transform>(e);

    switch (tw.type) {
    case TowerType::Neutrophil: {
        // Spawns a few short-lived micro-units that drift outward along the
        // local flow direction, and drops a timed NET slow zone. Both halves
        // of the "swarm response" mechanic on one ability, since the header
        // allows "alternatively/additionally".
        Vec2 dir = math::normalize_safe(world.flow().sample(tf.position));
        if (dir.x == 0.0f && dir.y == 0.0f) dir = Vec2{1.0f, 0.0f};
        for (int i = 0; i < 3; ++i) {
            const f32 jitter = world.rng().range_f(-0.5f, 0.5f);
            const f32 c = std::cos(jitter);
            const f32 s = std::sin(jitter);
            Vec2 vel{dir.x * c - dir.y * s, dir.x * s + dir.y * c};
            vel *= math::max(st.range, 1.0f);

            const entt::entity u = registry.create();
            registry.emplace<comp::Transform>(u, comp::Transform{tf.position, 0.0f, 0.4f});
            registry.emplace<comp::Velocity>(u, comp::Velocity{vel, math::length(vel)});
            registry.emplace<comp::Ephemeral>(u, comp::Ephemeral{1.5f, tower});
            registry.emplace<comp::Sprite>(u, comp::Sprite{Vec4{0.3f, 0.6f, 1.0f, 1.0f}, 0.3f, 100, 2});
        }

        const entt::entity net = registry.create();
        registry.emplace<comp::Transform>(net, comp::Transform{tf.position, 0.0f, 1.0f});
        registry.emplace<comp::Sprite>(net, comp::Sprite{Vec4{0.2f, 0.8f, 0.9f, 0.5f}, st.range, 101, 0});
        registry.emplace<priv::ActiveNet>(net, priv::ActiveNet{tf.position, st.range, 3.0f, tower});
        break;
    }
    case TowerType::MastCell: {
        sim::DamageField nova;
        nova.shape = sim::FieldShape::Circle;
        nova.origin = tf.position;
        nova.radius = st.range * 2.0f;
        nova.kill_rate = st.kill_rate * 8.0f;
        nova.falloff = 1.0f;
        nova.family_mask = st.family_mask;
        nova.lifetime = 0.2f; // one-shot: expires on its own after a couple ticks
        nova.owner = tower;
        world.damage().submit(nova);
        break;
    }
    case TowerType::ComplementCascade: {
        sim::DamageField chain;
        chain.shape = sim::FieldShape::Chain;
        chain.origin = tf.position;
        chain.radius = st.range;
        chain.kill_rate = st.kill_rate;
        chain.family_mask = st.family_mask;
        chain.lifetime = 0.2f;
        chain.owner = tower;
        world.damage().submit(chain);
        break;
    }
    default:
        return false; // no active ability defined for this type
    }

    tw.ability_cooldown = st.ability_cooldown;
    return true;
}

EntityId TowerSystem::find_target(const sim::SimWorld& world, Vec2 origin, f32 range, u8 family_mask,
                                  bool require_detect_hidden) const {
    const entt::registry& registry = world.ecs().registry();
    auto view = registry.view<const comp::NamedAgent, const comp::Transform, const comp::Health>();

    entt::entity best = entt::null;
    f32 best_d2 = range * range;
    for (auto e : view) {
        const comp::NamedAgent& agent = view.get<const comp::NamedAgent>(e);
        const u8 fam_bit = static_cast<u8>(1u << static_cast<u8>(agent.family));
        if ((family_mask & fam_bit) == 0) continue;

        const comp::Health& health = view.get<const comp::Health>(e);
        if (health.dead()) continue;

        if (const auto* brain = registry.try_get<comp::AiBrain>(e)) {
            if (brain->state == comp::AiState::Burrowed && !require_detect_hidden) continue;
        }

        const comp::Transform& tf = view.get<const comp::Transform>(e);
        const f32 d2 = math::length_sq(tf.position - origin);
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = e;
        }
    }
    return best == entt::null ? EntityId{} : world.ecs().to_id(best);
}

void TowerSystem::register_systems(sim::SimWorld& world) {
    ensure_default_stats(*this);
    sim::EcsWorld& ecs = world.ecs();

    ecs.add_system(sim::SystemPhase::PreUpdate, "tower_cooldowns", 20, &system_tower_cooldowns);

    ecs.add_system(sim::SystemPhase::Combat, "tower_macrophage", 0,
                   [this](sim::SystemContext& ctx) { system_macrophage(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_neutrophil", 1,
                   [this](sim::SystemContext& ctx) { system_neutrophil(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_dendritic", 2, &system_dendritic);
    ecs.add_system(sim::SystemPhase::Combat, "tower_cytotoxic_t", 3,
                   [this](sim::SystemContext& ctx) { system_cytotoxic_t(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_bcell", 4,
                   [this](sim::SystemContext& ctx) { system_bcell(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_nkcell", 5,
                   [this](sim::SystemContext& ctx) { system_nkcell(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_mastcell", 6,
                   [this](sim::SystemContext& ctx) { system_mastcell(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_complement", 7,
                   [this](sim::SystemContext& ctx) { system_complement(*this, ctx); });
    ecs.add_system(sim::SystemPhase::Combat, "tower_net_upkeep", 8, &system_net_upkeep);

    ecs.add_system(sim::SystemPhase::Movement, "tower_ephemeral_drift", 50, &system_ephemeral_drift);
}

} // namespace immune::game
