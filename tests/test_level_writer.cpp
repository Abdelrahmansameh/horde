// Tests for game/level/LevelWriter — the serializer half of the level format.
//
// The load-bearing property is ROUND-TRIP: parse -> write -> parse must
// reproduce the LevelDef. Without it, an editor's Save silently corrupts
// content, and no amount of UI polish matters. The sweep over assets/levels is
// what makes that a build failure rather than something an author finds after
// overwriting their work.
//
// The second property is IDEMPOTENCE: writing an already-canonical level gives
// byte-identical text. That is what lets `--level-fmt --check` be a CI gate,
// and what stops a no-op save producing a diff.
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "platform/FileIO.h"

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace immune;
using namespace immune::game;

namespace {

std::vector<std::string> shipped_levels() {
    std::vector<std::string> out;
    const std::string dir = platform::asset_path("levels");
    REQUIRE(std::filesystem::is_directory(dir));
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".json") out.push_back(e.path().string());
    }
    return out;
}

/// parse(text) or a hard test failure with the loader's message.
LevelDef parse(const std::string& text) {
    LevelLoader loader;
    LevelDef def;
    const LevelLoadResult r = loader.load_string(text, def);
    INFO("loader error: " << r.error);
    REQUIRE(r.ok);
    return def;
}

} // namespace

// ---- The sweep ------------------------------------------------------------

TEST_CASE("every shipped level round-trips through the writer", "[level][writer]") {
    const std::vector<std::string> paths = shipped_levels();
    REQUIRE(paths.size() >= 10);

    for (const std::string& path : paths) {
        INFO("level = " << path);
        LevelLoader loader;
        LevelDef original;
        REQUIRE(loader.load_file(path, original).ok);

        const std::string text = level_to_json(original);
        const LevelDef reparsed = parse(text);
        REQUIRE(level_equal(original, reparsed));

        // Idempotence: the canonical form is a fixed point.
        REQUIRE(level_to_json(reparsed) == text);
    }
}

TEST_CASE("the writer's layout is much more compact than a plain dump",
          "[level][writer]") {
    // Not a style preference: omit-defaults plus inline vectors is what keeps a
    // save from turning a readable file into a wall of "elite_id": 0 and
    // six-line coordinate pairs. Measured against nlohmann's own pretty-print
    // of the SAME document, which is what the writer would produce without any
    // of its layout rules -- a stable comparison that does not depend on how
    // the shipped files happen to be formatted today.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_file(platform::asset_path("levels/elbow_turn.json"), def).ok);

    const std::string canonical = level_to_json(def);
    const std::string plain = nlohmann::json::parse(canonical).dump(2);

    const auto lines = [](const std::string& t) {
        return static_cast<usize>(std::count(t.begin(), t.end(), '\n'));
    };
    INFO("canonical " << lines(canonical) << " lines, plain dump " << lines(plain));
    REQUIRE(lines(canonical) * 2 < lines(plain));
}

// ---- The four traps the header calls out ----------------------------------

TEST_CASE("box rotation round-trips through degrees, not radians", "[level][writer]") {
    // ObstacleDef::rotation is RADIANS in the struct and DEGREES in the file.
    // Writing the raw field would turn a 30-degree box into a 30-radian one on
    // the next load, which is a 1719-degree rotation and looks like a bug in
    // the rasterizer rather than in the writer.
    const char* kJson = R"JSON({
  "schema": 1, "name": "rot", "region": "test",
  "world": { "min": [0,0], "max": [64,64], "cell_size": 1.0 },
  "vessels": [ { "id": "main", "points": [ {"p":[4,32],"w":10}, {"p":[60,32],"w":10} ] } ],
  "obstacles": [
    { "id": "b", "shape": "box", "pos": [32,32], "half_extents": [6,2], "rotation": 30 }
  ],
  "spawn_points": [ { "id": "p0", "pos": [4,32], "radius": 3 } ],
  "objectives": [ { "id": "o", "pos": [60,32], "radius": 4 } ],
  "waves": [ { "name": "w", "spawns": [ { "family": "virus", "count": 10 } ] } ]
})JSON";

    const LevelDef def = parse(kJson);
    REQUIRE(def.obstacles.size() == 1);
    REQUIRE(def.obstacles[0].rotation == Catch::Approx(30.0f * 3.14159265f / 180.0f).epsilon(1e-4));

    const std::string text = level_to_json(def);
    INFO(text);
    REQUIRE(text.find("\"rotation\": 30") != std::string::npos);
    REQUIRE(level_equal(def, parse(text)));
}

