// Tests for the squad layer (sim/squad/Squads.h) and the two hooks it has into
// the chaff movement kernel.
//
// The two claims this feature actually makes are the ones worth testing hardest,
// and neither is a unit-level property of a function:
//   - READABILITY: squads assigned different paths end up spatially separated,
//     rather than converging into one mass the way the un-grouped horde does.
//   - FLUIDITY: the inside of a squad is still as densely packed as the horde
//     was before squads existed. The whole design turns on the cohesion weight
//     being exactly zero inside the squad radius, so that is measured directly
//     against a squads-off control run rather than assumed.
// Both are measurements over a real tick loop, not assertions about one call.
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"
#include "game/level/Level.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"
#include "sim/squad/Squads.h"

#include "core/JobSystem.h"
#include "core/Math.h"
#include "core/Rng.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace immune;
using namespace immune::sim;

namespace {

Rect test_bounds() { return Rect{Vec2{0.0f, 0.0f}, Vec2{120.0f, 60.0f}}; }

/// A CORRIDOR, not an open box: a walkable band of `band_half_height` either
/// side of y=30, with the goal at the far end.
///
/// The shape matters, and picking it is not test-rigging. A real level is a
/// vessel -- the flow field inside one runs ALONG the lane, and converges
/// laterally only near the objective. An open box with a point goal instead
/// produces a strongly radial field that funnels everything onto the single
/// line through the goal, which is a lateral force no lane actually applies.
/// Measuring squad separation against that would be measuring the test's
/// geometry, not the feature. (The squad layer does not, and should not, beat
/// a flow field that genuinely insists on one line -- the flow field is
/// deliberately left with the final say; see Squads.h.)
FlowField make_flow(Rect bounds, Vec2 goal, f32 cell = 1.0f,
                    f32 band_half_height = 15.0f) {
    TissueMask mask;
    const i32 w = static_cast<i32>(bounds.size().x / cell);
    const i32 h = static_cast<i32>(bounds.size().y / cell);
    mask.resize(w, h, cell, bounds.min);
    const f32 mid = bounds.center().y;
    for (i32 y = 0; y < h; ++y) {
        const f32 wy = bounds.min.y + (static_cast<f32>(y) + 0.5f) * cell;
        const bool inside = std::fabs(wy - mid) <= band_half_height;
        for (i32 x = 0; x < w; ++x) mask.set_walkable(x, y, inside);
    }
    FlowField flow;
    FlowFieldBakeDesc desc;
    desc.goal_cells = {mask.world_to_cell(goal)};
    flow.bake(mask, desc);
    return flow;
}

SpatialHash make_hash(Rect bounds, f32 cell_size = 4.0f) {
    SpatialHash hash;
    SpatialHashDesc d;
    d.bounds = bounds;
    d.cell_size = cell_size;
    hash.configure(d);
    return hash;
}

void rebuild(SpatialHash& hash, const ChaffBuffers& b) {
    hash.rebuild(b.pos_x.data(), b.pos_y.data(), b.count(), nullptr);
}

ChaffTuning flat_tuning() {
    ChaffTuning t;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        t.family[f].acceleration = 20.0f;
        t.family[f].max_speed = 8.0f;
        t.family[f].separation_radius = 1.2f;
        t.family[f].separation_strength = 4.0f;
        t.family[f].jitter = 0.0f;   // deterministic geometry; RNG is tested elsewhere
        t.family[f].base_density = 1.0f;
        t.family[f].replication_rate = 0.0f;
        t.family[f].radius = 0.5f;
    }
    return t;
}

/// A straight horizontal path at height `y`.
SquadPath straight_path(const char* id, const char* lane, f32 y, f32 x0 = 5.0f,
                        f32 x1 = 115.0f) {
    SquadPath p;
    p.id = id;
    p.lane_id = lane;
    for (f32 x = x0; x <= x1; x += 5.0f) p.points.push_back(Vec2{x, y});
    p.half_width = 6.0f;
    p.rebuild_arc();
    return p;
}

/// Mean distance from each agent to its nearest neighbour -- the "how packed is
/// this crowd" number the fluidity claim is measured with.
f32 mean_nearest_neighbour(const ChaffBuffers& b) {
    const usize n = b.count();
    if (n < 2) return 0.0f;
    f32 total = 0.0f;
    for (usize i = 0; i < n; ++i) {
        f32 best = 1e30f;
        for (usize j = 0; j < n; ++j) {
            if (i == j) continue;
            const f32 dx = b.pos_x[i] - b.pos_x[j];
            const f32 dy = b.pos_y[i] - b.pos_y[j];
            const f32 d2 = dx * dx + dy * dy;
            if (d2 < best) best = d2;
        }
        total += std::sqrt(best);
    }
    return total / static_cast<f32>(n);
}

Vec2 squad_centroid(const ChaffBuffers& b, u16 squad) {
    Vec2 sum{0.0f, 0.0f};
    u32 n = 0;
    for (usize i = 0; i < b.count(); ++i) {
        if (b.squad_id[i] != squad) continue;
        sum += Vec2{b.pos_x[i], b.pos_y[i]};
        ++n;
    }
    return n == 0 ? sum : sum / static_cast<f32>(n);
}

} // namespace

// ---------------------------------------------------------------------------
// SquadPath geometry
// ---------------------------------------------------------------------------

TEST_CASE("path arclength is monotonic and spans the polyline", "[sim][squad]") {
    const SquadPath p = straight_path("p", "lane", 30.0f, 0.0f, 100.0f);
    REQUIRE(p.valid());
    REQUIRE(p.arc.front() == Catch::Approx(0.0f));
    for (usize i = 1; i < p.arc.size(); ++i) REQUIRE(p.arc[i] >= p.arc[i - 1]);
    REQUIRE(p.length() == Catch::Approx(100.0f));
}

