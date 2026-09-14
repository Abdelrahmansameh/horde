// app/EditorMode.cpp — see EditorMode.h.
#include "app/EditorMode.h"

#include "core/Log.h"
#include "game/level/LevelWriter.h"
#include "platform/FileIO.h"

#include <utility>

namespace immune::app {

void EditorMode::set_bake_desc(const game::GeometryBakeDesc& d) {
    flow_smoothing_radius_ = d.flow_smoothing_radius;
    flow_wall_cost_ = d.flow_wall_cost;
    flow_wall_falloff_ = d.flow_wall_falloff;
    flow_wall_exponent_ = d.flow_wall_exponent;
    bake_dirty_ = true;
}

bool EditorMode::open(const std::string& path) {
    game::LevelLoader loader;
    game::LevelDef def;
    const game::LevelLoadResult r = loader.load_file(path, def);
    if (!r.ok) {
        // Leave the current document alone. A failed open that also wiped what
        // you had open would be the worst possible moment to lose work.
        last_error_ = r.error;
        IMMUNE_LOG_ERROR("editor: cannot open '%s': %s", path.c_str(), r.error.c_str());
        return false;
    }
    doc_.set_document(std::move(def), path);
    last_error_.clear();
    rebake();
    return true;
}

void EditorMode::create(game::LevelTemplate t, const game::TemplateParams& params) {
    doc_.set_document(game::make_template(t, params), {});
    last_error_.clear();
    rebake();
}

bool EditorMode::revert() {
    if (doc_.source_path().empty()) {
        last_error_ = "this level has never been saved, so there is nothing to revert to";
        return false;
    }
    return open(doc_.source_path());
}

bool EditorMode::blocked_from_saving() const { return error_count_ > 0; }

bool EditorMode::save(const std::string& path, bool force) {
    const std::string target = path.empty() ? doc_.source_path() : path;
    if (target.empty()) {
        last_error_ = "no path: use Save As";
        return false;
    }
    if (error_count_ > 0 && !force) {
        last_error_ = "level has " + std::to_string(error_count_) +
                      " error(s); fix them or use Save anyway";
        return false;
    }

    // Back up first, and only then overwrite. `.bak` rather than `.json` is
    // deliberate: App::discover_levels() globs *.json, so a backup with a
    // .json extension would show up in the level list as a duplicate level.
    if (platform::file_exists(target)) {
        if (const auto prev = platform::read_text_file(target)) {
            if (!platform::write_text_file(target + ".bak", *prev)) {
                IMMUNE_LOG_WARN("editor: could not write backup '%s.bak'", target.c_str());
            }
        }
    }

    std::string err;
    if (!game::level_save_file(doc_.def(), target, err)) {
        last_error_ = err;
        return false;
    }

    // Round-trip check on the file we just wrote. A writer bug that drops a
    // field is caught HERE, while the author still has the document in memory,
    // rather than the next time somebody loads the level.
    game::LevelLoader loader;
    game::LevelDef back;
    const game::LevelLoadResult r = loader.load_file(target, back);
    if (!r.ok || !game::level_equal(doc_.def(), back)) {
        last_error_ = "saved file does not round-trip: " +
                      (r.ok ? std::string("content differs") : r.error);
        IMMUNE_LOG_ERROR("editor: %s", last_error_.c_str());
        return false;
    }

    doc_.set_source_path(target);
    doc_.mark_saved();
    last_error_.clear();
    IMMUNE_LOG_INFO("editor: saved '%s'", target.c_str());
    return true;
}

void EditorMode::rebake() {
    bake_dirty_ = false;

    game::GeometryBakeDesc desc;
    desc.flow_smoothing_radius = flow_smoothing_radius_;
    desc.flow_wall_cost = flow_wall_cost_;
    desc.flow_wall_falloff = flow_wall_falloff_;
    desc.flow_wall_exponent = flow_wall_exponent_;

    game::LevelLoader loader;
    const game::LevelLoadResult r =
        loader.bake_geometry(doc_.def(), desc, bake_.mask, bake_.sdf, bake_.flow, &bake_.stats,
                             &bake_.render_sdf);
    bake_.valid = r.ok;
    bake_.error = r.error;
    // The lane grid is what gives the tissue its per-lane hue in the viewport,
    // so it has to move with the geometry or the colours lag the shape.
    bake_.lanes = loader.build_lane_ownership_map(doc_.def());

    refresh_validation();
}

void EditorMode::tick() {
    if (!bake_dirty_) return;
    rebake();
}

void EditorMode::refresh_validation() {
    game::BakedGeometry geo;
    if (bake_.valid) {
        geo.mask = &bake_.mask;
        geo.sdf = &bake_.sdf;
        geo.flow = &bake_.flow;
    }
    issues_ = game::validate_level(doc_.def(), geo);
    error_count_ = 0;
    warning_count_ = 0;
    for (const game::Issue& i : issues_) {
        (i.severity == game::Issue::Severity::Error ? error_count_ : warning_count_) += 1;
    }
}

std::string EditorMode::title() const {
    std::string name = doc_.def().name;
    if (name.empty()) name = "untitled";
    if (doc_.dirty()) name += "*";
    return name;
}

} // namespace immune::app
