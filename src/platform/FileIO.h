// platform/FileIO.h — filesystem helpers. FROZEN CONTRACT.
//
// Everything returns by value / bool; no exceptions cross this boundary
// (docs/CONVENTIONS.md). Never called from the sim hot path.
#pragma once

#include "core/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace immune::platform {

/// Reads a whole file as text. std::nullopt on failure.
std::optional<std::string> read_text_file(const std::string& path);

/// Reads a whole file as bytes. std::nullopt on failure.
std::optional<std::vector<u8>> read_binary_file(const std::string& path);

bool write_text_file(const std::string& path, std::string_view contents);
bool write_binary_file(const std::string& path, const void* data, usize bytes);

bool file_exists(const std::string& path);
bool ensure_directory(const std::string& path);

/// Lists files directly inside `dir` whose name ends with `extension`
/// (e.g. ".json"). Returns full paths, sorted, for deterministic iteration.
std::vector<std::string> list_files(const std::string& dir, std::string_view extension);

/// Directory containing the running executable.
std::string executable_dir();

/// Repository/asset root. Resolution order:
///   1. $IMMUNE_ASSET_ROOT if set
///   2. the first ancestor of the executable directory containing "assets/"
///   3. the current working directory
/// Deterministic modes print the resolved root so a failed lookup is obvious.
const std::string& asset_root();

/// Joins asset_root() with a relative path, e.g. asset_path("shaders/chaff.vert").
std::string asset_path(std::string_view relative);

} // namespace immune::platform
