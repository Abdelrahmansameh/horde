// platform/Window.h — SDL2 window + OpenGL 4.5 core context. FROZEN CONTRACT.
//
// Wave 0 owns this file. Renderer code (Wave 1C) must go through GlContext for
// swap/vsync and must not call SDL_GL_* directly.
#pragma once

#include "core/Types.h"

#include <string>

struct SDL_Window;
using SDL_GLContext = void*;

namespace immune::platform {

struct WindowDesc {
    std::string title = "IMMUNE";
    i32 width = 1600;
    i32 height = 900;
    bool resizable = true;
    bool vsync = true;
    /// Headless creates a 1x1 hidden window. GL still works (needed by
    /// --screenshot, which renders offscreen and reads back), but nothing is
    /// presented. --bench and --sim-test run with no window at all.
    bool hidden = false;
    /// Requests a debug GL context with the KHR_debug callback wired to the log.
    bool gl_debug = false;
};

/// RAII SDL2 window owning a GL 4.5 core-profile context with glad loaded.
/// Construction never throws; check `ok()` and `error()`.
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Initialises SDL video, creates the window, requests a GL 4.5 core
    /// context, and loads glad. Returns false on failure (see error()).
    bool create(const WindowDesc& desc);
    void destroy();

    bool ok() const { return window_ != nullptr && gl_ != nullptr; }
    const std::string& error() const { return error_; }

    SDL_Window* sdl_window() const { return window_; }
    SDL_GLContext gl_context() const { return gl_; }

    i32 width() const { return width_; }
    i32 height() const { return height_; }
    f32 aspect() const { return height_ > 0 ? static_cast<f32>(width_) / static_cast<f32>(height_) : 1.0f; }

    void set_vsync(bool enabled);
    bool vsync() const { return vsync_; }

    /// Presents the backbuffer.
    void swap();

    /// Reports the GL version string of the created context ("" if not created).
    const std::string& gl_version_string() const { return gl_version_; }

    /// True once the user has requested close (Alt-F4 / window X). Input polling
    /// in InputState::poll() sets this.
    bool close_requested() const { return close_requested_; }
    void request_close() { close_requested_ = true; }

    /// Called by InputState::poll when SDL reports a resize.
    void on_resized(i32 w, i32 h) { width_ = w; height_ = h; }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    i32 width_ = 0;
    i32 height_ = 0;
    bool vsync_ = true;
    bool close_requested_ = false;
    std::string error_;
    std::string gl_version_;
};

/// Creates a GL 4.5 context with no visible window, for --screenshot in CI-like
/// conditions. Returns false if no GL device is available at all.
bool create_headless_gl(Window& out_window, i32 width, i32 height);

} // namespace immune::platform
