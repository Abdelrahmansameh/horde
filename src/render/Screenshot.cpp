#include "render/Screenshot.h"

#include "core/Log.h"

#include <glad/glad.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <filesystem>

namespace immune::render {

bool write_png_rgba(const std::string& path, const u8* pixels, i32 width, i32 height) {
    if (!pixels || width <= 0 || height <= 0) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    const int stride = width * 4;
    const int ok = stbi_write_png(path.c_str(), width, height, 4, pixels, stride);
    if (!ok) {
        IMMUNE_LOG_ERROR("stbi_write_png failed for '%s'", path.c_str());
        return false;
    }
    return true;
}

bool read_framebuffer_rgba(std::vector<u8>& out_rgba, i32 width, i32 height) {
    if (width <= 0 || height <= 0) return false;
    const usize row = static_cast<usize>(width) * 4u;
    out_rgba.assign(row * static_cast<usize>(height), 0u);

    std::vector<u8> flipped(out_rgba.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    if (glGetError() != GL_NO_ERROR) return false;

    // GL returns bottom-up; PNG wants top-down.
    for (i32 y = 0; y < height; ++y) {
        const u8* src = flipped.data() + static_cast<usize>(height - 1 - y) * row;
        u8* dst = out_rgba.data() + static_cast<usize>(y) * row;
        for (usize i = 0; i < row; ++i) dst[i] = src[i];
    }
    return true;
}

bool capture_framebuffer_png(const std::string& path, i32 width, i32 height) {
    std::vector<u8> pixels;
    if (!read_framebuffer_rgba(pixels, width, height)) {
        IMMUNE_LOG_ERROR("framebuffer readback failed (%dx%d)", width, height);
        return false;
    }
    return write_png_rgba(path, pixels.data(), width, height);
}

} // namespace immune::render
