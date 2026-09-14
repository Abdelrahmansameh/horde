// Tests for game/level/RenderSdf.h -- the smooth, corner-rounded distance
// field the tissue pass draws from. The claims under test: the field is a real
// signed distance (unit gradient, zero on the vessel wall the splines
// describe), it is smooth to well under a texel along a straight wall, the
// inside of a bend gets a fillet of the requested radius, and the fillet never
// eats wall an agent could actually reach.
#include "game/level/Level.h"
#include "game/level/RenderSdf.h"
#include "platform/FileIO.h"
#include "sim/flowfield/FlowField.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace immune;
using namespace immune::game;
using Catch::Approx;

namespace {

LevelDef load(const std::string& json) {
    LevelDef def;
    LevelLoader loader;
    const LevelLoadResult r = loader.load_string(json, def);
    REQUIRE(r.ok);
    return def;
}

/// A single straight horizontal lane 30 wide, y in [35, 65], well inside a
/// 200x100 world.
const char* kStraight = R"JSON({
  "schema": 1, "name": "straight", "region": "capillary",
  "world": { "min": [0, 0], "max": [200, 100], "cell_size": 0.5 },
  "vessels": [ { "id": "main", "points": [
      { "p": [30, 50], "w": 30.0 }, { "p": [100, 50], "w": 30.0 }, { "p": [170, 50], "w": 30.0 } ] } ],
  "spawn_points": [ { "id": "p0", "pos": [22, 50], "radius": 4.0 } ],
  "objectives": [ { "id": "organ", "pos": [178, 50], "radius": 5.0, "integrity": 100 } ],
  "waves": [ { "name": "w1", "prep_time": 5.0, "atp_reward": 40,
      "spawns": [ { "family": "virus", "count": 10, "start_time": 0.0, "duration": 1.0 } ] } ]
})JSON";

/// An L: right along y=30, then a hard corner at (120,30), then up. Control
/// points are dense round the corner so Catmull-Rom does not swing wide, which
/// makes the inside of the bend a genuine right angle.
const char* kElbow = R"JSON({
  "schema": 1, "name": "elbow", "region": "capillary",
  "world": { "min": [0, 0], "max": [200, 200], "cell_size": 0.5 },
  "vessels": [ { "id": "main", "points": [
      { "p": [10, 30], "w": 24.0 }, { "p": [60, 30], "w": 24.0 }, { "p": [110, 30], "w": 24.0 },
      { "p": [120, 30], "w": 24.0 }, { "p": [120, 40], "w": 24.0 },
      { "p": [120, 90], "w": 24.0 }, { "p": [120, 190], "w": 24.0 } ] } ],
  "spawn_points": [ { "id": "p0", "pos": [12, 30], "radius": 4.0 } ],
  "objectives": [ { "id": "organ", "pos": [120, 188], "radius": 5.0, "integrity": 100 } ],
  "waves": [ { "name": "w1", "prep_time": 5.0, "atp_reward": 40,
      "spawns": [ { "family": "virus", "count": 10, "start_time": 0.0, "duration": 1.0 } ] } ]
})JSON";

} // namespace

TEST_CASE("render sdf: a straight wall is a straight, unit-gradient distance", "[render_sdf]") {
    const LevelDef def = load(kStraight);
    const RenderSdf sdf = bake_render_sdf(def);
    REQUIRE(sdf.valid());
    // Padded past the level on every side.
    CHECK(sdf.bounds.min.x < def.world_bounds.min.x - 10.0f);
    CHECK(sdf.bounds.max.y > def.world_bounds.max.y + 10.0f);

    // Along the lane's centre the field is the half-width; along the wall it
    // is zero; a few units out it is minus that. Sampled at many x so a single
    // stair-step would show.
    f32 max_err = 0.0f;
    for (f32 x = 40.0f; x <= 160.0f; x += 0.37f) {
        // The centreline is the field's crease (both walls equidistant), and
        // bilinear sampling across a crease clips its peak by up to half a
        // texel; that is the one place a looser tolerance is legitimate.
        CHECK(sdf.sample(Vec2{x, 50.0f}) == Approx(15.0f).margin(sdf.cell_size * 0.5f));
        max_err = std::fmax(max_err, std::fabs(sdf.sample(Vec2{x, 65.0f}) - 0.0f));
        max_err = std::fmax(max_err, std::fabs(sdf.sample(Vec2{x, 35.0f}) - 0.0f));
        max_err = std::fmax(max_err, std::fabs(sdf.sample(Vec2{x, 71.0f}) + 6.0f));
        max_err = std::fmax(max_err, std::fabs(sdf.sample(Vec2{x, 59.0f}) - 6.0f));
    }
    std::fprintf(stderr, "[render_sdf] straight max_err=%.4f cell=%.3f R=%.2f\n", max_err,
                 sdf.cell_size, sdf.round_radius);
    // A tenth of a texel: the tolerance a binary EDT could never meet.
    CHECK(max_err < sdf.cell_size * 0.1f);
}