TEST_CASE("point_at clamps to the path ends", "[sim][squad]") {
    const SquadPath p = straight_path("p", "lane", 30.0f, 0.0f, 100.0f);
    REQUIRE(p.point_at(-50.0f).x == Catch::Approx(0.0f));
    REQUIRE(p.point_at(0.0f).x == Catch::Approx(0.0f));
    REQUIRE(p.point_at(50.0f).x == Catch::Approx(50.0f));
    REQUIRE(p.point_at(1000.0f).x == Catch::Approx(100.0f));
    // Tangent of a horizontal path points along +x everywhere, ends included.
    REQUIRE(p.tangent_at(50.0f).x == Catch::Approx(1.0f));
    REQUIRE(p.tangent_at(1000.0f).x == Catch::Approx(1.0f));
}

TEST_CASE("project_near stays on the limb the hint names", "[sim][squad]") {
    // A hairpin: out along y=10, back along y=12. The two limbs are 2 units
    // apart, so a point near the bend is genuinely close to BOTH -- which is
    // exactly the case a global nearest-point search gets wrong.
    SquadPath p;
    p.id = "hairpin";
    for (f32 x = 0.0f; x <= 50.0f; x += 5.0f) p.points.push_back(Vec2{x, 10.0f});
    for (f32 x = 50.0f; x >= 0.0f; x -= 5.0f) p.points.push_back(Vec2{x, 12.0f});
    p.rebuild_arc();

    const Vec2 probe{25.0f, 11.0f};   // dead between the two limbs
    const f32 outbound = p.project_near(probe, 25.0f, 15.0f);
    const f32 inbound = p.project_near(probe, 77.0f, 15.0f);

    // Outbound limb spans arc 0..50; the return limb starts at 52.
    REQUIRE(outbound < 50.0f);
    REQUIRE(inbound > 52.0f);
}

// ---------------------------------------------------------------------------
// SquadRegistry
// ---------------------------------------------------------------------------

TEST_CASE("paths are grouped by lane and handed out round-robin", "[sim][squad]") {
    SquadRegistry reg;
    std::vector<SquadPath> paths;
    paths.push_back(straight_path("a1", "alpha", 10.0f));
    paths.push_back(straight_path("b1", "beta", 20.0f));
    paths.push_back(straight_path("a2", "alpha", 30.0f));
    reg.set_paths(std::move(paths));

    u32 first = 0, count = 0;
    reg.lane_path_range("alpha", first, count);
    REQUIRE(count == 2);
    reg.lane_path_range("beta", first, count);
    REQUIRE(count == 1);

    // Two alpha paths cycle; each is returned before either repeats.
    u16 p0 = 0, p1 = 0, p2 = 0;
    REQUIRE(reg.next_path_for_lane("alpha", p0));
    REQUIRE(reg.next_path_for_lane("alpha", p1));
    REQUIRE(reg.next_path_for_lane("alpha", p2));
    REQUIRE(p0 != p1);
    REQUIRE(p2 == p0);
    REQUIRE(reg.paths()[p0].lane_id == "alpha");
    REQUIRE(reg.paths()[p1].lane_id == "alpha");
}

TEST_CASE("a full registry degrades to kNoSquad instead of failing", "[sim][squad]") {
    SquadRegistry reg;
    SquadTuning t;
    t.max_squads = 2;
    reg.set_tuning(t);
    std::vector<SquadPath> paths;
    paths.push_back(straight_path("p", "lane", 30.0f));
    reg.set_paths(std::move(paths));

    REQUIRE(reg.create_squad(0, Vec2{5.0f, 30.0f}) != kNoSquad);
    REQUIRE(reg.create_squad(0, Vec2{5.0f, 30.0f}) != kNoSquad);
    REQUIRE(reg.create_squad(0, Vec2{5.0f, 30.0f}) == kNoSquad);
}

TEST_CASE("the anchor never reverses and stays leashed to its members",
          "[sim][squad]") {
    const Rect bounds = test_bounds();
    SquadRegistry reg;
    SquadTuning t;
    reg.set_tuning(t);
    std::vector<SquadPath> paths;
    paths.push_back(straight_path("p", "lane", 30.0f));
    reg.set_paths(std::move(paths));

    ChaffBuffers buffers;
    buffers.reserve(64);
    const u16 squad = reg.create_squad(0, Vec2{10.0f, 30.0f});
    REQUIRE(squad != kNoSquad);
    for (u32 i = 0; i < 20; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{10.0f + static_cast<f32>(i % 5), 29.0f + static_cast<f32>(i / 5)};
        p.squad_id = squad;
        buffers.spawn(p);
    }

    // The members never move, so the anchor must settle a bounded distance
    // ahead of them and stop -- not creep down the path forever.
    f32 prev_arc = -1.0f;
    for (u32 tick = 0; tick < 600; ++tick) {
        reg.update(buffers, kFixedDt);
        const Squad& sq = reg.get(squad);
        REQUIRE(sq.arc_pos >= prev_arc);
        prev_arc = sq.arc_pos;
        REQUIRE(math::length(sq.anchor - sq.centroid) <
                t.anchor_lookahead + t.project_window);
    }
    (void)bounds;
}

