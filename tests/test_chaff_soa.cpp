// Guards the SoA invariants documented in sim/chaff/ChaffBuffers.h. Wave 1B
// must keep these passing while it adds the movement kernel.
#include "sim/chaff/ChaffBuffers.h"

#include "core/Rng.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {
ChaffSpawnParams make(f32 x, f32 y, PathogenFamily f = PathogenFamily::Virus, f32 density = 1.0f) {
    ChaffSpawnParams p;
    p.position = Vec2{x, y};
    p.family = f;
    p.density = density;
    return p;
}
} // namespace

TEST_CASE("chaff streams stay parallel and sized to capacity", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(1000);
    REQUIRE(b.capacity() == 1000);
    REQUIRE(b.count() == 0);
    REQUIRE(b.pos_x.size() == 1000);
    REQUIRE(b.pos_y.size() == b.pos_x.size());
    REQUIRE(b.vel_x.size() == b.pos_x.size());
    REQUIRE(b.vel_y.size() == b.pos_x.size());
    REQUIRE(b.family.size() == b.pos_x.size());
    REQUIRE(b.density.size() == b.pos_x.size());
    REQUIRE(b.flags.size() == b.pos_x.size());
}

TEST_CASE("spawn fills the streams and never reallocates", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(4);
    const f32* base = b.pos_x.data();

    for (int i = 0; i < 4; ++i) {
        REQUIRE(b.spawn(make(static_cast<f32>(i), 1.0f)).valid());
    }
    REQUIRE(b.count() == 4);
    REQUIRE(b.full());
    REQUIRE(b.pos_x.data() == base);       // no reallocation
    REQUIRE(!b.spawn(make(9.0f, 9.0f)).valid()); // capacity refusal
    REQUIRE(b.count() == 4);

    for (usize i = 0; i < b.count(); ++i) {
        REQUIRE((b.flags[i] & chaff_flags::kAlive) != 0);
    }
}

TEST_CASE("family counts and total density track spawns", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(16);

    // A family with nothing spawned counts zero rather than reading garbage.
    REQUIRE(b.family_count(PathogenFamily::Virus) == 0);
    REQUIRE(b.family_count(PathogenFamily::Bacteria) == 0);

    b.spawn(make(0, 0, PathogenFamily::Virus, 2.0f));
    b.spawn(make(1, 0, PathogenFamily::Virus, 3.0f));
    b.spawn(make(2, 0, PathogenFamily::Bacteria, 5.0f));

    REQUIRE(b.family_count(PathogenFamily::Virus) == 2);
    REQUIRE(b.family_count(PathogenFamily::Bacteria) == 1);
    REQUIRE(b.total_density() == 10.0f);
}

TEST_CASE("density loss kills at zero and is the only damage path", "[sim][chaff][damage]") {
    ChaffBuffers b;
    b.reserve(8);
    b.spawn(make(0, 0, PathogenFamily::Virus, 4.0f));

    b.apply_density_loss(0, 1.5f);
    REQUIRE(b.density[0] == 2.5f);
    REQUIRE((b.flags[0] & chaff_flags::kPendingKill) == 0);
    REQUIRE(b.total_density() == 2.5f);

    b.apply_density_loss(0, 10.0f);   // over-kill must clamp, not go negative
    REQUIRE(b.density[0] == 0.0f);
    REQUIRE((b.flags[0] & chaff_flags::kPendingKill) != 0);
    REQUIRE(b.total_density() == 0.0f);
}

TEST_CASE("compact swap-removes dead agents and keeps the array dense",
          "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(16);
    for (int i = 0; i < 10; ++i) b.spawn(make(static_cast<f32>(i), 0.0f));

    b.kill(2);
    b.kill(5);
    b.kill(9);
    const usize removed = b.compact();

    REQUIRE(removed == 3);
    REQUIRE(b.count() == 7);
    for (usize i = 0; i < b.count(); ++i) {
        REQUIRE((b.flags[i] & chaff_flags::kAlive) != 0);
        REQUIRE((b.flags[i] & chaff_flags::kPendingKill) == 0);
        REQUIRE(b.density[i] > 0.0f);           // invariant I4
    }
}

