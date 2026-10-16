// Read-only assets linked into a standalone Windows executable as RCDATA.
// Resource bytes remain owned by the executable for its entire lifetime.
#pragma once

#include "core/Types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace immune::platform {

inline constexpr std::string_view kEmbeddedAssetRoot = "immune-embedded:/";

/// Recognizes the reserved resource scheme, including Windows-style separators.
/// These paths never fall through to the host filesystem, even in loose builds.
bool is_embedded_path(std::string_view path);

/// No extraction or allocation: the returned bytes refer to the executable.
/// Returns nullopt for malformed paths, absent resources, or loose builds.
std::optional<std::span<const u8>> embedded_asset_data(std::string_view path);
bool embedded_directory_exists(std::string_view path);

/// Immediate children only; canonical full URIs, sorted by path.
std::vector<std::string> list_embedded_files(std::string_view directory,
                                           std::string_view extension);

} // namespace immune::platform
