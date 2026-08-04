// render/Screenshot.h — PNG capture via stb_image_write. FROZEN CONTRACT.
// Owner: Wave 0.
//
// This is the project's primary visual verification channel: sub-agents cannot
// look at a screen, but they CAN read a PNG. Keep it dependency-light and
// always working.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::render {

/// Writes RGBA8 pixel data to a PNG. `pixels` must hold width*height*4 bytes in
/// top-down row order. Returns false on any I/O or encode failure.
bool write_png_rgba(const std::string& path, const u8* pixels, i32 width, i32 height);

/// Reads the currently bound GL framebuffer and writes it to `path`.
/// Handles the GL bottom-up -> PNG top-down flip. Requires a current context.
bool capture_framebuffer_png(const std::string& path, i32 width, i32 height);

/// Reads the currently bound GL framebuffer into `out_rgba` (top-down).
bool read_framebuffer_rgba(std::vector<u8>& out_rgba, i32 width, i32 height);

} // namespace immune::render
