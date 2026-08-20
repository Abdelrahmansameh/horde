// config/ConfigStore.cpp
#include "config/ConfigStore.h"

#include "core/Log.h"
#include "platform/FileIO.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace immune::config {
namespace {

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char back = dir.back();
    return (back == '/' || back == '\\') ? dir + name : dir + "/" + name;
}

/// Strips \r so a file saved on Windows and one saved on Linux hash the same.
/// Without this the config hash recorded in a --sim-test report would depend on
/// the checkout's line endings rather than on the tuning values.
std::string normalize(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '\r') out += c;
    }
    return out;
}

const Json& empty_object() {
    static const Json value = Json::object();
    return value;
}

} // namespace

void ConfigStore::expect_file(std::string name) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.name == name; });
    if (it != entries_.end()) return;
    entries_.push_back(Entry{std::move(name), {}, Json::object()});
}

const ConfigStore::Entry* ConfigStore::find(std::string_view name) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.name == name; });
    return it == entries_.end() ? nullptr : &*it;
}

const Json& ConfigStore::file(std::string_view name) const {
    const Entry* entry = find(name);
    assert(entry != nullptr && "config file was never declared with expect_file()");
    return entry == nullptr ? empty_object() : entry->document;
}

LoadResult ConfigStore::load_dir(const std::string& dir) {
    // Parse everything into a staging copy first: a config file that fails
    // halfway must not leave half the game retuned.
    std::vector<Entry> staged;
    staged.reserve(entries_.size());

    for (const Entry& entry : entries_) {
        const std::string path = join_path(dir, entry.name);
        const std::optional<std::string> text = platform::read_text_file(path);
        if (!text.has_value()) {
            return LoadResult{false, "config: cannot read " + path, {}};
        }
        Entry fresh;
        fresh.name = entry.name;
        fresh.text = *text;
        try {
            fresh.document = Json::parse(*text);
        } catch (const std::exception& e) {
            return LoadResult{false, entry.name + ": JSON parse error: " + e.what(), {}};
        }
        if (!fresh.document.is_object()) {
            return LoadResult{false, entry.name + ": top level must be an object", {}};
        }
        staged.push_back(std::move(fresh));
    }

    entries_ = std::move(staged);
    dir_ = dir;
    loaded_ = true;
    return LoadResult{true, {}, {}};
}

bool ConfigStore::write_file(const std::string& dir, const std::string& name,
                             const Json& document, std::string& err) {
    if (!platform::ensure_directory(dir)) {
        err = "cannot create directory " + dir;
        return false;
    }
    const std::string path = join_path(dir, name);
    // Two-space indent matches assets/levels/*.json; the trailing newline keeps
    // the files diff-friendly.
    if (!platform::write_text_file(path, document.dump(2) + "\n")) {
        err = "cannot write " + path;
        return false;
    }
    return true;
}

bool ConfigStore::poll_changed(std::string& error_out) {
    if (!loaded_) return false;

    bool any_changed = false;
    for (const Entry& entry : entries_) {
        const std::optional<std::string> text =
            platform::read_text_file(join_path(dir_, entry.name));
        // A file that vanished (or is momentarily locked by an editor) is not
        // an error worth reporting every frame; the next poll will pick it up.
        if (text.has_value() && *text != entry.text) {
            any_changed = true;
            break;
        }
    }
    if (!any_changed) return false;

    const LoadResult result = load_dir(dir_);
    if (!result.ok) {
        error_out = result.error;
        return false;
    }
    return true;
}

u64 ConfigStore::hash() const {
    // FNV-1a, matching the sim's state_hash so the two read alike in reports.
    u64 h = 1469598103934665603ULL;
    const auto mix = [&h](std::string_view bytes) {
        for (const char c : bytes) {
            h ^= static_cast<u8>(c);
            h *= 1099511628211ULL;
        }
    };
    for (const Entry& entry : entries_) {
        mix(entry.name);
        mix(normalize(entry.text));
    }
    return h;
}

} // namespace immune::config
