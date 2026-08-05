// audio/AudioInternal.h — implementation details for Wave 3C's AudioEngine.
//
// NOT part of the frozen contract (Audio.h is). This header exists so the
// mixer logic (ring buffer, voice pool, synth patches, priority stealing,
// pan/envelope math) can be:
//   (a) shared between the real SDL callback path and the null-device
//       fallback path in Audio.cpp, and
//   (b) unit-tested directly (tests/test_audio.cpp includes this header) —
//       Audio.h's public surface alone gives no way to observe voice-pool
//       or ring-buffer internals, so white-box testing needs this.
//
// Everything here is header-only (`inline`/`constexpr`) so no new .cpp file
// needs to be added to CMakeLists.txt's immune_audio target list.
//
// Threading contract this file is written to satisfy:
//   - EventRing::push/pop are lock-free (atomic indices only), safe for
//     exactly one producer + one consumer thread.
//   - trigger_event / pump / render_block touch only a pre-allocated Mixer
//     (fixed-size arrays) and atomics: no heap allocation, no locks, no
//     exceptions, no calls into sim code. Safe to call from the SDL audio
//     callback thread.
//   - The *patch table* is a `constexpr` array (not a function-local static
//     behind a lazy-init guard) specifically so there is no possibility of
//     a hidden compiler-generated init lock firing the first time the audio
//     thread reads it.
#pragma once

#include "audio/Audio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace immune::audio::internal {

// ---------------------------------------------------------------------
// SPSC ring buffer of AudioEvent. Fixed capacity, power-of-two sized so
// index masking is a cheap AND. Overflow drops the new event (caller is
// expected to bump AudioEngine::dropped_).
// ---------------------------------------------------------------------
constexpr u32 kRingCapacity = 256; // power of two
constexpr u32 kRingMask = kRingCapacity - 1;

struct EventRing {
    std::array<AudioEvent, kRingCapacity> slots{};
    std::atomic<u32> write_idx{0};
    std::atomic<u32> read_idx{0};

    /// Producer-only. Returns false (and leaves the ring untouched) if full.
    bool push(const AudioEvent& ev) {
        const u32 w = write_idx.load(std::memory_order_relaxed);
        const u32 r = read_idx.load(std::memory_order_acquire);
        if (w - r >= kRingCapacity) return false;
        slots[w & kRingMask] = ev;
        write_idx.store(w + 1, std::memory_order_release);
        return true;
    }

    /// Consumer-only. Returns false if empty.
    bool pop(AudioEvent& out) {
        const u32 r = read_idx.load(std::memory_order_relaxed);
        const u32 w = write_idx.load(std::memory_order_acquire);
        if (r == w) return false;
        out = slots[r & kRingMask];
        read_idx.store(r + 1, std::memory_order_release);
        return true;
    }

    u32 size() const {
        return write_idx.load(std::memory_order_acquire) - read_idx.load(std::memory_order_acquire);
    }
    static constexpr u32 capacity() { return kRingCapacity; }
};

// ---------------------------------------------------------------------
// Mixing priority tiers (DESIGN.md sec 10: telegraph/warning cues must
// always be audible at max voice pressure; the ambient bed is ducked
// first). Protected voices are never stolen, full stop.
// ---------------------------------------------------------------------
enum class Priority : u8 { Ambient = 0, Normal = 1, High = 2, Protected = 3 };

// ---------------------------------------------------------------------
// Per-SoundId synth patch. Exact frequencies/envelopes are Wave 3C's
// creative call (DESIGN.md does not specify them); the *categories* are
// the requirement: TowerFire/Ability mechanical & short, ChaffDissolve a
// sustained density-scaled bed, Elite/Boss/Critical significant, UI cues
// short and unobtrusive.
// ---------------------------------------------------------------------
struct SynthPatch {
    VoiceKind kind;
    Priority priority;
    f32 base_freq;   // Hz
    f32 duration;    // seconds; <= 0 => sustained (bed-style voice, e.g. ChaffDissolve)
    f32 attack;      // seconds
    f32 filter;      // 0..1 brightness/noise-mix knob, meaning depends on `kind`
    f32 base_gain;   // patch's own trim, multiplied with the posted event's gain
};

