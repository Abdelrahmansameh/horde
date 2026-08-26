// Tests for game/editor/LevelDoc — the level editor's document model.
//
// The three properties that make an editor trustworthy, and all three are
// invisible in the UI until they are already broken:
//   1. UNDO RESTORES. undo(op(x)) must equal x, for every op.
//   2. A DRAG IS ONE UNDO ENTRY. Otherwise Ctrl+Z walks back one mouse-move at
//      a time and the feature is useless.
//   3. REFERENCES SURVIVE EDITS. Renaming a spawn point rewrites the waves that
//      named it; deleting one clears them. A document that can break its own
//      references is a document the loader will reject.
//
// All headless: no window, no GL, no ImGui.
#include "game/editor/LevelDoc.h"
#include "game/editor/LevelValidate.h"
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "platform/FileIO.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace immune;
using namespace immune::game;

namespace {

LevelDef fixture() {
    LevelDef d;
    d.schema = 1;
    d.name = "fixture";
    d.region = "test";
    d.world_bounds = Rect{Vec2{0.0f, 0.0f}, Vec2{200.0f, 100.0f}};
    d.cell_size = 0.5f;

    Vessel v;
    v.id = "main";
    v.lane_id = "main";
    v.points = {VesselPoint{Vec2{10.0f, 50.0f}, 20.0f}, VesselPoint{Vec2{100.0f, 50.0f}, 20.0f},
                VesselPoint{Vec2{190.0f, 50.0f}, 20.0f}};
    d.vessels.push_back(v);

    d.spawn_points.push_back(SpawnPoint{"p0", Vec2{12.0f, 50.0f}, 5.0f, ""});
    d.objectives.push_back(ObjectivePoint{"organ", Vec2{188.0f, 50.0f}, 5.0f, 100.0f});

    WaveDef w;
    w.name = "w1";
    w.prep_time = 8.0f;
    w.atp_reward = 50;
    SpawnEntry e;
    e.family = PathogenFamily::Virus;
    e.count = 100;
    e.duration = 4.0f;
    e.spawn_point_id = "p0";
    w.spawns.push_back(e);
    d.waves.push_back(w);
    return d;
}

LevelDoc make_doc() {
    LevelDoc doc;
    doc.set_document(fixture());
    return doc;
}

} // namespace

// ---- Undo restores --------------------------------------------------------

TEST_CASE("every operation is undoable and restores the document", "[level][edit]") {
    LevelDoc doc = make_doc();
    const LevelDef before = doc.def();

    const auto check_round_trip = [&](const char* what) {
        INFO("op = " << what);
        REQUIRE(doc.can_undo());
        REQUIRE_FALSE(level_equal(before, doc.def()));   // the op did something
        REQUIRE(doc.undo());
        REQUIRE(level_equal(before, doc.def()));
        REQUIRE(doc.redo());
        REQUIRE_FALSE(level_equal(before, doc.def()));
        REQUIRE(doc.undo());
        REQUIRE(level_equal(before, doc.def()));
    };

    SECTION("add vessel") {
        doc.add_vessel(Vec2{20.0f, 20.0f}, Vec2{80.0f, 20.0f});
        check_round_trip("add_vessel");
    }
    SECTION("insert vessel point") {
        doc.insert_vessel_point(0, 0, Vec2{55.0f, 60.0f});
        check_round_trip("insert_vessel_point");
    }
    SECTION("move a control point") {
        doc.move_element(ElementRef{ElementKind::Vessel, 0, 1}, Vec2{100.0f, 70.0f});
        check_round_trip("move_element");
    }
    SECTION("set width") {
        doc.set_vessel_point_width(0, 1, 40.0f);
        check_round_trip("set_vessel_point_width");
    }
    SECTION("set vessel type") {
        doc.set_vessel_type(0, VesselType::Lymphatic);
        check_round_trip("set_vessel_type");
    }
    SECTION("add each obstacle shape") {
        doc.add_obstacle(ObstacleShape::Disc, Vec2{60.0f, 50.0f});
        check_round_trip("add_obstacle disc");
    }
    SECTION("add spawn point") {
        doc.add_spawn_point(Vec2{30.0f, 50.0f});
        check_round_trip("add_spawn_point");
    }
    SECTION("add objective") {
        doc.add_objective(Vec2{160.0f, 50.0f});
        check_round_trip("add_objective");
    }
    SECTION("add zone") {
        doc.add_zone(Rect{Vec2{20.0f, 20.0f}, Vec2{80.0f, 80.0f}});
        check_round_trip("add_zone");
    }
    SECTION("add squad path") {
        doc.add_squad_path({Vec2{12.0f, 50.0f}, Vec2{188.0f, 50.0f}}, "main");
        check_round_trip("add_squad_path");
    }
    SECTION("add wave") {
        doc.add_wave();
        check_round_trip("add_wave");
    }
    SECTION("duplicate wave") {
        doc.duplicate_wave(0);
        check_round_trip("duplicate_wave");
    }
    SECTION("add spawn entry") {
        doc.add_spawn_entry(0);
        check_round_trip("add_spawn_entry");
    }
    SECTION("rename") {
        doc.rename_element(ElementRef{ElementKind::SpawnPoint, 0, -1}, "start");
        check_round_trip("rename_element");
    }
    SECTION("erase") {
        doc.add_spawn_point(Vec2{30.0f, 50.0f});
        const LevelDef two = doc.def();
        doc.erase(ElementRef{ElementKind::SpawnPoint, 1, -1});
        REQUIRE(doc.undo());
        REQUIRE(level_equal(two, doc.def()));
    }
    SECTION("duplicate") {
        doc.duplicate(ElementRef{ElementKind::Vessel, 0, -1});
        check_round_trip("duplicate");
    }
}

