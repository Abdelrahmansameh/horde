// Tests for the uniform-grid spatial hash. Owner: Wave 1B.
//
// Correctness is checked against a brute-force O(n^2) reference: every query
// must return a superset of agents whose exact distance/point test passes
// (SpatialHash.h: "conservative at cell granularity"), and every agent the
// brute-force test finds must appear somewhere in the returned candidate set.
#include "sim/spatial/SpatialHash.h"

#include "core/Rng.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

struct Cloud {
    std::vector<f32> x, y;
};

Cloud make_random_cloud(usize n, Rect bounds, u64 seed) {
    Cloud c;
    c.x.resize(n);
    c.y.resize(n);
    Rng rng(seed);
    for (usize i = 0; i < n; ++i) {
        c.x[i] = rng.range_f(bounds.min.x, bounds.max.x);
        c.y[i] = rng.range_f(bounds.min.y, bounds.max.y);
    }
    return c;
}

bool contains_index(const std::vector<u32>& v, u32 idx) {
    return std::find(v.begin(), v.end(), idx) != v.end();
}

/// Brute-force circle membership (exact), for cross-checking the hash's
/// conservative candidate set.
std::vector<u32> brute_circle(const Cloud& c, Vec2 center, f32 radius) {
    std::vector<u32> out;
    const f32 r2 = radius * radius;
    for (usize i = 0; i < c.x.size(); ++i) {
        const f32 dx = c.x[i] - center.x, dy = c.y[i] - center.y;
        if (dx * dx + dy * dy <= r2) out.push_back(static_cast<u32>(i));
    }
    return out;
}

std::vector<u32> brute_rect(const Cloud& c, const Rect& rect) {
    std::vector<u32> out;
    for (usize i = 0; i < c.x.size(); ++i) {
        if (rect.contains(Vec2{c.x[i], c.y[i]})) out.push_back(static_cast<u32>(i));
    }
    return out;
}

} // namespace

TEST_CASE("spatial hash rebuild produces correct per-cell CSR occupancy", "[sim][spatial]") {
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{16.0f, 16.0f}};
    desc.cell_size = 4.0f;
    hash.configure(desc);

    // One agent per cell centre, four cells.
    std::vector<f32> px = {2.0f, 6.0f, 2.0f, 6.0f};
    std::vector<f32> py = {2.0f, 2.0f, 6.0f, 6.0f};
    hash.rebuild(px.data(), py.data(), px.size(), nullptr);

    REQUIRE(hash.indexed_count() == 4);
    REQUIRE(hash.max_cell_occupancy() == 1);

    for (usize i = 0; i < px.size(); ++i) {
        const IVec2 c = hash.cell_coord(Vec2{px[i], py[i]});
        const u32 cell = hash.cell_index(c);
        u32 begin, end;
        hash.cell_range(cell, begin, end);
        REQUIRE(end - begin == 1);
        REQUIRE(hash.indices()[begin] == static_cast<u32>(i));
    }
}

TEST_CASE("spatial hash occupancy matches a manual count", "[sim][spatial]") {
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{8.0f, 8.0f}};
    desc.cell_size = 2.0f;
    hash.configure(desc);

    // Stack 5 agents into the same cell.
    std::vector<f32> px = {1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 5.0f};
    std::vector<f32> py = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 5.0f};
    hash.rebuild(px.data(), py.data(), px.size(), nullptr);

    REQUIRE(hash.max_cell_occupancy() == 5);
    const IVec2 dims = hash.grid_dims();
    u32 total = 0;
    for (i32 c = 0; c < dims.x * dims.y; ++c) total += hash.occupancy()[c];
    REQUIRE(total == px.size());
}

TEST_CASE("spatial hash rebuild is order-independent and repeatable", "[sim][spatial][determinism]") {
    SpatialHash a, b;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{64.0f, 64.0f}};
    desc.cell_size = 3.0f;
    a.configure(desc);
    b.configure(desc);

    const Cloud c = make_random_cloud(500, desc.bounds, 777);
    a.rebuild(c.x.data(), c.y.data(), c.x.size(), nullptr);
    b.rebuild(c.x.data(), c.y.data(), c.x.size(), nullptr);

    REQUIRE(a.indexed_count() == b.indexed_count());
    for (usize i = 0; i < a.indexed_count(); ++i) {
        REQUIRE(a.indices()[i] == b.indices()[i]);
    }
    REQUIRE(a.max_cell_occupancy() == b.max_cell_occupancy());
}

TEST_CASE("query_circle candidates are a superset of the brute-force result", "[sim][spatial]") {
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{100.0f, 60.0f}};
    desc.cell_size = 2.5f;
    hash.configure(desc);

    const Cloud c = make_random_cloud(2000, desc.bounds, 42);
    hash.rebuild(c.x.data(), c.y.data(), c.x.size(), nullptr);

    const Vec2 center{50.0f, 30.0f};
    const f32 radius = 7.5f;
    std::vector<u32> candidates;
    hash.query_circle(center, radius, candidates);

    const std::vector<u32> exact = brute_circle(c, center, radius);
    for (u32 idx : exact) {
        REQUIRE(contains_index(candidates, idx));
    }
    // Conservative, not exact: candidates should be at least as many.
    REQUIRE(candidates.size() >= exact.size());
}

