// vfx/DeathVfx.h — the per-family chaff death burst, as authored data.
//
// WHY THIS IS NOT IN Particles.h
// Particles.h is a frozen contract and its art authoring is hardcoded per
// tower: emit_for_event() switches on TowerType and spends literals. That is
// right for the towers, because a tower's look IS its identity and there are
// exactly six of them. A chaff death is the opposite kind of effect — it is the
// most-repeated visual in the game (every wave ends with hundreds of them), it
// belongs to the ENEMY rather than to whatever killed it, and it is the thing a
// designer will want to retune most often. So it is table-driven and lives in
// assets/config/enemies.json under families.<name>.death_vfx.
//
// WHY A FILE-SCOPE TABLE RATHER THAN A ParticleSystem MEMBER
// Exactly the arrangement render::family_color/family_visual already uses, for
// exactly the same reason: immune_vfx sits below immune_game and cannot see the
// config types, so the values have to be pushed DOWN at load
// (game::apply_enemy_config does it). Making it a table also means every
// ParticleSystem in the process — the app's, the screenshot harness's, a test's
// — picks the authored look up without separate wiring.
//
// The compiled-in defaults below are the shipping values. They are what
// game::default_game_config() bootstraps enemies.json from, and what a
// vfx-only unit test sees when no config was ever loaded.
#pragma once

#include "core/Types.h"

namespace immune::vfx {

/// Which SHAPE the body comes apart into. Deliberately the only thing about the
/// burst that is not a number: everything else — how many, how fast, how
/// ordered, how long — is authored, so a family can be retuned from the file
/// without ever touching this.
///
/// BOTH SHIPPED FAMILIES USE Burst. Lyse was the Bacteria's original look and
/// it was wrong: a Tracer is a soft glow whose opaque core is a fifth of its
/// quad, so a handful of them landing together composite into one blurry
/// smudge instead of reading as pieces. A death is an explosion of debris, and
/// debris has edges. Lyse stays available and finished for a family that should
/// read as genuinely liquid, but nothing authored uses it today.
enum class DeathStyle : u8 {
    /// Angular shards — hard silhouettes, per-piece seed-varied. What an
    /// exploding body looks like at any size.
    Burst = 0,
    /// Soft round droplets with a motion streak. Wet and edgeless; only picks
    /// up definition at high speed, and blurs together at low.
    Lyse = 1,
    Count = 2,
};

/// One family's death burst. A standard-layout aggregate of scalars so
/// config/Field.h can address every member by offset — see IMMUNE_CONFIG_FIELD.
///
/// SIZES ARE MULTIPLES OF THE AGENT'S OWN RADIUS, not world units. That is what
/// lets a bacterium's death outweigh a virus's without either one needing its
/// numbers hand-matched to its silhouette: change `visual.silhouette` and the
/// burst follows. SPEEDS are absolute world units/sec, because how fast debris
/// flies is not a function of how big the thing was.
///
/// A SIZE IS A QUAD, NOT AN INK FOOTPRINT. Every size here ends up as
/// ParticleSpawnParams::size, which assets/shaders/particle.vert turns into the
/// instance's quad, and each kind's SDF then fills a different fraction of that
/// quad. The two that matter for this effect are far apart:
///
///   Tracer  fills essentially the whole quad — drawn diameter ~= size.
///   Shard   is a 3-gon whose half-planes sit at 0.20..0.36 in quad-local
///           units, so it inks only about 0.28 * size across.
///
/// A Shard is close to solid inside that silhouette; a Tracer's fully-opaque
/// core is only about 0.2 of its quad and its tail is faded to an eighth. So a
/// Lyse family needs a `bit_size_*` near twice a Burst family's just to reach
/// the same weight -- and it still will not reach the same DEFINITION, which is
/// why nothing ships on Lyse.
struct FamilyDeathVfx {
    /// False draws nothing at all for this family. The off switch, so a
    /// performance test or a stylistic pass can kill the effect from the file.
    bool enabled = true;
    DeathStyle style = DeathStyle::Burst;

    /// The family's colour. Defaults to enemies.json's `visual.color` when the
    /// shipped config is generated, so the burst matches the body it came out
    /// of without anyone having to keep two numbers in sync — but it is
    /// authored separately, so a family CAN pop brighter than it lives.
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};

    /// Master multiplier over every size in the burst -- core, ring, debris and
    /// mist alike. The one knob to reach for when deaths read too big or too
    /// small across the board, rather than editing a dozen sizes in step.
    f32 scale = 1.20f;

    // ---- The core flash: one additive spark at the moment of death. --------
    f32 core_size = 1.20f;   ///< x agent radius. 0 omits the flash.
    /// Seconds. The detonation, not the aftermath -- but not so short it can
    /// fall between two rendered frames on a machine dropping below 60.
    f32 core_life = 0.07f;
    /// How far the flash is pushed toward white, 0..1. This is what makes a
    /// death read as an EVENT rather than as a slightly brighter agent.
    f32 core_white = 0.60f;