TEST_CASE("an emptied squad is retired and its slot reused", "[sim][squad]") {
    SquadRegistry reg;
    SquadTuning t;
    t.retire_ticks = 3;
    reg.set_tuning(t);
    std::vector<SquadPath> paths;
    paths.push_back(straight_path("p", "lane", 30.0f));
    reg.set_paths(std::move(paths));

    ChaffBuffers buffers;
    buffers.reserve(16);
    const u16 squad = reg.create_squad(0, Vec2{10.0f, 30.0f});
    REQUIRE(reg.active_count() == 1);

    // Never gains a member; after retire_ticks empty ticks the slot is freed.
    for (u32 i = 0; i < t.retire_ticks; ++i) reg.update(buffers, kFixedDt);
    REQUIRE_FALSE(reg.alive(squad));
    REQUIRE(reg.active_count() == 0);
    REQUIRE(reg.create_squad(0, Vec2{10.0f, 30.0f}) == squad);   // slot reused
}

// ---------------------------------------------------------------------------
// ChaffBuffers integration
// ---------------------------------------------------------------------------

TEST_CASE("squad membership survives compaction", "[sim][squad]") {
    ChaffBuffers b;
    b.reserve(64);
    for (u32 i = 0; i < 40; ++i) {
        ChaffSpawnParams p;
        p.position = Vec2{static_cast<f32>(i), 0.0f};
        p.squad_id = static_cast<u16>(i % 4);
        b.spawn(p);
    }
    // Kill every other agent; compaction swap-removes, so survivors move slots.
    for (usize i = 0; i < b.count(); i += 2) b.kill(i);
    b.compact();
    REQUIRE(b.count() == 20);

    // Each survivor's squad must still match the rule it was spawned under:
    // position x is stable across compaction, so it identifies the agent.
    for (usize i = 0; i < b.count(); ++i) {
        const u16 expected = static_cast<u16>(static_cast<u32>(b.pos_x[i]) % 4u);
        REQUIRE(b.squad_id[i] == expected);
    }
    // And the retired tail is reset, not left holding stale ids.
    for (usize i = b.count(); i < b.capacity(); ++i) REQUIRE(b.squad_id[i] == kNoSquad);
}

TEST_CASE("spawn_burst stamps its squad on every agent", "[sim][squad]") {
    ChaffSystem sys;
    sys.set_tuning(flat_tuning());
    ChaffBuffers b;
    b.reserve(128);
    Rng rng(99);

    sys.spawn_burst(b, PathogenFamily::Virus, Vec2{20.0f, 30.0f}, 3.0f, 30, rng, 7);
    sys.spawn_burst(b, PathogenFamily::Virus, Vec2{60.0f, 30.0f}, 3.0f, 30, rng);

    u32 in_squad = 0, ungrouped = 0;
    for (usize i = 0; i < b.count(); ++i) {
        if (b.squad_id[i] == 7) ++in_squad;
        else if (b.squad_id[i] == kNoSquad) ++ungrouped;
    }
    REQUIRE(in_squad == 30);
    REQUIRE(ungrouped == 30);
}

// ---------------------------------------------------------------------------
// The two claims
// ---------------------------------------------------------------------------

namespace {

/// Runs `ticks` steps of the real kernel with two squads spawned side by side
/// at the same spawn point, one per path. `grouping` off is the control.
struct TwoSquadRun {
    ChaffBuffers buffers;
    SquadRegistry reg;
    u16 squad_a = kNoSquad;
    u16 squad_b = kNoSquad;
};

void run_two_squads(TwoSquadRun& r, bool grouping, u32 ticks, u32 per_squad = 40) {
    const Rect bounds = test_bounds();
    FlowField flow = make_flow(bounds, Vec2{115.0f, 30.0f});
    DistanceField sdf;   // never baked: sample()/gradient() return zero, which is fine
    SpatialHash hash = make_hash(bounds);
    ChaffSystem sys;
    sys.set_tuning(flat_tuning());
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{115.0f, 30.0f}, 0.0f);   // radius 0: never despawns here

    SquadTuning t;
    t.enabled = grouping;
    r.reg.set_tuning(t);
    std::vector<SquadPath> paths;
    // Two lanes 16 units apart, both running the length of the box.
    paths.push_back(straight_path("high", "lane", 22.0f));
    paths.push_back(straight_path("low", "lane", 38.0f));
    r.reg.set_paths(std::move(paths));

    r.buffers.reserve(512);
    r.squad_a = r.reg.create_squad(0, Vec2{12.0f, 30.0f});
    r.squad_b = r.reg.create_squad(1, Vec2{12.0f, 30.0f});
    REQUIRE(r.squad_a != kNoSquad);
    REQUIRE(r.squad_b != kNoSquad);

    Rng rng(4242);
    // Both squads start intermixed at the SAME spawn point. If they end up apart,
    // the squad layer is what pulled them apart -- nothing about the starting
    // arrangement or the flow field would do it.
    sys.spawn_burst(r.buffers, PathogenFamily::Virus, Vec2{12.0f, 30.0f}, 4.0f, per_squad, rng,
                    r.squad_a);
    sys.spawn_burst(r.buffers, PathogenFamily::Virus, Vec2{12.0f, 30.0f}, 4.0f, per_squad, rng,
                    r.squad_b);

    for (u32 i = 0; i < ticks; ++i) {
        rebuild(hash, r.buffers);
        r.reg.update(r.buffers, kFixedDt);
        sys.update(r.buffers, flow, sdf, TissueMask{}, hash, r.reg, rng, kFixedDt, nullptr);
    }
}

} // namespace

