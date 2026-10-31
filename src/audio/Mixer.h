// audio/Mixer.h — the synthesizer and mix bus. SDL-free, so it is driven the
// same way by the device callback, the offline renderer and the tests.
//
// THREADING
// The game thread never touches a Mixer. It pushes small immutable Commands
// into a CommandRing (single producer, single consumer); whoever renders --
// SDL's audio thread in the game -- drains the ring at the top of each block
// and then calls render(). render() never allocates, locks, logs or reads
// game state: every pool here is a fixed array sized at compile time, and the
// reverb lines are allocated once in init().
//
// WHAT IS IN A FRAME OF AUDIO
//   voices  64 one-shot patch voices (audio/Patches.h), priority-stolen.
//   grains  96 tiny blips for events that come by the hundred (horde deaths,
//           rounds landing). About a third of a voice's cost each.
//   beds    3 sustained textures whose level follows a game quantity.
//   buses   UI / Alert / Friendly / Horde. An Alert voice ducks the two
//           combat buses so a leak or a wave horn is never buried.
//   room    a small Freeverb-style reverb every voice sends into.
//   master  volume, a peak compressor, then a soft clipper: the output never
//           exceeds +-1 however many sounds pile up.
//
// The pools are the hard ceiling on cost: a worst-case frame renders at most
// 64 voices + 96 grains + 3 beds, whatever the horde does. The scene layer
// (audio/SoundScape.h) is what keeps the pools from being needed.
#pragma once

#include "audio/Dsp.h"
#include "audio/Patches.h"

#include <array>
#include <atomic>
#include <vector>

namespace immune::audio {

enum class CommandType : u8 {
    None = 0,
    Voice,       ///< id = SoundId, pitch = frequency multiplier.
    Grain,       ///< id = GrainKind, pitch = frequency in Hz, param = decay multiplier.
    Bed,         ///< id = BedId, gain = target level 0..1, pan, param = activity 0..1.
    MasterGain,  ///< gain = target master volume (0 mutes; smoothed).
    StopAll,     ///< Fades every voice, grain and bed out over a few ms.
};

/// One instruction for the mixer. Trivially copyable; 24 bytes.
struct Command {
    CommandType type = CommandType::None;
    u8 id = 0;
    /// Seconds to wait after the mixer picks the command up. The scene uses
    /// it to spread a frame's worth of simultaneous events across the frame,
    /// so twenty deaths on one tick become a crackle rather than one loud click.
    f32 delay = 0.0f;
    f32 gain = 1.0f;
    f32 pitch = 1.0f;
    f32 pan = 0.0f;   ///< -1 left .. +1 right.
    f32 param = 1.0f;
};

/// Lock-free single-producer/single-consumer queue of Commands. Exactly one
/// thread may push and exactly one may pop. A full ring drops the newcomer
/// and counts it: losing a sound is always better than stalling a frame.
class CommandRing {
public:
    static constexpr u32 kCapacity = 2048;  // power of two

