#include "core/Rng.h"

#include <cmath>

namespace immune {

f32 Rng::next_gaussian() {
    // Irwin-Hall(4), centred and scaled. Good enough for jitter; not for stats.
    const f32 s = next_f32() + next_f32() + next_f32() + next_f32();
    return (s - 2.0f) * 1.7320508f;
}

Vec2 Rng::unit_disc() {
    for (;;) {
        const f32 x = range_f(-1.0f, 1.0f);
        const f32 y = range_f(-1.0f, 1.0f);
        if (x * x + y * y <= 1.0f) return Vec2{x, y};
    }
}

} // namespace immune
