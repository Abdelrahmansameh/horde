// sim/scar/Scars.cpp — laying and tearing down collagen scars.
// Scars.h states the contract; this file is the carve, the reinforce and
// the sweep.
#include "sim/scar/Scars.h"

#include "core/Math.h"
#include "sim/CombatEvents.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/flowfield/FlowField.h"
#include "sim/flowfield/LaneConnectivity.h"
#include "sim/flowfield/RuntimeBlock.h"
#include "sim/swarm/Swarmers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace immune::sim {

namespace priv {

/// The tissue cells a live scar displaced. Private to this file for the same
/// reason the clot's footprint is private to ActiveAbilities.cpp: it owns a
/// vector, and comp/ is for plain trivially-copyable state.
struct ScarFootprint {
    CarvedFootprint carved;
};

} // namespace priv

namespace {

/// entity.frag shape id for a scar. 5 is the Fibrin Clot; the scar is the
/// next runtime block along.
constexpr u16 kScarShape = 6;

/// Collagen: a pale, slightly warm rose, kept opaque so it reads as ground
/// the horde cannot cross. The renderer spends the alpha on integrity.
constexpr Vec4 kScarTint{0.98f, 0.82f, 0.78f, 1.0f};

CombatEvent scar_event(CombatEventType type, const comp::Scar& scar, Vec2 origin, f32 rotation) {
    CombatEvent e;
    e.type = type;
    e.source = scar.source;
    e.visual_id = scar.visual_id;
    e.origin = origin;
    e.secondary = origin;
    e.direction = Vec2{std::cos(rotation), std::sin(rotation)};
    e.radius = scar.half_extents.x;
    e.magnitude = scar.half_extents.y;
    return e;
}

} // namespace

f32 scar_rotation(const FlowField& flow, Vec2 center, EntityId owner, f32 tilt) {
    const f32 across = across_flow_rotation(flow, center);
    if (tilt <= 0.0f) return across;
    // Hash the site's bits and the owner into a unit fraction. Float bits,
    // not a quantised position, so two sites a hair apart still draw
    // different tilts; the owner so two towers' walls through one spot do
    // not share one.
    u32 hx, hy;
    static_assert(sizeof(hx) == sizeof(center.x), "f32 is 32 bits");
    std::memcpy(&hx, &center.x, sizeof(hx));
    std::memcpy(&hy, &center.y, sizeof(hy));
    u32 h = hx * 0x9E3779B1u;
    h ^= hy + 0x7F4A7C15u + (h << 6) + (h >> 2);
    h ^= owner.value * 0x85EBCA6Bu + (h << 6) + (h >> 2);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    const f32 unit = static_cast<f32>(h >> 8) * (2.0f / 16777216.0f) - 1.0f;   // [-1, 1)
    return across + unit * tilt;
}

ScarDesc scar_desc_from_profile(const SwarmerProfile& pr) {
    ScarDesc d;
    d.half_extents = Vec2{math::max(pr.scar_half_length, 0.0f), math::max(pr.scar_half_width, 0.0f)};
    d.tilt = math::max(pr.scar_tilt, 0.0f);
    d.max_health = math::max(pr.scar_health, 1.0f);
    d.lifetime = math::max(pr.scar_lifetime, 0.0f);
    d.spacing = math::max(pr.scar_spacing, 0.0f);
    d.reinforce = math::max(pr.scar_reinforce, 0.0f);
    d.max_scars = pr.max_scars;
    d.source = pr.source;
    return d;
}

void ScarSystem::reset() { stats_ = ScarStats{}; }

EntityId ScarSystem::nearest(const SimWorld& world, Vec2 p, f32 radius, EntityId owner) const {
    const entt::registry& registry = world.ecs().registry();
    auto view = registry.view<const comp::Scar, const comp::Transform, const comp::Health>();
    entt::entity best = entt::null;
    f32 best_d2 = radius * radius;
    for (auto e : view) {
        if (view.get<const comp::Health>(e).dead()) continue;
        const comp::Scar& sc = view.get<const comp::Scar>(e);
        if (owner.valid() && sc.owner != owner) continue;
        const f32 d2 = math::length_sq(view.get<const comp::Transform>(e).position - p);
        // Ties go to the lower entity so the answer does not depend on pool order.
        if (d2 < best_d2 || (d2 == best_d2 && best != entt::null && e < best)) {
            best_d2 = d2;
            best = e;
        }
    }
    return best == entt::null ? EntityId{} : world.ecs().to_id(best);
}