    bool push(const Command& c) {
        const u32 head = head_.load(std::memory_order_relaxed);
        const u32 tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= kCapacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buffer_[head & (kCapacity - 1)] = c;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(Command& out) {
        const u32 tail = tail_.load(std::memory_order_relaxed);
        const u32 head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;
        out = buffer_[tail & (kCapacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    u32 size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    u64 dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    std::array<Command, kCapacity> buffer_{};
    // Not cache-line separated: at a few hundred commands a second, false
    // sharing between the two indices costs nothing measurable.
    std::atomic<u32> head_{0};
    std::atomic<u32> tail_{0};
    std::atomic<u64> dropped_{0};
};

struct MixerStats {
    u32 active_voices = 0;
    u32 active_grains = 0;
    u32 peak_voices = 0;
    u32 peak_grains = 0;
    u64 voices_started = 0;
    u64 voices_stolen = 0;    ///< Cut early: instance cap or a full pool.
    u64 voices_rejected = 0;  ///< Refused: the pool was full of higher priorities.
    u64 grains_started = 0;
    u64 grains_stolen = 0;
    f32 peak = 0.0f;          ///< Largest |sample| written since reset_peak().
    /// Lowest compressor gain since reset_peak(); 1 = it never engaged.
    f32 limiter_gain = 1.0f;
    u64 frames_rendered = 0;
};

class Mixer {
public:
    static constexpr u32 kMaxVoices = 64;
    static constexpr u32 kMaxGrains = 96;
    /// Internal processing block; render() loops over longer requests.
    static constexpr u32 kBlock = 256;

    /// Allocates the reverb lines and resets every pool. Not real-time safe;
    /// call before the device starts pulling.
    void init(f32 sample_rate);
    f32 sample_rate() const { return sample_rate_; }

    void apply(const Command& c);
    /// Applies every command waiting in `ring`. Returns how many.
    u32 drain(CommandRing& ring);

    /// Writes `frames` interleaved stereo float samples, overwriting `out`.
    void render(f32* out, u32 frames);

    const MixerStats& stats() const { return stats_; }
    void reset_peak() {
        stats_.peak = 0.0f;
        stats_.limiter_gain = 1.0f;
    }

    // ---- Introspection for tests and the offline renderer -------------------
    u32 instances(SoundId id) const;
    f32 bed_level(BedId id) const { return beds_[static_cast<usize>(id)].level; }

private:
    struct Voice {
        bool active = false;
        bool stolen = false;
        SoundId id = SoundId::UiHover;
        u8 stage = 0;           // 0 attack, 1 hold, 2 decay
        u32 delay = 0;
        u32 hold = 0;
        u32 remaining = 0;
        u64 started = 0;
        f32 amp = 0.0f, attack_inc = 1.0f, decay_k = 0.0f;
        f32 ph1 = 0.0f, ph2 = 0.0f, phm = 0.0f;
        f32 ratio = 1.0f;
        f32 pitch_env = 1.0f, pitch_k = 0.0f;
        f32 fm_env = 1.0f, fm_k = 0.0f;
        f32 cut_env = 1.0f, cut_k = 0.0f;
        f32 partial_env = 1.0f, partial_k = 1.0f;
        f32 noise_env = 1.0f, noise_k = 1.0f;
        f32 t = 0.0f;
        f32 gain = 0.0f, gl = 0.0f, gr = 0.0f, send = 0.0f;
        f32 mix_low = 0.0f, mix_band = 0.0f, mix_high = 0.0f;
        u32 rng = 1;
        dsp::Svf svf;
    };

    struct Grain {
        bool active = false;
        u8 bus = 0;
        u32 delay = 0;
        u32 remaining = 0;
        f32 ph = 0.0f, inc = 0.0f, inc_mul = 1.0f;
        f32 amp = 0.0f, amp_k = 0.0f;
        f32 namp = 0.0f, namp_k = 0.0f, nlp = 0.0f, nlp_a = 0.5f;
        f32 gl = 0.0f, gr = 0.0f, send = 0.0f;
        u32 rng = 1;
    };

    struct Bed {
        f32 level = 0.0f, target = 0.0f;
        f32 pan = 0.0f, pan_target = 0.0f;
        f32 activity = 0.0f;
        f32 level_k = 0.0f;
        f32 lfo1 = 0.0f, lfo2 = 0.0f;
        f32 env = 0.0f, env_k = 0.0f;
        u32 next_pulse = 1;
        u32 rng = 1;
        dsp::Svf f1, f2;
    };

    struct Comb {
        std::vector<f32> line;
        u32 index = 0;
        f32 store = 0.0f;
        f32 process(f32 in, f32 feedback, f32 damp) {
            const f32 out = line[index];
            store = out * (1.0f - damp) + store * damp;
            line[index] = in + store * feedback;
            if (++index >= line.size()) index = 0;
            return out;
        }
    };

    struct Allpass {
        std::vector<f32> line;
        u32 index = 0;
        f32 process(f32 in) {
            const f32 delayed = line[index];
            line[index] = in + delayed * 0.5f;
            if (++index >= line.size()) index = 0;
            return delayed - in;
        }
    };

    void start_voice(const Command& c);
    void start_grain(const Command& c);
    void render_block(f32* out, u32 n);
    void render_voice(Voice& v, u32 n);
    void render_grain(Grain& g, u32 n);
    void render_bed(BedId id, Bed& b, u32 n);

    f32 sample_rate_ = 48000.0f;
    f32 inv_sr_ = 1.0f / 48000.0f;
    u64 clock_ = 0;
    u32 rng_ = 0x9E3779B9u;

    std::array<Voice, kMaxVoices> voices_{};
    std::array<Grain, kMaxGrains> grains_{};
    std::array<Bed, kBedCount> beds_{};

    f32 bus_l_[kBusCount][kBlock] = {};
    f32 bus_r_[kBusCount][kBlock] = {};
    f32 send_[kBlock] = {};

    std::array<Comb, 8> combs_{};       // 4 left, 4 right
    std::array<Allpass, 4> allpasses_{};  // 2 left, 2 right

    f32 master_ = 0.0f, master_target_ = 0.8f, master_k_ = 0.0f;
    f32 duck_ = 0.0f, duck_k_ = 0.0f;
    f32 kill_k_ = 0.0f;
    f32 comp_env_ = 0.0f, comp_attack_k_ = 0.0f, comp_release_k_ = 0.0f;

    MixerStats stats_{};
};

} // namespace immune::audio