    // ---- The shock ring. --------------------------------------------------
    // Kept deliberately faint. The ring shader whitens its own band by half at
    // birth, and this pass is additive over a red substrate, so a ring at high
    // alpha stops reading as the family's colour at all and turns into a pale
    // hoop -- which then dominates the burst, because a whole wave dies at once
    // and the hoops tile.
    f32 ring_size = 2.20f;   ///< x agent radius; the FINAL radius (Particles.h).
    f32 ring_life = 0.10f;   ///< Seconds. 0 omits the ring.
    f32 ring_alpha = 0.35f;

    // ---- The body coming apart. -------------------------------------------
    //
    // MANY, SMALL, FAST, BRIEF -- in that order. A death should read as a body
    // detonating into fragments that are gone before the eye tracks any one of
    // them, so the count is high, each piece is a fraction of the body it came
    // out of, and the whole thing is over inside a quarter second.
    //
    // The speed/drag pair is what makes it EXPLOSIVE rather than merely fast.
    // High launch speed against a high drag means a piece covers most of its
    // total travel in the first two or three frames and then decelerates hard,
    // which is the shape of a real burst. Cutting drag with the same speed just
    // produces a slow radial drift outward, which reads as a bloom.
    u32 bit_count = 24;
    f32 bit_size_min = 0.20f;   ///< x agent radius; see the quad note above
    f32 bit_size_max = 0.38f;
    f32 bit_speed_min = 14.0f;  ///< world units/sec
    f32 bit_speed_max = 30.0f;
    f32 bit_life_min = 0.12f;
    f32 bit_life_max = 0.24f;
    f32 bit_drag = 7.0f;
    /// World units/sec^2 on +Y. Negative sinks — debris should fall, and
    /// nothing else in this layer does, which is most of what sells it as
    /// matter rather than as light.
    f32 bit_buoyancy = -2.5f;
    f32 bit_spin = 16.0f;       ///< Max |radians/sec|, signed per piece.
    /// How ORDERED the spray is. 0 puts the pieces on perfectly even radial
    /// spokes — a shell failing all at once; 1 scatters them uniformly — a
    /// splatter. This is the dial between "popped" and "spilled", and it is a
    /// float rather than part of `style` precisely so it can be dialled.
    f32 bit_scatter = 0.15f;

    // ---- The soft remains hanging in the air. -----------------------------
    // Always alpha-blended, never additive: this is matter, and an additive
    // version would let a cleared wave wash out the whole lane at once.
    //
    // The Mist shader multiplies its own alpha by 0.75 and by sin(pi * age), so
    // `bloom_alpha` is a CEILING reached only at half life, not the opacity a
    // puff is ever actually drawn at. Values that look high here land low.
    //
    // Kept SHORTER-LIVED than the debris, deliberately. Mist is the one part of
    // this burst with no edges, so it is also the one part that can turn the
    // whole effect into a smudge -- and it does exactly that if it is still
    // hanging in the air after the fragments it was supposed to accompany have
    // gone. It is a puff of the body, not a lingering cloud.
    u32 bloom_count = 1;
    f32 bloom_size = 1.50f;   ///< x agent radius
    f32 bloom_life = 0.14f;
    f32 bloom_alpha = 0.40f;
    f32 bloom_speed = 2.20f;  ///< world units/sec, radially outward
    f32 bloom_rise = 1.60f;   ///< +Y accel; positive drifts up like a puff

    /// Fraction of the agent's velocity at death the whole burst inherits, so a
    /// pathogen killed at a sprint sprays forward instead of every death in the
    /// game reading as a stationary pop. 0 pins the burst in place.
    f32 inherit_velocity = 0.35f;
};

/// The look currently in force for `family`. Compiled-in defaults until
/// set_family_death_vfx() overrides them at config load.
const FamilyDeathVfx& family_death_vfx(PathogenFamily family);

/// Installs `look` for `family`. Called by game::apply_enemy_config() with the
/// values parsed out of enemies.json; nothing inside vfx/ ever calls it.
void set_family_death_vfx(PathogenFamily family, const FamilyDeathVfx& look);