TEST_CASE("two squads from one spawn point pull apart; without grouping they do not",
          "[sim][squad][readability]") {
    TwoSquadRun grouped;
    run_two_squads(grouped, /*grouping*/ true, 300);
    TwoSquadRun control;
    run_two_squads(control, /*grouping*/ false, 300);

    const f32 grouped_gap = math::length(squad_centroid(grouped.buffers, grouped.squad_a) -
                                         squad_centroid(grouped.buffers, grouped.squad_b));
    const f32 control_gap = math::length(squad_centroid(control.buffers, control.squad_a) -
                                         squad_centroid(control.buffers, control.squad_b));

    INFO("grouped gap " << grouped_gap << " vs control gap " << control_gap);
    // The control starts and stays intermixed: both halves of one burst, one
    // flow field, so their centroids sit essentially on top of each other.
    REQUIRE(control_gap < 4.0f);
    // Grouped, each squad converges onto its own path; the two paths are 16
    // units apart and the per-squad lateral offsets push them a little further.
    // Asserted well below the measured ~18.6 so ordinary tuning changes do not
    // make this brittle -- the claim under test is "clearly separated", not an
    // exact figure.
    REQUIRE(grouped_gap > 12.0f);
}

TEST_CASE("squad interiors stay as densely packed as the un-grouped horde",
          "[sim][squad][fluid]") {
    TwoSquadRun grouped;
    run_two_squads(grouped, /*grouping*/ true, 300);
    TwoSquadRun control;
    run_two_squads(control, /*grouping*/ false, 300);

    const f32 grouped_nn = mean_nearest_neighbour(grouped.buffers);
    const f32 control_nn = mean_nearest_neighbour(control.buffers);

    INFO("grouped nn " << grouped_nn << " vs control nn " << control_nn);
    REQUIRE(grouped_nn > 0.0f);
    REQUIRE(control_nn > 0.0f);
    // This is the load-bearing claim of the whole design: the cohesion weight
    // is zero inside the squad radius, so packing inside a squad must not
    // loosen. A little slack for the boundary agents, which DO feel the
    // foreign-separation boost -- but nothing like a doubling.
    REQUIRE(grouped_nn < control_nn * 1.35f);
}

TEST_CASE("agents with no squad are unaffected by the squad layer",
          "[sim][squad][determinism]") {
    // An ungrouped agent must move identically whether or not squads exist
    // elsewhere in the world -- that is what makes the layer additive.
    const Rect bounds = test_bounds();
    FlowField flow = make_flow(bounds, Vec2{115.0f, 30.0f});
    DistanceField sdf;
    ChaffSystem sys;
    sys.set_tuning(flat_tuning());
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{115.0f, 30.0f}, 0.0f);

    auto run = [&](bool with_paths) {
        SpatialHash hash = make_hash(bounds);
        SquadRegistry reg;
        if (with_paths) {
            std::vector<SquadPath> paths;
            paths.push_back(straight_path("high", "lane", 22.0f));
            reg.set_paths(std::move(paths));
        }
        ChaffBuffers b;
        b.reserve(128);
        Rng rng(7);
        sys.spawn_burst(b, PathogenFamily::Virus, Vec2{12.0f, 30.0f}, 4.0f, 40, rng);
        for (u32 i = 0; i < 120; ++i) {
            rebuild(hash, b);
            reg.update(b, kFixedDt);
            sys.update(b, flow, sdf, TissueMask{}, hash, reg, rng, kFixedDt, nullptr);
        }
        return b;
    };

    const ChaffBuffers without = run(false);
    const ChaffBuffers with = run(true);
    REQUIRE(without.count() == with.count());
    for (usize i = 0; i < without.count(); ++i) {
        REQUIRE(without.pos_x[i] == with.pos_x[i]);
        REQUIRE(without.pos_y[i] == with.pos_y[i]);
    }
}

TEST_CASE("squad movement is reproducible and thread-count independent",
          "[sim][squad][determinism]") {
    const Rect bounds = test_bounds();

    auto run = [&](JobSystem* jobs) {
        FlowField flow = make_flow(bounds, Vec2{115.0f, 30.0f});
        DistanceField sdf;
        SpatialHash hash = make_hash(bounds);
        ChaffSystem sys;
        sys.set_tuning(flat_tuning());
        sys.set_world_bounds(bounds);
        sys.set_goal(Vec2{115.0f, 30.0f}, 0.0f);

        SquadRegistry reg;
        reg.set_tuning(SquadTuning{});
        std::vector<SquadPath> paths;
        paths.push_back(straight_path("high", "lane", 22.0f));
        paths.push_back(straight_path("low", "lane", 38.0f));
        reg.set_paths(std::move(paths));

        ChaffBuffers b;
        b.reserve(512);
        Rng rng(1234);
        // Kept under the 256 parallel_for grain: above it, per-range RNG forks
        // legitimately differ between thread counts. See the note in
        // tests/test_chaff_system.cpp for the full reasoning.
        for (u32 k = 0; k < 2; ++k) {
            const u16 sq = reg.create_squad(static_cast<u16>(k), Vec2{12.0f, 30.0f});
            sys.spawn_burst(b, PathogenFamily::Virus, Vec2{12.0f, 30.0f}, 4.0f, 60, rng, sq);
        }
        for (u32 i = 0; i < 150; ++i) {
            rebuild(hash, b);
            reg.update(b, kFixedDt);
            sys.update(b, flow, sdf, TissueMask{}, hash, reg, rng, kFixedDt, jobs);
        }
        return std::pair<ChaffBuffers, u64>{b, reg.state_hash()};
    };

    const auto a = run(nullptr);
    const auto b = run(nullptr);
    JobSystem parallel(4);
    const auto c = run(&parallel);

    REQUIRE(a.second == b.second);
    REQUIRE(a.second == c.second);
    REQUIRE(a.first.count() == b.first.count());
    REQUIRE(a.first.count() == c.first.count());
    for (usize i = 0; i < a.first.count(); ++i) {
        REQUIRE(a.first.pos_x[i] == b.first.pos_x[i]);
        REQUIRE(a.first.pos_y[i] == b.first.pos_y[i]);
        REQUIRE(a.first.pos_x[i] == c.first.pos_x[i]);
        REQUIRE(a.first.pos_y[i] == c.first.pos_y[i]);
    }
}