TEST_CASE("render sdf: a vessel reaching the edge runs off the grid", "[render_sdf]") {
    LevelDef def = load(kStraight);
    // Push the ends to the world edge.
    def.vessels[0].points.front().position = Vec2{2.0f, 50.0f};
    def.vessels[0].points.back().position = Vec2{198.0f, 50.0f};
    const RenderSdf sdf = bake_render_sdf(def);
    // Well outside the world the lane is still open, at full width.
    CHECK(sdf.sample(Vec2{-30.0f, 50.0f}) == Approx(15.0f).margin(0.2f));
    CHECK(sdf.sample(Vec2{240.0f, 50.0f}) == Approx(15.0f).margin(0.2f));
    // Whereas a vessel that ends inside the world keeps its cap.
    const RenderSdf capped = bake_render_sdf(load(kStraight));
    CHECK(capped.sample(Vec2{-30.0f, 50.0f}) < -10.0f);
}

TEST_CASE("render sdf: the inside of a bend is filleted to the round radius", "[render_sdf]") {
    const LevelDef def = load(kElbow);
    RenderSdfDesc desc;
    desc.round_frac = 0.25f;   // 6 units on a 24-wide lane
    const RenderSdf sdf = bake_render_sdf(def, desc);
    REQUIRE(sdf.valid());
    const f32 R = sdf.round_radius;
    CHECK(R == Approx(6.0f));

    // Horizontal leg: lumen y in (18, 42). Vertical leg: lumen x in (108, 132).
    // The inside of the bend is the wall quadrant x < 108, y > 42, whose tip
    // is at (108, 42).
    const Vec2 tip{108.0f, 42.0f};
    // Unfilleted, the tip sits on the wall (d = 0). Filleted with radius R,
    // the arc's deepest point along the bisector is R(sqrt2 - 1) into the old
    // wall, so the tip is now inside the lumen by that much.
    const f32 expect = R * (std::sqrt(2.0f) - 1.0f);
    const f32 at_tip = sdf.sample(tip);
    std::fprintf(stderr, "[render_sdf] elbow tip d=%.3f expect=%.3f\n", at_tip, expect);
    CHECK(at_tip == Approx(expect).margin(0.35f));
    // The arc's centre sits R from both walls; the field there is exactly -R.
    CHECK(sdf.sample(tip + Vec2{-R, R}) == Approx(-R).margin(0.4f));
    // Far along each wall the fillet has no effect: still exactly on the wall.
    CHECK(sdf.sample(Vec2{108.0f, 42.0f + 3.0f * R}) == Approx(0.0f).margin(0.15f));
    CHECK(sdf.sample(Vec2{108.0f - 3.0f * R, 42.0f}) == Approx(0.0f).margin(0.15f));
    // The outer corner is a convex lumen arc of radius 12 about (120, 30) and
    // is untouched: the field on it is still zero.
    const Vec2 outer = Vec2{120.0f, 30.0f} + Vec2{12.0f, -12.0f} * static_cast<f32>(1.0 / std::sqrt(2.0));
    CHECK(sdf.sample(outer) == Approx(0.0f).margin(0.2f));
}