inline constexpr std::array<SynthPatch, static_cast<usize>(SoundId::Count)> kPatchTable = {{
    /*None*/              SynthPatch{VoiceKind::Sine,        Priority::Ambient,   0.0f,    0.01f, 0.0f,  0.0f, 0.0f},
    /*TowerPlace*/         SynthPatch{VoiceKind::Sine,        Priority::Normal,    523.25f, 0.14f, 0.002f,0.5f, 0.55f},
    /*TowerUpgrade*/       SynthPatch{VoiceKind::Fm,          Priority::Normal,    440.0f,  0.28f, 0.004f,0.6f, 0.6f},
    /*TowerSell*/          SynthPatch{VoiceKind::Subtractive, Priority::Normal,    300.0f,  0.20f, 0.002f,0.4f, 0.5f},
    /*TowerFire*/          SynthPatch{VoiceKind::Noise,       Priority::Normal,    900.0f,  0.045f,0.001f,0.7f, 0.35f},
    /*TowerAbility*/       SynthPatch{VoiceKind::Fm,          Priority::High,      220.0f,  0.32f, 0.003f,0.55f,0.7f},
    /*ChaffDissolve*/      SynthPatch{VoiceKind::Granular,    Priority::Ambient,   120.0f,  0.0f,  0.15f, 0.3f, 0.5f},
    /*EliteSpawn*/         SynthPatch{VoiceKind::Fm,          Priority::High,      180.0f,  0.42f, 0.02f, 0.5f, 0.75f},
    /*EliteDeath*/         SynthPatch{VoiceKind::Noise,       Priority::High,      260.0f,  0.5f,  0.005f,0.65f,0.85f},
    /*BossTelegraph*/      SynthPatch{VoiceKind::Fm,          Priority::Protected, 90.0f,   1.1f,  0.05f, 0.8f, 0.95f},
    /*WaveStart*/          SynthPatch{VoiceKind::Sine,        Priority::High,      392.0f,  0.5f,  0.01f, 0.4f, 0.65f},
    /*WaveClear*/          SynthPatch{VoiceKind::Fm,          Priority::High,      587.33f, 0.6f,  0.01f, 0.45f,0.7f},
    /*ObjectiveHit*/       SynthPatch{VoiceKind::Subtractive, Priority::High,      150.0f,  0.18f, 0.001f,0.6f, 0.75f},
    /*ObjectiveCritical*/  SynthPatch{VoiceKind::Fm,          Priority::Protected, 660.0f,  0.55f, 0.005f,0.85f,1.0f},
    /*HistamineNova*/      SynthPatch{VoiceKind::Noise,       Priority::High,      500.0f,  0.35f, 0.005f,0.7f, 0.7f},
    /*ComplementCascade*/  SynthPatch{VoiceKind::Fm,          Priority::High,      330.0f,  0.5f,  0.01f, 0.5f, 0.7f},
    /*NetDeploy*/          SynthPatch{VoiceKind::Subtractive, Priority::Normal,    200.0f,  0.22f, 0.005f,0.45f,0.55f},
    /*UiClick*/            SynthPatch{VoiceKind::Sine,        Priority::Ambient,   1200.0f, 0.035f,0.001f,0.2f, 0.25f},
    /*UiInvalid*/          SynthPatch{VoiceKind::Sine,        Priority::Ambient,   160.0f,  0.09f, 0.001f,0.3f, 0.3f},
}};

inline const SynthPatch& patch_for(SoundId id) {
    const usize idx = static_cast<usize>(id);
    if (idx >= kPatchTable.size()) return kPatchTable[0];
    return kPatchTable[idx];
}

// ---------------------------------------------------------------------
// One voice-pool slot.
// ---------------------------------------------------------------------
struct Voice {
    bool active = false;
    SoundId source = SoundId::None;
    Priority priority = Priority::Ambient;
    VoiceKind kind = VoiceKind::Sine;

    f32 freq = 440.0f;
    f32 age = 0.0f;
    f32 duration = 0.0f; // <= 0 => sustained
    f32 attack = 0.0f;
    f32 filter = 0.5f;

    f32 gain = 0.0f;        // current (smoothed/enveloped) output gain
    f32 target_gain = 0.0f; // desired gain, from the posted event
    f32 pan = 0.0f;         // -1..1, computed each pump() from listener x
    Vec2 position{0.0f, 0.0f};

    f32 sustain_silence_timer = 0.0f; // sustained voices: seconds since last refresh
    f32 phase = 0.0f;                 // oscillator / filter state
    u32 rng_state = 0x9e3779b9u;      // per-voice xorshift seed
};

constexpr u32 kMaxVoiceSlots = 128; // hard cap; AudioConfig::max_voices clamps into this

