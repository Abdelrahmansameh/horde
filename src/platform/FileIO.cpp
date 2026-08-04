#include "platform/FileIO.h"

#include "core/Log.h"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace immune::platform {

std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return contents;
}

std::optional<std::vector<u8>> read_binary_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const auto size = static_cast<usize>(in.tellg());
    in.seekg(0);
    std::vector<u8> data(size);
    if (size > 0) in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

bool write_text_file(const std::string& path, std::string_view contents) {
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return out.good();
}

bool write_binary_file(const std::string& path, const void* data, usize bytes) {
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (bytes > 0) out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return out.good();
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !fs::is_directory(path, ec);
}

bool ensure_directory(const std::string& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) return fs::is_directory(path, ec);
    return fs::create_directories(path, ec);
}

std::vector<std::string> list_files(const std::string& dir, std::string_view extension) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (extension.empty() ||
            (name.size() >= extension.size() &&
             name.compare(name.size() - extension.size(), extension.size(), extension) == 0)) {
            out.push_back(entry.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string executable_dir() {
    char* base = SDL_GetBasePath();
    if (!base) {
        std::error_code ec;
        return fs::current_path(ec).string();
    }
    std::string result(base);
    SDL_free(base);
    if (!result.empty() && (result.back() == '/' || result.back() == '\\')) result.pop_back();
    return result;
}

const std::string& asset_root() {
    static const std::string root = [] {
        if (const char* env = std::getenv("IMMUNE_ASSET_ROOT")) {
            if (env[0] != '\0') return std::string(env);
        }
        std::error_code ec;
        fs::path p(executable_dir());
        for (int i = 0; i < 8 && !p.empty(); ++i) {
            if (fs::exists(p / "assets", ec)) return (p).string();
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        fs::path cwd = fs::current_path(ec);
        for (int i = 0; i < 8 && !cwd.empty(); ++i) {
            if (fs::exists(cwd / "assets", ec)) return cwd.string();
            if (!cwd.has_parent_path() || cwd.parent_path() == cwd) break;
            cwd = cwd.parent_path();
        }
        return fs::current_path(ec).string();
    }();
    return root;
}

std::string asset_path(std::string_view relative) {
    fs::path p = fs::path(asset_root()) / "assets" / fs::path(std::string(relative));
    return p.string();
}

} // namespace immune::platform
