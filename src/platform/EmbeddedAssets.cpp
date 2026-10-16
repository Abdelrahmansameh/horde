#include "platform/EmbeddedAssets.h"

#ifdef IMMUNE_EMBEDDED_ASSETS
#include "EmbeddedAssetIndex.h"

#include <windows.h>
#endif

#include <algorithm>

namespace immune::platform {
namespace {

constexpr std::string_view kScheme = "immune-embedded:";

// The table uses slash-separated, root-relative paths. Normalize separators and
// harmless dot components because existing callers may join with fs::path.
// Parent traversal is rejected so resource names cannot have ambiguous aliases.
std::optional<std::string> resource_path(std::string_view path) {
    if (!is_embedded_path(path)) return std::nullopt;
    path.remove_prefix(kScheme.size());
    if (path.empty() || (path.front() != '/' && path.front() != '\\')) return std::nullopt;
    std::string result;
    while (!path.empty()) {
        const auto end = path.find_first_of("/\\");
        const std::string_view component = path.substr(0, end);
        if (component == "..") return std::nullopt;
        if (!component.empty() && component != ".") {
            if (!result.empty()) result += '/';
            result.append(component);
        }
        if (end == std::string_view::npos) break;
        path.remove_prefix(end + 1);
    }
    return result;
}

} // namespace

bool is_embedded_path(std::string_view path) {
    return path.starts_with(kScheme);
}

std::optional<std::span<const u8>> embedded_asset_data(std::string_view path) {
    const auto relative = resource_path(path);
    if (!relative) return std::nullopt;
#ifdef IMMUNE_EMBEDDED_ASSETS
    const auto it = std::lower_bound(detail::kEmbeddedAssets.begin(), detail::kEmbeddedAssets.end(),
                                    std::string_view(*relative),
                                    [](const auto& entry, std::string_view name) {
                                        return entry.path < name;
                                    });
    if (it == detail::kEmbeddedAssets.end() || it->path != *relative) return std::nullopt;
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(it->resource_id), MAKEINTRESOURCEW(10));
    if (!resource) return std::nullopt;
    const DWORD bytes = SizeofResource(module, resource);
    if (bytes == 0) return std::span<const u8>{};
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) return std::nullopt;
    const auto* data = static_cast<const u8*>(LockResource(loaded));
    if (!data) return std::nullopt;
    return std::span<const u8>(data, static_cast<usize>(bytes));
#else
    return std::nullopt;
#endif
}

bool embedded_directory_exists(std::string_view path) {
    const auto relative = resource_path(path);
    if (!relative) return false;
#ifdef IMMUNE_EMBEDDED_ASSETS
    const std::string prefix = relative->empty() ? "" : *relative + '/';
    return std::any_of(detail::kEmbeddedAssets.begin(), detail::kEmbeddedAssets.end(),
                       [&prefix](const auto& entry) { return entry.path.starts_with(prefix); });
#else
    return false;
#endif
}

std::vector<std::string> list_embedded_files(std::string_view directory,
                                           std::string_view extension) {
    std::vector<std::string> result;
    const auto relative = resource_path(directory);
    if (!relative) return result;
#ifdef IMMUNE_EMBEDDED_ASSETS
    const std::string prefix = relative->empty() ? "" : *relative + '/';
    for (const auto& entry : detail::kEmbeddedAssets) {
        if (!entry.path.starts_with(prefix)) continue;
        const std::string_view name = entry.path.substr(prefix.size());
        if (name.empty() || name.find('/') != std::string_view::npos) continue;
        if (!name.ends_with(extension)) continue;
        result.emplace_back(std::string(kEmbeddedAssetRoot) + std::string(entry.path));
    }
#else
    (void)extension;
#endif
    return result;
}

} // namespace immune::platform
