// sim/hostile/Harpoon.h — the Spore's attack: a liquid hose fired across the
// lane, anchored in one friendly cell and pumped full of toxin.
//
// WHAT IT IS
// A spore is ordinary chaff -- same SoA slot, same movement kernel, same
// damage paths -- that fights from far away. Every few seconds it picks the
// BIGGEST friendly in range (a Macrophage before a Neutrophil before a
// Cytotoxic T), plants itself, and:
//
//   DRAW     squeezes up for `windup` seconds, turned toward its target;
//   FIRE     shoots a hose at it, the tip flying at `extend_speed` and
//            homing on the target's membrane;
//   PUMP     once anchored, pushes `pulses` slugs of toxin down the hose,
//            `pulse_interval` apart, each travelling at `pulse_speed`. A slug
//            deals `pulse_damage` when it REACHES the target -- not when it
//            leaves -- so what the player sees arrive is what hurts;
//   RECOVER  lets go, reels the hose in and reloads for `reload` seconds.
//
// Target choice is by body size, nothing else: no minimum range, no dead
// zone. A Cytotoxic T latched onto the spore itself is a legal target, it is
// just always the last one picked, which is what lets a swarm of them get in
// and kill it while it is busy with the Macrophage.
//
// It stands its ground while it fights (`hold`) and also while it is
// reloading with a target still in range: a spore with something to shoot at
// stops at the edge of its range and sieges it rather than walking into the
// cell's reach. Planted, it is not walked forward and not shoved forward by
// the crowd behind it either. With nothing in range it walks the lane.
//
// SHOOTER STATE lives on the chaff streams (harpoon_state / harpoon_timer /
// harpoon_scan / harpoon_sighted / hold), so it travels with compaction. The
// HOSE lives here, in HostileSystem's HarpoonLine list, which holds the
// shooter by ChaffHandle and the target the way ToxinShot does (swarmer slot
// + generation, or tower id). A hose whose shooter dies is severed: it stays
// on screen, slack, for `retract_seconds` and does no more damage.
//
// DETERMINISM
// Serial, chaff index order, no RNG. Lines are appended in shooter order and
// removed by swap, which is a pure function of the same state.
#pragma once

#include "core/Types.h"
#include "sim/chaff/ChaffBuffers.h"

namespace immune::sim {

/// Most slugs one shot can carry; HarpoonParams::pulses is clamped to it.
/// Four, because harpoon.frag takes the slugs in as one vec4.
inline constexpr u32 kHarpoonMaxPulses = 4;

/// What ChaffBuffers::harpoon_state holds.
namespace harpoon_state {
inline constexpr u8 kReady = 0;    ///< Looking for a target. timer: unused.
inline constexpr u8 kDraw = 1;     ///< Planted and squeezing up. timer: windup left.
inline constexpr u8 kFire = 2;     ///< Hose in flight. timer: seconds since it left.
inline constexpr u8 kPump = 3;     ///< Hose anchored, slugs going down it. timer: since the last slug left.
inline constexpr u8 kRecover = 4;  ///< Let go, reloading. timer: reload left.
} // namespace harpoon_state

/// One family's harpoon (enemies.json "harpoon"). Off for every family but
/// the Spore.
struct HarpoonParams {
    bool enabled = false;
    // ---- gameplay ----------------------------------------------------------
    f32 range = 44.0f;           ///< Shooter centre to target membrane, world units.
    f32 windup = 0.45f;          ///< Seconds planted and squeezing before the hose leaves.
    f32 extend_speed = 150.0f;   ///< Hose tip, world units/sec.
    u32 pulses = 0;              ///< Slugs per shot, at most kHarpoonMaxPulses; 0 pumps until the target dies.
    f32 pulse_damage = 84.0f;    ///< Hit points one slug takes off when it arrives; two slugs kill a base Macrophage (120).
    f32 pulse_interval = 0.26f;  ///< Seconds between slugs leaving the nozzle.
    f32 pulse_speed = 70.0f;     ///< Slug speed down the hose, world units/sec.
    f32 reload = 6.0f;           ///< Seconds from letting go to the next windup: how long before it can pick a new target.
    f32 retry = 0.25f;           ///< Seconds between target searches.
    f32 break_range = 56.0f;     ///< The hose lets go past this, world units.
    f32 hold = 1.0f;             ///< Fraction of its speed and crowd push a planted shooter gives up; 1 stands still.
    // ---- look (renderer only) ---------------------------------------------
    f32 retract_seconds = 0.3f;  ///< Hose reeling back in / a severed one fading.
    f32 thickness = 0.27f;       ///< Hose half-width, world units.
    f32 bulge = 3.0f;            ///< Extra half-width over a slug, as a multiple of thickness.
    f32 slug_length = 2.2f;      ///< World units a slug's swelling spans.
    f32 whip = 1.4f;             ///< Sideways swing of the hose in flight, world units.
    f32 sway = 0.35f;            ///< Sideways ripple left on an anchored hose, world units.
    f32 wavelength = 9.0f;       ///< World units between the hose's crests.
    Vec4 fluid_color{1.0f, 0.93f, 0.62f, 1.0f};  ///< The toxin inside the hose and its slugs.
};

/// Per-family table the RENDERER reads, filled by game::apply_enemy_config().
/// The sim reads its own copy on HostileTuning, same arrangement as
/// sim/burrow/Burrow.h's family_burrow().
const HarpoonParams& family_harpoon(PathogenFamily family);
void set_family_harpoon(PathogenFamily family, const HarpoonParams& params);

/// One hose, from the moment a spore picks its target to the moment the hose
/// is reeled in. While the shooter is still drawing `reach` is 0 and nothing
/// is drawn.
struct HarpoonLine {
    ChaffHandle shooter{};
    u8 family = 0;
    /// The target, as ToxinShot names it: a swarmer slot + generation, or a
    /// tower's EntityId value with `target_tower` set.
    u32 target_index = 0;
    u32 target_generation = 0;
    bool target_tower = false;
    /// The nozzle and the point on the target's membrane facing it, this tick
    /// and last, so the renderer can interpolate the ends with the bodies.
    Vec2 origin{0.0f, 0.0f};
    Vec2 prev_origin{0.0f, 0.0f};
    Vec2 anchor{0.0f, 0.0f};
    Vec2 prev_anchor{0.0f, 0.0f};
    /// World units of hose out of the nozzle along origin -> anchor.
    f32 reach = 0.0f;
    /// Seconds since the hose left the nozzle (the whip's clock); <0 drawing.
    f32 age = -1.0f;
    /// Seconds since it anchored; <0 while still flying.
    f32 attached = -1.0f;
    /// Seconds since it let go (reeling in, or severed); <0 while engaged.
    f32 released = -1.0f;
    bool severed = false;
    f32 next_pulse = 0.0f;
    u8 pulses_fired = 0;
    /// Slugs in the hose: 0..1 from nozzle to anchor, <0 when not in it.
    f32 pulse[kHarpoonMaxPulses] = {-1.0f, -1.0f, -1.0f, -1.0f};
    /// Stable per-shot cosmetic seed (the shooter's generation).
    u32 seed = 0;
};

} // namespace immune::sim
