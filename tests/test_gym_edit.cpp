// tests/test_gym_edit.cpp — the `edit` gym command family.
//
// The point of exposing the level editor through the SAME command language
// everything else uses is that the GUI cannot become a second API: the panels
// are a typist for LevelDoc, `edit` is a typist for LevelDoc, and a divergence
// between them would show up as one of these tests failing.
//
// It also makes editor operations reachable from --exec, and therefore from
// --sim-test and screenshot regressions -- which is what these tests actually
// prove, since they drive the commands with no window and no ImGui at all.
#include "game/editor/LevelDoc.h"
#include "game/editor/LevelTemplates.h"
#include "game/editor/LevelValidate.h"
#include "game/gym/GymCommands.h"
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "platform/FileIO.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using namespace immune;
using namespace immune::game;

namespace {

/// A context with a document and nothing else -- no world, no towers, no
/// window. That is the whole point: the edit commands must not need any of it.
struct EditCtx {
    LevelDoc doc;
    GymContext ctx;
    u32 changes = 0;

    EditCtx() {
        doc.set_document(make_template(LevelTemplate::StraightLane, TemplateParams{}));
        ctx.doc = &doc;
        ctx.doc_changed = [this]() { ++changes; };
    }

    GymResult run(const std::string& line) { return gym_execute(ctx, line); }
};

} // namespace

TEST_CASE("edit reports clearly when no document is open", "[gym][edit]") {
    // Every other gym command in a context that cannot serve it says so rather
    // than silently doing nothing; this one is no different.
    GymContext bare;
    const GymResult r = gym_execute(bare, "edit list");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.message.find("editor") != std::string::npos);
}

TEST_CASE("edit list describes the document", "[gym][edit]") {
    EditCtx e;
    const GymResult r = e.run("edit list");
    INFO(r.message);
    REQUIRE(r.ok);
    REQUIRE(r.message.find("vessel") != std::string::npos);
    REQUIRE(r.message.find("spawn") != std::string::npos);
    REQUIRE(r.message.find("objective") != std::string::npos);
}

TEST_CASE("edit new builds each template", "[gym][edit]") {
    EditCtx e;
    for (const char* name : {"straight", "fork", "switchback", "convergent", "multi"}) {
        INFO("template " << name);
        const GymResult r = e.run(std::string("edit new ") + name);
        INFO(r.message);
        REQUIRE(r.ok);
        REQUIRE_FALSE(has_errors(validate_level(e.doc.def())));
    }
    SECTION("and rejects an unknown one") {
        REQUIRE_FALSE(e.run("edit new nonsense").ok);
    }
}

TEST_CASE("edit adds each element kind", "[gym][edit]") {
    EditCtx e;
    const usize v0 = e.doc.def().vessels.size();
    const usize o0 = e.doc.def().obstacles.size();
    const usize s0 = e.doc.def().spawn_points.size();
    const usize g0 = e.doc.def().objectives.size();
    const usize z0 = e.doc.def().placement_zones.size();

    REQUIRE(e.run("edit vessel add 60,60 300,60 20").ok);
    REQUIRE(e.doc.def().vessels.size() == v0 + 1);

    REQUIRE(e.run("edit obstacle disc 200,130 r 10").ok);
    REQUIRE(e.run("edit obstacle capsule 240,130 r 6").ok);
    REQUIRE(e.run("edit obstacle box 280,130 r 8").ok);
    REQUIRE(e.run("edit obstacle polygon 320,130 r 8").ok);
    REQUIRE(e.run("edit obstacle ridge 360,130 r 8").ok);
    REQUIRE(e.doc.def().obstacles.size() == o0 + 5);

    REQUIRE(e.run("edit spawn add 70,130").ok);
    REQUIRE(e.doc.def().spawn_points.size() == s0 + 1);

    REQUIRE(e.run("edit objective add 400,130").ok);
    REQUIRE(e.doc.def().objectives.size() == g0 + 1);

    REQUIRE(e.run("edit zone 20,20 200,200").ok);
    REQUIRE(e.doc.def().placement_zones.size() == z0 + 1);
    // The tag vector is index-aligned with the zone vector; if the command
    // bypassed the document op it would drift.
    REQUIRE(e.doc.def().placement_zone_tags.size() == e.doc.def().placement_zones.size());

    // Every add re-bakes.
    REQUIRE(e.changes >= 9);

    // ...and the result is still a level that saves and reloads.
    LevelLoader loader;
    LevelDef back;
    const LevelLoadResult r = loader.load_string(level_to_json(e.doc.def()), back);
    INFO(r.error);
    REQUIRE(r.ok);
    REQUIRE(level_equal(e.doc.def(), back));
}

