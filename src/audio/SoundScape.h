// audio/SoundScape.h — what the game sounds like. Runs on the game thread,
// once per rendered frame, and turns that frame's happenings into a small,
// bounded set of mixer Commands.
//
// THE HORDE PROBLEM
// A busy wave raises hundreds of combat events a tick: deaths, shots, latches.
// One sound per event would mean hundreds of voices, a wall of noise, and an
// audio thread that cannot keep up. The approach is the one crowd-heavy games
// use (Wwise/FMOD call the pieces playback limits, virtual voices and
// crossfaded loops), in four layers:
//
//  1. AGGREGATE. Events are binned per category for the frame, never played
//     as they arrive. A category keeps a count and a few representative
//     positions (reservoir-sampled, so they are an unbiased spread).
//  2. RATE-LIMIT. Each category has a token bucket: a sustained rate and a
//     small burst. A frame plays at most min(events, tokens, per-frame cap)
//     sounds; the events it could not voice are not lost -- they raise the
//     loudness of the ones it did, by a fixed amount per doubling and capped.
//     Loudness therefore grows logarithmically with the crowd, the way a
//     real crowd's does, instead of linearly into clipping.
//  3. CULL BY AUDIBILITY. Positions are judged against the camera view. Off
//     screen fades to silence within 40% of a view width; only categories
//     that matter wherever they happen (a tower lost) keep a floor. Pan
//     follows screen x.
//  4. TEXTURE PAST THE LIMIT. When pathogens die faster than discrete pops
//     can carry, the pops thin out (each a little quieter) and a continuous
//     fizz bed fades in -- the audio LOD switch, like rain: individual drops
//     up close, a hiss in a downpour. Feeding pathogens and the size of the
//     crowd on screen drive two more beds the same way.
//
// Plus the hygiene that keeps a dense mix pleasant: random pitch choices from
// the pentatonic set (no machine-gun repeats), sub-frame onset jitter (twenty
// simultaneous pops become a crackle, not one click), and ducking under alerts.
//
// COST. One pass over the event span with a switch, a few reservoir writes
// per event, then a fixed loop over ~16 categories: tens of microseconds for
// thousands of events, zero allocations. At most kVoiceBudget voices and
// kGrainBudget grains are posted per frame whatever the horde does.
//
// Purely cosmetic: reads events and counts, never writes game state, and its
// RNG is its own. Detaching it cannot change a simulation result.
#pragma once

#include "audio/Mixer.h"
#include "audio/Patches.h"
#include "core/Types.h"
#include "sim/CombatEvents.h"

namespace immune::audio {

/// Discrete sounds the game asks for by name rather than through combat events.
enum class Cue : u8 {
    UiHover = 0,
    UiClick,
    UiDeny,
    UiOpen,
    UiClose,
    Deploy,     ///< variant = TowerType of the cell landing.
    Purchase,   ///< A Strengthen Immunity node bought.
    Ability,    ///< variant = game::AbilityId (0 complement, 1 fever, 2 clot).
    WaveStart,
    WaveClear,
    Victory,
    Defeat,
    Count
};
inline constexpr u32 kCueCount = static_cast<u32>(Cue::Count);
const char* cue_name(Cue c);

/// The continuous quantities the beds and alerts follow, sampled once a frame.
struct HordeState {
    /// The sim advanced this frame. Beds fade out while it is paused.
    bool live = false;
    /// Pathogens inside (or just outside) the view, and their mean pan.
    u32 visible = 0;
    f32 visible_pan = 0.0f;
    /// Pathogens riding a friendly right now (SimSnapshot::chaff_latched).
    u32 latched = 0;
    /// Cumulative leaks into the objective; each increase is a hit.
    u64 leaked_total = 0;
};

/// Counts the pathogens near `view` and their mean pan, from position
/// streams. Samples at most ~4096 agents (scaled back up), so it costs a few
/// microseconds at any horde size.
void measure_presence(const f32* x, const f32* y, usize count, const Rect& view, HordeState& out);

struct SceneStats {
    u64 events_seen = 0;
    u64 culled = 0;          ///< Too far off screen to hear.
    u64 folded = 0;          ///< Heard as loudness on another voice, not their own.
    u64 voices_posted = 0;
    u64 grains_posted = 0;
    u64 posts_dropped = 0;   ///< The ring was full.
    f32 death_rate = 0.0f;   ///< Smoothed audible pathogen deaths per second.
    u32 last_frame_voices = 0;
    u32 last_frame_grains = 0;
};

class SoundScape {
public:
    /// New voices / grains one frame may post, across every category.
    static constexpr u32 kVoiceBudget = 12;
    static constexpr u32 kGrainBudget = 16;

