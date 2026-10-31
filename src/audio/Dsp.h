// audio/Dsp.h — the signal-processing primitives every synth voice is built
// from. Header-only and allocation-free: everything here runs on the audio
// callback thread, per sample.
//
// Oscillators take PHASE IN TURNS ([0,1) is one cycle) rather than radians, so
// a phase accumulator is one add and a wrap, and a table lookup needs no
// multiply by 2*pi. The sine is a table rather than std::sin because the mixer
// evaluates it a few million times a second; the table is built once during
// static initialisation, before the audio thread exists.
#pragma once

#include "core/Types.h"

#include <cmath>

namespace immune::audio::dsp {

inline constexpr f32 kPi = 3.14159265358979f;
inline constexpr f32 kHalfPi = 1.57079632679490f;

struct SineTable {
    static constexpr i32 kSize = 1024;
    f32 v[kSize + 1];
    SineTable() {
        for (i32 i = 0; i <= kSize; ++i) {
            v[i] = static_cast<f32>(std::sin(2.0 * 3.14159265358979323846 * i / kSize));
        }
    }
};
inline const SineTable kSineTable{};

/// Wraps any phase into [0,1).
inline f32 wrap01(f32 p) {
    p -= static_cast<f32>(static_cast<i32>(p));
    return p < 0.0f ? p + 1.0f : p;
}

/// sin(2*pi*phase) for phase in [0,1). Linear interpolation of a 1024-entry
/// table: worst error about 5e-6, far below anything audible.
inline f32 sine(f32 phase) {
    const f32 x = phase * static_cast<f32>(SineTable::kSize);
    const i32 i = static_cast<i32>(x);
    const f32 f = x - static_cast<f32>(i);
    const f32 a = kSineTable.v[i];
    return a + (kSineTable.v[i + 1] - a) * f;
}

inline f32 triangle(f32 phase) {
    const f32 t = phase < 0.5f ? phase : 1.0f - phase;
    return 4.0f * t - 1.0f;
}

/// Polynomial band-limited step: subtracted at a saw/square discontinuity it
/// removes most of the aliasing a naive edge folds back into the audible band.
inline f32 poly_blep(f32 t, f32 dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline f32 saw(f32 phase, f32 inc) { return 2.0f * phase - 1.0f - poly_blep(phase, inc); }

inline f32 square(f32 phase, f32 inc) {
    f32 s = phase < 0.5f ? 1.0f : -1.0f;
    s += poly_blep(phase, inc);
    s -= poly_blep(wrap01(phase + 0.5f), inc);
    return s;
}

/// xorshift32 white noise in [-1,1). `state` must never be zero.
inline f32 noise(u32& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<f32>(static_cast<i32>(state)) * (1.0f / 2147483648.0f);
}

/// Uniform [0,1) from the same generator.
inline f32 noise01(u32& state) { return noise(state) * 0.5f + 0.5f; }

inline f32 semitones_to_ratio(f32 semis) { return std::exp2(semis * (1.0f / 12.0f)); }

/// Per-sample multiplier that decays by 1/e over `seconds`.
inline f32 decay_coeff(f32 seconds, f32 sample_rate) {
    if (seconds <= 0.0f) return 0.0f;
    return std::exp(-1.0f / (seconds * sample_rate));
}

/// Equal-power pan. `pan` in [-1,1]; centre gives -3 dB on each side, so a
/// sound sweeping across the screen keeps a constant loudness.
inline void pan_gains(f32 pan, f32& left, f32& right) {
    const f32 p = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
    const f32 a = (p + 1.0f) * 0.125f;  // turns: 0 .. 0.25, i.e. 0 .. pi/2
    left = sine(a + 0.25f);             // cos
    right = sine(a);
}

/// Topology-preserving-transform state-variable filter (Zavalishin). Stable
/// under fast cutoff modulation, which every sweep in the patch table relies
/// on, and gives low/band/high from one state.
struct Svf {
    f32 ic1 = 0.0f, ic2 = 0.0f;
    f32 a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;

    void set(f32 cutoff_hz, f32 q, f32 sample_rate) {
        f32 w = cutoff_hz / sample_rate;
        w = w < 0.0005f ? 0.0005f : (w > 0.45f ? 0.45f : w);
        const f32 g = std::tan(kPi * w);
        k = 1.0f / (q > 0.05f ? q : 0.05f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    /// One sample in; low/band/high out through the references.
    void tick(f32 v0, f32& low, f32& band, f32& high) {
        const f32 v3 = v0 - ic2;
        const f32 v1 = a1 * ic1 + a2 * v3;
        const f32 v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        low = v2;
        band = v1;
        high = v0 - k * v1 - v2;
    }

    void reset() { ic1 = ic2 = 0.0f; }
};

/// Smooth saturation for the master bus: linear near zero, approaching +-1.
/// A rational tanh approximation; exact enough that the knee is inaudible.
inline f32 soft_clip(f32 x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    const f32 x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

} // namespace immune::audio::dsp