bool ScarSystem::overlaps(const SimWorld& world, const Bar& bar) const {
    const entt::registry& registry = world.ecs().registry();
    auto view = registry.view<const comp::Scar, const comp::Transform, const comp::Health>();
    for (auto e : view) {
        if (view.get<const comp::Health>(e).dead()) continue;
        const comp::Scar& sc = view.get<const comp::Scar>(e);
        const comp::Transform& tf = view.get<const comp::Transform>(e);
        const Bar other{tf.position, sc.half_extents, tf.rotation};
        if (bars_overlap(bar, other)) return true;
    }
    return false;
}

u32 ScarSystem::count_owned(const SimWorld& world, EntityId owner) const {
    const entt::registry& registry = world.ecs().registry();
    auto view = registry.view<const comp::Scar, const comp::Health>();
    u32 n = 0;
    for (auto e : view) {
        if (view.get<const comp::Health>(e).dead()) continue;
        if (view.get<const comp::Scar>(e).owner == owner) ++n;
    }
    return n;
}

EntityId ScarSystem::build(SimWorld& world, const ScarDesc& desc, ScarBuildResult* result,
                           CombatEventSink* events) {
    const auto finish = [&](ScarBuildResult r, EntityId id) {
        if (result) *result = r;
        return id;
    };

    entt::registry& registry = world.ecs().registry();

    // Lays collagen on an existing scar instead of starting a new one. The
    // reinforced scar's ceiling is its own max: reinforcing never grows a
    // wall past what a fresh one stands with.
    const auto reinforce = [&](EntityId target) -> EntityId {
        if (!target.valid() || desc.reinforce <= 0.0f) return finish(ScarBuildResult::Dropped, EntityId{});
        const entt::entity e = world.ecs().from_id(target);
        if (!registry.valid(e) || !registry.all_of<comp::Health>(e)) {
            return finish(ScarBuildResult::Dropped, EntityId{});
        }
        comp::Health& hp = registry.get<comp::Health>(e);
        hp.current = math::min(hp.max, hp.current + desc.reinforce);
        ++stats_.reinforced_total;
        return finish(ScarBuildResult::Reinforced, target);
    };

    // In the way: a live scar inside the spacing takes the collagen.
    if (desc.spacing > 0.0f) {
        const EntityId blocking = nearest(world, desc.center, desc.spacing);
        if (blocking.valid()) return reinforce(blocking);
    }
    // At the cap: the owner's nearest scar takes it, wherever it stands.
    if (desc.max_scars > 0 && desc.owner.valid() && count_owned(world, desc.owner) >= desc.max_scars) {
        const EntityId own = nearest(world, desc.center, 1e9f, desc.owner);
        return reinforce(own);
    }

    TissueMask& mask = world.tissue();
    if (mask.width() <= 0 || mask.height() <= 0) {
        ++stats_.refused_total;
        return finish(ScarBuildResult::OffTissue, EntityId{});
    }
    {
        const IVec2 c = mask.world_to_cell(desc.center);
        if (!mask.walkable(c.x, c.y)) {
            ++stats_.refused_total;
            return finish(ScarBuildResult::OffTissue, EntityId{});
        }
    }
    if (desc.half_extents.x <= 0.0f || desc.half_extents.y <= 0.0f) {
        ++stats_.refused_total;
        return finish(ScarBuildResult::NoCells, EntityId{});
    }

    Bar bar;
    bar.center = desc.center;
    bar.half_extents = desc.half_extents;
    bar.rotation = scar_rotation(world.flow(), desc.center, desc.owner, desc.tilt);

    // A wall that seals the lane is refused outright, never laid and lived
    // with: the incremental rebake cannot represent a dead-end pocket
    // (LaneConnectivity.h), and the horde would bunch at an invisible line
    // upstream instead of pressing against the collagen.
    const Rect bounds = bar.bounds();
    const bool sever = would_sever_lane(mask, world.flow(), bounds, [&](i32 x, i32 y) {
        return bar.contains(mask.cell_to_world(x, y));
    });
    if (sever) {
        ++stats_.refused_total;
        return finish(ScarBuildResult::WouldSever, EntityId{});
    }

    priv::ScarFootprint fp;
    fp.carved = carve_block(mask, world.flow(), bar);
    if (fp.carved.empty()) {
        ++stats_.refused_total;
        return finish(ScarBuildResult::NoCells, EntityId{});
    }

    const entt::entity e = registry.create();
    registry.emplace<comp::Transform>(e, comp::Transform{bar.center, bar.rotation, 1.0f});
    comp::Scar scar;
    scar.half_extents = bar.half_extents;
    scar.duration = desc.lifetime;
    scar.remaining = desc.lifetime;
    scar.owner = desc.owner;
    scar.source = desc.source;
    scar.visual_id = desc.visual_id;
    registry.emplace<comp::Scar>(e, scar);
    // Armor stays 0 for the same reason a tower's does: toughness is more
    // integrity, not a per-bite floor, so a lone virus still matters.
    registry.emplace<comp::Health>(e, comp::Health{desc.max_health, desc.max_health, 0.0f});
    // Sprite size is the quad's full width; the renderer squashes the shape
    // to the bar's aspect from comp::Scar (entity.frag shape 6).
    registry.emplace<comp::Sprite>(e, comp::Sprite{kScarTint, bar.half_extents.x * 2.0f, kScarShape, 1});
    registry.emplace<priv::ScarFootprint>(e, std::move(fp));

    ++stats_.built_total;
    if (events) events->push(scar_event(CombatEventType::ScarBuilt, scar, bar.center, bar.rotation));
    return finish(ScarBuildResult::Built, world.ecs().to_id(e));
}

