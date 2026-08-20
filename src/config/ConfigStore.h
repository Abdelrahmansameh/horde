// config/ConfigStore.h — owns the on-disk tuning files and the live registry.
//
// RATIONALE
//  - One store per process, owned by App. It reads assets/config/*.json, keeps
//    both the raw text (for change detection and hashing) and the parsed Json
//    (for the per-system loaders), and exposes the Registry that makes every
//    field addressable from the gym console.
//  - Change detection compares file CONTENT, not mtime, exactly as
//    render/Shader.cpp does for GLSL: editors touch mtimes on save-with-no-edit
//    and some sync a whole tree at once, and a spurious reload of a balance
//    file is a visible gameplay hitch.
//  - A reload that fails to parse keeps the last good state and reports the
//    error. Mid-edit a JSON file is syntactically broken for several seconds;
//    that must not take the game down.
//  - hash() feeds the --sim-test and --bench reports so a run records which
//    tuning it was produced with. Config is a determinism input now.
#pragma once

#include "config/Json.h"
#include "config/Registry.h"

#include <string>
#include <vector>

namespace immune::config {

struct LoadResult {
    bool ok = false;
    std::string error;                  ///< Empty iff ok. Carries the field path.
    std::vector<std::string> warnings;
};

class ConfigStore {
public:
    /// Declares a file the store expects to find. Every declared file must
    /// exist and parse: the config is authoritative, so a missing towers.json
    /// is a hard failure rather than a silent fallback.
    void expect_file(std::string name);

    /// Reads and parses every expected file from `dir`. On failure nothing is
    /// mutated, so a failed reload leaves the previous config intact.
    LoadResult load_dir(const std::string& dir);

    /// Parsed document for a declared file. Fails an assert if `name` was not
    /// declared, since that is a programming error rather than a data error.
    const Json& file(std::string_view name) const;

    /// Writes `document` to <dir>/<name>, pretty-printed with a stable key
    /// order. Creates `dir` if needed.
    static bool write_file(const std::string& dir, const std::string& name,
                           const Json& document, std::string& err);

    /// Re-reads the expected files. Returns true if any file's text changed and
    /// the new text parsed. On a parse error the old state is kept and the
    /// message goes to `error_out`.
    bool poll_changed(std::string& error_out);

    /// FNV-1a over every file's name and normalized text, in declared order.
    u64 hash() const;

    const std::string& dir() const { return dir_; }
    bool loaded() const { return loaded_; }

    Registry& registry() { return registry_; }
    const Registry& registry() const { return registry_; }

private:
    struct Entry {
        std::string name;
        std::string text;   ///< Raw file text, for change detection and hashing.
        Json document;
    };

    const Entry* find(std::string_view name) const;

    std::string dir_;
    std::vector<Entry> entries_;
    Registry registry_;
    bool loaded_ = false;
};

} // namespace immune::config