// ---- A drag is one undo entry --------------------------------------------

TEST_CASE("a gesture coalesces into exactly one undo entry", "[level][edit]") {
    LevelDoc doc = make_doc();
    const LevelDef before = doc.def();

    // Simulate a drag: sixty mouse-moves at 60fps for one second.
    doc.begin_gesture("move point");
    for (i32 i = 0; i < 60; ++i) {
        doc.move_element(ElementRef{ElementKind::Vessel, 0, 1},
                         Vec2{100.0f, 50.0f + static_cast<f32>(i)});
    }
    doc.end_gesture();

    REQUIRE(doc.can_undo());
    REQUIRE(doc.undo_label() == "move point");
    REQUIRE(doc.undo());
    REQUIRE(level_equal(before, doc.def()));
    // ...and there is nothing left behind it.
    REQUIRE_FALSE(doc.can_undo());
}

TEST_CASE("a gesture that changes nothing leaves no undo entry", "[level][edit]") {
    // A click that selects but does not drag still brackets a gesture. If that
    // pushed an entry, the undo stack would fill with no-ops the user has to
    // press Ctrl+Z through before anything happens.
    LevelDoc doc = make_doc();
    doc.begin_gesture("move point");
    doc.move_element(ElementRef{ElementKind::Vessel, 0, 1}, Vec2{100.0f, 50.0f});   // same place
    doc.end_gesture();
    REQUIRE_FALSE(doc.can_undo());
}

TEST_CASE("nested gestures commit once, at the outermost end", "[level][edit]") {
    LevelDoc doc = make_doc();
    doc.begin_gesture("outer");
    doc.begin_gesture("inner");
    doc.move_element(ElementRef{ElementKind::Vessel, 0, 0}, Vec2{20.0f, 60.0f});
    doc.end_gesture();
    REQUIRE_FALSE(doc.can_undo());   // inner end must not commit
    doc.end_gesture();
    REQUIRE(doc.can_undo());
    REQUIRE(doc.undo_label() == "outer");
}

TEST_CASE("a new edit clears the redo stack", "[level][edit]") {
    LevelDoc doc = make_doc();
    doc.add_spawn_point(Vec2{30.0f, 50.0f});
    REQUIRE(doc.undo());
    REQUIRE(doc.can_redo());
    doc.add_objective(Vec2{150.0f, 50.0f});
    REQUIRE_FALSE(doc.can_redo());
}

// ---- References survive edits --------------------------------------------

