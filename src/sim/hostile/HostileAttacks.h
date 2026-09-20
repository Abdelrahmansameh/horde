// sim/hostile/HostileAttacks.h — the horde fighting back: pathogens against
// towers and swarmers.
// Owner: the enemy-attacks wave.
//
// WHAT THIS IS
// Until this layer existed the horde was a river the player carved and the
// river never carved back: a tower sat in the lane forever, a swarmer only
// ever died of old age. Now the two pathogen families each have a way of
// killing the player's cells, expressed as data on the family and run here:
//
//   Virus     LATCH.  A virus that touches a tower or a swarmer grabs on and
//                     rides it, feeding at latch_dps until the host dies (or
//                     the virus does). It stops walking the lane: the chaff
//                     kernel freezes a chaff_flags::kLatched agent and this
//                     pass owns its position, planting it on the host's
//                     membrane and carrying it wherever the host goes. A
//                     host can carry only so many (latch_cap_*); the rest of
//                     the crowd walks on.
//   Bacteria  AURA.   A bacterium burns every friendly whose body is inside
//                     aura_radius of its centre, at aura_dps, continuously.
//                     No state, no target, nothing to grab: it damages by
//                     being near, and a lane full of them is a lane a
//                     swarmer cannot loiter in.
//
// Either family may carry either attack -- they are per-family numbers in
// HostileFamilyParams, not code paths keyed on the family id -- and a family
// with both at zero is exactly the old harmless horde. Towers and swarmers
// are both hosts and both aura victims; "friendly" below means either. A
// collagen scar (sim/scar) is listed as a tower-shaped host too, with one
// difference: it is a BAR, not a disc. FriendlyTower::half_extents says so,
// and a bar host is measured to its nearest face, latched along that face
// and burned wherever an aura reaches any part of it -- so a wall laid
// across a lane is eaten from the side the horde presses on.
//
// WHY IT IS ITS OWN LAYER, AND WHY IT ITERATES FRIENDLIES
// Ten thousand pathogens hunting for something to bite would be ten thousand
// hash queries a tick. There are never more than a few thousand swarmers and
// a few dozen towers, so the pass runs the other way round: every FRIENDLY
// queries the chaff hash once around itself, finds the pathogens in reach,
// and takes the aura damage and the new passengers from what it found. That
// is the same direction the swarmer BODIES pass already walks, at the same
// cost, and it is independent of total chaff count. Latched passengers are
// then one serial walk of the chaff store in index order (a flag test per
// agent, real work only for the latched few).
//
// The one thing the chaff kernel knows about any of this is the kLatched
// bit, which it treats exactly like kHidden. That keeps the hot path hot.
//
// TICK PLACEMENT (SimWorld::tick, step 4c'')
// After the swarmer update and after apply_swarmer_effects(), so passengers
// are planted on this tick's swarmer positions and a unit this pass kills
// has already had its last say; before chaff compaction, so a virus killed
// while latched is compacted in the same pass as everything else. A swarmer
// killed here carries kPendingKill into the next swarmer update, which skips
// it and compacts it. Tower damage is not applied here at all: the pass
// accumulates it into FriendlyTowerList::damage and SimWorld lands it on the
// ECS afterwards (apply_hostile_effects), the same arrangement the swarmer
// kernel has with NamedTargetList.
//
// DETERMINISM
// Serial, index order, no RNG. A passenger's spot on its host's membrane is
// derived from its own generation, so it is stable for its life and needs no
// per-agent offset streams; the lunge toward that spot is a pure function of
// the passenger's position, the host's, and dt. Hash walks are capped,
// nearest cell first, in CSR order, which is a pure function of positions.
//
// RIDING. Every tick a passenger is first carried by the host's last-tick
// motion (its own stored velocity IS the host's velocity, see pass 1), then
// lunges from there toward its spot on the membrane. That is what lets a
// unit on the move keep its passengers on its skin instead of towing them
// on a string, without a per-passenger offset stream.
#pragma once

#include "core/Types.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/spatial/SpatialHash.h"
#include "sim/swarm/Swarmers.h"

#include <vector>

