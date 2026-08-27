// Tests for game/editor/LevelValidate — the editor's validator, and the one
// --level-check runs in CI.
//
// One fixture per rule, each with a clean case and a failing case, because a
// validator that cannot be shown to FIRE is worth nothing: the failure mode of
// a validator is silence, and silence looks exactly like success.
//
// The bake-dependent rules get real geometry via bake_geometry(), which is the
// whole reason that function was split out of instantiate() -- these checks
// need a mask and a flow field, and constructing a SimWorld to get one would
// make them untestable headlessly.
#include "game/editor/LevelValidate.h"
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "sim/flowfield/FlowField.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace immune;
using namespace immune::game;

namespace {

/// A minimal but complete and playable level: one straight 12-wide lane across
/// a 64x32 world, spawn at the left end, objective at the right.
LevelDef base_level() {
    LevelDef d;
    d.schema = 1;
    d.name = "fixture";
    d.region = "test";
    d.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{64.0f, 32.0f}};
    d.cell_size = 0.5f;

    Vessel v;
    v.id = "main";
    v.lane_id = "main";
    v.points = {VesselPoint{Vec2{6.0f, 16.0f}, 12.0f}, VesselPoint{Vec2{32.0f, 16.0f}, 12.0f},
                VesselPoint{Vec2{58.0f, 16.0f}, 12.0f}};
    d.vessels.push_back(v);

    d.spawn_points.push_back(SpawnPoint{"p0", Vec2{8.0f, 16.0f}, 3.0f, ""});
    d.objectives.push_back(
        ObjectivePoint{"organ", Vec2{56.0f, 16.0f}, Vec2{4.0f, 4.0f}, 0.0f, 100.0f});

    WaveDef w;
    w.name = "w1";
    w.prep_time = 8.0f;
    w.atp_reward = 50;
    SpawnEntry e;
    e.family = PathogenFamily::Virus;
    e.count = 100;
    e.duration = 4.0f;
    w.spawns.push_back(e);
    d.waves.push_back(w);
    return d;
}

/// Owns the three buffers so a test can hand validate_level() real geometry.
struct Baked {
    sim::TissueMask mask;
    sim::DistanceField sdf;
    sim::FlowField flow;

    BakedGeometry view() { return BakedGeometry{&mask, &sdf, &flow}; }
};

Baked bake(const LevelDef& d) {
    Baked b;
    LevelLoader loader;
    REQUIRE(loader.bake_geometry(d, GeometryBakeDesc{}, b.mask, b.sdf, b.flow).ok);
    return b;
}

