// game/abilities/ActiveAbilities.cpp — implementation of the frozen
// ActiveAbilities.h contract. Owner: Wave 4C.
#include "game/abilities/ActiveAbilities.h"

#include "game/abilities/AbilityConfigApply.h"

#include "core/Math.h"
#include "sim/SimWorld.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/LaneConnectivity.h"

#include <cmath>
#include <utility>
#include <vector>

namespace immune::game {

namespace priv {

/// The tissue cells a live Fibrin Clot carved, so its dissolve can hand them
/// back. Private to this file for the same reason TowerSystem.cpp keeps its
/// TowerRecord out of Components.h: it owns a vector, and comp/ is for plain
/// trivially-copyable state. The public half (geometry, clock) is
/// comp::Barrier, which the renderer and tower placement read.
///
/// Only cells that were WALKABLE when the clot dropped are recorded, and only
/// those are restored. A cell the level authored solid, or one under a tower
/// placed before the clot, is never touched in either direction -- so a clot
/// laid across a tower cannot un-block that tower's footprint when it goes.
struct ClotFootprint {
    std::vector<u32> carved_cells;   ///< TissueMask::index() of each cell.
    Rect dirty_bounds{};             ///< What to hand FlowField::mark_dirty.
};

} // namespace priv

namespace {

/// Oriented-box membership in the bar's own frame. `cs`/`sn` are the bar's
/// rotation, `he` its half extents (x along the bar).
bool inside_bar(Vec2 p, Vec2 center, Vec2 he, f32 cs, f32 sn) {
    const Vec2 d = p - center;
    const f32 lx = d.x * cs + d.y * sn;
    const f32 ly = -d.x * sn + d.y * cs;
    return std::fabs(lx) <= he.x && std::fabs(ly) <= he.y;
}

/// World-space AABB of a rotated bar: the support along each axis.
Rect bar_bounds(Vec2 center, Vec2 he, f32 cs, f32 sn) {
    const f32 bx = std::fabs(cs) * he.x + std::fabs(sn) * he.y;
    const f32 by = std::fabs(sn) * he.x + std::fabs(cs) * he.y;
    return Rect{center - Vec2{bx, by}, center + Vec2{bx, by}};
}

/// Decrements every live clot, and when one runs out hands its cells back to
/// the tissue mask and marks the flow field dirty over them, exactly as
/// TowerSystem::sell does for a footprint. Lives in the sim tick (Combat
/// phase, after the tower systems) rather than in ActiveAbilitySystem::tick
/// so the restore is deterministic with respect to everything else in the
/// tick and does not depend on the ability system being stepped at all --
/// the same argument TowerSystem.cpp makes for the Neutrophil's ActiveNet.
void system_clot_upkeep(sim::SystemContext& ctx) {
    static thread_local std::vector<entt::entity> expired;
    expired.clear();
    auto view = ctx.registry.view<sim::comp::Barrier>();
    for (auto e : view) {
        sim::comp::Barrier& b = view.get<sim::comp::Barrier>(e);
        b.remaining -= ctx.dt;
        if (b.remaining <= 0.0f) expired.push_back(e);
    }
    for (entt::entity e : expired) {
        if (const auto* fp = ctx.registry.try_get<priv::ClotFootprint>(e)) {
            sim::TissueMask& mask = ctx.world.tissue();
            const u32 w = static_cast<u32>(mask.width());
            for (u32 idx : fp->carved_cells) {
                mask.set_walkable(static_cast<i32>(idx % w), static_cast<i32>(idx / w), true);
            }
            if (!fp->carved_cells.empty()) ctx.world.flow().mark_dirty(fp->dirty_bounds);
        }
        ctx.registry.destroy(e);
    }
}

} // namespace

void ActiveAbilitySystem::load_defaults() {
    // Names are identity, not tuning, so they stay here; every number comes
    // from assets/config/abilities.json (or, until one is applied, from the
    // DESIGN.md §5.6 values seeded in AbilityConfigApply.cpp).
    static const char* kNames[kAbilityCount] = {
        "Complement Cascade Burst",
        "Histamine Flare",
        "Fever Response",
        "Fibrin Clot",
    };
    const AbilityConfig& cfg = ability_config();
    for (u32 i = 0; i < kAbilityCount; ++i) {
        AbilityDef& d = defs_[i];
        const AbilityTuning& t = cfg.ability[i];
        d.name = kNames[i];
        d.cooldown_seconds = t.cooldown_seconds;
        d.radius = t.radius;
        d.kill_rate = t.kill_rate;
        d.field_duration = t.field_duration;
        d.fever_cooldown_relief = t.fever_cooldown_relief;
        d.barrier_half_length = t.barrier_half_length;
        d.barrier_half_width = t.barrier_half_width;
    }
    for (f32& r : cooldown_remaining_) r = 0.0f;
}

void ActiveAbilitySystem::register_systems(sim::SimWorld& world) {
    // Sort key 10: after tower_net_upkeep (8) and marked_upkeep (9), so the
    // tower systems' Combat-phase order is untouched.
    world.ecs().add_system(sim::SystemPhase::Combat, "ability_clot_upkeep", 10, &system_clot_upkeep);
}

void ActiveAbilitySystem::tick(f32 dt) {
    for (f32& r : cooldown_remaining_) {
        if (r > 0.0f) r = math::max(0.0f, r - dt);
    }
}

bool ActiveAbilitySystem::ready(AbilityId id) const {
    return cooldown_remaining_[static_cast<u32>(id)] <= 0.0f;
}

AbilityStatus ActiveAbilitySystem::status(AbilityId id) const {
    const u32 i = static_cast<u32>(id);
    AbilityStatus s;
    s.cooldown_remaining = cooldown_remaining_[i];
    s.cooldown_total = defs_[i].cooldown_seconds;
    s.ready = s.cooldown_remaining <= 0.0f;
    return s;
}

bool ActiveAbilitySystem::cast(sim::SimWorld& world, AbilityId id, Vec2 target_point) {
    if (!ready(id)) return false;
    const u32 i = static_cast<u32>(id);
    const AbilityDef& d = defs_[i];

    switch (id) {
        case AbilityId::ComplementCascadeBurst: {
            // Resolves through the existing Chain-shape damage-field contract
            // -- the same jump-through-dense-clusters resolution a Complement
            // Cascade tower uses, just triggered once, globally, at a point
            // rather than continuously from a placed tower. A short positive
            // lifetime (not <=0) marks this as one-shot: DamageSystem drops it
            // after clear_transient() rather than expecting a per-tick refresh.
            sim::DamageField field;
            field.shape = sim::FieldShape::Chain;
            field.origin = target_point;
            field.radius = d.radius;
            field.kill_rate = d.kill_rate;
            field.lifetime = 0.1f;
            world.damage().submit(field);
            break;
        }
        case AbilityId::HistamineFlare: {
            sim::DamageField field;
            field.shape = sim::FieldShape::Circle;
            field.origin = target_point;
            field.radius = d.radius;
            field.kill_rate = d.kill_rate;
            field.falloff = 1.0f; // soft-edged nova, not a hard cutoff
            field.lifetime = d.field_duration;
            world.damage().submit(field);
            break;
        }
        case AbilityId::FeverResponse: {
            // No damage-field equivalent exists for "buff every tower" -- this
            // is the one ability that reaches into the ECS directly. An
            // instant cooldown-relief burst (read/write the already-public
            // comp::Tower.cooldown) was chosen deliberately over a sustained
            // rate multiplier: it needs no revert bookkeeping across ticks,
            // so there's no risk of a buff window silently outliving its
            // intended duration if something goes wrong elsewhere.
            auto view = world.ecs().registry().view<sim::comp::Tower>();
            for (auto e : view) {
                sim::comp::Tower& t = view.get<sim::comp::Tower>(e);
                t.cooldown = math::max(0.0f, t.cooldown - d.fever_cooldown_relief);
            }
            break;
        }
        case AbilityId::FibrinClot: {
            // A temporary tower footprint, in effect: carve the bar out of the
            // tissue mask and let the flow field reroute around it. Chaff
            // collide with it for free (ChaffSystem's contain_to_tissue reads
            // the mask), tower placement refuses it (validate checks
            // comp::Barrier), and the SDF -- which is never rebaked at runtime,
            // for the reason ChaffSystem.cpp gives -- is left alone, exactly
            // as a tower leaves it.
            //
            // ORIENTATION is the one thing not fixed: the bar lies ACROSS the
            // local flow, so one click drops it as a dam rather than a
            // divider. A point with no flow (off the baked field) falls back
            // to a horizontal bar, but such a point is rejected below anyway.
            sim::TissueMask& mask = world.tissue();
            if (mask.width() <= 0 || mask.height() <= 0) return false;
            if (world.sdf().sample(target_point) <= 0.0f) return false;   // not on tissue

            const Vec2 he{math::max(d.barrier_half_length, 0.0f),
                          math::max(d.barrier_half_width, 0.0f)};
            if (he.x <= 0.0f || he.y <= 0.0f) return false;

            const Vec2 flow_dir = math::normalize_safe(world.flow().sample(target_point));
            Vec2 along{1.0f, 0.0f};
            if (flow_dir.x != 0.0f || flow_dir.y != 0.0f) along = Vec2{-flow_dir.y, flow_dir.x};
            const f32 rotation = std::atan2(along.y, along.x);
            const f32 cs = std::cos(rotation);
            const f32 sn = std::sin(rotation);
            const Rect bounds = bar_bounds(target_point, he, cs, sn);

            // Same rule as a tower: a block that seals the local lane is
            // refused outright, not allowed and lived with. Temporary or not,
            // the incremental rebake cannot represent a dead-end pocket
            // (LaneConnectivity.h), and the horde would bunch at an invisible
            // line upstream of the clot instead of pressing against it.
            const bool sever = sim::would_sever_lane(mask, world.flow(), bounds, [&](i32 x, i32 y) {
                return inside_bar(mask.cell_to_world(x, y), target_point, he, cs, sn);
            });
            if (sever) return false;

            // Carve. Cell-centre membership, the convention every other mask
            // stamp uses (ObstacleRaster.h), padded by a cell so a bar whose
            // edge falls mid-cell still tests the cells it grazes.
            priv::ClotFootprint fp;
            const IVec2 c0 = mask.world_to_cell(bounds.min);
            const IVec2 c1 = mask.world_to_cell(bounds.max);
            for (i32 y = c0.y - 1; y <= c1.y + 1; ++y) {
                for (i32 x = c0.x - 1; x <= c1.x + 1; ++x) {
                    if (!mask.walkable(x, y)) continue;
                    if (!inside_bar(mask.cell_to_world(x, y), target_point, he, cs, sn)) continue;
                    mask.set_walkable(x, y, false);
                    fp.carved_cells.push_back(static_cast<u32>(mask.index(x, y)));
                }
            }
            fp.dirty_bounds = bounds;
            if (!fp.carved_cells.empty()) world.flow().mark_dirty(bounds);

            entt::registry& registry = world.ecs().registry();
            const entt::entity e = registry.create();
            registry.emplace<sim::comp::Transform>(e, sim::comp::Transform{target_point, rotation, 1.0f});
            sim::comp::Barrier barrier;
            barrier.half_extents = he;
            barrier.remaining = math::max(d.field_duration, 0.0f);
            barrier.duration = barrier.remaining;
            registry.emplace<sim::comp::Barrier>(e, barrier);
            registry.emplace<priv::ClotFootprint>(e, std::move(fp));
            // Sprite size is the quad's full width; the renderer squashes the
            // shape to the bar's aspect from comp::Barrier (entity.frag shape
            // 5). Fibrin's pale straw colour, kept opaque so it reads as a
            // solid the horde cannot cross, unlike the translucent NET.
            registry.emplace<sim::comp::Sprite>(
                e, sim::comp::Sprite{Vec4{0.96f, 0.86f, 0.62f, 1.0f}, he.x * 2.0f, 5, 1});
            break;
        }
        case AbilityId::Count:
            return false;
    }

    cooldown_remaining_[i] = d.cooldown_seconds;
    return true;
}

const char* ability_name(AbilityId id) {
    switch (id) {
        case AbilityId::ComplementCascadeBurst: return "Complement Cascade Burst";
        case AbilityId::HistamineFlare: return "Histamine Flare";
        case AbilityId::FeverResponse: return "Fever Response";
        case AbilityId::FibrinClot: return "Fibrin Clot";
        case AbilityId::Count: break;
    }
    return "?";
}

} // namespace immune::game
