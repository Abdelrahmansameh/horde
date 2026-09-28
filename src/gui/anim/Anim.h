// gui/anim/Anim.h — tweens, springs, and the design canvas's looping motions.
//
// All UI motion runs on the RENDER clock (the dt the frame measured), never
// the sim clock, so the UI keeps breathing while the game is paused.
//
// The Loop presets are the canvas's CSS keyframes, ported one for one so a
// widget can name the animation the artboard used (docs/ui-concepts/canvas,
// the <helmet> style block of every artboard): beat 1.4 s (0.6 s fast), wobble
// 2.2 s, halo 1.6 s, pulse 1 s, spin 8 s, throb 0.6 s, flow 1.8 s.
#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <cmath>

namespace immune::gui {

enum class Ease : u8 { Linear, InOutSine, OutCubic, InOutCubic, OutBack };

inline f32 ease(Ease e, f32 t) {
    t = math::saturate(t);
    switch (e) {
        case Ease::Linear: return t;
        case Ease::InOutSine: return 0.5f - 0.5f * std::cos(math::kPi * t);
        case Ease::OutCubic: {
            const f32 u = 1.0f - t;
            return 1.0f - u * u * u;
        }
        case Ease::InOutCubic:
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        case Ease::OutBack: {
            constexpr f32 c1 = 1.70158f, c3 = c1 + 1.0f;
            const f32 u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
    }
    return t;
}

/// A value moving from one number to another over a fixed time.
class Tween {
public:
    explicit Tween(f32 value = 0.0f) : from_(value), to_(value), value_(value) {}

    void start(f32 to, f32 duration, Ease e = Ease::OutCubic) {
        from_ = value_;
        to_ = to;
        duration_ = math::max(duration, 1e-4f);
        elapsed_ = 0.0f;
        ease_ = e;
    }
    void snap(f32 v) {
        from_ = to_ = value_ = v;
        elapsed_ = duration_;
    }
    void update(f32 dt) {
        if (elapsed_ >= duration_) {
            value_ = to_;
            return;
        }
        elapsed_ = math::min(elapsed_ + dt, duration_);
        value_ = math::lerp(from_, to_, ease(ease_, elapsed_ / duration_));
    }
    f32 value() const { return value_; }
    f32 target() const { return to_; }
    bool done() const { return elapsed_ >= duration_; }

private:
    f32 from_, to_, value_;
    f32 duration_ = 1e-4f;
    f32 elapsed_ = 1e-4f;
    Ease ease_ = Ease::OutCubic;
};

/// A critically-tuned spring: hover/press bounce, the deny shake's return.
/// Semi-implicit Euler at sub-steps of at most 1/120 s, so a long frame cannot
/// make it explode.
class Spring {
public:
    explicit Spring(f32 value = 0.0f, f32 stiffness = 420.0f, f32 damping = 26.0f)
        : value_(value), target_(value), stiffness_(stiffness), damping_(damping) {}

    void set_target(f32 t) { target_ = t; }
    void snap(f32 v) {
        value_ = target_ = v;
        velocity_ = 0.0f;
    }
    void kick(f32 velocity) { velocity_ += velocity; }
    void update(f32 dt) {
        while (dt > 0.0f) {
            const f32 h = math::min(dt, 1.0f / 120.0f);
            const f32 accel = stiffness_ * (target_ - value_) - damping_ * velocity_;
            velocity_ += accel * h;
            value_ += velocity_ * h;
            dt -= h;
        }
    }
    f32 value() const { return value_; }
    f32 target() const { return target_; }
    f32 velocity() const { return velocity_; }
    bool settled(f32 eps = 1e-3f) const {
        return std::fabs(value_ - target_) < eps && std::fabs(velocity_) < eps;
    }

private:
    f32 value_, target_;
    f32 velocity_ = 0.0f;
    f32 stiffness_, damping_;
};

/// The canvas's infinite loops, as functions of time (seconds).
namespace loop {

/// Fraction through a period, 0..1.
inline f32 phase(f32 t, f32 period) {
    const f32 x = t / period;
    return x - std::floor(x);
}

/// `beat`: 0%/100% scale 1, 12% 1.06, 24% 0.99, 36% 1.04 — a heartbeat.
/// Period 1.4 s (0.6 s for anim-beat-fast), eased in-out between keys.
inline f32 beat_scale(f32 t, f32 period = 1.4f) {
    static constexpr f32 kKeys[5] = {0.0f, 0.12f, 0.24f, 0.36f, 1.0f};
    static constexpr f32 kVals[5] = {1.0f, 1.06f, 0.99f, 1.04f, 1.0f};
    const f32 p = phase(t, period);
    for (int i = 0; i < 4; ++i) {
        if (p <= kKeys[i + 1]) {
            const f32 u = (p - kKeys[i]) / (kKeys[i + 1] - kKeys[i]);
            return math::lerp(kVals[i], kVals[i + 1], ease(Ease::InOutSine, u));
        }
    }
    return 1.0f;
}

/// `wobble`: rotate -1.5deg..1.5deg and scale 1..1.03, 2.2 s.
inline f32 wobble_rotation(f32 t, f32 period = 2.2f) {
    const f32 s = -std::cos(phase(t, period) * math::kTwoPi);  // -1 at 0%, +1 at 50%
    return s * 1.5f * math::kPi / 180.0f;
}
inline f32 wobble_scale(f32 t, f32 period = 2.2f) {
    return 1.0f + 0.03f * (0.5f - 0.5f * std::cos(phase(t, period) * math::kTwoPi));
}

/// `halo`: opacity 0.35..1, 1.6 s.
inline f32 halo_opacity(f32 t, f32 period = 1.6f) {
    return 0.35f + 0.65f * (0.5f - 0.5f * std::cos(phase(t, period) * math::kTwoPi));
}

/// `pulse`: opacity 1..0.5, 1 s.
inline f32 pulse_opacity(f32 t, f32 period = 1.0f) {
    return 1.0f - 0.5f * (0.5f - 0.5f * std::cos(phase(t, period) * math::kTwoPi));
}

/// `spin`: one turn every 8 s.
inline f32 spin_rotation(f32 t, f32 period = 8.0f) { return phase(t, period) * math::kTwoPi; }

/// `throb`: scale 1..1.015, 0.6 s.
inline f32 throb_scale(f32 t, f32 period = 0.6f) {
    return 1.0f + 0.015f * (0.5f - 0.5f * std::cos(phase(t, period) * math::kTwoPi));
}

/// `flow`: stroke-dashoffset 0 -> -44 every 1.8 s (dashes travel forward).
inline f32 flow_dash_offset(f32 t, f32 period = 1.8f, f32 distance = 44.0f) {
    return -distance * phase(t, period);
}

} // namespace loop

} // namespace immune::gui
