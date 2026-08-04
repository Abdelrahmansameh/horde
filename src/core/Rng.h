// core/Rng.h — seeded PRNG (PCG32). FROZEN CONTRACT.
//
// Determinism rule (docs/CONVENTIONS.md): simulation code MUST NOT call rand(),
// std::random_device, or any global generator. Every sim function that needs
// randomness takes an `Rng&` explicitly, sourced from SimContext. This is what
// makes --sim-test and --screenshot reproducible.
#pragma once

#include "core/Types.h"

namespace immune {

/// PCG32 (O'Neill 2014). 64-bit state, 32-bit output, cheap and well-distributed.
/// Trivially copyable: a whole Rng can be snapshotted to fork a deterministic
/// sub-stream without disturbing the parent.
class Rng {
public:
    Rng() : Rng(0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL) {}
    explicit Rng(u64 seed, u64 stream = 0xda3e39cb94b95bdbULL) { reseed(seed, stream); }

    void reseed(u64 seed, u64 stream = 0xda3e39cb94b95bdbULL) {
        state_ = 0u;
        inc_ = (stream << 1u) | 1u;
        next_u32();
        state_ += seed;
        next_u32();
    }

    /// Uniform in [0, 2^32).
    u32 next_u32() {
        const u64 old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    u64 next_u64() { return (static_cast<u64>(next_u32()) << 32) | next_u32(); }

    /// Uniform in [0, bound). Debiased via rejection. bound == 0 returns 0.
    u32 next_below(u32 bound) {
        if (bound == 0) return 0;
        const u32 threshold = (~bound + 1u) % bound;
        for (;;) {
            const u32 r = next_u32();
            if (r >= threshold) return r % bound;
        }
    }

    /// Uniform in [lo, hi]. Requires hi >= lo.
    i32 range_i(i32 lo, i32 hi) {
        return lo + static_cast<i32>(next_below(static_cast<u32>(hi - lo + 1)));
    }

    /// Uniform in [0, 1).
    f32 next_f32() { return static_cast<f32>(next_u32() >> 8) * 0x1.0p-24f; }

    /// Uniform in [lo, hi).
    f32 range_f(f32 lo, f32 hi) { return lo + (hi - lo) * next_f32(); }

    /// True with the given probability.
    bool chance(f32 p) { return next_f32() < p; }

    /// Approximately standard-normal (sum of 4 uniforms, cheap; not exact).
    f32 next_gaussian();

    /// Uniform point inside the unit disc.
    Vec2 unit_disc();

    /// Deterministic independent sub-stream, e.g. one per worker thread or system.
    Rng fork(u64 stream_id) const { return Rng(state_ ^ (stream_id * 0x9e3779b97f4a7c15ULL), stream_id); }

    u64 raw_state() const { return state_; }
    u64 raw_inc() const { return inc_; }

private:
    u64 state_ = 0;
    u64 inc_ = 0;
};

} // namespace immune