TEST_CASE("compact handles all-dead and none-dead", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(8);
    for (int i = 0; i < 5; ++i) b.spawn(make(static_cast<f32>(i), 0.0f));
    REQUIRE(b.compact() == 0);
    REQUIRE(b.count() == 5);

    for (usize i = 0; i < b.count(); ++i) b.kill(i);
    REQUIRE(b.compact() == 5);
    REQUIRE(b.count() == 0);
    REQUIRE(b.total_density() == 0.0f);
}

TEST_CASE("clear resets counts without dropping capacity", "[sim][chaff][soa]") {
    ChaffBuffers b;
    b.reserve(32);
    for (int i = 0; i < 20; ++i) b.spawn(make(static_cast<f32>(i), 0.0f));
    b.clear();
    REQUIRE(b.count() == 0);
    REQUIRE(b.capacity() == 32);
    REQUIRE(b.total_density() == 0.0f);
    REQUIRE(b.family_count(PathogenFamily::Virus) == 0);
}

// ---------------------------------------------------------------------------
// Wave 1B additions: handle resolution across compaction, and invariants under
// heavy spawn/kill/compact churn.
// ---------------------------------------------------------------------------

TEST_CASE("a handle resolves to its agent's new slot after compaction moves it",
          "[sim][chaff][soa][handle]") {
    ChaffBuffers b;
    b.reserve(8);
    std::vector<ChaffHandle> handles;
    for (int i = 0; i < 6; ++i) handles.push_back(b.spawn(make(static_cast<f32>(i), 0.0f)));
    for (const auto& h : handles) REQUIRE(h.valid());

    // Kill everything except the last agent, forcing compact() to swap it all
    // the way down to slot 0.
    for (int i = 0; i < 5; ++i) b.kill(static_cast<usize>(i));
    REQUIRE(b.compact() == 5);
    REQUIRE(b.count() == 1);

    // The survivor's handle must resolve to its NEW index (0), not its old one (5).
    const usize resolved = b.resolve(handles[5]);
    REQUIRE(resolved == 0);
    REQUIRE(b.pos_x[resolved] == 5.0f);

    // Every dead agent's handle must now resolve to npos.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(b.resolve(handles[static_cast<usize>(i)]) == ChaffBuffers::npos);
    }
}

TEST_CASE("handles stay uniquely resolvable across many spawn/kill/compact cycles",
          "[sim][chaff][soa][handle]") {
    ChaffBuffers b;
    b.reserve(64);
    Rng rng(123);

    // A parallel record of which handles we believe are currently alive.
    std::vector<ChaffHandle> alive;

    for (int round = 0; round < 200; ++round) {
        // Spawn a few.
        for (int s = 0; s < 3; ++s) {
            if (b.full()) break;
            ChaffHandle h = b.spawn(make(rng.range_f(-100.0f, 100.0f), 0.0f,
                                        PathogenFamily::Virus, 1.0f));
            if (h.valid()) alive.push_back(h);
        }
        // Every surviving handle must still resolve to a live, correctly-flagged slot.
        for (const auto& h : alive) {
            const usize idx = b.resolve(h);
            REQUIRE(idx != ChaffBuffers::npos);
            REQUIRE((b.flags[idx] & chaff_flags::kAlive) != 0);
        }
        // Kill roughly a third of them, by resolved index.
        std::vector<ChaffHandle> next_alive;
        for (const auto& h : alive) {
            const usize idx = b.resolve(h);
            if (idx != ChaffBuffers::npos && rng.chance(0.33f)) {
                b.kill(idx);
            } else if (idx != ChaffBuffers::npos) {
                next_alive.push_back(h);
            }
        }
        b.compact();
        alive = next_alive;

        // No two currently-alive handles may resolve to the same slot, and no
        // two live slots may share a generation (I1 + handle uniqueness).
        for (usize i = 0; i < alive.size(); ++i) {
            for (usize j = i + 1; j < alive.size(); ++j) {
                REQUIRE(alive[i].generation != alive[j].generation);
            }
        }
    }
}