// ---------------------------------------------------------------------
// Owns the whole mixer's mutable state. Heap-allocated once by
// AudioEngine::init() (game thread — allocation there is fine) and then
// only ever touched by exactly one of {game thread via update()/pump(),
// SDL callback thread}, never both for the same AudioEngine — see
// Audio.cpp for how that single-consumer invariant is kept.
// ---------------------------------------------------------------------
struct Mixer {
    EventRing ring;
    std::array<Voice, kMaxVoiceSlots> voices{};
    u32 voice_limit = 64;
    u32 sample_rate = 48000;
    i32 chaff_bed_slot = -1; // index of the persistent ChaffDissolve bed voice, or -1
    u64 trigger_count = 0;

    // Written by update() (game thread), read by the audio callback thread.
    std::atomic<f32> listener_x{0.0f};
    std::atomic<f32> music_intensity{0.0f}; // smoothed 0..1
    std::atomic<f32> master_gain{0.8f};
    std::atomic<bool> paused{false};

    f32 music_phase[3] = {0.0f, 0.0f, 0.0f};
};

// ---------------------------------------------------------------------
// Pure helpers — unit-testable in isolation, no Mixer required.
// ---------------------------------------------------------------------
inline f32 finite_or(f32 v, f32 fallback) { return std::isfinite(v) ? v : fallback; }

/// Screen-space x-pan. `half_width` approximates half the visible world
/// width in the units `position`/`listener_center` use; Audio.h's update()
/// only receives a listener center (no viewport width), so this constant is
/// a Wave 3C judgment call, not a spec requirement.
inline f32 compute_pan(f32 voice_x, f32 listener_x, f32 half_width = 40.0f) {
    const f32 vx = finite_or(voice_x, 0.0f);
    const f32 lx = finite_or(listener_x, 0.0f);
    const f32 hw = half_width > 1e-4f ? half_width : 1e-4f;
    return std::clamp((vx - lx) / hw, -1.0f, 1.0f);
}

/// Linear attack/decay envelope multiplier, 0..1, for a one-shot voice.
inline f32 envelope_gain(const Voice& v) {
    if (v.duration <= 0.0f) {
        if (v.attack > 0.0f && v.age < v.attack) return std::clamp(v.age / v.attack, 0.0f, 1.0f);
        return 1.0f;
    }
    if (v.attack > 0.0f && v.age < v.attack) return std::clamp(v.age / v.attack, 0.0f, 1.0f);
    const f32 decay_len = std::max(v.duration - v.attack, 1e-4f);
    const f32 t = std::clamp((v.age - v.attack) / decay_len, 0.0f, 1.0f);
    return 1.0f - t;
}

inline bool voice_finished(const Voice& v) { return v.duration > 0.0f && v.age >= v.duration; }

/// Priority-aware allocation: prefer a free slot; otherwise steal the
/// lowest-priority active voice (tie-break: oldest), never a Protected one.
/// Returns -1 if no slot is available (pool full of Protected voices, or
/// voice_limit == 0).
inline i32 find_free_or_steal(Mixer& m) {
    for (u32 i = 0; i < m.voice_limit; ++i)
        if (!m.voices[i].active) return static_cast<i32>(i);

    i32 best = -1;
    Priority best_prio = Priority::Protected;
    f32 best_age = -1.0f;
    for (u32 i = 0; i < m.voice_limit; ++i) {
        const Voice& v = m.voices[i];
        if (!v.active || v.priority == Priority::Protected) continue;
        if (v.priority < best_prio || (v.priority == best_prio && v.age > best_age)) {
            best = static_cast<i32>(i);
            best_prio = v.priority;
            best_age = v.age;
        }
    }
    return best;
}