TEST_CASE("placement zone tags merge back into their zone object", "[level][writer]") {
    // placement_zones and placement_zone_tags are two index-aligned vectors in
    // the struct and ONE object in the file. A writer that emits them as two
    // arrays produces a file the parser silently reads as untagged zones.
    const char* kJson = R"JSON({
  "schema": 1, "name": "zones", "region": "test",
  "world": { "min": [0,0], "max": [64,64], "cell_size": 1.0 },
  "vessels": [ { "id": "main", "points": [ {"p":[4,32],"w":10}, {"p":[60,32],"w":10} ] } ],
  "spawn_points": [ { "id": "p0", "pos": [4,32], "radius": 3 } ],
  "objectives": [ { "id": "o", "pos": [60,32], "radius": 4 } ],
  "placement_zones": [
    { "min": [8,8], "max": [30,50], "concentrated": true, "priority": 2.5 },
    { "min": [32,8], "max": [56,50] }
  ],
  "waves": [ { "name": "w", "spawns": [ { "family": "virus", "count": 10 } ] } ]
})JSON";

    const LevelDef def = parse(kJson);
    REQUIRE(def.placement_zone_tags.size() == 2);
    REQUIRE(def.placement_zone_tags[0].concentrated);
    REQUIRE(def.placement_zone_tags[0].priority == Catch::Approx(2.5f));

    const std::string text = level_to_json(def);
    INFO(text);
    // The tagged zone carries both fields; the plain one carries neither.
    REQUIRE(text.find("\"concentrated\": true") != std::string::npos);
    REQUIRE(text.find("\"priority\": 2.5") != std::string::npos);
    REQUIRE(text.find("\"concentrated\": false") == std::string::npos);

    const LevelDef back = parse(text);
    REQUIRE(level_equal(def, back));
    REQUIRE_FALSE(back.placement_zone_tags[1].concentrated);
    REQUIRE(back.placement_zone_tags[1].priority == Catch::Approx(1.0f));
}

TEST_CASE("wave index is never written", "[level][writer]") {
    // Array position IS the index the director walks (parse_wave). Writing it
    // creates a second source of truth that a reorder can put out of step.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_file(platform::asset_path("levels/plaque_field.json"), def).ok);
    REQUIRE(def.waves.size() > 1);
    REQUIRE(def.waves[1].index == 1);

    const std::string text = level_to_json(def);
    REQUIRE(text.find("\"index\"") == std::string::npos);
    // ...and it comes back correct anyway, from position alone.
    REQUIRE(parse(text).waves[1].index == 1);
}

TEST_CASE("obstacle points keep their two spellings", "[level][writer]") {
    // A polygon vertex is a bare [x,y]; a ridge point is {"p":[x,y],"w":w}.
    // Writing a ridge as bare pairs loses every width, and parse_obstacle_point
    // gives bare pairs width 0 -- so the level then fails to load with "every
    // ridge point needs a positive 'w'". That failure is at least loud; the
    // reverse (widths invented for a polygon) would not be.
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_file(platform::asset_path("levels/plaque_field.json"), def).ok);

    const std::string text = level_to_json(def);
    INFO(text);
    REQUIRE(text.find("\"shape\": \"polygon\"") != std::string::npos);
    REQUIRE(text.find("\"shape\": \"ridge\"") != std::string::npos);

    const LevelDef back = parse(text);
    REQUIRE(level_equal(def, back));

    // Specifically: the ridge kept its per-point widths.
    for (const ObstacleDef& o : back.obstacles) {
        if (o.shape != ObstacleShape::Ridge) continue;
        REQUIRE(o.points.size() >= 2);
        for (const VesselPoint& p : o.points) REQUIRE(p.width > 0.0f);
    }
}