// ===========================================================================
// SWARMER DEATH — a friendly unit bursting (CombatEventType::SwarmerDeath).
//
// Same arrangement as FamilyDeathVfx, keyed by the tower that released the
// unit and authored in assets/config/towers.json under
// towers.<name>.death_vfx (game::apply_tower_config pushes it down).
//
// It is the opposite KIND of effect, deliberately: a pathogen death is a small
// violent pop; a unit dying is a CELL BURSTING. Its body tears into pieces of
// membrane and cytoplasm that start out as the whole cell and drift apart,
// the nucleus breaks into lumps, granules spill, and all of it fades. Matter,
// not light: no flash, no ring, nothing additive, and the colours are what the
// unit is made of (assets/shaders/swarmer.frag), not the tower's bright
// identity hue.
//
// SIZES AND SPEEDS ARE MULTIPLES OF THE UNIT'S BODY RADIUS (SwarmerProfile::
// size), so a macrophage comes apart as wide, relative to itself, as a T cell,
// and retuning a unit's size never needs this touched. Counts are absolute.
// Every size is the visible extent of the piece, not its particle quad.
// ===========================================================================
struct SwarmerDeathVfx {
    /// False draws nothing when this tower's units die.
    bool enabled = true;

    /// The cytoplasm at mid depth, tint already mixed in. The shader lightens
    /// it toward the membrane and darkens it deep inside, as swarmer.frag does.
    Vec4 cytoplasm{0.84f, 0.85f, 0.85f, 1.0f};
    Vec4 nucleus{0.42f, 0.36f, 0.55f, 1.0f};

    /// Fraction of the unit's velocity at death the remains carry, and the
    /// speed (world units/sec) that is capped at first, so a unit killed at
    /// launch speed does not smear its remains downrange.
    f32 inherit_velocity = 0.33f;
    f32 inherit_speed_cap = 40.0f;

    // ---- The body tearing. -------------------------------------------------
    /// How many pieces the body tears into. They tile the cell at birth.
    u32 piece_count = 6;
    /// x body radius: where the pieces' membrane sits at birth. The living
    /// body's main disc is ~0.7 of its radius (its pseudopods reach ~0.9), so
    /// much above 0.7 and the remains start out bigger than the cell was.
    f32 piece_radius = 0.70f;
    /// Radians each piece reaches past its slice on both sides, so the cell is
    /// whole at birth where two neighbouring tears happen to agree.
    f32 piece_overlap = 0.05f;
    /// Size at the end of life as a fraction of size at birth. Below 1 the
    /// torn membrane contracts as it goes; 1 holds it; never above 1 unless
    /// the remains should swell.
    f32 piece_end_scale = 0.75f;
    f32 piece_speed_min = 3.3f;    ///< x body radius / sec
    f32 piece_speed_max = 8.0f;
    /// Radians a piece may drift off its own slice. 0 opens the cell like a
    /// flower; a little tears it unevenly.
    f32 piece_heading_jitter = 0.35f;
    f32 piece_life_min = 0.38f;    ///< Seconds
    f32 piece_life_max = 0.49f;
    f32 piece_drag = 4.7f;         ///< Per-second velocity damping
    /// Max |radians/sec|, signed per piece. Kept low: a piece turns about the
    /// cell's old centre, not its own, so spin swings it sideways.
    f32 piece_spin = 0.8f;

    // ---- The nucleus breaking. ---------------------------------------------
    u32 nucleus_count = 2;
    f32 nucleus_size_min = 0.11f;  ///< x body radius, the lump's radius
    f32 nucleus_size_max = 0.15f;
    /// x body radius: how far from the centre a lump starts.
    f32 nucleus_spread = 0.12f;
    f32 nucleus_speed_min = 1.3f;  ///< x body radius / sec
    f32 nucleus_speed_max = 2.9f;
    f32 nucleus_life_min = 0.41f;
    f32 nucleus_life_max = 0.53f;
    f32 nucleus_drag = 4.0f;
    f32 nucleus_spin = 2.0f;
    f32 nucleus_end_scale = 0.65f;

    // ---- Granules spilling. ------------------------------------------------
    u32 granule_count = 6;
    /// How far the granules are pushed from the cytoplasm colour toward white.
    f32 granule_whiten = 0.20f;
    f32 granule_size_min = 0.045f; ///< x body radius, the granule's radius
    f32 granule_size_max = 0.075f;
    f32 granule_spread_min = 0.10f;///< x body radius from the centre at birth
    f32 granule_spread_max = 0.60f;
    f32 granule_speed_min = 5.3f;  ///< x body radius / sec
    f32 granule_speed_max = 9.3f;
    f32 granule_life_min = 0.26f;
    f32 granule_life_max = 0.41f;
    f32 granule_drag = 5.3f;
    f32 granule_end_scale = 0.65f;

    /// How lumpy the nucleus pieces and granules are: 0 is round.
    f32 lump_wobble = 0.10f;
};

/// The look currently in force for units released by `source`. TowerType::
/// Count (no tower: the generic bomber of authored scenes) has its own slot,
/// compiled in, never authored.
const SwarmerDeathVfx& swarmer_death_vfx(TowerType source);

/// Installs `look` for `source`. Called by game::apply_tower_config() with
/// the values parsed out of towers.json; nothing inside vfx/ ever calls it.
void set_swarmer_death_vfx(TowerType source, const SwarmerDeathVfx& look);

} // namespace immune::vfx