    SoundScape();

    /// Where commands go. nullptr keeps the bookkeeping running but posts
    /// nothing (no device, or muted at the source).
    void set_output(CommandRing* ring) { out_ = ring; }
    /// The camera's visible world rectangle: the listener.
    void set_view(const Rect& visible_world);

    /// Folds one frame's combat events into the frame's tallies.
    void on_combat_events(const sim::CombatEvent* events, usize count);
    /// A named sound, centred (UI) or placed in the world.
    void cue(Cue c, u8 variant = 0);
    void cue_at(Cue c, Vec2 world, u8 variant = 0);

    /// Ends the frame: spends budgets, posts voices, updates beds.
    /// `dt` is real (unscaled) seconds since the last call.
    void update(const HordeState& horde, f32 dt);

    /// Level change: forgets tallies and leak baselines, fades everything.
    void reset();

    const SceneStats& stats() const { return stats_; }

private:
    struct Rep {
        f32 pan = 0.0f;
        f32 audibility = 0.0f;
        f32 size = 1.0f;
    };
    static constexpr u32 kReps = 4;
    static constexpr u32 kDeathReps = 8;
    struct Tally {
        u32 count = 0;          // audible events this frame
        u32 seen = 0;           // all events this frame, audible or not
        Rep reps[kReps];
        f32 tokens = 0.0f;
        f32 backlog = 0.0f;     // recent events folded into nothing yet
    };
    struct DeathTally {
        u32 count = 0;
        u32 seen = 0;
        Rep reps[kDeathReps];
    };

    f32 audibility(Vec2 world, f32* pan) const;
    void tally(u32 category, Vec2 world, f32 size = 1.0f);
    void post(const Command& c);
    void voice(SoundId id, f32 gain, f32 pitch, f32 pan, f32 delay);
    void grain(GrainKind kind, f32 hz, f32 gain, f32 pan, f32 delay, f32 decay_scale = 1.0f);
    void bed(BedId id, f32 level, f32 pan, f32 activity);
    void play_cue(Cue c, f32 pan, u8 variant);
    void spend_categories(f32 dt);
    void spend_deaths(f32 dt);
    u32 rand_u32();
    f32 rand01();

    CommandRing* out_ = nullptr;
    Rect view_{Vec2{-50.0f, -50.0f}, Vec2{50.0f, 50.0f}};
    Vec2 view_center_{0.0f, 0.0f};
    Vec2 view_half_{50.0f, 50.0f};
    u32 rng_ = 0xC0FFEE11u;
    f32 frame_dt_ = 1.0f / 60.0f;

    Tally tallies_[32]{};
    DeathTally deaths_[kFamilyCount]{};
    f32 death_tokens_ = 0.0f;
    f32 latch_pan_ = 0.0f;
    f32 leak_tokens_ = 2.0f;
    u64 leaked_seen_ = 0;
    bool leak_baseline_ = false;
    u32 leaks_pending_ = 0;
    f32 cue_cooldown_[kCueCount]{};
    u32 frame_voices_ = 0;
    u32 frame_grains_ = 0;
    SceneStats stats_{};
};

} // namespace immune::audio