/// Consumes one posted event: refreshes the persistent ChaffDissolve bed in
/// place, or allocates/steals a slot and initializes a fresh voice.
inline void trigger_event(Mixer& m, const AudioEvent& ev) {
    if (ev.id == SoundId::None || ev.id >= SoundId::Count) return;
    const SynthPatch& patch = patch_for(ev.id);
    const f32 gain = std::clamp(finite_or(ev.gain, 1.0f), 0.0f, 4.0f);
    const f32 intensity = std::clamp(finite_or(ev.intensity, 1.0f), 0.0f, 1.0f);
    const f32 pitch = std::clamp(finite_or(ev.pitch, 1.0f), 0.1f, 4.0f);
    const Vec2 pos{finite_or(ev.position.x, 0.0f), finite_or(ev.position.y, 0.0f)};

    if (ev.id == SoundId::ChaffDissolve) {
        // Density-scaled ambient bed: one persistent voice, refreshed in
        // place rather than one voice per event (AudioEvent::intensity is
        // documented for exactly this).
        if (m.chaff_bed_slot >= 0 && static_cast<u32>(m.chaff_bed_slot) < m.voice_limit) {
            Voice& v = m.voices[m.chaff_bed_slot];
            if (v.active && v.source == SoundId::ChaffDissolve) {
                v.target_gain = gain * intensity * patch.base_gain;
                v.freq = patch.base_freq * pitch;
                v.position = pos;
                v.sustain_silence_timer = 0.0f;
                return;
            }
        }
        const i32 slot = find_free_or_steal(m);
        if (slot < 0) return;
        Voice& v = m.voices[static_cast<u32>(slot)];
        v = Voice{};
        v.active = true;
        v.source = ev.id;
        v.priority = patch.priority;
        v.kind = patch.kind;
        v.freq = patch.base_freq * pitch;
        v.duration = 0.0f;
        v.attack = patch.attack;
        v.filter = patch.filter;
        v.target_gain = gain * intensity * patch.base_gain;
        v.position = pos;
        v.rng_state = 0x9e3779b9u ^ (static_cast<u32>(m.trigger_count) * 2654435761u + 1u);
        m.chaff_bed_slot = slot;
        ++m.trigger_count;
        return;
    }

    const i32 slot = find_free_or_steal(m);
    if (slot < 0) return;
    Voice& v = m.voices[static_cast<u32>(slot)];
    v = Voice{};
    v.active = true;
    v.source = ev.id;
    v.priority = patch.priority;
    v.kind = patch.kind;
    v.freq = patch.base_freq * pitch;
    v.duration = std::max(patch.duration, 0.01f);
    v.attack = std::min(patch.attack, v.duration * 0.5f);
    v.filter = patch.filter;
    v.target_gain = gain * patch.base_gain;
    v.gain = v.target_gain * envelope_gain(v);
    v.position = pos;
    v.rng_state = 0x9e3779b9u ^ (static_cast<u32>(m.trigger_count) * 2654435761u + 1u);
    ++m.trigger_count;
}

/// Drains the ring buffer and advances every active voice by `dt` seconds:
/// envelopes decay, the ChaffDissolve bed fades out if unrefreshed, and pan
/// is recomputed from the last listener position update() wrote. Must be
/// called from exactly one thread for a given Mixer (see Audio.cpp).
inline void pump(Mixer& m, f32 dt) {
    dt = std::clamp(finite_or(dt, 0.0f), 0.0f, 1.0f);

    AudioEvent ev;
    while (m.ring.pop(ev)) trigger_event(m, ev);

    const f32 listener_x = m.listener_x.load(std::memory_order_relaxed);
    for (u32 i = 0; i < m.voice_limit; ++i) {
        Voice& v = m.voices[i];
        if (!v.active) continue;
        v.age += dt;
        v.pan = compute_pan(v.position.x, listener_x);

        if (v.duration <= 0.0f) {
            v.sustain_silence_timer += dt;
            const f32 smoothing = dt > 0.0f ? std::clamp(dt * 6.0f, 0.0f, 1.0f) : 0.0f;
            v.gain += (v.target_gain - v.gain) * smoothing;
            if (v.sustain_silence_timer > 0.5f) v.target_gain = 0.0f;
            if (v.sustain_silence_timer > 1.5f && v.gain < 0.001f) {
                v.active = false;
                if (m.chaff_bed_slot == static_cast<i32>(i)) m.chaff_bed_slot = -1;
            }
        } else {
            v.gain = v.target_gain * envelope_gain(v);
            if (voice_finished(v)) v.active = false;
        }
    }
}

/// Called once per rendered frame regardless of device mode: stores the
/// listener position and smooths the adaptive-music intensity target.
inline void update_listener_and_music(Mixer& m, Vec2 listener_center, f32 horde_intensity01, f32 dt) {
    m.listener_x.store(finite_or(listener_center.x, 0.0f), std::memory_order_relaxed);

    const f32 target = std::clamp(finite_or(horde_intensity01, 0.0f), 0.0f, 1.0f);
    const f32 clamped_dt = std::clamp(finite_or(dt, 0.0f), 0.0f, 1.0f);
    // Framerate-independent exponential approach, ~0.4s time constant: the
    // pad audibly swells/releases within roughly a second of a density
    // change (DESIGN.md sec 10's "rising tension .. releasing after a wave
    // clears"), rather than snapping instantly or lagging for many seconds.
    constexpr f32 kTimeConstant = 0.4f;
    const f32 rate = 1.0f - std::exp(-clamped_dt / kTimeConstant);
    const f32 cur = m.music_intensity.load(std::memory_order_relaxed);
    m.music_intensity.store(std::clamp(cur + (target - cur) * rate, 0.0f, 1.0f), std::memory_order_relaxed);
}

