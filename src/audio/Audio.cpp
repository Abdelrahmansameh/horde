// audio/Audio.cpp — Wave 3C: procedural synthesis and mixing.
//
// Audio.h is a frozen contract (no fields to store mixer state in), so all
// mutable state (ring buffer, voice pool, SDL device handle) lives in a
// heap-allocated EngineImpl looked up through a small lock-free registry
// keyed by `this`. See the registry comment below for why a registry
// (rather than adding a pointer field to AudioEngine) was necessary.
#include "audio/Audio.h"
#include "audio/AudioInternal.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace immune::audio {

using namespace internal;

namespace {

// ---------------------------------------------------------------------
// this-> Impl* registry.
//
// AudioEngine's private section (frozen) has no pimpl slot, so post()/
// update()/etc. need another way to find their heap-allocated mixer state.
// A small fixed array of atomic<AudioEngine*>/atomic<EngineImpl*> pairs,
// scanned linearly and updated with compare_exchange, gives O(1)-in-practice
// lookup with zero locks and zero allocation — safe to call from post()
// on the game thread. It is never touched by the SDL audio callback: the
// callback receives its EngineImpl* directly as SDL userdata at device-open
// time, so it never needs to consult this registry at all.
constexpr int kMaxInstances = 8;
std::atomic<const AudioEngine*> g_owner[kMaxInstances]{};

struct EngineImpl;
std::atomic<EngineImpl*> g_impl[kMaxInstances]{};

// ---------------------------------------------------------------------
// Per-engine state that needs SDL (device handle) glued to the SDL-free
// Mixer from AudioInternal.h.
// ---------------------------------------------------------------------
struct EngineImpl {
    Mixer mixer;
    SDL_AudioDeviceID device = 0;
    bool has_real_device = false;
};

EngineImpl* find_impl(const AudioEngine* self) {
    for (int i = 0; i < kMaxInstances; ++i) {
        if (g_owner[i].load(std::memory_order_acquire) == self)
            return g_impl[i].load(std::memory_order_acquire);
    }
    return nullptr;
}

bool register_impl(const AudioEngine* self, EngineImpl* impl) {
    for (int i = 0; i < kMaxInstances; ++i) {
        const AudioEngine* expected = nullptr;
        if (g_owner[i].compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
            g_impl[i].store(impl, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void unregister_impl(const AudioEngine* self) {
    for (int i = 0; i < kMaxInstances; ++i) {
        if (g_owner[i].load(std::memory_order_acquire) == self) {
            g_impl[i].store(nullptr, std::memory_order_release);
            g_owner[i].store(nullptr, std::memory_order_release);
            return;
        }
    }
}

// ---------------------------------------------------------------------
// SDL audio callback. Runs on SDL's dedicated audio thread. Touches only
// the pre-allocated Mixer via fixed arrays and atomics: no allocation, no
// locks, no calls into sim/game code, matching Audio.h's threading rule.
// ---------------------------------------------------------------------
void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    auto* mixer = static_cast<Mixer*>(userdata);
    const u32 frames = static_cast<u32>(std::max(len, 0)) / static_cast<u32>(sizeof(float) * 2);
    const f32 block_dt = mixer->sample_rate > 0
        ? static_cast<f32>(frames) / static_cast<f32>(mixer->sample_rate)
        : 0.0f;
    // This Mixer's ring buffer/voice pool has exactly one consumer for its
    // whole lifetime: the callback, once a real device is open (see
    // AudioEngine::update(), which only pumps the mixer itself when running
    // with null_device / no hardware).
    pump(*mixer, block_dt);
    render_block(*mixer, reinterpret_cast<float*>(stream), frames, mixer->sample_rate);
}

} // namespace

bool AudioEngine::init(const AudioConfig& config) {
    config_ = config;
    error_.clear();
    dropped_ = 0;
    ready_ = false;

    auto* impl = new EngineImpl();
    impl->mixer.voice_limit = std::min<u32>(config.max_voices, kMaxVoiceSlots);
    impl->mixer.sample_rate = static_cast<u32>(std::max(config.sample_rate, 8000));
    impl->mixer.master_gain.store(std::isfinite(config.master_gain) ? config.master_gain : 0.8f,
                                   std::memory_order_relaxed);

    if (!register_impl(this, impl)) {
        delete impl;
        error_ = "audio: too many concurrent AudioEngine instances (registry full)";
        return false;
    }

    if (config.null_device) {
        // Headless contract: mixer logic still runs (see update()), but we
        // must never touch SDL's audio device APIs at all.
        impl->has_real_device = false;
        ready_ = true;
        return true;
    }

    SDL_InitSubSystem(SDL_INIT_AUDIO); // idempotent if already initialized elsewhere

    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = static_cast<int>(impl->mixer.sample_rate);
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = static_cast<Uint16>(std::clamp(config.buffer_frames, 64, 8192));
    want.callback = sdl_audio_callback;
    want.userdata = &impl->mixer;

    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) {
        // No usable audio hardware (common on CI/build machines). Run as a
        // functioning-but-silent mixer instead of failing init — gameplay
        // and event plumbing must not depend on real audio hardware being
        // present.
        impl->has_real_device = false;
        error_ = std::string("audio device open failed, running silent: ") + SDL_GetError();
        ready_ = true;
        return true;
    }

    impl->device = dev;
    impl->has_real_device = true;
    impl->mixer.sample_rate = static_cast<u32>(have.freq > 0 ? have.freq : want.freq);
    SDL_PauseAudioDevice(dev, 0);
    ready_ = true;
    return true;
}

void AudioEngine::shutdown() {
    if (EngineImpl* impl = find_impl(this)) {
        if (impl->has_real_device && impl->device != 0) {
            SDL_CloseAudioDevice(impl->device);
        }
        unregister_impl(this);
        delete impl;
    }
    ready_ = false;
}

void AudioEngine::post(const AudioEvent& event) {
    if (!ready_) return;
    EngineImpl* impl = find_impl(this);
    if (!impl) return;
    if (!impl->mixer.ring.push(event)) {
        ++dropped_;
    }
}

void AudioEngine::update(Vec2 listener_center, f32 horde_intensity01, f32 dt) {
    if (!ready_) return;
    EngineImpl* impl = find_impl(this);
    if (!impl) return;

    update_listener_and_music(impl->mixer, listener_center, horde_intensity01, dt);

    if (!impl->has_real_device) {
        // Nothing else ever drains this mixer's ring buffer or ages its
        // voices (no SDL callback thread exists for it), so do that work
        // here on the caller's thread. This is deliberately the same pump()
        // the real callback uses — voices allocate/trigger/decay exactly as
        // they would with a real device, just without ever reaching SDL.
        const f32 safe_dt = std::isfinite(dt) ? std::max(dt, 0.0f) : 0.0f;
        pump(impl->mixer, safe_dt);
    }
}

void AudioEngine::set_master_gain(f32 g) {
    config_.master_gain = g;
    if (EngineImpl* impl = find_impl(this)) {
        const f32 clamped = std::isfinite(g) ? std::clamp(g, 0.0f, 4.0f) : 0.0f;
        impl->mixer.master_gain.store(clamped, std::memory_order_relaxed);
    }
}

void AudioEngine::set_paused(bool paused) {
    if (EngineImpl* impl = find_impl(this)) {
        impl->mixer.paused.store(paused, std::memory_order_relaxed);
    }
}

} // namespace immune::audio