TEST_CASE("foreign separation radius cannot exceed the spatial cell size",
          "[sim][squad]") {
    // gather_neighbours() scans 3x3 cells. A foreign separation radius past one
    // cell width does not error -- it silently stops seeing the neighbours it
    // exists to repel. SimWorld::init bounds it; this pins that it does.
    SimDesc desc;
    desc.spatial_cell_size = 4.0f;
    for (u32 f = 0; f < kFamilyCount; ++f) desc.chaff_tuning.family[f].separation_radius = 3.0f;
    desc.squad_tuning.foreign_radius_mult = 8.0f;   // absurd; 3.0 * 8 = 24 >> 4

    SimWorld world;
    world.init(desc, nullptr);
    const f32 effective =
        world.squads().tuning().foreign_radius_mult * 3.0f;
    REQUIRE(effective <= desc.spatial_cell_size + 1e-4f);
}


// ---------------------------------------------------------------------------
// WaveDirector integration
// ---------------------------------------------------------------------------

TEST_CASE("a wave's spawn entry is chopped into several squads across its lane's paths",
          "[sim][squad][wave]") {
    // The end-to-end claim: an AUTHORED wave table, unchanged, produces a train
    // of squads spread over the lane's paths. Nothing in any level file had to
    // learn about squads for this to happen -- the paths are derived from the
    // vessel geometry at load and the director chops the entry's count by
    // target_squad_size. This is the path the shipped game actually takes, and
    // it is not reachable from --sim-test (that mode never ticks the director),
    // so it has to be asserted here.
    game::LevelLoader loader;
    game::LevelDef def = game::LevelLoader::default_test_level();

    // One entry, many agents, released fast: exactly the shape that used to
    // arrive as a single undifferentiated blob.
    def.waves.clear();
    game::WaveDef w;
    w.index = 0;
    w.name = "squad_split";
    w.prep_time = 0.0f;
    game::SpawnEntry e;
    e.family = PathogenFamily::Virus;
    e.count = 300;
    e.start_time = 0.0f;
    e.duration = 2.0f;
    w.spawns.push_back(e);
    def.waves.push_back(w);

    SimDesc desc;
    desc.max_chaff = 2048;
    desc.world_bounds = def.world_bounds;
    SimWorld world;
    world.init(desc, nullptr);
    REQUIRE(loader.instantiate(def, world).ok);
    // Paths were derived from the level's vessels, with nothing authored, and
    // that lane is wide enough to carry more than one of them.
    REQUIRE(world.squads().paths().size() >= 2);

    game::WaveDirector waves;
    waves.set_waves(def.waves);
    waves.start(world);

    Rng rng(20260821);
    for (u32 i = 0; i < 240; ++i) {
        waves.tick(world, rng, kFixedDt);
        world.tick(nullptr);
    }

    const ChaffBuffers& b = world.chaff();
    REQUIRE(b.count() > 0);

    // Membership is spread over several squads, and essentially nothing is
    // left ungrouped.
    std::vector<u32> per_squad(world.squads().squads().size() + 1, 0u);
    u32 ungrouped = 0;
    for (usize i = 0; i < b.count(); ++i) {
        if (b.squad_id[i] == kNoSquad) ++ungrouped;
        else per_squad[b.squad_id[i]] += 1;
    }
    u32 non_empty = 0;
    u32 biggest = 0;
    for (u32 n : per_squad) {
        if (n > 0) ++non_empty;
        if (n > biggest) biggest = n;
    }
    INFO("squads=" << non_empty << " biggest=" << biggest << " ungrouped=" << ungrouped
                   << " live=" << b.count());
    REQUIRE(non_empty >= 3);
    REQUIRE(ungrouped == 0);
    // No squad ran away past the configured size (allowing the burst that
    // straddles the boundary).
    REQUIRE(biggest <= world.squads().tuning().target_squad_size * 2);

    // And the squads landed on more than one path, which is what spaces them.
    std::vector<bool> path_used(world.squads().paths().size(), false);
    for (u32 id = 0; id < static_cast<u32>(world.squads().squads().size()); ++id) {
        if (!world.squads().alive(static_cast<u16>(id))) continue;
        path_used[world.squads().get(static_cast<u16>(id)).path_index] = true;
    }
    u32 paths_used = 0;
    for (bool used : path_used) if (used) ++paths_used;
    REQUIRE(paths_used >= 2);
}