/// Cheap deterministic-enough noise generator for the audio thread (no
/// <random> heap state; this is sound, not sim, so it has no bearing on
/// DESIGN.md's determinism/state_hash rule).
inline f32 xorshift_sample(u32& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (static_cast<f32>(state & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu)) * 2.0f - 1.0f;
}

inline constexpr f32 kTau = 6.2831853f;

inline f32 synth_sample(Voice& v, f32 sample_dt) {
    switch (v.kind) {
        case VoiceKind::Sine: {
            v.phase += v.freq * sample_dt;
            v.phase -= std::floor(v.phase);
            return std::sin(v.phase * kTau);
        }
        case VoiceKind::Fm: {
            v.phase += v.freq * sample_dt;
            v.phase -= std::floor(v.phase);
            const f32 mod = std::sin(v.phase * kTau * 2.0f) * v.filter * 3.0f;
            return std::sin(v.phase * kTau + mod);
        }
        case VoiceKind::Noise:
            return xorshift_sample(v.rng_state) * (0.3f + v.filter * 0.7f);
        case VoiceKind::Subtractive: {
            const f32 n = xorshift_sample(v.rng_state);
            v.phase = v.phase * (1.0f - v.filter) + n * v.filter; // 1-pole lowpass, `phase` doubles as filter state
            return v.phase;
        }
        case VoiceKind::Granular: {
            v.phase += v.freq * sample_dt;
            v.phase -= std::floor(v.phase);
            const f32 tone = std::sin(v.phase * kTau) * 0.4f;
            const f32 grain = xorshift_sample(v.rng_state) * 0.6f;
            return tone + grain * v.filter;
        }
    }
    return 0.0f;
}

/// Real PCM synthesis for the SDL callback path only (never invoked in
/// null_device mode — see Audio.cpp). Writes interleaved stereo float32.
inline void render_block(Mixer& m, float* out, u32 frames, u32 sample_rate) {
    for (u32 i = 0; i < frames * 2; ++i) out[i] = 0.0f;
    if (sample_rate == 0 || m.paused.load(std::memory_order_relaxed)) return;

    const f32 master = std::clamp(m.master_gain.load(std::memory_order_relaxed), 0.0f, 4.0f);
    const f32 sample_dt = 1.0f / static_cast<f32>(sample_rate);

    for (u32 vi = 0; vi < m.voice_limit; ++vi) {
        Voice& v = m.voices[vi];
        if (!v.active || v.gain <= 0.0002f) continue;
        const f32 pan = std::clamp(v.pan, -1.0f, 1.0f);
        const f32 lg = std::clamp(0.5f - pan * 0.5f, 0.0f, 1.0f);
        const f32 rg = std::clamp(0.5f + pan * 0.5f, 0.0f, 1.0f);
        for (u32 s = 0; s < frames; ++s) {
            const f32 sample = synth_sample(v, sample_dt) * v.gain * master;
            out[s * 2 + 0] += sample * lg;
            out[s * 2 + 1] += sample * rg;
        }
    }

    // Adaptive intensity music: a small stack of detuned sine layers whose
    // gain and top-layer frequency track horde_intensity01, crossfading the
    // texture rather than switching discrete stems (Wave 3C's choice; the
    // header only requires "audibly responds to intensity changing").
    const f32 intensity = m.music_intensity.load(std::memory_order_relaxed);
    const f32 music_gain = 0.12f + intensity * 0.18f;
    const f32 freqs[3] = {55.0f, 82.5f, 55.0f * (1.5f + intensity)};
    const f32 layer_gain[3] = {0.5f, 0.25f, 0.25f};
    for (u32 s = 0; s < frames; ++s) {
        f32 mix = 0.0f;
        for (int layer = 0; layer < 3; ++layer) {
            m.music_phase[layer] += freqs[layer] * sample_dt;
            m.music_phase[layer] -= std::floor(m.music_phase[layer]);
            mix += std::sin(m.music_phase[layer] * kTau) * layer_gain[layer];
        }
        mix *= music_gain * master;
        out[s * 2 + 0] += mix;
        out[s * 2 + 1] += mix;
    }

    for (u32 i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

} // namespace immune::audio::internal
