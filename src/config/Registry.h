// config/Registry.h — path-addressable view over the live config structs.
//
// RATIONALE
//  - `config set towers.macrophage.2.stats.damage 200` from the gym console has
//    to reach the exact same bytes the JSON loader wrote. Rather than give each
//    system its own setter, each system BINDS its structs here once after load,
//    and the registry resolves a dotted path to (Schema, instance, field).
//  - Bindings point at memory owned elsewhere and are invalidated by a reload,
//    so a reload re-binds. Nothing here owns config data.
//  - Iteration order is declaration order, kept stable so `config list` output
//    and error suggestions are deterministic.
#pragma once

#include "config/Field.h"

#include <string>
#include <string_view>
#include <vector>

namespace immune::config {

/// One config struct instance, reachable at `path`.
struct Binding {
    std::string path;      ///< e.g. "towers.macrophage.2.stats"
    const Schema* schema = nullptr;
    void* base = nullptr;
};

class Registry {
public:
    /// Binds an instance at `path`. Re-binding the same path replaces it, which
    /// is what a hot reload does.
    void bind(std::string path, const Schema& schema, void* base);
    void clear();

    /// Full paths of every addressable field ("towers.macrophage.2.stats.damage"),
    /// in binding order. Used by `config list` and by did-you-mean suggestions.
    std::vector<std::string> field_paths() const;

    /// Resolves "<binding path>.<field>". False with `err` set if either half
    /// does not resolve.
    bool get(std::string_view field_path, std::string& out, std::string& err) const;
    bool set(std::string_view field_path, std::string_view value, std::string& err);

    /// Docstring for a field path, or empty. Powers `config get --doc`.
    std::string_view doc(std::string_view field_path) const;

    usize binding_count() const { return bindings_.size(); }

private:
    /// Splits at the last '.' and looks the prefix up. Returns nullptr on miss.
    const Binding* resolve(std::string_view field_path, std::string_view& field_out) const;

    std::vector<Binding> bindings_;
};

} // namespace immune::config