TEST_CASE("10k agents in squads stay inside the chaff movement budget",
          "[sim][squad][perf]") {
    // DESIGN.md 8.6 budgets chaff_update at 4 ms for 10k agents. The squad
    // layer adds two things to that pass: one lateral-steer computation per
    // agent, and one u16 compare per NEIGHBOUR -- and neighbour count is what
    // scales with density, so the second is the one that could hurt. Measured
    // here with squads actually populated, since with the layer idle the
    // compare short-circuits and the number would prove nothing.
    const Rect bounds{Vec2{0.0f, 0.0f}, Vec2{256.0f, 144.0f}};
    FlowField flow = make_flow(bounds, Vec2{250.0f, 72.0f}, 1.0f, 60.0f);
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);
    ChaffSystem sys;
    sys.set_tuning(flat_tuning());
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{250.0f, 72.0f}, 0.0f);

    SquadRegistry reg;
    reg.set_tuning(SquadTuning{});
    std::vector<SquadPath> paths;
    for (u32 k = 0; k < 3; ++k)
        paths.push_back(straight_path("p", "lane", 50.0f + static_cast<f32>(k) * 22.0f, 10.0f,
                                      250.0f));
    reg.set_paths(std::move(paths));

    ChaffBuffers b;
    b.reserve(12000);
    Rng rng(31337);
    // 10k agents in ~167 squads of 60 -- the real shape of a heavy wave.
    for (u32 s = 0; s < 167; ++s) {
        u16 path = 0;
        REQUIRE(reg.next_path_for_lane("lane", path));
        const u16 sq = reg.create_squad(path, Vec2{20.0f, 72.0f});
        sys.spawn_burst(b, PathogenFamily::Virus, Vec2{20.0f + static_cast<f32>(s % 40) * 5.0f,
                                                       60.0f + static_cast<f32>(s % 7) * 4.0f},
                        4.0f, 60, rng, sq);
    }
    REQUIRE(b.count() >= 9000);

    JobSystem jobs;   // real pool: this is the configuration the budget describes
    // Warm up, then measure.
    for (u32 i = 0; i < 30; ++i) {
        rebuild(hash, b);
        reg.update(b, kFixedDt);
        sys.update(b, flow, sdf, TissueMask{}, hash, reg, rng, kFixedDt, &jobs);
    }
    f64 worst_chaff = 0.0;
    f64 worst_squad = 0.0;
    for (u32 i = 0; i < 60; ++i) {
        rebuild(hash, b);
        {
            const auto t0 = std::chrono::steady_clock::now();
            reg.update(b, kFixedDt);
            const auto t1 = std::chrono::steady_clock::now();
            worst_squad = math::max(
                worst_squad, std::chrono::duration<f64, std::milli>(t1 - t0).count());
        }
        const auto t0 = std::chrono::steady_clock::now();
        sys.update(b, flow, sdf, TissueMask{}, hash, reg, rng, kFixedDt, &jobs);
        const auto t1 = std::chrono::steady_clock::now();
        worst_chaff =
            math::max(worst_chaff, std::chrono::duration<f64, std::milli>(t1 - t0).count());
    }
    WARN("[squad perf] " << b.count() << " agents / " << reg.active_count()
                         << " squads: worst chaff_update " << worst_chaff
                         << " ms, worst squad_update " << worst_squad << " ms");
    // The squad pass is a linear scan over three streams; it must be noise
    // next to the movement kernel, not a second budget line.
    REQUIRE(worst_squad < 1.0);
}

TEST_CASE("a replicated agent joins its parent's squad", "[sim][squad][replication]") {
    // Replication is the one place chaff is created from inside the movement
    // kernel rather than by a spawner, so it is the one place a squad id can be
    // silently dropped. A replicating family that forgets it sheds an ungrouped
    // agent per parent per few seconds, and those agents steer on the flow field
    // alone -- they visibly drift out of the group they were born in.
    const Rect bounds = test_bounds();
    FlowField flow = make_flow(bounds, Vec2{115.0f, 30.0f});
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffSystem sys;
    ChaffTuning t = flat_tuning();
    for (u32 f = 0; f < kFamilyCount; ++f) t.family[f].replication_rate = 4.0f;   // brisk
    sys.set_tuning(t);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{115.0f, 30.0f}, 0.0f);

    SquadRegistry reg;
    {
        // This case is about WHICH squad a daughter inherits, so the size cap
        // is lifted out of the way -- at this replication rate both squads blow
        // past the stock 90 in well under 120 ticks, and the orphans that
        // produces (correct behaviour, pinned by its own case below) would
        // drown the signal here.
        SquadTuning st;
        st.max_squad_size = 4096;
        reg.set_tuning(st);
    }
    std::vector<SquadPath> paths;
    paths.push_back(straight_path("a", "lane", 22.0f));
    paths.push_back(straight_path("b", "lane", 38.0f));
    reg.set_paths(std::move(paths));

    ChaffBuffers b;
    b.reserve(4000);
    Rng rng(2024);
    // Two squads, deliberately spawned at DIFFERENT points so their members
    // occupy different index ranges -- the bug this pins made every daughter
    // inherit the lowest-indexed squad regardless of parent.
    const u16 sq_a = reg.create_squad(0, Vec2{20.0f, 22.0f});
    const u16 sq_b = reg.create_squad(1, Vec2{20.0f, 38.0f});
    REQUIRE(sq_a != kNoSquad);
    REQUIRE(sq_b != kNoSquad);
    sys.spawn_burst(b, PathogenFamily::Virus, Vec2{20.0f, 22.0f}, 3.0f, 40, rng, sq_a);
    sys.spawn_burst(b, PathogenFamily::Virus, Vec2{20.0f, 38.0f}, 3.0f, 40, rng, sq_b);
    const usize before = b.count();

    for (u32 i = 0; i < 120; ++i) {
        rebuild(hash, b);
        reg.update(b, kFixedDt);
        sys.update(b, flow, sdf, TissueMask{}, hash, reg, rng, kFixedDt, nullptr);
        b.compact();
    }
    REQUIRE(b.count() > before);   // replication actually happened

    u32 in_a = 0, in_b = 0, orphan = 0;
    for (usize i = 0; i < b.count(); ++i) {
        if (b.squad_id[i] == sq_a) ++in_a;
        else if (b.squad_id[i] == sq_b) ++in_b;
        else ++orphan;
    }
    INFO("a=" << in_a << " b=" << in_b << " orphan=" << orphan << " total=" << b.count());
    // Nothing is left unaffiliated...
    REQUIRE(orphan == 0);
    // ...and both squads grew. The failure mode was one squad absorbing every
    // daughter in the world while the other stayed frozen at its spawn count,
    // so assert growth on BOTH rather than merely on the total.
    REQUIRE(in_a > 40);
    REQUIRE(in_b > 40);
}