// ---- Omit-defaults, and its identity exception ----------------------------

TEST_CASE("defaults are omitted but identity fields are not", "[level][writer]") {
    const char* kJson = R"JSON({
  "schema": 1, "name": "defaults", "region": "test",
  "world": { "min": [0,0], "max": [64,64], "cell_size": 0.5 },
  "vessels": [ { "id": "main", "lane_id": "main", "vessel_type": "artery",
                 "points": [ {"p":[4,32],"w":10}, {"p":[60,32],"w":10} ], "children": [] } ],
  "spawn_points": [ { "id": "p0", "pos": [4,32], "radius": 3.0, "lane_id": "" } ],
  "objectives": [ { "id": "o", "pos": [60,32], "radius": 5.0, "integrity": 100.0 } ],
  "ambient_drift": [0.0, 0.0],
  "waves": [ { "name": "w", "prep_time": 20.0, "atp_reward": 0, "modifier": "none",
               "spawns": [ { "family": "virus", "count": 0, "elite_id": 0,
                             "start_time": 0.0, "duration": 1.0 } ] } ]
})JSON";

    const std::string text = level_to_json(parse(kJson));
    INFO(text);

    // Every field above equals its default, so none of them survives...
    REQUIRE(text.find("\"lane_id\"") == std::string::npos);
    REQUIRE(text.find("\"vessel_type\"") == std::string::npos);
    REQUIRE(text.find("\"children\"") == std::string::npos);
    REQUIRE(text.find("\"integrity\"") == std::string::npos);
    REQUIRE(text.find("\"elite_id\"") == std::string::npos);
    REQUIRE(text.find("\"start_time\"") == std::string::npos);
    REQUIRE(text.find("\"duration\"") == std::string::npos);
    REQUIRE(text.find("\"prep_time\"") == std::string::npos);
    REQUIRE(text.find("\"atp_reward\"") == std::string::npos);
    REQUIRE(text.find("\"modifier\"") == std::string::npos);
    REQUIRE(text.find("\"ambient_drift\"") == std::string::npos);
    REQUIRE(text.find("\"cell_size\"") == std::string::npos);   // 0.5 is the default

    // ...except the ones that say what the record IS. `{"duration": 4}` is a
    // legal spawn entry meaning zero viruses, and no author should have to know
    // the defaults to read it.
    REQUIRE(text.find("\"schema\": 1") != std::string::npos);
    REQUIRE(text.find("\"family\": \"virus\"") != std::string::npos);
    REQUIRE(text.find("\"count\": 0") != std::string::npos);
    REQUIRE(text.find("\"waves\"") != std::string::npos);
    REQUIRE(text.find("\"w\": 10") != std::string::npos);       // vessel point width
}

