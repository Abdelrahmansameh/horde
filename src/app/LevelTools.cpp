// app/LevelTools.cpp — --level-check and --level-fmt. See LevelTools.h.
//
// The point of both is that the editor gets no exemption from the project's
// rule that everything is verifiable without a screen: --level-check runs the
// exact validator the editor's red halos come from, and --level-fmt runs the
// exact writer its Save uses. If these two pass in CI, an editor session cannot
// produce content that surprises the game.
#include "app/LevelTools.h"

#include "app/Cli.h"
#include "core/Clock.h"
#include "core/Log.h"
#include "game/editor/LevelValidate.h"
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "platform/FileIO.h"
#include "sim/flowfield/FlowField.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace immune::app {
namespace {

using json = nlohmann::json;

/// A single .json file, or every *.json directly inside a directory. Sorted, so
/// a CI report is diffable run to run.
std::vector<std::string> resolve_targets(const std::string& path, std::string& err) {
    std::vector<std::string> out;
    std::error_code ec;
    if (path.empty()) {
        err = "no path given";
        return out;
    }
    if (std::filesystem::is_directory(path, ec)) {
        out = platform::list_files(path, ".json");
        if (out.empty()) err = "no .json files in '" + path + "'";
        return out;
    }
    if (!platform::file_exists(path)) {
        err = "no such file or directory: '" + path + "'";
        return out;
    }
    out.push_back(path);
    return out;
}

/// The geometry bake for the checks that need it. Owned here rather than in a
/// SimWorld: bake_geometry() exists precisely so a caller can get the walkable
/// shape without constructing and destroying a whole world for it.
struct Bake {
    sim::TissueMask mask;
    sim::DistanceField sdf;
    sim::FlowField flow;
    bool ok = false;
    std::string error;
    /// Wall-clock cost of the whole rasterize->carve->SDF->flow chain.
    ///
    /// Reported because it is an AUTHORING fact, not just a profiling one: the
    /// editor re-runs this bake on every gesture, so a level whose bake is slow
    /// is a level that edits badly, and the author should be able to see that
    /// number before the tool feels sluggish rather than after.
    f64 ms = 0.0;
    i64 cells = 0;
    game::GeometryBakeStats stats;
};

Bake bake(const game::LevelDef& def) {
    Bake b;
    // Default GeometryBakeDesc: no wall-proximity cost, no smoothing. Neither
    // affects WHERE the walkable cells are or what is reachable from what,
    // which is all the validator asks -- and leaving them at zero keeps
    // --level-check independent of assets/config, so a tuning edit can never
    // change whether a level passes CI.
    const game::LevelLoader loader;
    const game::LevelLoadResult r =
        loader.bake_geometry(def, game::GeometryBakeDesc{}, b.mask, b.sdf, b.flow, &b.stats);
    b.ms = b.stats.total_ms;
    b.ok = r.ok;
    b.error = r.error;
    b.cells = static_cast<i64>(b.mask.width()) * static_cast<i64>(b.mask.height());
    return b;
}

json issue_to_json(const game::Issue& i) {
    json j;
    j["severity"] = game::severity_to_string(i.severity);
    j["message"] = i.message;
    if (i.ref.valid()) {
        j["element"] = game::element_kind_to_string(i.ref.kind);
        if (i.ref.index >= 0) j["index"] = i.ref.index;
        if (i.ref.sub >= 0) j["sub_index"] = i.ref.sub;
    }
    if (i.has_anchor) j["at"] = json::array({i.anchor.x, i.anchor.y});
    return j;
}

} // namespace