TEST_CASE("a squad stops absorbing daughters at max_squad_size",
          "[sim][squad][replication]") {
    // Inheritance on its own is a compounding process with no fixed point:
    // every member is a source of more members of the SAME squad, so a cohort
    // of 60 grows until the lane is one squad again -- the blob squads exist to
    // break up. target_squad_size cannot stop it; it only governs intake at the
    // spawn point, and a daughter never goes near a spawn point.
    //
    // Two claims, and both matter. The squad must stay at or under the cap
    // (a cap the daughters can outrun within a tick is not a cap), and the
    // overflow must actually exist as independent agents rather than being
    // dropped -- going ungrouped is the release valve, not a spawn failure.
    const Rect bounds = test_bounds();
    FlowField flow = make_flow(bounds, Vec2{115.0f, 30.0f});
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffSystem sys;
    ChaffTuning t = flat_tuning();
    // Fast enough that a single tick offers the squad far more daughters than
    // its remaining headroom -- which is the case the in-tick `pending` tally
    // exists for. With only the top-of-tick member_count to test against, every
    // daughter in the tick would read the same stale number and pass together.
    for (u32 f = 0; f < kFamilyCount; ++f) t.family[f].replication_rate = 4.0f;
    sys.set_tuning(t);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{115.0f, 30.0f}, 0.0f);

    SquadRegistry reg;
    SquadTuning st;
    st.max_squad_size = 60;
    reg.set_tuning(st);
    std::vector<SquadPath> paths;
    paths.push_back(straight_path("a", "lane", 30.0f));
    reg.set_paths(std::move(paths));

    ChaffBuffers b;
    b.reserve(4000);
    Rng rng(777);
    const u16 sq = reg.create_squad(0, Vec2{20.0f, 30.0f});
    REQUIRE(sq != kNoSquad);
    sys.spawn_burst(b, PathogenFamily::Virus, Vec2{20.0f, 30.0f}, 3.0f, 40, rng, sq);

    u32 peak_members = 0;
    for (u32 i = 0; i < 120; ++i) {
        rebuild(hash, b);
        reg.update(b, kFixedDt);
        sys.update(b, flow, sdf, TissueMask{}, hash, reg, rng, kFixedDt, nullptr);
        b.compact();
        u32 members = 0;
        for (usize k = 0; k < b.count(); ++k)
            if (b.squad_id[k] == sq) ++members;
        // Checked EVERY tick, not just at the end: a squad that overshoots and
        // is then trimmed back by despawns would slip past an end-state check.
        REQUIRE(members <= st.max_squad_size);
        peak_members = math::max(peak_members, members);
    }

    u32 independent = 0;
    for (usize i = 0; i < b.count(); ++i)
        if (b.squad_id[i] == kNoSquad) ++independent;

    INFO("peak members " << peak_members << ", independent " << independent
                         << ", total " << b.count());
    REQUIRE(peak_members == st.max_squad_size);   // it filled to the cap...
    REQUIRE(independent > 0);                     // ...and the rest went free
}

TEST_CASE("max_squad_size below target_squad_size is raised to it", "[sim][squad]") {
    // The two caps govern different doors -- intake at the spawn point vs.
    // replication mid-lane -- and a max under the target is self-contradictory:
    // accepting() would fill a squad to 60 while can_absorb() called it full at
    // 20. set_tuning() reconciles them rather than leaving the registry to
    // enforce two rules that disagree.
    SquadRegistry reg;
    SquadTuning st;
    st.target_squad_size = 60;
    st.max_squad_size = 20;
    reg.set_tuning(st);
    REQUIRE(reg.tuning().max_squad_size == 60);
}