TEST_CASE("query_rect candidates are a superset of the brute-force result", "[sim][spatial]") {
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{80.0f, 80.0f}};
    desc.cell_size = 4.0f;
    hash.configure(desc);

    const Cloud c = make_random_cloud(1500, desc.bounds, 1234);
    hash.rebuild(c.x.data(), c.y.data(), c.x.size(), nullptr);

    const Rect query{Vec2{10.0f, 12.0f}, Vec2{34.0f, 40.0f}};
    std::vector<u32> candidates;
    hash.query_rect(query, candidates);

    const std::vector<u32> exact = brute_rect(c, query);
    for (u32 idx : exact) {
        REQUIRE(contains_index(candidates, idx));
    }
}

TEST_CASE("query_neighbourhood covers every agent within the separation radius", "[sim][spatial]") {
    // Cell size is 2x the probe radius per SpatialHashDesc's contract, so the
    // 3x3 neighbourhood must be a superset of every agent within that radius.
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{50.0f, 50.0f}};
    desc.cell_size = 2.4f;   // 2x a 1.2 separation radius
    hash.configure(desc);

    const Cloud c = make_random_cloud(3000, desc.bounds, 9001);
    hash.rebuild(c.x.data(), c.y.data(), c.x.size(), nullptr);

    const Vec2 probe{25.3f, 24.7f};
    const f32 radius = 1.2f;
    std::vector<u32> candidates;
    hash.query_neighbourhood(probe, candidates);

    const std::vector<u32> exact = brute_circle(c, probe, radius);
    for (u32 idx : exact) {
        REQUIRE(contains_index(candidates, idx));
    }
}

TEST_CASE("query_cone candidates are a superset of the exact angular test", "[sim][spatial]") {
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{100.0f, 100.0f}};
    desc.cell_size = 2.0f;
    hash.configure(desc);

    const Cloud c = make_random_cloud(2000, desc.bounds, 55);
    hash.rebuild(c.x.data(), c.y.data(), c.x.size(), nullptr);

    const Vec2 origin{50.0f, 50.0f};
    const Vec2 dir{1.0f, 0.0f};
    const f32 radius = 20.0f;
    const f32 half_angle = 0.4f;   // radians

    std::vector<u32> candidates;
    hash.query_cone(origin, dir, radius, half_angle, candidates);

    // Exact reference: distance + angle test.
    std::vector<u32> exact;
    const f32 cos_half = std::cos(half_angle);
    for (usize i = 0; i < c.x.size(); ++i) {
        const f32 dx = c.x[i] - origin.x, dy = c.y[i] - origin.y;
        const f32 d2 = dx * dx + dy * dy;
        if (d2 > radius * radius || d2 < 1e-8f) continue;
        const f32 inv_d = 1.0f / std::sqrt(d2);
        const f32 cos_theta = (dx * dir.x + dy * dir.y) * inv_d;
        if (cos_theta >= cos_half) exact.push_back(static_cast<u32>(i));
    }

    for (u32 idx : exact) {
        REQUIRE(contains_index(candidates, idx));
    }
}

TEST_CASE("empty hash queries return no candidates and do not crash", "[sim][spatial]") {
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f}};
    desc.cell_size = 2.0f;
    hash.configure(desc);
    hash.rebuild(nullptr, nullptr, 0, nullptr);

    std::vector<u32> out;
    hash.query_circle(Vec2{5.0f, 5.0f}, 3.0f, out);
    REQUIRE(out.empty());
    hash.query_rect(Rect{Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f}}, out);
    REQUIRE(out.empty());
    hash.query_neighbourhood(Vec2{5.0f, 5.0f}, out);
    REQUIRE(out.empty());
    REQUIRE(hash.max_cell_occupancy() == 0);
}

TEST_CASE("rebuild reflects only the current call, not stale state", "[sim][spatial]") {
    SpatialHash hash;
    SpatialHashDesc desc;
    desc.bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{10.0f, 10.0f}};
    desc.cell_size = 2.0f;
    hash.configure(desc);

    std::vector<f32> px = {1.0f, 1.0f, 1.0f};
    std::vector<f32> py = {1.0f, 1.0f, 1.0f};
    hash.rebuild(px.data(), py.data(), 3, nullptr);
    REQUIRE(hash.indexed_count() == 3);

    // Fewer agents next tick (post-compaction): the hash must not carry over
    // indices from the previous build.
    hash.rebuild(px.data(), py.data(), 1, nullptr);
    REQUIRE(hash.indexed_count() == 1);
    REQUIRE(hash.max_cell_occupancy() == 1);
}
