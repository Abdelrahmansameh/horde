// audio/Audio.h — procedural synthesis and mixing. FROZEN CONTRACT.
// Owner: Wave 3C.
//
// RATIONALE
// Zero binary assets is a locked project decision, so there are no WAV files:
// every sound is synthesized at runtime from noise/FM/subtractive voices. The
// SDL2 audio callback runs on its own thread and must never allocate, lock, or
// call into the sim — gameplay pushes immutable AudioEvent values through a
// lock-free ring buffer and the mixer picks them up.
#pragma once

#include "core/Types.h"

#include <string>

namespace immune::audio {

/// Gameplay-meaningful sound events. The synth patch for each lives in Wave 3C.
enum class SoundId : u16 {
    None = 0,
    TowerPlace, TowerUpgrade, TowerSell, TowerFire, TowerAbility,
    ChaffDissolve,          ///< Mass erosion; density-scaled, not per-agent.
    EliteSpawn, EliteDeath, BossTelegraph,
    WaveStart, WaveClear, ObjectiveHit, ObjectiveCritical,
    HistamineNova, ComplementCascade, NetDeploy,
    UiClick, UiInvalid,
    Count
};

/// Voice families the synth exposes.
enum class VoiceKind : u8 { Noise = 0, Sine, Fm, Subtractive, Granular };

struct AudioEvent {
    SoundId id = SoundId::None;
    /// World position; the mixer pans by screen-space x. NaN-free required.
    Vec2 position{0.0f, 0.0f};
    f32 gain = 1.0f;
    f32 pitch = 1.0f;
    /// Density/intensity 0..1. Mass events (ChaffDissolve) scale one voice by
    /// this rather than triggering one voice per agent.
    f32 intensity = 1.0f;
};

struct AudioConfig {
    i32 sample_rate = 48000;
    i32 buffer_frames = 512;
    u32 max_voices = 64;
    f32 master_gain = 0.8f;
    f32 music_gain = 0.6f;
    f32 sfx_gain = 1.0f;
    /// Headless modes open a null device: the mixer still runs its logic (so
    /// event plumbing is exercised) but nothing reaches the OS.
    bool null_device = false;
};

class AudioEngine {
public:
    bool init(const AudioConfig& config);
    void shutdown();
    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }

    /// Non-blocking, safe from the game thread. Dropped if the queue is full;
    /// dropping a sound is always preferable to stalling the sim.
    void post(const AudioEvent& event);

    /// Called once per rendered frame from the game thread. Updates listener
    /// position and the adaptive music intensity layer.
    void update(Vec2 listener_center, f32 horde_intensity01, f32 dt);

    void set_master_gain(f32 g);
    void set_paused(bool paused);

    /// Number of events dropped because the queue was full — a Wave 3C
    /// regression signal.
    u64 dropped_events() const { return dropped_; }

private:
    AudioConfig config_{};
    bool ready_ = false;
    u64 dropped_ = 0;
    std::string error_;
};

} // namespace immune::audio