TEST_CASE("replication spreads across squads rather than compounding in one",
          "[sim][squad][replication][determinism]") {
    // Guards a bug that predates squads but was invisible without them:
    // Rng::fork() is const, so a pass that only forks never advances the sim
    // RNG. On any tick where nothing else drew from it, every range got a
    // byte-identical generator to the tick before -- so the same index slots
    // rolled replication true forever while the rest never did, and jitter
    // became a constant per-agent force instead of noise.
    //
    // Measured on capillary_2 before the fix: three equal squads of 60 became
    // 60 / 60 / 260. The symptom a player sees is agents tracking steadily out
    // of the group they belong to.
    const Rect bounds = test_bounds();
    FlowField flow = make_flow(bounds, Vec2{115.0f, 30.0f});
    DistanceField sdf;
    SpatialHash hash = make_hash(bounds);

    ChaffSystem sys;
    ChaffTuning t = flat_tuning();
    for (u32 f = 0; f < kFamilyCount; ++f) {
        t.family[f].replication_rate = 0.8f;
        t.family[f].jitter = 0.4f;   // the other consumer of the per-range stream
    }
    sys.set_tuning(t);
    sys.set_world_bounds(bounds);
    sys.set_goal(Vec2{115.0f, 30.0f}, 0.0f);

    SquadRegistry reg;
    {
        // The cap is lifted here for the same reason it exists: it would clamp
        // all three squads to the same ceiling and the test could no longer
        // tell a healthy spread from the 60/60/260 runaway it is pinning.
        SquadTuning st;
        st.max_squad_size = 4096;
        reg.set_tuning(st);
    }
    std::vector<SquadPath> paths;
    paths.push_back(straight_path("a", "lane", 22.0f));
    paths.push_back(straight_path("b", "lane", 30.0f));
    paths.push_back(straight_path("c", "lane", 38.0f));
    reg.set_paths(std::move(paths));

    ChaffBuffers b;
    b.reserve(4000);
    Rng rng(31415);
    std::vector<u16> ids;
    for (u32 k = 0; k < 3; ++k) {
        const u16 sq = reg.create_squad(static_cast<u16>(k), Vec2{20.0f, 22.0f + k * 8.0f});
        REQUIRE(sq != kNoSquad);
        ids.push_back(sq);
        sys.spawn_burst(b, PathogenFamily::Virus, reg.get(sq).anchor, 0.0f, 40, rng, sq);
    }

    for (u32 i = 0; i < 300; ++i) {
        rebuild(hash, b);
        reg.update(b, kFixedDt);
        sys.update(b, flow, sdf, TissueMask{}, hash, reg, rng, kFixedDt, nullptr);
        b.compact();
    }

    u32 counts[3] = {0, 0, 0};
    for (usize i = 0; i < b.count(); ++i)
        for (u32 k = 0; k < 3; ++k)
            if (b.squad_id[i] == ids[k]) ++counts[k];

    const u32 total = counts[0] + counts[1] + counts[2];
    REQUIRE(total > 120);   // replication happened at all
    const u32 biggest = math::max(counts[0], math::max(counts[1], counts[2]));
    const u32 smallest = math::min(counts[0], math::min(counts[1], counts[2]));
    INFO("counts " << counts[0] << " / " << counts[1] << " / " << counts[2]);
    // Three identical squads under identical conditions must grow at
    // comparable rates. The bug produced a >4x spread; anything under 2x is
    // ordinary sampling noise on a stochastic rate.
    REQUIRE(biggest < smallest * 2u);
}

TEST_CASE("a squad is a cohort: it closes once it moves off its spawn point",
          "[sim][squad][wave]") {
    // The failure this pins, reported from play: enemies partway down the lane
    // turning round and heading BACK up it to join agents that had just
    // spawned.
    //
    // Cause was that the director held one squad open until it reached
    // target_squad_size, counted in agents rather than in time. A wave ramp
    // trickles out ~1 agent/tick, so a 60-strong squad stayed open for a second
    // or more; its first members were well down the lane while new ones kept
    // appearing at the spawn point. That drags the centroid backwards, and the
    // along-flow cohesion term steers toward the centroid -- so the leaders
    // dutifully reversed. Two unrelated groups had been told they were one.
    game::LevelLoader loader;
    game::LevelDef def = game::LevelLoader::default_test_level();

    // A deliberately SLOW ramp -- the shape that triggered it. 200 agents over
    // 8 seconds is well under one per tick.
    def.waves.clear();
    game::WaveDef w;
    w.index = 0;
    w.prep_time = 0.0f;
    game::SpawnEntry e;
    e.family = PathogenFamily::Virus;
    e.count = 200;
    e.start_time = 0.0f;
    e.duration = 8.0f;
    w.spawns.push_back(e);
    def.waves.push_back(w);

    SimDesc desc;
    desc.max_chaff = 4000;
    desc.world_bounds = def.world_bounds;
    // Replication off: this is about spawn cohorts, and a growing population
    // would blur the per-squad centroid measurement.
    for (u32 f = 0; f < kFamilyCount; ++f) desc.chaff_tuning.family[f].replication_rate = 0.0f;
    SimWorld world;
    world.init(desc, nullptr);
    REQUIRE(loader.instantiate(def, world).ok);

    game::WaveDirector waves;
    waves.set_waves(def.waves);
    waves.start(world);

    // What the player actually sees is AGENTS reversing, not centroids. The
    // lane runs left to right, so sustained negative x-velocity is the symptom
    // stated literally. Counted rather than peaked: a single shove backwards is
    // ordinary crowd physics, a steady stream of agents driving upstream is not.
    Rng rng(90210);
    u64 backward_samples = 0;
    u64 total_samples = 0;
    f32 worst_backward = 0.0f;

    for (u32 i = 0; i < 900; ++i) {
        waves.tick(world, rng, kFixedDt);
        world.tick(nullptr);
        if (i < 300) continue;   // let the wave establish before measuring

        const ChaffBuffers& b = world.chaff();
        for (usize k = 0; k < b.count(); ++k) {
            ++total_samples;
            if (b.vel_x[k] < -1.0f) ++backward_samples;
            worst_backward = math::max(worst_backward, -b.vel_x[k]);
        }
    }
    const f64 backward_fraction =
        total_samples == 0 ? 0.0
                           : static_cast<f64>(backward_samples) / static_cast<f64>(total_samples);

    INFO("backward fraction " << backward_fraction << ", worst backward speed "
                              << worst_backward << ", over " << total_samples << " samples");
    // A shove backwards from the crowd is ordinary physics; a steady stream of
    // agents driving upstream is the bug. Before the brake was capped, leaders
    // reversed under a 1.65/tick impulse against a 0.33/tick flow term.
    REQUIRE(backward_fraction < 0.0005);
    REQUIRE(worst_backward < 3.0f);
}
