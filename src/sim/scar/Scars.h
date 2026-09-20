// sim/scar/Scars.h — collagen scars: the walls a Fibroblast's builders lay.
// Owner: the fibroblast wave.
//
// WHAT A SCAR IS
// A runtime block (sim/flowfield/RuntimeBlock.h): an oriented bar carved out
// of the tissue mask, laid ACROSS the local flow so it stands as a dam rather
// than a divider. The horde has to go round it -- the flow field reroutes,
// the chaff containment pass will not let an agent step into it -- and the
// player's own swarmers walk straight through it, because swarmers only ever
// read the SDF and the SDF never hears about runtime blocks. That asymmetry
// is the whole design: a scar is a wall for pathogens and nothing else.
//
// Unlike the Fibrin Clot it is not on a clock. It is an ECS entity with a
// Health, and SimWorld lists it to the hostile pass as a host exactly like a
// tower: viruses latch along its faces and feed, bacteria burn it from
// inside their aura. At zero integrity upkeep() hands its cells back to the
// mask, marks the flow dirty, destroys the entity and announces it. A
// profile may also give scars a lifetime, in which case they dissolve on
// their own as well; shipped, they stand until chewed down.
//
// WHERE IT COMES FROM
// A builder swarmer (SwarmerKind::Builder) reaching its site records a
// SwarmerBuild; SimWorld::apply_swarmer_effects hands it to build() with the
// scar numbers read off the unit's profile. build() may refuse: a bar that
// would seal the lane is never laid (LaneConnectivity.h, the same rule the
// clot and, once, tower footprints followed), and one arriving inside the
// spacing of a live scar reinforces that scar instead -- collagen is laid on
// collagen -- rather than stacking a second wall on the first. The owner's
// live-scar cap is honoured the same way.
//
// COST. A build is one BFS window (would_sever_lane) plus one mask stamp,
// a few times a second across the whole board at most. upkeep() is a view
// walk over the handful of live scars. Nothing here is per-agent.
//
// DETERMINISM. Entities are created and destroyed in the order requests
// arrive (the swarmer store's index order) and in sorted entity order
// respectively, never in EnTT pool order, so the events -- and the VFX RNG
// draws they cause -- come out the same every run.
#pragma once

#include "core/Types.h"
#include "sim/ecs/EcsWorld.h"

#include <vector>

namespace immune::sim {

class SimWorld;
class CombatEventSink;
class FlowField;
struct SwarmerProfile;
struct Bar;

/// One request to lay a scar. The bar's orientation is not part of it: it is
/// read off the flow field at the site when the scar goes down -- across the
/// local arrows, plus a random tilt of up to `tilt` (scar_rotation below).
struct ScarDesc {
    Vec2 center{0.0f, 0.0f};
    Vec2 half_extents{5.0f, 0.9f};
    /// Max random tilt off the perpendicular, radians, either way. 0 = square.
    f32 tilt = 0.0f;
    f32 max_health = 250.0f;
    /// Seconds before it dissolves on its own; 0 = permanent.
    f32 lifetime = 0.0f;
    /// No live scar centre may be closer than this; a request inside the
    /// spacing reinforces that scar instead (see `reinforce`).
    f32 spacing = 6.0f;
    /// Hit points added to the scar that blocked this request. 0 = the
    /// request is simply dropped.
    f32 reinforce = 60.0f;
    /// The owner's live-scar cap; 0 = unlimited. At the cap the request
    /// reinforces the owner's nearest scar instead of building.
    u32 max_scars = 4;
    EntityId owner{};
    TowerType source = TowerType::Fibroblast;
    u16 visual_id = 0;
};

/// The ScarDesc a builder of this profile lays. One place for the mapping so
/// the site picker (game/towers) and the build (SimWorld) read the same
/// numbers.
ScarDesc scar_desc_from_profile(const SwarmerProfile& profile);

/// The rotation a scar laid at `center` gets: across the flow there, tilted
/// by a value in [-tilt, tilt] hashed off the site and the owner. A pure
/// function of its inputs and no RNG stream, so the site picker at release
/// and the build on arrival agree on the bar (the sever check has to be run
/// against the bar that will actually be laid), and so a replay gets the
/// same walls.
f32 scar_rotation(const FlowField& flow, Vec2 center, EntityId owner, f32 tilt);

enum class ScarBuildResult : u8 {
    Built = 0,
    Reinforced,     ///< Another scar was in the way (or the cap hit) and took the collagen.
    OffTissue,      ///< The site is not on walkable tissue.
    WouldSever,     ///< The bar would seal the lane; nothing was laid.
    NoCells,        ///< The bar covered no walkable cell (it lay on rock).
    Dropped,        ///< Blocked, and the profile gives nothing to reinforce with.
};

struct ScarStats {
    u32 live = 0;
    u64 built_total = 0;
    u64 reinforced_total = 0;
    u64 lost_total = 0;        ///< Chewed down by the horde.
    u64 dissolved_total = 0;   ///< Ran out their lifetime.
    u64 refused_total = 0;     ///< Would have sealed a lane, or lay on rock.
};

class ScarSystem {
public:
    /// Forgets every counter. init() on the world calls this; the entities
    /// themselves go with the registry reset.
    void reset();

    /// Lays a scar (or reinforces the one in the way). The bar is carved out
    /// of the world's tissue mask and the flow marked dirty; the entity
    /// carries comp::Transform, comp::Scar, comp::Health and comp::Sprite.
    /// Returns the built scar's id on Built, the reinforced scar's id on
    /// Reinforced, and an invalid id otherwise. `events` may be null.
    EntityId build(SimWorld& world, const ScarDesc& desc, ScarBuildResult* result,
                   CombatEventSink* events);

    /// Nearest live scar whose centre is within `radius` of `p`, optionally
    /// only those `owner` laid. Invalid if none.
    EntityId nearest(const SimWorld& world, Vec2 p, f32 radius, EntityId owner = EntityId{}) const;

    /// Whether `bar` physically overlaps any live scar's own bar. Distance
    /// between centers (see `nearest`, `spacing`) is not enough to answer
    /// this: a long scar's far end can sit well past `spacing` from its
    /// center while still standing under a bar laid there.
    bool overlaps(const SimWorld& world, const Bar& bar) const;

    /// Live scars `owner` laid.
    u32 count_owned(const SimWorld& world, EntityId owner) const;

    /// Tears down every scar at zero integrity or past its lifetime: cells
    /// back to the mask, flow marked dirty, entity destroyed, one event
    /// raised. Run once per tick after the hostile damage has landed.
    void upkeep(SimWorld& world, f32 dt, CombatEventSink* events);

    const ScarStats& stats() const { return stats_; }

private:
    ScarStats stats_{};
};

} // namespace immune::sim
