// sim/CombatEvents.h — the one-way sim -> VFX channel. FROZEN CONTRACT.
// Owner: Wave 6A (producer side), Wave 6B (consumer side).
//
// WHY A CHANNEL INSTEAD OF LETTING VFX READ SIM STATE
// The renderer already reads post-tick sim state directly for anything that
// *persists* (agents, towers, fields). That works because those things are
// still there when the frame draws. Combat spectacle is the opposite: a muzzle
// flash, an impact pop, a chain arc, and a shatter are all instants. They
// happen at a tick and are gone. Polling state after the tick cannot recover
// them — by the time the renderer looks, the round is retired and the agent is
// compacted away.
//
// So the sim announces instants into this buffer, and the VFX layer drains it
// once per frame and turns each one into particles. This keeps the dependency
// pointing one way (sim knows nothing about rendering) and keeps determinism
// intact: events are an OUTPUT of the tick, never an input to it. Attaching or
// detaching a sink, or dropping every event on the floor, must not change
// state_hash() by one bit.
//
// CAPACITY AND OVERFLOW
// Fixed capacity, allocated once, never grown during a tick (CONVENTIONS.md:
// no allocation inside a sim tick). Overflow drops the newest event and bumps
// `dropped`. Dropping is always acceptable here — a missing pop is a cosmetic
// non-event, whereas a reallocation mid-tick is a hard rule violation. The
// counter exists so a dropped-event storm is visible in the debug overlay
// rather than silently degrading the look.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune::sim {

enum class CombatEventType : u8 {
    /// A tower fired. `origin` = muzzle, `direction` = aim. Gunner uses this
    /// per round; the beam/cone towers use it once per activation.
    MuzzleFlash = 0,
    /// A projectile hit an agent. `origin` = impact point.
    ProjectileImpact = 1,
    /// A projectile timed out or left the world without hitting anything.
    ProjectileExpired = 2,
    /// Mortar's digestive burst. `radius` = the full expanding ring radius.
    Explosion = 3,
    /// Laser sweep. `origin` -> `secondary` is the full beam segment.
    BeamFired = 4,
    /// One Tesla chain hop. `origin` -> `secondary`. One event per hop, so a
    /// 5-target chain emits 5 events and the VFX layer draws 5 jagged links.
    ChainArc = 5,
    /// Cryo cone pulse. `direction` = cone axis, `radius` = reach,
    /// `arc_radians` = half-angle.
    ConePulse = 6,
    /// An agent became fully encased by Cryo. Drives the "PING" ripple.
    Freeze = 7,
    /// A freeze expired and the shell broke apart.
    Shatter = 8,
    /// Blade rotor passed through an agent. `origin` = contact point,
    /// `direction` = blade travel, for the slash arc.
    BladeSlash = 9,
    /// A fluid particle was stopped hard — by a vessel wall or by a thick
    /// enough crowd. `origin` = the contact point, `direction` = the heading it
    /// had just before, `magnitude` = the speed it lost. Raised at most
    /// FluidTuning::max_splash_events times a tick, because the fluid layer can
    /// have hundreds of particles land in the same frame and one shared sink
    /// serves every tower on the board.
    ///
    /// NOTE this is a cosmetic GARNISH, not the splash itself. The actual
    /// splashing is the fluid solver moving real particles, and it is drawn
    /// from live sim state by the fluid render pass. These events only feed the
    /// fine droplet spray that a particle-based surface cannot resolve.
    FluidSplash = 10,
    /// A chaff agent was killed by the player. `origin` = where it died,
    /// `direction` = the heading it died on, `magnitude` = its SPEED at death
    /// (world units/sec, unclamped), `radius` = its family's body radius, and
    /// `target_family` = which family — that last one is the whole point, since
    /// the VFX layer draws this from the per-family table in vfx/DeathVfx.h.
    ///
    /// `source` is always TowerType::Count. Not an oversight: this is raised at
    /// compaction, which is the only place that sees every death but knows
    /// nothing about what caused any of them — aggregate damage means a chaff
    /// agent has no "last hit" to attribute (sim/damage/DamageField.h). It is
    /// also the right answer visually. A death burst is the ENEMY's identity,
    /// not the killer's, so a player can tell what popped without knowing what
    /// shot it.
    ///
    /// Raised at most SimDesc::max_chaff_death_events times a tick, for the
    /// same reason FluidSplash is capped: a wave clearing kills hundreds of
    /// agents on one tick, and letting that flood the shared sink would starve
    /// every tower's own effect at exactly the moment the screen is busiest.
    ChaffDeath = 11,
    Count = 12,
};