TEST_CASE("heavy spawn/kill/compact churn preserves I1-I4", "[sim][chaff][soa][invariants]") {
    ChaffBuffers b;
    b.reserve(500);
    Rng rng(9001);

    for (int round = 0; round < 500; ++round) {
        // Random spawns.
        const int to_spawn = static_cast<int>(rng.next_below(5));
        for (int s = 0; s < to_spawn; ++s) {
            if (b.full()) break;
            b.spawn(make(rng.range_f(-50.0f, 50.0f), rng.range_f(-50.0f, 50.0f),
                        static_cast<PathogenFamily>(rng.next_below(kFamilyCount)),
                        rng.range_f(0.5f, 5.0f)));
        }
        // Random kills and density damage.
        for (usize i = 0; i < b.count(); ++i) {
            if (rng.chance(0.05f)) b.kill(i);
            else if (rng.chance(0.1f)) b.apply_density_loss(i, rng.range_f(0.1f, 2.0f));
        }
        b.compact();

        // I1: every live slot is flagged alive and not pending-kill.
        // I4: every live slot has positive density.
        for (usize i = 0; i < b.count(); ++i) {
            REQUIRE((b.flags[i] & chaff_flags::kAlive) != 0);
            REQUIRE((b.flags[i] & chaff_flags::kPendingKill) == 0);
            REQUIRE(b.density[i] > 0.0f);
        }
        // I2: streams stay parallel.
        REQUIRE(b.pos_x.size() == b.capacity());
        REQUIRE(b.pos_y.size() == b.capacity());
        REQUIRE(b.vel_x.size() == b.capacity());
        REQUIRE(b.vel_y.size() == b.capacity());
        REQUIRE(b.family.size() == b.capacity());
        REQUIRE(b.density.size() == b.capacity());
        REQUIRE(b.flags.size() == b.capacity());
        REQUIRE(b.generation.size() == b.capacity());
        // I3: never over capacity.
        REQUIRE(b.count() <= b.capacity());

        // total_density() and family_count() must match a from-scratch scan —
        // they are maintained incrementally and must never drift.
        f32 sum = 0.0f;
        u32 fam_counts[kFamilyCount] = {};
        for (usize i = 0; i < b.count(); ++i) {
            sum += b.density[i];
            ++fam_counts[b.family[i]];
        }
        REQUIRE(b.total_density() == Catch::Approx(sum).margin(0.01f));
        for (u32 f = 0; f < kFamilyCount; ++f) {
            REQUIRE(b.family_count(static_cast<PathogenFamily>(f)) == fam_counts[f]);
        }
    }
}

TEST_CASE("total_density is exactly zero once every agent is gone, even under drift",
          "[sim][chaff][soa]") {
    // total_density_ is a running accumulator (+= on spawn, -= on damage/
    // compact), not recomputed from the live array, so float32 rounding can
    // leave it at a tiny nonzero residual after enough small subtractions --
    // the test above tolerates exactly that with a 0.01f margin. A residue
    // that's merely "close to zero" is harmless almost everywhere it's read,
    // except the wave-clear check (WaveDirector's `horde_gone`), which does
    // a strict `<= 0.0f` comparison: any leftover positive residue defeats
    // it forever and the game silently falls back to the full Clearing-phase
    // timeout on every wave, no matter how long you actually wait. This
    // reproduces that drift with many tiny apply_density_loss() calls, then
    // asserts the documented count()==0 short-circuit holds exactly.
    ChaffBuffers b;
    b.reserve(64);
    Rng rng(20260805);

    for (int agent = 0; agent < 40; ++agent) {
        b.spawn(make(rng.range_f(-50.0f, 50.0f), rng.range_f(-50.0f, 50.0f),
                    static_cast<PathogenFamily>(rng.next_below(kFamilyCount)),
                    rng.range_f(3.0f, 8.0f)));
    }

    // Whittle every agent down via many small, non-round losses rather than
    // one clean kill -- this is what actually accumulates float error.
    while (b.count() > 0) {
        for (usize i = 0; i < b.count(); ++i) {
            b.apply_density_loss(i, 0.03f);
        }
        b.compact();
    }

    REQUIRE(b.count() == 0);
    REQUIRE(b.total_density() == 0.0f);
}
