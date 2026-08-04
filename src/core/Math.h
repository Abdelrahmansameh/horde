// core/Math.h — small scalar/vector helpers used across sim and render.
// Header-only and branch-light: safe to call from the hot path.
#pragma once

#include "core/Types.h"

#include <cmath>

namespace immune::math {

inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = 2.0f * kPi;
inline constexpr f32 kEpsilon = 1e-6f;

template <typename T>
constexpr T min(T a, T b) { return a < b ? a : b; }
template <typename T>
constexpr T max(T a, T b) { return a > b ? a : b; }
template <typename T>
constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr f32 saturate(f32 v) { return clamp(v, 0.0f, 1.0f); }
constexpr f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
inline Vec2 lerp(Vec2 a, Vec2 b, f32 t) { return a + (b - a) * t; }

/// Inverse lerp, guarded against a degenerate range.
constexpr f32 inv_lerp(f32 a, f32 b, f32 v) {
    const f32 d = b - a;
    return (d > kEpsilon || d < -kEpsilon) ? (v - a) / d : 0.0f;
}

constexpr f32 smoothstep01(f32 t) {
    t = saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

inline f32 length_sq(Vec2 v) { return v.x * v.x + v.y * v.y; }
inline f32 length(Vec2 v) { return std::sqrt(length_sq(v)); }

/// Normalizes, returning (0,0) for a near-zero vector instead of NaN.
inline Vec2 normalize_safe(Vec2 v) {
    const f32 l2 = length_sq(v);
    if (l2 < kEpsilon * kEpsilon) return Vec2{0.0f, 0.0f};
    const f32 inv = 1.0f / std::sqrt(l2);
    return Vec2{v.x * inv, v.y * inv};
}

/// Clamps a vector's magnitude to `max_len`.
inline Vec2 clamp_length(Vec2 v, f32 max_len) {
    const f32 l2 = length_sq(v);
    if (l2 <= max_len * max_len || l2 < kEpsilon) return v;
    return v * (max_len / std::sqrt(l2));
}

/// Bilinear interpolation of four grid corner values.
constexpr f32 bilerp(f32 v00, f32 v10, f32 v01, f32 v11, f32 tx, f32 ty) {
    return lerp(lerp(v00, v10, tx), lerp(v01, v11, tx), ty);
}
inline Vec2 bilerp(Vec2 v00, Vec2 v10, Vec2 v01, Vec2 v11, f32 tx, f32 ty) {
    return lerp(lerp(v00, v10, tx), lerp(v01, v11, tx), ty);
}

} // namespace immune::math
