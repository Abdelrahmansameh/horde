// config/Registry.cpp
#include "config/Registry.h"

#include "config/Json.h"

#include <algorithm>

namespace immune::config {

void Registry::bind(std::string path, const Schema& schema, void* base) {
    const auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                 [&](const Binding& b) { return b.path == path; });
    if (it != bindings_.end()) {
        it->schema = &schema;
        it->base = base;
        return;
    }
    bindings_.push_back(Binding{std::move(path), &schema, base});
}

void Registry::clear() { bindings_.clear(); }

std::vector<std::string> Registry::field_paths() const {
    std::vector<std::string> out;
    for (const Binding& b : bindings_) {
        for (const Field& f : b.schema->fields) {
            out.push_back(b.path + "." + f.name);
        }
    }
    return out;
}

const Binding* Registry::resolve(std::string_view field_path,
                                 std::string_view& field_out) const {
    const usize dot = field_path.rfind('.');
    if (dot == std::string_view::npos) return nullptr;
    const std::string_view prefix = field_path.substr(0, dot);
    field_out = field_path.substr(dot + 1);
    const auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                 [&](const Binding& b) { return b.path == prefix; });
    return it == bindings_.end() ? nullptr : &*it;
}

namespace {

/// "no such path" is the most common console typo, so spend a little effort
/// pointing at the nearest real one.
std::string unknown_path_error(std::string_view field_path,
                               const std::vector<std::string>& known) {
    std::vector<std::string_view> views;
    views.reserve(known.size());
    for (const std::string& k : known) views.emplace_back(k);
    const std::string_view suggestion = closest_name(field_path, views);
    std::string err = "no config field at " + quote(field_path);
    if (!suggestion.empty()) err += " (did you mean " + quote(suggestion) + "?)";
    return err;
}

} // namespace

bool Registry::get(std::string_view field_path, std::string& out, std::string& err) const {
    std::string_view field;
    const Binding* binding = resolve(field_path, field);
    if (binding == nullptr || !field_to_string(*binding->schema, binding->base, field, out)) {
        err = unknown_path_error(field_path, field_paths());
        return false;
    }
    return true;
}

bool Registry::set(std::string_view field_path, std::string_view value, std::string& err) {
    std::string_view field;
    // resolve() is const; the binding it names is not, and bindings_ is ours.
    const Binding* found = resolve(field_path, field);
    if (found == nullptr || found->schema->find(field) == nullptr) {
        err = unknown_path_error(field_path, field_paths());
        return false;
    }
    auto* binding = const_cast<Binding*>(found);
    if (!field_from_string(*binding->schema, binding->base, field, value, err)) {
        err = std::string(field_path) + ": " + err;
        return false;
    }
    return true;
}

std::string_view Registry::doc(std::string_view field_path) const {
    std::string_view field;
    const Binding* binding = resolve(field_path, field);
    if (binding == nullptr) return {};
    const Field* f = binding->schema->find(field);
    return f == nullptr ? std::string_view{} : std::string_view{f->doc};
}

} // namespace immune::config
