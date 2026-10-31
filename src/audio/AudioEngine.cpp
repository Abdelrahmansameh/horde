#include "audio/AudioEngine.h"

#include <SDL.h>

#include <algorithm>

namespace immune::audio {

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(const AudioConfig& config) {
    shutdown();
    error_.clear();
    volume_ = std::clamp(config.master_volume, 0.0f, 1.0f);

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        error_ = std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError();
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = std::clamp(config.sample_rate, 22050, 96000);
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = static_cast<Uint16>(std::clamp(config.buffer_frames, 128, 4096));
    want.callback = &AudioEngine::callback;
    want.userdata = this;

    mixer_ = std::make_unique<Mixer>();
    ring_ = std::make_unique<CommandRing>();

    // Rate and block size may differ from the request (the mixer adapts);
    // format and channel count are converted by SDL if the device disagrees,
    // so the callback can always write interleaved stereo float.
    SDL_AudioSpec have{};
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(
        nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (dev == 0) {
        error_ = std::string("SDL_OpenAudioDevice failed: ") + SDL_GetError();
        mixer_.reset();
        ring_.reset();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    mixer_->init(static_cast<f32>(have.freq));
    stats_ = EngineStats{};
    stats_.sample_rate = have.freq;
    stats_.buffer_frames = have.samples;
    perf_frequency_ = std::max<u64>(SDL_GetPerformanceFrequency(), 1);
    device_ = dev;
    push_master();
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void AudioEngine::shutdown() {
    if (device_ != 0) {
        // Closing waits for any callback in flight, so the mixer can be freed
        // right after without the audio thread touching it.
        SDL_CloseAudioDevice(device_);
        device_ = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    mixer_.reset();
    ring_.reset();
}

void AudioEngine::set_volume(f32 volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    push_master();
}

void AudioEngine::set_muted(bool muted) {
    muted_ = muted;
    push_master();
}

void AudioEngine::push_master() {
    if (!running()) return;
    Command c;
    c.type = CommandType::MasterGain;
    c.gain = muted_ ? 0.0f : volume_;
    ring_->push(c);
}

EngineStats AudioEngine::stats() const {
    EngineStats s = stats_;
    s.active_voices = a_voices_.load(std::memory_order_relaxed);
    s.active_grains = a_grains_.load(std::memory_order_relaxed);
    s.peak_voices = a_peak_voices_.load(std::memory_order_relaxed);
    s.voices_stolen = a_stolen_.load(std::memory_order_relaxed);
    s.voices_rejected = a_rejected_.load(std::memory_order_relaxed);
    s.load_average = a_load_avg_.load(std::memory_order_relaxed);
    s.load_peak = a_load_peak_.load(std::memory_order_relaxed);
    s.output_peak = a_output_peak_.load(std::memory_order_relaxed);
    s.commands_dropped = ring_ ? ring_->dropped() : 0;
    return s;
}

void AudioEngine::callback(void* user, unsigned char* stream, int bytes) {
    // SDL's audio thread. Touches only the mixer, the ring's consumer end and
    // the stat atomics -- no locks, no allocation, no game state.
    auto* self = static_cast<AudioEngine*>(user);
    const u64 t0 = SDL_GetPerformanceCounter();
    const u32 frames = static_cast<u32>(bytes) / static_cast<u32>(sizeof(f32) * 2u);
    self->mixer_->drain(*self->ring_);
    self->mixer_->render(reinterpret_cast<f32*>(stream), frames);
    const u64 t1 = SDL_GetPerformanceCounter();

    const f32 elapsed = static_cast<f32>(t1 - t0) / static_cast<f32>(self->perf_frequency_);
    const f32 budget = static_cast<f32>(frames) / self->mixer_->sample_rate();
    const f32 load = budget > 0.0f ? elapsed / budget : 0.0f;
    const f32 avg = self->a_load_avg_.load(std::memory_order_relaxed);
    self->a_load_avg_.store(avg + (load - avg) * 0.02f, std::memory_order_relaxed);
    if (load > self->a_load_peak_.load(std::memory_order_relaxed)) {
        self->a_load_peak_.store(load, std::memory_order_relaxed);
    }
    const MixerStats& ms = self->mixer_->stats();
    self->a_voices_.store(ms.active_voices, std::memory_order_relaxed);
    self->a_grains_.store(ms.active_grains, std::memory_order_relaxed);
    self->a_peak_voices_.store(ms.peak_voices, std::memory_order_relaxed);
    self->a_stolen_.store(ms.voices_stolen, std::memory_order_relaxed);
    self->a_rejected_.store(ms.voices_rejected, std::memory_order_relaxed);
    self->a_output_peak_.store(ms.peak, std::memory_order_relaxed);
}

} // namespace immune::audio