void ScarSystem::upkeep(SimWorld& world, f32 dt, CombatEventSink* events) {
    entt::registry& registry = world.ecs().registry();
    static thread_local std::vector<entt::entity> gone;
    gone.clear();

    auto view = registry.view<comp::Scar, const comp::Health>();
    u32 live = 0;
    for (auto e : view) {
        comp::Scar& sc = view.get<comp::Scar>(e);
        if (sc.duration > 0.0f) sc.remaining -= dt;
        const bool chewed = view.get<const comp::Health>(e).dead();
        const bool expired = sc.duration > 0.0f && sc.remaining <= 0.0f;
        if (chewed || expired) {
            gone.push_back(e);
        } else {
            ++live;
        }
    }
    stats_.live = live;
    if (gone.empty()) return;

    // Sorted so the restores, and the events they raise, come out in an
    // order that does not depend on EnTT's pool layout.
    std::sort(gone.begin(), gone.end());
    for (entt::entity e : gone) {
        const comp::Scar sc = registry.get<comp::Scar>(e);
        const bool chewed = registry.get<comp::Health>(e).dead();
        Vec2 origin{0.0f, 0.0f};
        f32 rotation = 0.0f;
        if (const auto* tf = registry.try_get<comp::Transform>(e)) {
            origin = tf->position;
            rotation = tf->rotation;
        }
        if (const auto* fp = registry.try_get<priv::ScarFootprint>(e)) {
            restore_block(world.tissue(), world.flow(), fp->carved);
        }
        if (chewed) ++stats_.lost_total;
        else ++stats_.dissolved_total;
        if (events) {
            CombatEvent ev = scar_event(CombatEventType::ScarDestroyed, sc, origin, rotation);
            ev.magnitude = chewed ? 1.0f : 0.0f;
            events->push(ev);
        }
        registry.destroy(e);
    }
}

} // namespace immune::sim
