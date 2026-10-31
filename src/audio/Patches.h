// audio/Patches.h — the sound design, as data.
//
// Every sound in the game is synthesized at runtime from a row in one of three
// tables: a one-shot VOICE patch (oscillators + noise through a filter and an
// envelope), a GRAIN kind (a tiny, cheap blip used thousands of times a minute
// for horde deaths and round hits), or a sustained BED (a texture whose level
// follows a continuous quantity such as how many pathogens are feeding).
//
// READABILITY RULES the tables follow, so a player can tell what happened
// with their eyes shut:
//   * Friendly cells sound clean and tonal: sines, triangles, bells.
//   * The horde sounds organic and noisy: filtered noise, squelch, buzz.
//   * A pathogen dying is a soft bubble pop -- the reward sound, and the
//     most-heard sound in the game, so it is short, quiet and never harsh.
//   * A friendly dying slides DOWN and darkens -- the loss sound.
//   * Alerts (a leak, a wave arriving) live in the low register, where
//     nothing else in combat sits, so they cut through a busy mix.
//
// TUNING. Every pitched sound is tuned to D major pentatonic (D E F# A B).
// Any two notes of that set are consonant, so a hundred overlapping blips
// from different sources still form a chord instead of a cluster. Patch base
// frequencies are D in some octave; the scene picks scale degrees from it.
#pragma once

#include "core/Types.h"

namespace immune::audio {

enum class Bus : u8 {
    Ui = 0,      ///< Menus and HUD. Never ducked, never reverberant.
    Alert,       ///< Wave/objective/run-end stingers. Ducks the combat buses.
    Friendly,    ///< Immune-cell attacks and deaths.
    Horde,       ///< Pathogen attacks, deaths and the horde beds.
    Count
};
inline constexpr u32 kBusCount = static_cast<u32>(Bus::Count);

/// Voice-stealing order. A full pool steals the quietest voice of the lowest
/// priority below the newcomer's; Protected voices are never stolen.
enum class Priority : u8 { Low = 0, Normal, High, Protected };

enum class Wave : u8 { Sine = 0, Triangle, Saw, Square };
enum class FilterMode : u8 { Off = 0, LowPass, BandPass, HighPass };

/// One-shot voice patches. Order is the patch table's order.
enum class SoundId : u8 {
    // ---- UI ------------------------------------------------------------------
    UiHover = 0,
    UiClick,
    UiDeny,        ///< One muted note; the scene plays two, falling.
    UiChime,       ///< A soft bell note: open/close/purchase/victory figures.
    Plop,          ///< A cell landing on the tissue.
    // ---- Alerts --------------------------------------------------------------
    WaveSwell,     ///< Soft horn chord: a wave is coming.
    Thump,         ///< Low heartbeat kick: the organ was hit; run-end weight.
    Warn,          ///< Short low tone layered on a leak thump.
    Lament,        ///< Slow falling note for the defeat figure.
    // ---- Friendly attacks ----------------------------------------------------
    ShooterFire,   ///< Neutrophil round leaving the cell.
    Engulf,        ///< Macrophage pseudopod grabbing.
    Stab,          ///< Cytotoxic T latching on to a pathogen.
    MucusBurst,    ///< Goblet bomber's mucus splat.
    Knock,         ///< Fibroblast laying collagen.
    Zap,           ///< Legacy beam/chain/cone emitters (gym only).
    Sparkle,       ///< Complement cascade cast: one note of a rising run.
    Whoosh,        ///< Fever response cast.
    ClotThud,      ///< Fibrin clot cast.
    // ---- Friendly deaths -----------------------------------------------------
    CellDeath,     ///< A deployed cell bursting.
    TowerLost,     ///< A whole tower brought down.
    ScarBreak,     ///< A collagen wall chewed through.
    // ---- Horde attacks -------------------------------------------------------
    Latch,         ///< Virus grabbing a host membrane.
    Spit,          ///< Bacteria toxin pellet leaving.
    Sizzle,        ///< Toxin pellet landing on a friendly.
    Harpoon,       ///< Spore hose biting into a friendly.
    Glug,          ///< A slug of toxin arriving down the hose.
    Chew,          ///< The horde knocking a piece out of a wall.
    Count
};
inline constexpr u32 kSoundCount = static_cast<u32>(SoundId::Count);

/// A one-shot voice. Times are seconds, frequencies Hz, gains linear.
struct Patch {
    const char* name = "";
    Bus bus = Bus::Friendly;
    Priority priority = Priority::Normal;
    /// Live instances of this patch allowed at once; one more steals the
    /// oldest. The scene rate-limits too -- this is the mixer's backstop.
    u8 max_instances = 4;

