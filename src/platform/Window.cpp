#include "platform/Window.h"

#include "core/Log.h"

#include <glad/glad.h>
#include <SDL.h>

namespace immune::platform {
namespace {

void APIENTRY gl_debug_callback(GLenum /*source*/, GLenum type, GLuint /*id*/,
                                GLenum severity, GLsizei /*length*/,
                                const GLchar* message, const void* /*user*/) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    const auto level = (type == GL_DEBUG_TYPE_ERROR) ? log::Level::Error : log::Level::Warn;
    log::writef(level, "GL: %s", message);
}

} // namespace

Window::~Window() { destroy(); }

bool Window::create(const WindowDesc& desc) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        error_ = std::string("SDL_Init(VIDEO) failed: ") + SDL_GetError();
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    if (desc.gl_debug) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    }

    Uint32 flags = SDL_WINDOW_OPENGL;
    if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;

    window_ = SDL_CreateWindow(desc.title.c_str(), SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, desc.width, desc.height, flags);
    if (!window_) {
        error_ = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }

    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        error_ = std::string("SDL_GL_CreateContext(4.5 core) failed: ") + SDL_GetError();
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0) {
        error_ = "gladLoadGL failed";
        destroy();
        return false;
    }

    width_ = desc.width;
    height_ = desc.height;
    SDL_GL_GetDrawableSize(window_, &width_, &height_);
    set_vsync(desc.vsync);

    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    gl_version_ = ver ? ver : "unknown";

    if (desc.gl_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(gl_debug_callback, nullptr);
    }

    IMMUNE_LOG_INFO("GL context created: %s (%dx%d)", gl_version_.c_str(), width_, height_);
    return true;
}

void Window::destroy() {
    if (gl_) {
        SDL_GL_DeleteContext(gl_);
        gl_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

void Window::set_vsync(bool enabled) {
    vsync_ = enabled;
    if (gl_) SDL_GL_SetSwapInterval(enabled ? 1 : 0);
}

void Window::swap() {
    if (window_) SDL_GL_SwapWindow(window_);
}

bool create_headless_gl(Window& out_window, i32 width, i32 height) {
    WindowDesc desc;
    desc.title = "IMMUNE (headless)";
    desc.width = width;
    desc.height = height;
    desc.resizable = false;
    desc.vsync = false;
    desc.hidden = true;
    return out_window.create(desc);
}

} // namespace immune::platform
