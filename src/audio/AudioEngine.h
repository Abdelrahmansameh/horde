// audio/AudioEngine.h — the SDL audio device around a Mixer.
//
// Owns the OS device, the Mixer that renders into it and the CommandRing the
// game thread fills. SDL calls the mixer from its own thread; the only things
// that cross threads are the ring (SPSC, game -> audio) and a few relaxed
// atomics of statistics (audio -> game). Nothing here can block the game.
//
// FAILURE IS SILENT, NEVER FATAL. No audio device (a headless box, a
// disconnected headset, a remote desktop) leaves the engine "not running":
// commands() returns nullptr and the scene simply stops producing. Audio is
// cosmetic; it must never decide whether the game starts.
#pragma once

#include "audio/Mixer.h"
#include "core/Types.h"

#include <atomic>
#include <memory>
#include <string>

namespace immune::audio {

struct AudioConfig {
    i32 sample_rate = 48000;
    /// Device buffer in frames. 512 at 48 kHz is ~11 ms: low enough that a
    /// click lands with the press, high enough not to underrun on a busy frame.
    i32 buffer_frames = 512;
    f32 master_volume = 0.8f;
};

/// Callback-side numbers, published once per device block.
struct EngineStats {
    u32 active_voices = 0;
    u32 active_grains = 0;
    u32 peak_voices = 0;
    u64 voices_stolen = 0;
    u64 voices_rejected = 0;
    u64 commands_dropped = 0;
    /// Rendering cost as a fraction of the block's real-time duration, as a
    /// smoothed average and as the worst block seen. 0.05 = 5% of one core.
    f32 load_average = 0.0f;
    f32 load_peak = 0.0f;
    f32 output_peak = 0.0f;
    i32 sample_rate = 0;
    i32 buffer_frames = 0;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Opens the default output device. False (with error()) when there is
    /// none; the game runs on without sound.
    bool init(const AudioConfig& config);
    void shutdown();
    bool running() const { return device_ != 0; }
    const std::string& error() const { return error_; }

    /// The game thread's end of the ring, or nullptr when no device is open.
    CommandRing* commands() { return running() ? ring_.get() : nullptr; }
    f32 sample_rate() const { return static_cast<f32>(stats_.sample_rate); }

    void set_volume(f32 volume);
    f32 volume() const { return volume_; }
    void set_muted(bool muted);
    bool muted() const { return muted_; }

    EngineStats stats() const;

private:
    static void callback(void* user, unsigned char* stream, int bytes);
    void push_master();

    std::unique_ptr<Mixer> mixer_;
    std::unique_ptr<CommandRing> ring_;
    u32 device_ = 0;
    std::string error_;
    f32 volume_ = 0.8f;
    bool muted_ = false;
    EngineStats stats_{};
    u64 perf_frequency_ = 1;

    std::atomic<u32> a_voices_{0};
    std::atomic<u32> a_grains_{0};
    std::atomic<u32> a_peak_voices_{0};
    std::atomic<u64> a_stolen_{0};
    std::atomic<u64> a_rejected_{0};
    std::atomic<f32> a_load_avg_{0.0f};
    std::atomic<f32> a_load_peak_{0.0f};
    std::atomic<f32> a_output_peak_{0.0f};
};

} // namespace immune::audio