TEST_CASE("renaming a spawn point rewrites the waves that named it", "[level][edit]") {
    // The reason id hygiene lives in the document and not the UI: a wave entry
    // left naming the old id is a dangling reference the loader rejects, and
    // WaveDirector would silently fall back to spawn_points[0] at runtime.
    LevelDoc doc = make_doc();
    REQUIRE(doc.def().waves[0].spawns[0].spawn_point_id == "p0");

    doc.rename_element(ElementRef{ElementKind::SpawnPoint, 0, -1}, "start");
    REQUIRE(doc.def().spawn_points[0].id == "start");
    REQUIRE(doc.def().waves[0].spawns[0].spawn_point_id == "start");
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

TEST_CASE("renaming a vessel carries its lane and its children", "[level][edit]") {
    LevelDoc doc = make_doc();
    doc.add_vessel(Vec2{100.0f, 50.0f}, Vec2{140.0f, 90.0f});
    doc.mutable_def().vessels[0].children.push_back(doc.def().vessels[1].id);
    doc.mutable_def().spawn_points[0].lane_id = "main";
    doc.add_squad_path({Vec2{12.0f, 50.0f}, Vec2{188.0f, 50.0f}}, "main");

    doc.rename_element(ElementRef{ElementKind::Vessel, 0, -1}, "trunk");
    REQUIRE(doc.def().vessels[0].id == "trunk");
    REQUIRE(doc.def().vessels[0].lane_id == "trunk");
    REQUIRE(doc.def().spawn_points[0].lane_id == "trunk");
    REQUIRE(doc.def().squad_paths[0].lane_id == "trunk");
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

TEST_CASE("renaming a lane rewrites every reference", "[level][edit]") {
    LevelDoc doc = make_doc();
    doc.mutable_def().spawn_points[0].lane_id = "main";
    doc.add_squad_path({Vec2{12.0f, 50.0f}, Vec2{188.0f, 50.0f}}, "main");

    doc.rename_lane("main", "artery");
    REQUIRE(doc.def().vessels[0].lane_id == "artery");
    REQUIRE(doc.def().spawn_points[0].lane_id == "artery");
    REQUIRE(doc.def().squad_paths[0].lane_id == "artery");
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

TEST_CASE("deleting a spawn point clears the waves that named it", "[level][edit]") {
    // Empty spawn_point_id is the documented "any spawn point", so clearing is
    // the one rewrite that keeps the level loadable.
    LevelDoc doc = make_doc();
    doc.add_spawn_point(Vec2{30.0f, 50.0f});
    REQUIRE(doc.erase(ElementRef{ElementKind::SpawnPoint, 0, -1}));
    REQUIRE(doc.def().waves[0].spawns[0].spawn_point_id.empty());
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

TEST_CASE("deleting a vessel clears it from other vessels' children", "[level][edit]") {
    LevelDoc doc = make_doc();
    const i32 child = doc.add_vessel(Vec2{100.0f, 50.0f}, Vec2{140.0f, 90.0f});
    doc.mutable_def().vessels[0].children.push_back(doc.def().vessels[static_cast<usize>(child)].id);

    REQUIRE(doc.erase(ElementRef{ElementKind::Vessel, child, -1}));
    REQUIRE(doc.def().vessels[0].children.empty());
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

TEST_CASE("new ids are unique", "[level][edit]") {
    LevelDoc doc = make_doc();
    doc.add_spawn_point(Vec2{30.0f, 50.0f});
    doc.add_spawn_point(Vec2{40.0f, 50.0f});
    doc.add_spawn_point(Vec2{50.0f, 50.0f});
    REQUIRE(doc.def().spawn_points.size() == 4);
    for (usize i = 0; i < doc.def().spawn_points.size(); ++i) {
        for (usize k = i + 1; k < doc.def().spawn_points.size(); ++k) {
            REQUIRE(doc.def().spawn_points[i].id != doc.def().spawn_points[k].id);
        }
    }
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

TEST_CASE("a duplicated vessel becomes its own lane", "[level][edit]") {
    // Copying the lane_id would silently merge the copy into the original's
    // lane, changing how every wave on that lane reads.
    LevelDoc doc = make_doc();
    doc.duplicate(ElementRef{ElementKind::Vessel, 0, -1});
    REQUIRE(doc.def().vessels.size() == 2);
    REQUIRE(doc.def().vessels[0].lane_id != doc.def().vessels[1].lane_id);
    REQUIRE(doc.def().vessels[0].id != doc.def().vessels[1].id);
}

// ---- Refusals that keep the document loadable ----------------------------

TEST_CASE("the document refuses edits that would make it unloadable", "[level][edit]") {
    LevelDoc doc = make_doc();
    SECTION("the last vessel") {
        REQUIRE_FALSE(doc.erase(ElementRef{ElementKind::Vessel, 0, -1}));
    }
    SECTION("the last spawn point") {
        REQUIRE_FALSE(doc.erase(ElementRef{ElementKind::SpawnPoint, 0, -1}));
    }
    SECTION("the last objective") {
        REQUIRE_FALSE(doc.erase(ElementRef{ElementKind::Objective, 0, -1}));
    }
    SECTION("the last wave") {
        REQUIRE_FALSE(doc.erase(ElementRef{ElementKind::Wave, 0, -1}));
    }
    SECTION("the last spawn entry of a wave") {
        REQUIRE_FALSE(doc.erase(ElementRef{ElementKind::Wave, 0, 0}));
    }
    SECTION("a vessel's third-from-last control point") {
        REQUIRE(doc.def().vessels[0].points.size() == 3);
        REQUIRE(doc.erase(ElementRef{ElementKind::Vessel, 0, 1}));
        REQUIRE_FALSE(doc.erase(ElementRef{ElementKind::Vessel, 0, 0}));
    }
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

TEST_CASE("changing an obstacle's shape produces a legal obstacle", "[level][edit]") {
    // ObstacleDef is one flat struct with three shared geometry fields and each
    // shape reads a different subset, so a naive shape switch leaves a box with
    // zero extents or a ridge whose points have no widths -- both of which the
    // parser rejects outright.
    const ObstacleShape all[] = {ObstacleShape::Disc, ObstacleShape::Capsule, ObstacleShape::Box,
                                 ObstacleShape::Polygon, ObstacleShape::Ridge};
    for (ObstacleShape from : all) {
        for (ObstacleShape to : all) {
            LevelDoc doc = make_doc();
            doc.add_obstacle(from, Vec2{100.0f, 50.0f}, 6.0f);
            doc.set_obstacle_shape(0, to);

            INFO("from " << obstacle_shape_to_string(from) << " to "
                         << obstacle_shape_to_string(to));
            // The real test: it survives a save and a reload.
            const std::string json = level_to_json(doc.def());
            LevelLoader loader;
            LevelDef back;
            const LevelLoadResult r = loader.load_string(json, back);
            INFO("load error: " << r.error);
            REQUIRE(r.ok);
            REQUIRE(back.obstacles.size() == 1);
            REQUIRE(back.obstacles[0].shape == to);
        }
    }
}

TEST_CASE("every added obstacle shape round-trips through the writer", "[level][edit]") {
    LevelDoc doc = make_doc();
    doc.add_obstacle(ObstacleShape::Disc, Vec2{40.0f, 50.0f}, 5.0f);
    doc.add_obstacle(ObstacleShape::Capsule, Vec2{70.0f, 50.0f}, 5.0f);
    doc.add_obstacle(ObstacleShape::Box, Vec2{100.0f, 50.0f}, 5.0f);
    doc.add_obstacle(ObstacleShape::Polygon, Vec2{130.0f, 50.0f}, 5.0f);
    doc.add_obstacle(ObstacleShape::Ridge, Vec2{160.0f, 50.0f}, 5.0f);

    LevelLoader loader;
    LevelDef back;
    const LevelLoadResult r = loader.load_string(level_to_json(doc.def()), back);
    INFO("load error: " << r.error);
    REQUIRE(r.ok);
    REQUIRE(level_equal(doc.def(), back));
}

// ---- Hit testing ----------------------------------------------------------

TEST_CASE("hit testing puts points before bodies", "[level][edit]") {
    // A control point sitting on top of a filled shape has to stay grabbable,
    // or a vessel point inside its own lumen becomes unselectable.
    LevelDoc doc = make_doc();
    const ElementRef hit = doc.hit_test(Vec2{100.0f, 50.0f}, 3.0f);
    REQUIRE(hit.kind == ElementKind::Vessel);
    REQUIRE(hit.index == 0);
    REQUIRE(hit.sub == 1);   // the point, not the body
}

TEST_CASE("hit testing falls through to the body away from any handle",
          "[level][edit]") {
    LevelDoc doc = make_doc();
    const ElementRef hit = doc.hit_test(Vec2{55.0f, 52.0f}, 3.0f);
    REQUIRE(hit.kind == ElementKind::Vessel);
    REQUIRE(hit.sub == -1);
}

TEST_CASE("hit testing finds an obstacle over the vessel under it", "[level][edit]") {
    // An obstacle is a small thing deliberately placed on top of a big one.
    LevelDoc doc = make_doc();
    doc.add_obstacle(ObstacleShape::Disc, Vec2{55.0f, 50.0f}, 4.0f);
    const ElementRef hit = doc.hit_test(Vec2{55.0f, 51.0f}, 1.0f);
    REQUIRE(hit.kind == ElementKind::Obstacle);
}

TEST_CASE("hit testing misses empty space", "[level][edit]") {
    LevelDoc doc = make_doc();
    REQUIRE_FALSE(doc.hit_test(Vec2{100.0f, 95.0f}, 2.0f).valid());
}

TEST_CASE("marquee selection collects the points inside it", "[level][edit]") {
    LevelDoc doc = make_doc();
    const Selection s = doc.hit_test_rect(Rect{Vec2{0.0f, 40.0f}, Vec2{110.0f, 60.0f}});
    // Two vessel control points, the spawn point; not the objective at x=188.
    REQUIRE(s.size() >= 3);
    for (const ElementRef& r : s) REQUIRE(r.kind != ElementKind::Objective);
}

// ---- Dirty flag -----------------------------------------------------------

TEST_CASE("the dirty flag tracks real changes only", "[level][edit]") {
    LevelDoc doc = make_doc();
    REQUIRE_FALSE(doc.dirty());

    doc.move_element(ElementRef{ElementKind::Vessel, 0, 1}, Vec2{100.0f, 70.0f});
    REQUIRE(doc.dirty());

    doc.mark_saved();
    REQUIRE_FALSE(doc.dirty());

    doc.undo();
    REQUIRE(doc.dirty());   // undoing past the save point is still a change

    SECTION("sub-quantum noise is not a change") {
        LevelDoc d2 = make_doc();
        d2.mutable_def().vessels[0].points[0].position.x += 1e-5f;
        REQUIRE_FALSE(d2.dirty());
    }
}

// ---- Waves ----------------------------------------------------------------

TEST_CASE("wave index always matches array position", "[level][edit]") {
    // `index` is not authorable: array position IS the order the director
    // walks, so any reorder has to restamp it or the two disagree.
    LevelDoc doc = make_doc();
    doc.add_wave();
    doc.add_wave();
    REQUIRE(doc.def().waves.size() == 3);

    REQUIRE(doc.move_wave(2, 0));
    for (usize i = 0; i < doc.def().waves.size(); ++i) {
        REQUIRE(doc.def().waves[i].index == static_cast<u32>(i));
    }

    doc.duplicate_wave(0);
    for (usize i = 0; i < doc.def().waves.size(); ++i) {
        REQUIRE(doc.def().waves[i].index == static_cast<u32>(i));
    }

    REQUIRE(doc.erase(ElementRef{ElementKind::Wave, 1, -1}));
    for (usize i = 0; i < doc.def().waves.size(); ++i) {
        REQUIRE(doc.def().waves[i].index == static_cast<u32>(i));
    }
}

TEST_CASE("a new wave continues the ramp", "[level][edit]") {
    // A wave that is easier and pays less than the one before it is never what
    // an author wanted, and the validator would warn about it immediately.
    LevelDoc doc = make_doc();
    const i32 idx = doc.add_wave();
    const WaveDef& prev = doc.def().waves[0];
    const WaveDef& next = doc.def().waves[static_cast<usize>(idx)];
    REQUIRE(next.prep_time <= prev.prep_time);
    REQUIRE(next.atp_reward >= prev.atp_reward);
    REQUIRE_FALSE(next.spawns.empty());
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));
}

// ---- Editing a real shipped level ----------------------------------------

TEST_CASE("a shipped level survives a round of edits", "[level][edit][content]") {
    LevelLoader loader;
    LevelDef def;
    REQUIRE(loader.load_file(platform::asset_path("levels/plaque_field.json"), def).ok);

    LevelDoc doc;
    doc.set_document(def, "plaque_field.json");
    REQUIRE_FALSE(doc.dirty());

    doc.begin_gesture("edit");
    doc.add_obstacle(ObstacleShape::Ridge, Vec2{240.0f, 130.0f}, 6.0f);
    doc.insert_vessel_point(0, 1, Vec2{190.0f, 128.0f});
    doc.add_spawn_point(Vec2{45.0f, 130.0f});
    doc.add_wave();
    doc.end_gesture();

    REQUIRE(doc.dirty());
    REQUIRE_FALSE(has_errors(validate_level(doc.def())));

    // It still saves and reloads.
    LevelDef back;
    const LevelLoadResult r = loader.load_string(level_to_json(doc.def()), back);
    INFO("load error: " << r.error);
    REQUIRE(r.ok);
    REQUIRE(level_equal(doc.def(), back));

    // And the whole thing undoes in one step.
    REQUIRE(doc.undo());
    REQUIRE(level_equal(def, doc.def()));
    REQUIRE_FALSE(doc.dirty());
}
