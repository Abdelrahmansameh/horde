// gui/backend/GlBackend.h — draws a gui::DrawList with OpenGL 4.5.
//
// One program (assets/shaders/gui.{vert,frag}), one vertex/index stream, the
// shape records as an SSBO, and two atlases bound for the whole frame (fonts
// on unit 0, icons on unit 1). Draw calls split only on a change of scissor,
// a stencil clip push/pop, or an offscreen layer.
//
// Runs after the world passes and before ImGui (dev tools stay on top). It
// leaves blend, scissor, stencil and the framebuffer binding as it found them
// so neither neighbour has to know it exists.
#pragma once

#include "core/Types.h"

#include <memory>
#include <string>

namespace immune::gui {

class DrawList;
class Atlas;

struct GuiFrameStats {
    u32 draw_calls = 0;
    u32 vertices = 0;
    u32 indices = 0;
    u32 shapes = 0;
    u32 layers = 0;
    u32 stencil_clips = 0;
};

class GlBackend {
public:
    GlBackend();
    ~GlBackend();
    GlBackend(const GlBackend&) = delete;
    GlBackend& operator=(const GlBackend&) = delete;

    /// Needs a current GL 4.5 context. False on failure; see error().
    bool init();
    void shutdown();
    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }

    /// Draws `dl` into the currently bound draw framebuffer of size
    /// `fb_width` x `fb_height`. `scale` is framebuffer px per logical px.
    /// `fonts` / `icons` may be null when the frame uses none.
    void render(const DrawList& dl, const Atlas* fonts, const Atlas* icons, i32 fb_width, i32 fb_height,
                f32 scale);

    /// Recompiles gui.vert/gui.frag if they changed on disk.
    void poll_shader_reload();

    const GuiFrameStats& stats() const { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
    std::string error_;
    GuiFrameStats stats_{};
};

} // namespace immune::gui
