// Wave 0 stub. Implementation is owned by Wave 3C.
#include "audio/Audio.h"

namespace immune::audio {

bool AudioEngine::init(const AudioConfig& config) {
    config_ = config;
    ready_ = true;   // null mixer: accepts and discards events
    error_.clear();
    return true;
}

void AudioEngine::shutdown() { ready_ = false; }
void AudioEngine::post(const AudioEvent&) {}
void AudioEngine::update(Vec2, f32, f32) {}
void AudioEngine::set_master_gain(f32 g) { config_.master_gain = g; }
void AudioEngine::set_paused(bool) {}

} // namespace immune::audio