TEST_CASE("edit point adds and widths", "[gym][edit]") {
    EditCtx e;
    const usize before = e.doc.def().vessels[0].points.size();
    REQUIRE(e.run("edit point add 0 at 240,140").ok);
    REQUIRE(e.doc.def().vessels[0].points.size() == before + 1);

    REQUIRE(e.run("edit point 0 1 w 42").ok);
    REQUIRE(e.doc.def().vessels[0].points[1].width == 42.0f);

    SECTION("and reports bad arguments") {
        REQUIRE_FALSE(e.run("edit point add notanumber at 1,2").ok);
        REQUIRE_FALSE(e.run("edit point 0 1 w notanumber").ok);
        REQUIRE_FALSE(e.run("edit point").ok);
    }
}

TEST_CASE("edit undo and redo walk the document history", "[gym][edit]") {
    EditCtx e;
    const LevelDef start = e.doc.def();

    REQUIRE(e.run("edit obstacle disc 200,130 r 10").ok);
    REQUIRE_FALSE(level_equal(start, e.doc.def()));

    REQUIRE(e.run("edit undo").ok);
    REQUIRE(level_equal(start, e.doc.def()));

    REQUIRE(e.run("edit redo").ok);
    REQUIRE_FALSE(level_equal(start, e.doc.def()));

    REQUIRE(e.run("edit undo").ok);
    REQUIRE_FALSE(e.run("edit undo").ok);   // nothing left
}

TEST_CASE("edit validate is a usable gate", "[gym][edit]") {
    // Errors make the command FAIL, so `--exec "edit validate"` can gate a
    // script instead of forcing the caller to grep the message.
    EditCtx e;
    const GymResult clean = e.run("edit validate");
    INFO(clean.message);
    REQUIRE(clean.ok);

    // Break it: an objective off the tissue is an error the JSON-only rules
    // cannot see, so use one they can -- a wave naming a spawn point that is
    // about to stop existing.
    e.doc.mutable_def().waves[0].spawns[0].spawn_point_id = "ghost";
    const GymResult broken = e.run("edit validate");
    INFO(broken.message);
    REQUIRE_FALSE(broken.ok);
    REQUIRE(broken.message.find("ghost") != std::string::npos);
}

TEST_CASE("edit save writes a loadable level", "[gym][edit][io]") {
    EditCtx e;
    std::string written;
    e.ctx.doc_save = [&](const std::string& path, bool force, std::string& err) {
        (void)force;
        written = path;
        std::string werr;
        if (level_save_file(e.doc.def(), path, werr)) return true;
        err = werr;
        return false;
    };

    const char* t = std::getenv("TEMP");
    const std::string out = (t ? std::string(t) : std::string(".")) + "/gym_edit_save.json";
    REQUIRE(e.run("edit save " + out).ok);
    REQUIRE(written == out);

    LevelLoader loader;
    LevelDef back;
    const LevelLoadResult r = loader.load_file(out, back);
    INFO(r.error);
    REQUIRE(r.ok);
    REQUIRE(level_equal(e.doc.def(), back));

    SECTION("and says so when the context cannot save") {
        EditCtx bare;
        REQUIRE_FALSE(bare.run("edit save /nowhere.json").ok);
    }
}

TEST_CASE("edit rejects unknown subcommands", "[gym][edit]") {
    EditCtx e;
    REQUIRE_FALSE(e.run("edit nonsense").ok);
    REQUIRE_FALSE(e.run("edit").ok);
    REQUIRE_FALSE(e.run("edit obstacle notashape 1,2").ok);
}

TEST_CASE("edit appears in help", "[gym][edit]") {
    // The command table is the discovery surface; a command missing from it is
    // a command nobody finds.
    GymContext bare;
    const GymResult r = gym_execute(bare, "help");
    REQUIRE(r.ok);
    REQUIRE(r.message.find("edit") != std::string::npos);
}