int run_level_check(const Options& options) {
    std::string err;
    const std::vector<std::string> targets = resolve_targets(options.level_path, err);
    if (targets.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    json report;
    report["levels"] = json::array();
    u32 error_files = 0;
    u32 total_errors = 0;
    u32 total_warnings = 0;

    for (const std::string& path : targets) {
        json entry;
        entry["path"] = path;

        game::LevelLoader loader;
        game::LevelDef def;
        const game::LevelLoadResult load = loader.load_file(path, def);
        if (!load.ok) {
            // A parse failure is one error and there is nothing else to say
            // about the file; carry on to the next so one bad level does not
            // hide the state of the other thirteen.
            entry["ok"] = false;
            entry["errors"] = 1;
            entry["warnings"] = 0;
            entry["issues"] = json::array({json{{"severity", "error"}, {"message", load.error}}});
            report["levels"].push_back(std::move(entry));
            ++error_files;
            ++total_errors;
            continue;
        }

        entry["name"] = def.name;
        entry["region"] = def.region;

        Bake b = bake(def);
        entry["bake_ms"] = b.ms;
        entry["cells"] = b.cells;
        entry["bake_stages_ms"] = json{{"rasterize", b.stats.rasterize_ms},
                                       {"carve", b.stats.carve_ms},
                                       {"sdf", b.stats.sdf_ms},
                                       {"wall_cost", b.stats.wall_cost_ms},
                                       {"flow", b.stats.flow_ms}};
        game::BakedGeometry geo;
        if (b.ok) {
            geo.mask = &b.mask;
            geo.sdf = &b.sdf;
            geo.flow = &b.flow;
        } else {
            entry["bake_error"] = b.error;
        }

        const std::vector<game::Issue> issues = game::validate_level(def, geo);
        u32 errors = 0;
        u32 warnings = 0;
        json arr = json::array();
        for (const game::Issue& i : issues) {
            (i.severity == game::Issue::Severity::Error ? errors : warnings) += 1;
            arr.push_back(issue_to_json(i));
        }
        entry["ok"] = errors == 0;
        entry["errors"] = errors;
        entry["warnings"] = warnings;
        entry["issues"] = std::move(arr);
        report["levels"].push_back(std::move(entry));

        total_errors += errors;
        total_warnings += warnings;
        if (errors > 0) ++error_files;

        // Human-readable to stderr, so stdout stays pure JSON.
        for (const game::Issue& i : issues) {
            std::fprintf(stderr, "%s: %s: %s\n", path.c_str(),
                         game::severity_to_string(i.severity), i.message.c_str());
        }
    }

    report["checked"] = static_cast<u32>(targets.size());
    report["failed"] = error_files;
    report["errors"] = total_errors;
    report["warnings"] = total_warnings;
    report["ok"] = error_files == 0;

    std::fprintf(stdout, "%s\n", report.dump(2).c_str());
    return error_files == 0 ? 0 : 1;
}

int run_level_fmt(const Options& options) {
    std::string err;
    const std::vector<std::string> targets = resolve_targets(options.level_path, err);
    if (targets.empty()) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    u32 changed = 0;
    u32 failed = 0;
    for (const std::string& path : targets) {
        game::LevelLoader loader;
        game::LevelDef def;
        const game::LevelLoadResult load = loader.load_file(path, def);
        if (!load.ok) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), load.error.c_str());
            ++failed;
            continue;
        }

        const std::string canonical = game::level_to_json(def);
        const auto current = platform::read_text_file(path);
        if (current && *current == canonical) continue;

        // Re-parsing the canonical text before writing is the safety net that
        // makes this mode usable on content people care about: a writer bug
        // that drops a field is caught HERE, on a file we are about to
        // overwrite, rather than the next time someone loads the level.
        game::LevelDef round_tripped;
        const game::LevelLoadResult back = loader.load_string(canonical, round_tripped);
        if (!back.ok || !game::level_equal(def, round_tripped)) {
            std::fprintf(stderr, "%s: REFUSING to rewrite -- canonical form does not round-trip%s%s\n",
                         path.c_str(), back.ok ? "" : ": ", back.ok ? "" : back.error.c_str());
            ++failed;
            continue;
        }

        ++changed;
        if (options.check_only) {
            std::fprintf(stderr, "%s: not canonical\n", path.c_str());
            continue;
        }
        if (!platform::write_text_file(path, canonical)) {
            std::fprintf(stderr, "%s: cannot write\n", path.c_str());
            ++failed;
            continue;
        }
        std::fprintf(stderr, "%s: rewritten\n", path.c_str());
    }

    json report;
    report["checked"] = static_cast<u32>(targets.size());
    report["changed"] = changed;
    report["failed"] = failed;
    report["mode"] = options.check_only ? "check" : "write";
    report["ok"] = failed == 0 && (!options.check_only || changed == 0);
    std::fprintf(stdout, "%s\n", report.dump(2).c_str());

    if (failed > 0) return 1;
    return (options.check_only && changed > 0) ? 1 : 0;
}

} // namespace immune::app
