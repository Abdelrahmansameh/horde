#include "core/Clock.h"

namespace immune {

f64 WallClock::lap() {
    const auto now = std::chrono::steady_clock::now();
    const f64 dt = std::chrono::duration<f64>(now - start_).count();
    start_ = now;
    return dt;
}

void FixedClock::begin_frame() {
    const auto now = std::chrono::steady_clock::now();
    if (!started_) {
        last_ = now;
        started_ = true;
        frame_delta_ = 0.0;
        return;
    }
    frame_delta_ = std::chrono::duration<f64>(now - last_).count();
    last_ = now;

    f64 scaled = frame_delta_ * static_cast<f64>(time_scale_);
    if (scaled > max_frame_seconds_) scaled = max_frame_seconds_;
    if (scaled < 0.0) scaled = 0.0;
    accumulator_ += scaled * static_cast<f64>(kTicksPerSecond);
}

void FixedClock::advance_manual(f64 seconds) {
    frame_delta_ = seconds;
    accumulator_ += seconds * static_cast<f64>(kTicksPerSecond);
}

bool FixedClock::consume_tick() {
    // The accumulator is kept in *tick units*, not seconds, so consuming a tick
    // is an exact `-= 1.0` and error can only ever enter at accumulate time.
    // Accumulating seconds and subtracting 1/60 instead loses a tick per second
    // to rounding (one simulated second yields 59 ticks, not 60).
    // kAccumEpsilon absorbs the residual error from summing frame deltas.
    constexpr f64 kAccumEpsilon = 1e-9;
    if (accumulator_ < 1.0 - kAccumEpsilon) return false;
    accumulator_ -= 1.0;
    if (accumulator_ < 0.0) accumulator_ = 0.0;
    ++tick_;
    return true;
}

void FixedClock::reset() {
    started_ = false;
    accumulator_ = 0.0;
    frame_delta_ = 0.0;
    tick_ = 0;
}

} // namespace immune
