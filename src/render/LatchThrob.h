// render/LatchThrob.h — authorable look of a pathogen feeding on a host.
//
// A chaff agent that has latched onto a tower or a swarmer (sim/hostile,
// chaff_flags::kLatched) is planted on the host's membrane and never moves
// again until one of them dies. Drawn as the same still sprite it was in the
// lane, it read as a decal stuck to the cell. This table is the motion that
// says "feeding": the body throbs and pumps toward the host as if liquid were
// being pushed out of it into the cell, entirely in chaff.frag.
//
// It lives in render/ rather than beside HitFlash and ReplicationSplit in
// sim/chaff because, unlike those two, NO sim path consumes any of it: the
// hostile pass writes a heading (ChaffBuffers::latch_heading) and that is the
// whole hand-over. The batcher reads `enabled` and `rate`; the fragment stage
// reads the amplitudes. Nothing here may make a gameplay decision.
//
// Same arrangement as FamilyVisual (ChaffBatcher.h): compiled-in defaults
// that assets/config/enemies.json overrides at load, so a renderer that is
// never handed a config draws exactly what it always did.
#pragma once

#include "core/Types.h"

namespace immune::render {

struct LatchThrobParams {
    bool enabled = true;      ///< False draws a latched agent exactly as a walking one.
    /// Strokes per second of the pump. The rhythm is deliberately irregular
    /// (three sines at incommensurate rates in chaff.frag) so this is the
    /// average, not a metronome.
    f32 rate = 1.1f;
    /// Amplitudes are in the sprite's LOCAL units, where the virus capsid has
    /// radius 0.36 and its spikes stand 0.17 off it (chaff.frag), so 0.04 is
    /// about a tenth of the body.
    f32 throb = 0.06f;        ///< Whole-body breathing on the stroke.
    f32 slosh = 0.09f;        ///< Volume shifted front-to-back per stroke: the push into the host.
    f32 wave = 0.07f;         ///< Height of the peristaltic slugs rolling toward the host.
    f32 wave_count = 1.5f;    ///< Slugs per half-circumference; more is finer and faster.
    f32 ripple = 0.02f;       ///< Fine two-frequency shimmer on the skin between strokes.
    /// The bellows: how much the whole body compresses toward the host on
    /// the push (and stretches on the refill), as a fraction of its length.
    f32 squash = 0.12f;
    /// Length of the probe, the tapering tube reaching from the capsid into
    /// the host with the cargo beading down it. 0 draws no probe.
    f32 probe = 0.3f;
    f32 stream = 0.7f;        ///< Brightness of the beaded cargo channel running into the host, 0..1.
    /// How much the push lights the whole body, as a fraction of its colour.
    /// The one part of the effect that survives gameplay zoom.
    f32 glow = 0.15f;
};

const LatchThrobParams& family_latch_throb(PathogenFamily family);
void set_family_latch_throb(PathogenFamily family, const LatchThrobParams& params);

} // namespace immune::render
