// sim/chaff/HitFlash.h — the per-family "I just got hit" tint, as authored data.
//
// WHAT IT IS
// A chaff agent that loses density flares toward a colour (white, shipped) and
// fades back over a tenth of a second. It is the smallest possible answer to
// "is my tower actually doing anything" — the death burst only fires on the
// last hit, and everything before that was, until now, completely silent.
//
// WHY THIS LIVES IN sim/ AND NOT IN vfx/ NEXT TO DeathVfx
// DeathVfx is pure art: the sim announces a death through CombatEvents and the
// vfx layer owns everything after that, so its table can sit above sim/. A hit
// flash cannot work that way. It is per-agent STATE that outlives the instant
// it was raised — the agent has to remember it is flashing across the frames it
// takes to fade — and the only place that state can live is next to the agent,
// in ChaffBuffers. That puts the *setter* (ChaffBuffers::apply_density_loss)
// and the *decay* (ChaffSystem::update) inside sim/, and sim/ cannot see vfx/.
//
// So the table lands here, at the bottom, where sim/ can write it and render/
// (which links sim/) can read it. Same file-scope-table shape as
// vfx::family_death_vfx and render::family_visual, and for the same reason:
// game::apply_enemy_config pushes the authored values DOWN at load, and every
// consumer in the process — the game's renderer, a screenshot harness, a unit
// test — picks them up without separate wiring.
//
// IT IS COSMETIC AND MUST STAY COSMETIC
// `ChaffBuffers::hit_flash` is deliberately absent from SimWorld::state_hash().
// Retuning any number here, or disabling the effect outright, must not move the
// hash by a bit — exactly the contract sim/CombatEvents.h states for the other
// half of the spectacle layer. Nothing in the movement or damage kernels ever
// READS the stream; they only write it and decay it.
#pragma once

#include "core/Types.h"

namespace immune::sim {

/// What a second hit does to an agent that is already flashing.
enum class HitFlashRetrigger : u8 {
    /// Keep whichever is brighter. The default, and the only one of the three
    /// that cannot make a hit read as LESS than it was: a chip of splash damage
    /// landing a frame after a cannon round must not dim the cannon round.
    Highest = 0,
    /// Overwrite unconditionally. Every hit restarts the fade at its own
    /// intensity, so a stream of small hits reads as a stutter rather than as
    /// one sustained glow.
    Refresh = 1,
    /// Add, saturating at 1. Makes overlapping damage sources pile up into a
    /// full white-out — the right choice for a family that should visibly cook
    /// under sustained fire rather than shimmer at a constant level.
    Accumulate = 2,
    Count = 3,
};

/// One family's hit flash. A standard-layout aggregate of scalars so
/// config/Field.h can address every member by offset — see IMMUNE_CONFIG_FIELD.
///
/// THE STORED VALUE IS 0..1 AND DECAYS LINEARLY. Everything shaped — the
/// response curve, the peak opacity, the size punch — is applied at DRAW time
/// from this table, not baked into the stream. That is what makes the whole
/// effect hot-reloadable: change `curve` mid-wave and the agents already
/// mid-flash re-render under the new curve on the very next frame.
struct HitFlashParams {
    /// False stores nothing and draws nothing for this family. The off switch,
    /// so a performance pass or a stylistic one can kill the effect from the
    /// file — and it costs nothing when off, because apply_density_loss()
    /// early-outs on it before touching the stream.
    bool enabled = true;
    HitFlashRetrigger retrigger = HitFlashRetrigger::Highest;

    /// What the body flares toward. White is the shipped value and the reason
    /// this feature exists, but it is authored rather than hardcoded so a
    /// family can flare toward its own hot colour instead.
    /// ALPHA IS IGNORED — how opaque the flare gets is `strength`'s job, and
    /// two knobs for one quantity is how they drift apart.
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    /// Peak mix toward `color`, 0..1, reached at full intensity. 1 replaces the
    /// body's shaded colour outright; the shipped 0.9 leaves a trace of the
    /// family hue at the peak so a white-hot agent is still identifiable.
    f32 strength = 0.90f;

    /// Seconds to fade from full intensity back to nothing.
    ///
    /// Short, and it has to be: this fires on EVERY damage source in the game,
    /// including the ones that tick sixty times a second, so a long fade turns
    /// a tower's whole aura into a permanently white patch of screen. Long
    /// enough to survive a frame at 60 fps, not much longer.
    /// 0 (or negative) disables the effect as surely as `enabled = false`.
    f32 duration = 0.12f;

    /// Exponent applied to the stored intensity before it is drawn.
    ///
    /// THE MOST IMPORTANT KNOB HERE, and the one that makes a single effect
    /// serve both kinds of damage this game has. Chaff is hurt by discrete
    /// events (a Neutrophil round removing most of a virion at once) AND by
    /// continuous fields (a Macrophage aura shaving a few percent per tick),
    /// and both arrive through the same call. A linear response would draw the
    /// aura case at a constant ~20% white — a permanent haze over every crowd
    /// standing in range — which is not feedback, it is fog.
    ///
    /// An exponent above 1 crushes the low end and leaves the high end alone:
    /// at 1.8, a field tick's 0.22 intensity draws at 0.06 (a shimmer you read
    /// as "these are being worked on") while a round's 1.0 still draws at 1.0
    /// (a hit). Below 1 does the opposite, for a family that should glow under
    /// sustained fire.
    f32 curve = 1.80f;