    // Tone: oscillator 1, optionally phase-modulated, plus one partial.
    Wave wave = Wave::Sine;
    f32 freq = 440.0f;
    f32 tone = 1.0f;
    f32 partial_ratio = 0.0f;   ///< Second sine at freq*ratio; 0 = off.
    f32 partial_gain = 0.0f;
    f32 partial_decay = 0.0f;   ///< Its own decay; bells lose upper partials first.
    f32 fm_ratio = 0.0f;        ///< Phase-modulator frequency ratio; 0 = off.
    f32 fm_index = 0.0f;        ///< Modulation depth in turns at onset.
    f32 fm_decay = 0.05f;
    /// Pitch envelope: starts `pitch_semis` away and settles exponentially.
    f32 pitch_semis = 0.0f;
    f32 pitch_time = 0.02f;
    /// Linear glide, semitones per second (negative falls). The loss sounds.
    f32 pitch_drift = 0.0f;

    // Noise layer, mixed before the filter.
    f32 noise = 0.0f;
    f32 noise_decay = 0.0f;     ///< 0 = follows the amplitude envelope only.

    // Filter on the sum.
    FilterMode filter = FilterMode::Off;
    f32 cutoff = 8000.0f;
    f32 q = 0.707f;
    f32 cutoff_octaves = 0.0f;  ///< Sweep start, octaves above cutoff (negative = below).
    f32 cutoff_time = 0.05f;
    bool key_track = false;     ///< Cutoff follows the trigger's pitch.

    // Amplitude: linear attack, hold, exponential decay.
    f32 attack = 0.002f;
    f32 hold = 0.0f;
    f32 decay = 0.1f;
    f32 length = 0.5f;          ///< Hard upper bound on the voice's life.
    f32 gain = 0.5f;
    f32 reverb = 0.1f;          ///< Send level into the shared room.
};

const Patch& patch(SoundId id);
const char* sound_name(SoundId id);

/// Grains: tiny voices for events that arrive by the hundred. A grain is one
/// sine with an exponential pitch glide and a noise transient -- about a third
/// of a full voice's cost -- and is never filtered beyond a one-pole.
enum class GrainKind : u8 {
    Bubble = 0,  ///< Rising pitch: the classic bubble. Virus deaths.
    Pop,         ///< Falling pitch with body. Bacteria deaths.
    Squelch,     ///< Low, falling, noisy. Parasite deaths.
    Puff,        ///< Mostly noise, soft. Spore deaths.
    Tick,        ///< Very short high tap. Rounds landing.
    Count
};
inline constexpr u32 kGrainKindCount = static_cast<u32>(GrainKind::Count);

struct GrainShape {
    const char* name = "";
    Bus bus = Bus::Horde;
    f32 decay = 0.03f;         ///< Amplitude time constant.
    f32 glide_octaves = 0.0f;  ///< Total pitch travel over ~3 decays (+ rises).
    f32 noise = 0.0f;          ///< Transient level relative to the tone.
    f32 noise_decay = 0.004f;
    f32 noise_lowpass = 0.5f;  ///< One-pole coefficient: 0 = dark, 1 = bright.
    f32 reverb = 0.08f;
};

const GrainShape& grain_shape(GrainKind kind);

/// Sustained beds. The scene sets a target level; the mixer smooths to it.
enum class BedId : u8 {
    Murmur = 0,  ///< The horde's presence: soft low breathing, grows with the crowd on screen.
    Gnaw,        ///< Pathogens feeding on friendlies: a chewing texture.
    Fizz,        ///< Mass death beyond what discrete grains can carry.
    Count
};
inline constexpr u32 kBedCount = static_cast<u32>(BedId::Count);

// ---- Pitch ---------------------------------------------------------------------

/// D major pentatonic degrees as semitones above D. Index past the end wraps
/// into the next octave, so degree 7 is E an octave up.
inline constexpr i32 kPentatonic[5] = {0, 2, 4, 7, 9};
inline constexpr f32 kD2 = 73.416f;
inline constexpr f32 kD3 = 146.832f;
inline constexpr f32 kD4 = 293.665f;
inline constexpr f32 kD5 = 587.330f;
inline constexpr f32 kD6 = 1174.659f;

/// Semitones above the octave's D for a (possibly negative) scale degree.
inline i32 degree_semitones(i32 degree) {
    i32 octave = degree / 5;
    i32 d = degree % 5;
    if (d < 0) {
        d += 5;
        --octave;
    }
    return octave * 12 + kPentatonic[d];
}

} // namespace immune::audio