TEST_CASE("non-default fields survive", "[level][writer]") {
    // The mirror of the test above: omit-defaults must not become omit-always.
    const char* kJson = R"JSON({
  "schema": 1, "name": "kept", "region": "organ_chamber",
  "world": { "min": [0,0], "max": [64,64], "cell_size": 1.0 },
  "vessels": [ { "id": "v0", "lane_id": "lymph", "vessel_type": "lymphatic",
                 "points": [ {"p":[4,32],"w":10}, {"p":[60,32],"w":12} ],
                 "children": ["v1"] },
               { "id": "v1", "lane_id": "lymph", "vessel_type": "lymphatic",
                 "points": [ {"p":[60,32],"w":12}, {"p":[60,60],"w":8} ] } ],
  "spawn_points": [ { "id": "p0", "pos": [4,32], "radius": 6.5, "lane_id": "lymph" } ],
  "objectives": [ { "id": "o", "pos": [60,60], "radius": 7.5, "integrity": 250.0 } ],
  "squad_paths": [ { "id": "sp", "lane_id": "lymph",
                     "points": [[4,32],[60,32],[60,60]], "half_width": 9.0 } ],
  "ambient_drift": [0.25, -0.5],
  "waves": [ { "name": "one", "prep_time": 8.0, "atp_reward": 50, "modifier": "fever",
               "spawns": [ { "family": "bacteria", "count": 120, "elite_id": 3,
                             "start_time": 1.5, "duration": 4.0,
                             "spawn_point_id": "p0" } ] } ]
})JSON";

    const LevelDef def = parse(kJson);
    const std::string text = level_to_json(def);
    INFO(text);

    REQUIRE(text.find("\"lane_id\": \"lymph\"") != std::string::npos);
    REQUIRE(text.find("\"vessel_type\": \"lymphatic\"") != std::string::npos);
    REQUIRE(text.find("\"children\": [\"v1\"]") != std::string::npos);
    REQUIRE(text.find("\"integrity\": 250") != std::string::npos);
    REQUIRE(text.find("\"elite_id\": 3") != std::string::npos);
    REQUIRE(text.find("\"modifier\": \"fever\"") != std::string::npos);
    REQUIRE(text.find("\"half_width\": 9") != std::string::npos);
    REQUIRE(text.find("\"spawn_point_id\": \"p0\"") != std::string::npos);
    REQUIRE(text.find("\"ambient_drift\"") != std::string::npos);
    REQUIRE(text.find("\"cell_size\": 1") != std::string::npos);

    REQUIRE(level_equal(def, parse(text)));
}

// ---- level_equal itself ---------------------------------------------------

TEST_CASE("level_equal notices each kind of difference", "[level][writer]") {
    LevelLoader loader;
    LevelDef a;
    REQUIRE(loader.load_file(platform::asset_path("levels/plaque_field.json"), a).ok);
    REQUIRE(level_equal(a, a));

    SECTION("a moved control point") {
        LevelDef b = a;
        b.vessels[0].points[1].position.x += 1.0f;
        REQUIRE_FALSE(level_equal(a, b));
    }
    SECTION("a renamed lane") {
        LevelDef b = a;
        b.vessels[0].lane_id = "other";
        REQUIRE_FALSE(level_equal(a, b));
    }
    SECTION("a resized obstacle") {
        LevelDef b = a;
        REQUIRE_FALSE(b.obstacles.empty());
        b.obstacles[0].radius += 0.5f;
        REQUIRE_FALSE(level_equal(a, b));
    }
    SECTION("a rotated box") {
        LevelDef b = a;
        for (ObstacleDef& o : b.obstacles) {
            if (o.shape != ObstacleShape::Box) continue;
            o.rotation += 0.1f;   // ~5.7 degrees, far above the tolerance
        }
        REQUIRE_FALSE(level_equal(a, b));
    }
    SECTION("a retagged placement zone") {
        LevelDef b = a;
        REQUIRE_FALSE(b.placement_zone_tags.empty());
        b.placement_zone_tags[0].concentrated = !b.placement_zone_tags[0].concentrated;
        REQUIRE_FALSE(level_equal(a, b));
    }
    SECTION("an edited wave") {
        LevelDef b = a;
        b.waves[0].spawns[0].count += 1;
        REQUIRE_FALSE(level_equal(a, b));
    }
    SECTION("a dropped wave") {
        LevelDef b = a;
        b.waves.pop_back();
        REQUIRE_FALSE(level_equal(a, b));
    }
    SECTION("sub-quantum float noise is NOT a difference") {
        // The writer carries three decimals, so nothing finer is observable in
        // a file -- and the editor's dirty flag must not light up for it.
        LevelDef b = a;
        b.vessels[0].points[0].position.x += 1e-5f;
        REQUIRE(level_equal(a, b));
    }
}