TEST_CASE("render sdf: the sim mask IS the drawn field on every shipped level",
          "[render_sdf][content]") {
    // The invariant the whole feature rests on: what the tissue pass draws as
    // lumen is exactly where the sim lets agents go. bake_geometry() writes
    // the field's sign back into walkability, so here every sim cell's
    // walkable bit must match the field's sign at that cell's centre -- in
    // BOTH directions. A thin septum or a capsule bar swallowed by the
    // closing cannot hide: it would have been swallowed in the sim too, so
    // this also pins the wall-protection by checking the stamped geometry
    // against the field before the write-back (walls never get thinner).
    const std::string dir = platform::asset_path("levels");
    REQUIRE(std::filesystem::is_directory(dir));

    u32 checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const std::string path = entry.path().string();
        INFO("level = " << path);

        LevelLoader loader;
        LevelDef def;
        REQUIRE(loader.load_file(path, def).ok);

        sim::TissueMask mask;
        sim::DistanceField sdf;
        sim::FlowField flow;
        GeometryBakeDesc bake;
        GeometryBakeStats stats;
        RenderSdf rs;
        REQUIRE(loader.bake_geometry(def, bake, mask, sdf, flow, &stats, &rs).ok);
        REQUIRE(rs.valid());

        u32 mismatch = 0, cells = 0;
        for (i32 y = 0; y < mask.height(); ++y) {
            for (i32 x = 0; x < mask.width(); ++x) {
                const f32 d = rs.sample(mask.cell_to_world(x, y));
                ++cells;
                if (mask.walkable(x, y) != (d > 0.0f)) ++mismatch;
            }
        }
        std::fprintf(stderr,
                     "[render_sdf] %-36s cell=%.3f R=%.2f bake=%.1fms  mismatch=%u/%u\n",
                     def.name.c_str(), rs.cell_size, rs.round_radius, stats.render_sdf_ms,
                     mismatch, cells);
        CHECK(mismatch == 0);
        ++checked;
    }
    REQUIRE(checked >= 13);
}

TEST_CASE("render sdf: the closing never thins a wall a level authored", "[render_sdf][content]") {
    // The protection in bake_render_sdf() exists for exactly one reason: a
    // wall that is thinner than the fillet's depth bound (a septum between
    // switchback passes, a bar across a chamber) would otherwise be closed
    // over. Every wall cell the stamp produced that is at least a couple of
    // sim cells deep must still be wall in the field.
    const std::string dir = platform::asset_path("levels");
    u32 checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const std::string path = entry.path().string();
        INFO("level = " << path);
        LevelLoader loader;
        LevelDef def;
        REQUIRE(loader.load_file(path, def).ok);

        RenderSdfDesc desc;
        // No run-off past the world edge: the shoulders between a vessel's
        // end cap and its straight run-off are the one place the field shows
        // lumen the stamp never had, on purpose.
        desc.edge_extend_widths = 0.0f;
        const RenderSdf rs = bake_render_sdf(def, desc);
        RenderSdfDesc raw = desc;
        raw.round_frac = 0.0f;   // the stamped geometry, no closing at all
        const RenderSdf plain = bake_render_sdf(def, raw);
        REQUIRE(rs.valid());
        REQUIRE(plain.valid());
        const f32 depth = rs.round_radius * desc.round_depth_frac;

        // Wall deeper than the fillet bound in the plain field must be wall
        // in the closed one; anything shallower is fillet territory.
        u32 eaten = 0, walls = 0;
        f32 worst = 0.0f;
        for (i32 y = 0; y < rs.height; ++y) {
            for (i32 x = 0; x < rs.width; ++x) {
                const usize i = static_cast<usize>(y) * static_cast<usize>(rs.width) + static_cast<usize>(x);
                const f32 before = plain.distance[i];
                if (before > -(depth + rs.cell_size)) continue;
                ++walls;
                const f32 after = rs.distance[i];
                if (after > rs.cell_size) { ++eaten; worst = std::fmax(worst, after); }
            }
        }
        std::fprintf(stderr, "[render_sdf] %-36s deep walls=%u eaten=%u (worst %.2f)\n",
                     def.name.c_str(), walls, eaten, worst);
        CHECK(eaten == 0);
        ++checked;
    }
    REQUIRE(checked >= 13);
}
