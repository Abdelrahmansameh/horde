// Wave 0 stub. Implementation is owned by Wave 3A.
#include "game/wave/WaveDirector.h"

#include "core/Rng.h"
#include "sim/SimWorld.h"

namespace immune::game {

void WaveDirector::set_waves(std::vector<WaveDef> waves) { waves_ = std::move(waves); }

std::vector<WaveDef> WaveDirector::generate(const std::string&, u32, Rng&) { return {}; }

void WaveDirector::start(sim::SimWorld&) {
    status_ = WaveStatus{};
    status_.all_waves_complete = waves_.empty();
    wave_time_ = 0.0f;
}

void WaveDirector::tick(sim::SimWorld&, Rng&, f32) {}

void WaveDirector::request_early_start() { early_start_requested_ = true; }

const WaveDef* WaveDirector::next_wave() const {
    const usize next = static_cast<usize>(status_.wave_index) + 1u;
    return next < waves_.size() ? &waves_[next] : nullptr;
}

} // namespace immune::game