/// One instantaneous combat happening. Trivially copyable, kept small and flat
/// so the whole buffer is a memcpy-friendly array.
struct CombatEvent {
    CombatEventType type = CombatEventType::MuzzleFlash;
    /// Cosmetic tier/variant selector, passed straight through from the tower
    /// or projectile that raised it. The sim assigns no meaning to it; the VFX
    /// layer uses it to escalate a tower's look tier by tier.
    u16 visual_id = 0;
    /// Which tower type raised this, so VFX can colour-code without a lookup.
    /// TowerType::Count means "not from a tower" (environmental hazards).
    TowerType source = TowerType::Count;
    /// Pathogen family involved, where one is (impacts, freezes, shatters).
    /// PathogenFamily::Count means "not agent-specific".
    PathogenFamily target_family = PathogenFamily::Count;

    Vec2 origin{0.0f, 0.0f};
    /// Beam/chain endpoint. Unused (and left at origin) by point events.
    Vec2 secondary{0.0f, 0.0f};
    /// Unit aim/travel direction. Unused by events that do not have one.
    Vec2 direction{1.0f, 0.0f};

    f32 radius = 0.0f;       ///< Explosion/cone reach, or impact pop size.
    f32 arc_radians = 0.0f;  ///< Cone half-angle. Cone events only.
    /// Rough "how big should this read" scalar (damage dealt, mass removed).
    /// Lets one event type render at wildly different intensities without the
    /// VFX layer having to re-derive anything from sim state.
    f32 magnitude = 1.0f;
};

/// Fixed-capacity, append-only-per-tick event buffer. The sim pushes; the VFX
/// layer drains once per frame via events() then calls clear().
///
/// Single-threaded by contract. If a future parallel tower pass needs to raise
/// events from worker threads, give each worker its own sink and merge in a
/// deterministic order — do NOT make push() atomic and call it a day, because
/// event ORDER would then depend on thread scheduling, and the VFX layer's
/// per-event RNG would desync a replay's visuals from its recording.
class CombatEventSink {
public:
    /// Allocates once. Call at level load, never during a tick.
    void reserve(usize max_events) {
        capacity_ = max_events;
        events_.reserve(max_events);
    }

    /// Appends an event, or drops it and bumps dropped() when at capacity.
    /// noexcept and allocation-free by construction.
    void push(const CombatEvent& e) {
        if (events_.size() >= capacity_) { ++dropped_; return; }
        events_.push_back(e);
    }

    const std::vector<CombatEvent>& events() const { return events_; }
    usize size() const { return events_.size(); }
    usize capacity() const { return capacity_; }

    /// Number of events dropped since the last reset. Surfaced in the debug
    /// overlay so an under-sized buffer is diagnosable instead of invisible.
    u64 dropped() const { return dropped_; }
    void reset_dropped() { dropped_ = 0; }

    /// Drops every buffered event. The VFX layer calls this after draining.
    /// Keeps capacity, so this never reallocates.
    void clear() { events_.clear(); }

private:
    std::vector<CombatEvent> events_;
    usize capacity_ = 0;
    u64 dropped_ = 0;
};

} // namespace immune::sim