namespace immune::sim {

class CombatEventSink;
class TissueMask;

/// One family's way of hurting the player's cells. All zero = harmless.
struct HostileFamilyParams {
    /// Hit points per second one latched agent takes off its host. 0 means
    /// this family never latches.
    f32 latch_dps = 0.0f;
    /// Extra reach past body contact (pathogen radius + host body) at which a
    /// latch happens. Small: a latch is a touch, not a lunge.
    f32 latch_reach = 0.4f;
    /// Most passengers one swarmer / one tower will carry at once. Beyond
    /// this the rest of the crowd walks on by.
    u32 latch_cap_swarmer = 4;
    u32 latch_cap_tower = 24;
    /// ...and one scar (sim/scar): a wall is long, so it carries more.
    u32 latch_cap_scar = 48;
    /// THE LUNGE. A latching agent is not teleported to its spot on the
    /// host's membrane; it goes there, at full `latch_speed` from the first
    /// tick (no ease-in: the grab is a jerk), and brakes hard as it arrives:
    /// inside `latch_ease_distance` of its spot the speed is scaled by
    /// (distance / ease_distance) ^ latch_ease_power, so a high power is a
    /// very late, very heavy ease-out. World units/sec, world units, and a
    /// bare exponent. A settle floor (kLatchSettle in the .cpp) guarantees it
    /// still closes the last sliver rather than hanging a hair off the host.
    f32 latch_speed = 30.0f;
    f32 latch_ease_distance = 1.2f;
    f32 latch_ease_power = 3.0f;
    /// Hit points per second dealt to every friendly whose body is inside
    /// `aura_radius` of this agent's centre. 0 means no aura.
    f32 aura_dps = 0.0f;
    f32 aura_radius = 3.0f;
};

struct HostileTuning {
    HostileFamilyParams family[kFamilyCount]{};
    /// Master switch. Off, the pass does nothing at all and the world is the
    /// pre-hostile world; a SimDesc built without a game config ships it OFF
    /// with every family harmless, so a bare test world is unchanged.
    bool enabled = false;
    /// Chaff one friendly will inspect per tick in its hash walk, nearest
    /// cells first (see walk_circle in the .cpp). Truncating in that order
    /// keeps the walk deterministic and drops only the far candidates; the
    /// cap only bites in a jam, where the friendly is not surviving the tick
    /// anyway. Sized so a tower buried in a dense wave still sees its whole
    /// own cell and the ring around it.
    u32 max_attackers = 128;
    /// PathogenLatch events raised per tick, out of the shared sink.
    u32 max_latch_events = 64;
};

/// One tower as this pass sees it. SimWorld rebuilds the list every tick from
/// the ECS (towers with a Health), sorted by id so a passenger's held
/// EntityId resolves with a binary search.
struct FriendlyTower {
    EntityId id{};
    Vec2 position{0.0f, 0.0f};
    /// Body radius: the footprint, which is also the drawn sprite's radius.
    /// For a bar host (below) this is the bar's bounding radius, so one
    /// query circle around `position` still covers the whole body.
    f32 radius = 1.0f;
    f32 health = 1.0f;
    /// Which tower type, for the events this pass raises.
    TowerType type = TowerType::Count;
    u16 visual_id = 0;
    /// A BAR host -- a scar -- when `half_extents.x` > 0: `.x` along
    /// `rotation`, `.y` across it, about `position`. Zero means a disc of
    /// `radius`, which is every tower.
    Vec2 half_extents{0.0f, 0.0f};
    f32 rotation = 0.0f;
    bool is_bar() const { return half_extents.x > 0.0f && half_extents.y > 0.0f; }
};

struct FriendlyTowerList {
    std::vector<FriendlyTower> items;   ///< Sorted by id.value ascending.
    /// Hit points to take off items[i] this tick. SimWorld applies it.
    std::vector<f32> damage;
    /// Passengers riding items[i], filled by the pass. Exposed so the HUD can
    /// say "under attack" without a second walk of the chaff store.
    std::vector<u32> passengers;

    static constexpr usize npos = static_cast<usize>(-1);
    void clear() { items.clear(); damage.clear(); passengers.clear(); }
    void add(const FriendlyTower& t) {
        items.push_back(t);
        damage.push_back(0.0f);
        passengers.push_back(0u);
    }
    void sort();
    usize find(EntityId id) const;
};

struct HostileStats {
    u32 latched = 0;          ///< Passengers riding a host at the end of the tick.
    u32 latches_new = 0;      ///< Of those, grabbed on this tick.
    u32 released = 0;         ///< Passengers whose host went away this tick.
    u32 aura_hits = 0;        ///< (friendly, bacterium) pairs inside an aura this tick.
    u32 swarmers_killed = 0;
    f32 swarmer_damage = 0.0f;
    f32 tower_damage = 0.0f;  ///< Queued against towers; SimWorld lands it.
};

class HostileSystem {
public:
    void set_tuning(const HostileTuning& t) { tuning_ = t; }
    const HostileTuning& tuning() const { return tuning_; }

    /// Pathogen body radius per family, for the contact distances. Same
    /// hand-over SwarmerSystem::set_chaff_radii makes, for the same reason:
    /// the chaff store carries no radius stream.
    void set_chaff_radii(const f32* radii, usize count);

    /// One tick. `hash` is the chaff hash built at the top of the tick. `mask`
    /// is the tissue walkability mask a latched passenger's membrane spot is
    /// kept inside of: a host's own position is always on walkable ground, so
    /// any spot that strays off it (a ring point that lands past a host
    /// parked at the tissue/vessel border, a bar's far face beyond the edge)
    /// is pulled back onto the host rather than left to render off the map.
    /// A default-constructed mask (width 0) disables the check, same as an
    /// unbaked field everywhere else in the sim.
    /// Mutates: chaff positions/velocities/flags/host streams of latched
    /// agents; swarmer health and kill flags; `towers.damage` and
    /// `towers.passengers`. `events` may be null and never changes the result.
    HostileStats update(ChaffBuffers& chaff, const SpatialHash& hash, SwarmerBuffers& swarmers,
                        FriendlyTowerList& towers, const TissueMask& mask, f32 dt,
                        CombatEventSink* events);

    const HostileStats& last_stats() const { return last_; }

private:
    HostileTuning tuning_{};
    HostileStats last_{};
    f32 chaff_radius_[kFamilyCount]{};
    /// Passengers per swarmer slot this tick. Sized to the swarmer store's
    /// capacity on first use and never after.
    std::vector<u32> swarmer_passengers_;
    /// Per-chaff visit stamp for a bar host's walk, which is several circle
    /// walks along the bar that overlap: a pathogen inside two of them is
    /// still one attacker. Sized to the chaff store's capacity on first use.
    std::vector<u32> visit_stamp_;
    u32 visit_gen_ = 0;
};

} // namespace immune::sim