bool has_message(const std::vector<Issue>& issues, const std::string& needle,
                 Issue::Severity sev) {
    for (const Issue& i : issues) {
        if (i.severity == sev && i.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool has_error(const std::vector<Issue>& v, const std::string& n) {
    return has_message(v, n, Issue::Severity::Error);
}
bool has_warning(const std::vector<Issue>& v, const std::string& n) {
    return has_message(v, n, Issue::Severity::Warning);
}

} // namespace

// ---- The clean case -------------------------------------------------------

TEST_CASE("a well-formed level produces no issues", "[level][validate]") {
    const LevelDef d = base_level();
    Baked b = bake(d);
    const std::vector<Issue> issues = validate_level(d, b.view());
    for (const Issue& i : issues) INFO(i.message);
    REQUIRE(issues.empty());
    REQUIRE_FALSE(has_errors(issues));
}

TEST_CASE("every shipped level is error-free", "[level][validate][content]") {
    // The CI gate, as a unit test. A level that fails this is broken content:
    // --level-check would reject it and the editor would refuse to save it.
    const std::string dir = platform::asset_path("levels");
    const std::vector<std::string> files = platform::list_files(dir, ".json");
    REQUIRE(files.size() >= 10);

    for (const std::string& path : files) {
        INFO("level = " << path);
        LevelLoader loader;
        LevelDef d;
        REQUIRE(loader.load_file(path, d).ok);
        Baked b = bake(d);
        const std::vector<Issue> issues = validate_level(d, b.view());
        for (const Issue& i : issues) {
            if (i.severity == Issue::Severity::Error) FAIL("error: " << i.message);
        }
    }
}

// ---- Structural rules -----------------------------------------------------

TEST_CASE("structural gaps are errors", "[level][validate]") {
    SECTION("no vessels") {
        LevelDef d = base_level();
        d.vessels.clear();
        REQUIRE(has_error(validate_level(d), "no vessels"));
    }
    SECTION("no spawn points") {
        LevelDef d = base_level();
        d.spawn_points.clear();
        REQUIRE(has_error(validate_level(d), "no spawn points"));
    }
    SECTION("no objectives") {
        LevelDef d = base_level();
        d.objectives.clear();
        REQUIRE(has_error(validate_level(d), "no objectives"));
    }
    SECTION("no waves") {
        LevelDef d = base_level();
        d.waves.clear();
        REQUIRE(has_error(validate_level(d), "no waves"));
    }
    SECTION("bad schema") {
        LevelDef d = base_level();
        d.schema = 7;
        REQUIRE(has_error(validate_level(d), "schema"));
    }
    SECTION("inverted world bounds") {
        LevelDef d = base_level();
        d.world_bounds = Rect{Vec2{64.0f, 32.0f}, Vec2{0.0f, 0.0f}};
        REQUIRE(has_error(validate_level(d), "world bounds"));
    }
}

TEST_CASE("duplicate ids are errors", "[level][validate]") {
    // Not caught anywhere today. Two spawn points sharing an id makes
    // WaveDirector bind waves to whichever one it finds first, so the horde
    // comes out of a lane the author did not pick and nothing says so.
    SECTION("spawn points") {
        LevelDef d = base_level();
        d.spawn_points.push_back(SpawnPoint{"p0", Vec2{10.0f, 16.0f}, 3.0f, ""});
        REQUIRE(has_error(validate_level(d), "spawn point id 'p0' is used twice"));
    }
    SECTION("vessels") {
        LevelDef d = base_level();
        d.vessels.push_back(d.vessels[0]);
        REQUIRE(has_error(validate_level(d), "vessel id 'main' is used twice"));
    }
    SECTION("objectives") {
        LevelDef d = base_level();
        d.objectives.push_back(d.objectives[0]);
        REQUIRE(has_error(validate_level(d), "objective id 'organ' is used twice"));
    }
}

TEST_CASE("dangling references are errors", "[level][validate]") {
    SECTION("wave names an unknown spawn point") {
        LevelDef d = base_level();
        d.waves[0].spawns[0].spawn_point_id = "nope";
        REQUIRE(has_error(validate_level(d), "unknown spawn point 'nope'"));
    }
    SECTION("squad path names an unknown lane") {
        LevelDef d = base_level();
        SquadPathDef sp;
        sp.id = "sp";
        sp.lane_id = "ghost";
        sp.points = {Vec2{8.0f, 16.0f}, Vec2{56.0f, 16.0f}};
        d.squad_paths.push_back(sp);
        REQUIRE(has_error(validate_level(d), "unknown lane 'ghost'"));
    }
    SECTION("vessel names an unknown child") {
        // `children` drives nothing today, but a dangling id is still a mistake
        // and the editor's branch-weld drag is about to give the field a job.
        LevelDef d = base_level();
        d.vessels[0].children.push_back("ghost");
        REQUIRE(has_error(validate_level(d), "unknown child vessel 'ghost'"));
    }
    SECTION("spawn point names an unknown lane") {
        LevelDef d = base_level();
        d.spawn_points[0].lane_id = "ghost";
        REQUIRE(has_error(validate_level(d), "unknown lane 'ghost'"));
    }
}

TEST_CASE("geometry outside the world is an error", "[level][validate]") {
    SECTION("vessel point") {
        LevelDef d = base_level();
        d.vessels[0].points[1].position = Vec2{999.0f, 16.0f};
        REQUIRE(has_error(validate_level(d), "outside the world bounds"));
    }
    SECTION("objective") {
        LevelDef d = base_level();
        d.objectives[0].position = Vec2{-5.0f, 16.0f};
        REQUIRE(has_error(validate_level(d), "outside the world bounds"));
    }
}

TEST_CASE("non-positive sizes are errors", "[level][validate]") {
    SECTION("vessel width") {
        LevelDef d = base_level();
        d.vessels[0].points[1].width = 0.0f;
        REQUIRE(has_error(validate_level(d), "non-positive width"));
    }
    SECTION("spawn radius") {
        LevelDef d = base_level();
        d.spawn_points[0].radius = 0.0f;
        REQUIRE(has_error(validate_level(d), "non-positive radius"));
    }
    SECTION("objective integrity") {
        LevelDef d = base_level();
        d.objectives[0].integrity = 0.0f;
        REQUIRE(has_error(validate_level(d), "non-positive integrity"));
    }
}

// ---- The rules that need the bake ----------------------------------------

TEST_CASE("a spawn point off the lumen is caught", "[level][validate][bake]") {
    // THE most common authoring error, and completely invisible in the text:
    // the number looks plausible and the point is inside the vessel wall.
    // Level.h has promised this check since the schema was written and
    // LevelLoader::validate() has never had it.
    LevelDef d = base_level();
    d.spawn_points[0].position = Vec2{8.0f, 2.0f};   // well below the 12-wide lane

    // Without geometry the rule cannot fire...
    REQUIRE_FALSE(has_error(validate_level(d), "not on tissue"));

    // ...with it, it does.
    Baked b = bake(d);
    REQUIRE(has_error(validate_level(d, b.view()), "spawn point 'p0' is not on tissue"));
}

TEST_CASE("an objective off the lumen is caught", "[level][validate][bake]") {
    LevelDef d = base_level();
    d.objectives[0].position = Vec2{56.0f, 30.0f};
    Baked b = bake(d);
    REQUIRE(has_error(validate_level(d, b.view()), "objective 'organ' is not on tissue"));
}

TEST_CASE("an obstacle that seals a lane is caught", "[level][validate][bake]") {
    // The pinched-switchback catcher. Connectivity is a property of the whole
    // grid, so the baked flow field is the only place this question can be
    // answered -- which is exactly why the editor bakes for real.
    LevelDef d = base_level();
    ObstacleDef bar;
    bar.id = "wall";
    bar.shape = ObstacleShape::Box;
    bar.position = Vec2{32.0f, 16.0f};
    bar.half_extents = Vec2{2.0f, 12.0f};   // taller than the 12-wide lumen
    d.obstacles.push_back(bar);

    Baked b = bake(d);
    const std::vector<Issue> issues = validate_level(d, b.view());
    REQUIRE(has_error(issues, "cannot reach any objective"));

    SECTION("and a bar that does NOT seal it is fine") {
        LevelDef open = base_level();
        ObstacleDef small = bar;
        small.half_extents = Vec2{2.0f, 3.0f};   // leaves room either side
        open.obstacles.push_back(small);
        Baked ob = bake(open);
        const std::vector<Issue> clean = validate_level(open, ob.view());
        for (const Issue& i : clean) INFO(i.message);
        REQUIRE_FALSE(has_errors(clean));
    }
}

TEST_CASE("reachability is checked even with no obstacles", "[level][validate][bake]") {
    // instantiate() gates this on `!def.obstacles.empty()`, deliberately, so it
    // cannot start rejecting pre-existing content. The editor has no such
    // constraint: a lane you have just dragged apart is worth reporting.
    LevelDef d = base_level();
    // Split the lane in two with a gap in the middle -- no obstacle involved.
    d.vessels[0].points = {VesselPoint{Vec2{6.0f, 16.0f}, 12.0f},
                           VesselPoint{Vec2{20.0f, 16.0f}, 12.0f}};
    Vessel far;
    far.id = "far";
    far.lane_id = "far";
    far.points = {VesselPoint{Vec2{46.0f, 16.0f}, 12.0f}, VesselPoint{Vec2{58.0f, 16.0f}, 12.0f}};
    d.vessels.push_back(far);

    REQUIRE(d.obstacles.empty());
    Baked b = bake(d);
    REQUIRE(has_error(validate_level(d, b.view()), "cannot reach any objective"));
}

TEST_CASE("a placement zone over solid ground warns", "[level][validate][bake]") {
    LevelDef d = base_level();
    d.placement_zones.push_back(Rect{Vec2{2.0f, 1.0f}, Vec2{10.0f, 4.0f}});   // below the lane
    d.placement_zone_tags.push_back(PlacementZoneTag{});
    Baked b = bake(d);
    const std::vector<Issue> issues = validate_level(d, b.view());
    REQUIRE(has_warning(issues, "covers no tissue"));
    REQUIRE_FALSE(has_errors(issues));   // advisory only
}

// ---- Warnings never block -------------------------------------------------

TEST_CASE("warnings are reported but do not block", "[level][validate]") {
    SECTION("a lane with no spawn point feeding it") {
        LevelDef d = base_level();
        Vessel side;
        side.id = "side";
        side.lane_id = "side";
        side.points = {VesselPoint{Vec2{20.0f, 16.0f}, 10.0f}, VesselPoint{Vec2{40.0f, 26.0f}, 10.0f}};
        d.vessels.push_back(side);
        const std::vector<Issue> issues = validate_level(d);
        REQUIRE(has_warning(issues, "no spawn point feeding it"));
        REQUIRE_FALSE(has_errors(issues));
    }
    SECTION("a vessel narrower than two cells") {
        LevelDef d = base_level();
        d.vessels[0].points[1].width = 0.5f;   // cell_size is 0.5
        const std::vector<Issue> issues = validate_level(d);
        REQUIRE(has_warning(issues, "rasterize broken"));
    }
    SECTION("a backwards ramp") {
        LevelDef d = base_level();
        WaveDef w2 = d.waves[0];
        w2.name = "w2";
        w2.prep_time = 20.0f;   // more prep than wave 1
        w2.atp_reward = 10;     // less ATP than wave 1
        d.waves.push_back(w2);
        const std::vector<Issue> issues = validate_level(d);
        REQUIRE(has_warning(issues, "more prep time"));
        REQUIRE(has_warning(issues, "less ATP"));
        REQUIRE_FALSE(has_errors(issues));
    }
    SECTION("a lane that authors exactly one squad path") {
        // Authoring one path disables the derived spread for the WHOLE lane,
        // silently dropping a 3-path lane to 1 and changing how every wave on
        // it reads.
        LevelDef d = base_level();
        SquadPathDef sp;
        sp.id = "sp";
        sp.lane_id = "main";
        sp.points = {Vec2{8.0f, 16.0f}, Vec2{56.0f, 16.0f}};
        d.squad_paths.push_back(sp);
        const std::vector<Issue> issues = validate_level(d);
        REQUIRE(has_warning(issues, "disables the derived spread"));
        REQUIRE_FALSE(has_errors(issues));
    }
}

// ---- Reporting shape ------------------------------------------------------

TEST_CASE("issues carry a selectable ref and a camera anchor", "[level][validate]") {
    // The editor's validation panel is only useful if clicking a row can select
    // the offending element and fly to it. That means every positional issue
    // has to carry both.
    LevelDef d = base_level();
    d.spawn_points[0].position = Vec2{8.0f, 2.0f};
    Baked b = bake(d);
    const std::vector<Issue> issues = validate_level(d, b.view());

    bool found = false;
    for (const Issue& i : issues) {
        if (i.message.find("not on tissue") == std::string::npos) continue;
        found = true;
        REQUIRE(i.ref.kind == ElementKind::SpawnPoint);
        REQUIRE(i.ref.index == 0);
        REQUIRE(i.has_anchor);
        REQUIRE(i.anchor.x == d.spawn_points[0].position.x);
    }
    REQUIRE(found);
}

TEST_CASE("errors sort before warnings", "[level][validate]") {
    // The panel's top row should always be the thing most worth fixing.
    LevelDef d = base_level();
    d.vessels[0].points[1].width = 0.5f;         // warning
    d.waves[0].spawns[0].spawn_point_id = "no";  // error
    const std::vector<Issue> issues = validate_level(d);
    REQUIRE(issues.size() >= 2);
    REQUIRE(issues.front().severity == Issue::Severity::Error);
    REQUIRE(issues.back().severity == Issue::Severity::Warning);
}