    /// Multiplier on the fraction of an agent's density one hit removed, before
    /// the value is stored (and clamped to 1). Raise it to make weak, frequent
    /// damage register; lower it to reserve the flash for real chunks.
    ///
    /// FRACTION OF *CURRENT* DENSITY, not of the family's spawn density: an
    /// agent at a tenth of its health flashes hard for a hit that a fresh one
    /// would shrug off. That is not an artifact of the cheaper formula, it is
    /// the read — an almost-dead agent lighting up is the game telling the
    /// player where the next kill is coming from.
    f32 gain = 1.00f;

    /// Hits that remove less than this fraction of an agent's density store
    /// nothing at all. A floor, not a shaping tool — `curve` does the shaping.
    /// Its job is to keep the very smallest ticks from writing to the stream
    /// (and so from costing a per-frame pack) for a flash the curve was going
    /// to crush to invisibility anyway.
    f32 min_fraction = 0.02f;

    /// Seconds the BODY of a killed agent keeps being drawn -- white, fading,
    /// and drifting on the velocity it died with -- after the sim has already
    /// removed it. 0 draws nothing and leaves death entirely to the burst.
    ///
    /// WHY THIS EXISTS AT ALL, AND WHY IT IS NOT OPTIONAL POLISH
    /// Measured on a bot-played run of skin_1_breach: 94,245 agent-frames were
    /// drawn and 29 of them -- 0.03% -- were an agent that was damaged and
    /// still alive. Chaff has essentially no wounded state. A Macrophage at
    /// tier 1 removes 1.03 density per tick and a virus carries 0.6, so an
    /// agent goes from untouched to gone inside a single 1/60 s tick and no
    /// frame is ever drawn in between. Every knob above can be cranked to
    /// absurdity and the screen does not change by more than one 255th.
    ///
    /// So for the overwhelming majority of enemies, THE KILLING BLOW IS THE
    /// ONLY HIT THEY EVER TAKE, and hit feedback on the body can only exist if
    /// the body outlives the agent. That is what this is: the flash keeps being
    /// drawn for a few frames after its owner is gone.
    ///
    /// It is a RENDER-side effect only. sim/ knows nothing about it; the
    /// renderer builds these instances from the CombatEventType::ChaffDeath
    /// events the sim already publishes, which is the same channel the death
    /// burst comes down. Nothing here can move state_hash().
    ///
    /// Kept to a handful of frames. This fires on every kill in the game, and a
    /// long linger would leave a wave's worth of white corpses standing in the
    /// lane after the wave that made them is over.
    f32 death_linger = 0.09f;

    /// Extra sprite diameter at full intensity, as a FRACTION of the family's
    /// silhouette. 0.08 puffs an agent 8% bigger on the frame it is hit.
    ///
    /// SHIPPED AT 0. render/ChaffBatcher.h states the standing rule — sprite
    /// size encodes threat tier and damage must not touch it — and that rule is
    /// about PERSISTENT size, which is what used to make a wounded horde read
    /// as a distant one. A punch that is gone in a tenth of a second does not
    /// re-encode anything, so the knob is offered; it is off by default because
    /// the default should be the rule, not the exception.
    f32 scale_punch = 0.0f;
};

/// The flash currently in force for `family`. Compiled-in defaults until
/// set_family_hit_flash() overrides them at config load.
const HitFlashParams& family_hit_flash(PathogenFamily family);

/// Installs `params` for `family`. Called by game::apply_enemy_config() with
/// the values parsed out of enemies.json; nothing inside sim/ ever calls it.
void set_family_hit_flash(PathogenFamily family, const HitFlashParams& params);

/// The intensity to STORE for a hit that removed `removed` density from an
/// agent that had `before` before it landed, or 0 for a hit this family does
/// not flash on.
///
/// Shared by the damage path and by the tests that pin its shape, so "what
/// counts as a hit" is answered in exactly one place. Pure: no state, no table
/// lookup beyond `params`.
inline f32 hit_flash_intensity(const HitFlashParams& params, f32 before, f32 removed) {
    if (!params.enabled || params.duration <= 0.0f) return 0.0f;
    if (removed <= 0.0f || before <= 0.0f) return 0.0f;
    const f32 fraction = removed / before;
    if (fraction < params.min_fraction) return 0.0f;
    const f32 raw = fraction * params.gain;
    return raw < 1.0f ? raw : 1.0f;
}

/// Folds a freshly computed `incoming` intensity into whatever the agent was
/// already carrying, per the family's retrigger rule. Also shared with the
/// tests, for the same reason as above.
inline f32 hit_flash_combine(HitFlashRetrigger mode, f32 current, f32 incoming) {
    switch (mode) {
        case HitFlashRetrigger::Refresh: return incoming;
        case HitFlashRetrigger::Accumulate: {
            const f32 sum = current + incoming;
            return sum < 1.0f ? sum : 1.0f;
        }
        case HitFlashRetrigger::Highest:
        case HitFlashRetrigger::Count:
            break;
    }
    return current > incoming ? current : incoming;
}

} // namespace immune::sim
