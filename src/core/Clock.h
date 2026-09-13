// core/Clock.h — fixed-timestep accumulator + wall-clock timing helpers.
// FROZEN CONTRACT.
//
// The sim advances in exact 1/60 s ticks. Render runs at whatever rate the
// display allows and interpolates using FixedClock::alpha(). Sim code must
// never read wall-clock time; it only ever sees Tick and kFixedDt.
#pragma once

#include "core/Types.h"

#include <chrono>

namespace immune {

/// High-resolution wall clock. Non-deterministic by nature: profiling only.
class WallClock {
public:
    WallClock() { reset(); }
    void reset() { start_ = std::chrono::steady_clock::now(); }

    /// Seconds since construction/reset.
    f64 elapsed_seconds() const {
        return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start_).count();
    }
    f64 elapsed_ms() const { return elapsed_seconds() * 1000.0; }

    /// Seconds since the last call to lap().
    f64 lap();

private:
    std::chrono::steady_clock::time_point start_{};
};

/// Scoped timer that writes its duration (ms) into a caller-owned double.
class ScopedTimer {
public:
    explicit ScopedTimer(f64& out_ms) : out_(out_ms) {}
    ~ScopedTimer() { out_ = clock_.elapsed_ms(); }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    f64& out_;
    WallClock clock_{};
};

/// Fixed-step accumulator driving the sim/render split.
///
/// Typical frame:
///     clock.begin_frame();
///     while (clock.consume_tick()) { sim.tick(); }
///     renderer.draw(clock.alpha());
class FixedClock {
public:
    FixedClock() = default;

    /// Samples real time and adds the elapsed span to the accumulator.
    /// The frame delta is clamped to max_frame_seconds() so a debugger pause or a
    /// hitch cannot produce a death-spiral of catch-up ticks.
    void begin_frame();

    /// Advances the accumulator by an explicit delta instead of real time.
    /// Headless modes (--bench, --sim-test, --screenshot) use this so the tick
    /// sequence is identical on every machine.
    void advance_manual(f64 seconds);

    /// Pops one fixed step if one is available. Returns false when the frame's
    /// remaining time is less than one tick.
    bool consume_tick();

    /// Fraction [0,1) of the way into the next tick — the render interpolation factor.
    f32 alpha() const { return static_cast<f32>(accumulator_); }

    /// Throws away unconsumed time without touching the frame delta or the
    /// tick counter. Call it on every frame the sim is NOT stepped: the
    /// accumulator is only ever drained by consume_tick(), so a frame that
    /// accumulates without consuming banks its ticks, and whoever consumes
    /// next pays for all of them at once. max_frame_seconds() bounds a single
    /// frame, not a run of them -- a minute on a menu is 3600 banked ticks.
    void drop_accumulated() { accumulator_ = 0.0; }

    /// Number of ticks consumed since construction.
    Tick tick() const { return tick_; }

    /// Real seconds elapsed in the last begin_frame() span (for FPS display only).
    f64 frame_delta() const { return frame_delta_; }

    /// Upper bound on a single frame's contribution to the accumulator.
    f64 max_frame_seconds() const { return max_frame_seconds_; }
    void set_max_frame_seconds(f64 s) { max_frame_seconds_ = s; }

    /// Sim speed multiplier (pause = 0, 2x fast-forward = 2). Render is unaffected.
    f32 time_scale() const { return time_scale_; }
    void set_time_scale(f32 s) { time_scale_ = s; }

    void reset();

private:
    std::chrono::steady_clock::time_point last_{};
    bool started_ = false;
    /// Unconsumed time, measured in TICKS (not seconds). See consume_tick().
    f64 accumulator_ = 0.0;
    f64 frame_delta_ = 0.0;
    f64 max_frame_seconds_ = 0.25; // 15 ticks max catch-up
    f32 time_scale_ = 1.0f;
    Tick tick_ = 0;
};

} // namespace immune
