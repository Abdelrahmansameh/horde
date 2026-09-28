#include "gui/backend/GlBackend.h"

#include "core/Math.h"
#include "gui/Atlas.h"
#include "gui/draw/DrawList.h"
#include "render/Gl.h"
#include "render/Shader.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace immune::gui {

namespace gl = render::gl;

namespace {

/// A mutable buffer re-specified each frame (orphaned), sized up as needed.
struct StreamBuffer {
    GLuint id = 0;
    usize capacity = 0;
    void upload(const void* data, usize bytes) {
        if (id == 0) glCreateBuffers(1, &id);
        if (bytes > capacity) {
            capacity = math::max<usize>(bytes, capacity * 2);
            capacity = math::max<usize>(capacity, 4096);
        }
        glNamedBufferData(id, static_cast<GLsizeiptr>(capacity), nullptr, GL_STREAM_DRAW);
        if (bytes > 0) glNamedBufferSubData(id, 0, static_cast<GLsizeiptr>(bytes), data);
    }
    void destroy() {
        if (id != 0) glDeleteBuffers(1, &id);
        id = 0;
        capacity = 0;
    }
};

/// A GPU mirror of a CPU Atlas, updated from its dirty rectangle.
struct AtlasTexture {
    gl::Texture2D tex;
    u32 generation = 0;
    const Atlas* source = nullptr;

    void sync(const Atlas& a) {
        const GLenum internal = a.channels() == 1 ? GL_R8 : GL_RGBA8;
        const GLenum format = a.channels() == 1 ? GL_RED : GL_RGBA;
        if (!tex.valid() || source != &a || generation != a.generation() || tex.width() != a.width() ||
            tex.height() != a.height()) {
            tex.create(a.width(), a.height(), internal, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            tex.upload(a.pixels(), format, GL_UNSIGNED_BYTE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            generation = a.generation();
            source = &a;
            const_cast<Atlas&>(a).clear_dirty();
            return;
        }
        const Atlas::Dirty& d = a.dirty();
        if (d.empty()) return;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, a.width());
        const u8* start = a.pixels() + (static_cast<usize>(d.y0) * static_cast<usize>(a.width()) +
                                        static_cast<usize>(d.x0)) * static_cast<usize>(a.channels());
        glTextureSubImage2D(tex.id(), 0, d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0, format, GL_UNSIGNED_BYTE, start);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        const_cast<Atlas&>(a).clear_dirty();
    }
};

/// One offscreen layer: colour plus its own stencil for clips inside it.
struct LayerTarget {
    GLuint fbo = 0;
    gl::Texture2D color;
    GLuint depth_stencil = 0;
    i32 width = 0, height = 0;

    bool ensure(i32 w, i32 h) {
        if (fbo != 0 && width == w && height == h) return true;
        destroy();
        if (!color.create(w, h, GL_RGBA8)) return false;
        glCreateRenderbuffers(1, &depth_stencil);
        glNamedRenderbufferStorage(depth_stencil, GL_DEPTH24_STENCIL8, w, h);
        glCreateFramebuffers(1, &fbo);
        glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, color.id(), 0);
        glNamedFramebufferRenderbuffer(fbo, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_stencil);
        width = w;
        height = h;
        return glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    void destroy() {
        if (fbo != 0) glDeleteFramebuffers(1, &fbo);
        if (depth_stencil != 0) glDeleteRenderbuffers(1, &depth_stencil);
        fbo = depth_stencil = 0;
        color.destroy();
        width = height = 0;
    }
};

} // namespace

struct GlBackend::Impl {
    render::ShaderManager shaders;
    render::ShaderProgram program;
    gl::VertexArray vao;
    StreamBuffer vbo, ibo, ssbo;
    AtlasTexture font_tex, icon_tex;
    gl::Texture2D blank;  // bound when a frame has no atlas
    std::vector<std::unique_ptr<LayerTarget>> layers;
};

GlBackend::GlBackend() = default;
GlBackend::~GlBackend() { shutdown(); }

bool GlBackend::init() {
    impl_ = std::make_unique<Impl>();
    impl_->program = impl_->shaders.load_graphics("gui", "gui.vert", "gui.frag");
    if (!impl_->program.valid()) {
        error_ = "gui shader: " + impl_->shaders.last_error();
        impl_.reset();
        return false;
    }
    if (!impl_->vao.create()) {
        error_ = "gui: cannot create vertex array";
        impl_.reset();
        return false;
    }
    const GLuint vao = impl_->vao.id();
    glEnableVertexArrayAttrib(vao, 0);
    glVertexArrayAttribFormat(vao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, pos));
    glVertexArrayAttribBinding(vao, 0, 0);
    glEnableVertexArrayAttrib(vao, 1);
    glVertexArrayAttribFormat(vao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(vao, 1, 0);
    glEnableVertexArrayAttrib(vao, 2);
    glVertexArrayAttribFormat(vao, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(Vertex, color));
    glVertexArrayAttribBinding(vao, 2, 0);
    glEnableVertexArrayAttrib(vao, 3);
    glVertexArrayAttribIFormat(vao, 3, 1, GL_UNSIGNED_INT, offsetof(Vertex, mode_record));
    glVertexArrayAttribBinding(vao, 3, 0);

    impl_->blank.create(1, 1, GL_RGBA8);
    const u32 zero = 0;
    impl_->blank.upload(&zero, GL_RGBA, GL_UNSIGNED_BYTE);
    ready_ = true;
    error_.clear();
    return true;
}

void GlBackend::shutdown() {
    if (!impl_) return;
    impl_->vbo.destroy();
    impl_->ibo.destroy();
    impl_->ssbo.destroy();
    for (auto& l : impl_->layers) l->destroy();
    impl_.reset();
    ready_ = false;
}

void GlBackend::poll_shader_reload() {
    if (!impl_) return;
    if (impl_->shaders.poll_reload() > 0) impl_->program = impl_->shaders.get("gui");
}

void GlBackend::render(const DrawList& dl, const Atlas* fonts, const Atlas* icons, i32 fb_width,
                       i32 fb_height, f32 scale) {
    stats_ = GuiFrameStats{};
    if (!ready_ || dl.empty() || fb_width <= 0 || fb_height <= 0) return;
    Impl& im = *impl_;

    // ---- Upload -----------------------------------------------------------
    im.vbo.upload(dl.vertices().data(), dl.vertices().size() * sizeof(Vertex));
    im.ibo.upload(dl.indices().data(), dl.indices().size() * sizeof(u32));
    // An SSBO binding may not be empty; keep at least one record.
    static const ShapeRecord kDummy{};
    if (dl.records().empty()) im.ssbo.upload(&kDummy, sizeof(ShapeRecord));
    else im.ssbo.upload(dl.records().data(), dl.records().size() * sizeof(ShapeRecord));
    if (fonts != nullptr) im.font_tex.sync(*fonts);
    if (icons != nullptr) im.icon_tex.sync(*icons);

    stats_.vertices = static_cast<u32>(dl.vertices().size());
    stats_.indices = static_cast<u32>(dl.indices().size());
    stats_.shapes = static_cast<u32>(dl.records().size());

    // ---- Save the state we touch ---------------------------------------------
    GLint prev_fbo = 0, prev_viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    const GLboolean prev_blend = glIsEnabled(GL_BLEND);
    const GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
    GLint prev_src_rgb = 0, prev_dst_rgb = 0, prev_src_a = 0, prev_dst_a = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &prev_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prev_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_dst_a);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);

    glUseProgram(im.program.gl_id);
    im.vao.bind();
    glVertexArrayVertexBuffer(im.vao.id(), 0, im.vbo.id, 0, sizeof(Vertex));
    glVertexArrayElementBuffer(im.vao.id(), im.ibo.id);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, im.ssbo.id);
    if (fonts != nullptr) im.font_tex.tex.bind_unit(0); else im.blank.bind_unit(0);
    if (icons != nullptr) im.icon_tex.tex.bind_unit(1); else im.blank.bind_unit(1);
    im.blank.bind_unit(2);

    const f32 sx = 2.0f * scale / static_cast<f32>(fb_width);
    const f32 sy = -2.0f * scale / static_cast<f32>(fb_height);
    glUniform4f(0, sx, sy, -1.0f, 1.0f);
    glUniform1f(1, dl.time());
    glUniform1f(2, 1.0f / scale);
    glUniform1i(3, 0);
    glUniform2f(4, fonts != nullptr ? static_cast<f32>(fonts->width()) : 1.0f,
                fonts != nullptr ? static_cast<f32>(fonts->height()) : 1.0f);

    // Layer 0 is whatever framebuffer the caller bound.
    while (im.layers.size() < dl.max_layer_depth() + 1) im.layers.push_back(std::make_unique<LayerTarget>());
    auto target_fbo = [&](u32 depth) -> GLuint {
        return depth == 0 ? static_cast<GLuint>(prev_fbo) : im.layers[depth]->fbo;
    };
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo(0));
    glViewport(0, 0, fb_width, fb_height);
    // The stencil is the clip stack; it has to start empty.
    bool any_stencil = false;
    for (const DrawCmd& c : dl.commands()) any_stencil |= c.kind == CmdKind::StencilPush;
    if (any_stencil) {
        glDisable(GL_SCISSOR_TEST);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
    }

    auto set_scissor = [&](const Rect& r) {
        const i32 x0 = static_cast<i32>(std::floor(r.min.x * scale));
        const i32 x1 = static_cast<i32>(std::ceil(r.max.x * scale));
        const i32 y0 = static_cast<i32>(std::floor(r.min.y * scale));
        const i32 y1 = static_cast<i32>(std::ceil(r.max.y * scale));
        glScissor(x0, fb_height - y1, math::max(x1 - x0, 0), math::max(y1 - y0, 0));
    };
    auto draw_range = [&](const DrawCmd& c) {
        if (c.index_count == 0) return;
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(c.index_count), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(static_cast<usize>(c.first_index) * sizeof(u32)));
        stats_.draw_calls += 1;
    };

    for (const DrawCmd& c : dl.commands()) {
        switch (c.kind) {
            case CmdKind::Draw:
                set_scissor(c.clip);
                glStencilFunc(GL_EQUAL, static_cast<GLint>(c.stencil_depth), 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                draw_range(c);
                break;
            case CmdKind::StencilPush:
            case CmdKind::StencilPop:
                set_scissor(c.clip);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glUniform1i(3, 1);
                if (c.kind == CmdKind::StencilPush) {
                    glStencilFunc(GL_EQUAL, static_cast<GLint>(c.stencil_depth), 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
                    stats_.stencil_clips += 1;
                } else {
                    glStencilFunc(GL_EQUAL, static_cast<GLint>(c.stencil_depth), 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
                }
                draw_range(c);
                glUniform1i(3, 0);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                break;
            case CmdKind::LayerBegin: {
                LayerTarget& l = *im.layers[c.layer_depth];
                if (!l.ensure(fb_width, fb_height)) break;
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, l.fbo);
                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClearStencil(0);
                glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                glEnable(GL_SCISSOR_TEST);
                stats_.layers += 1;
                break;
            }
            case CmdKind::LayerEnd: {
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo(c.layer_depth - 1));
                im.layers[c.layer_depth]->color.bind_unit(2);
                set_scissor(c.clip);
                glStencilFunc(GL_EQUAL, static_cast<GLint>(c.stencil_depth), 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                draw_range(c);
                im.blank.bind_unit(2);
                break;
            }
        }
    }

    // ---- Restore ----------------------------------------------------------
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindVertexArray(0);
    glUseProgram(0);
    glBlendFuncSeparate(static_cast<GLenum>(prev_src_rgb), static_cast<GLenum>(prev_dst_rgb),
                        static_cast<GLenum>(prev_src_a), static_cast<GLenum>(prev_dst_a));
    if (!prev_blend) glDisable(GL_BLEND);
    if (!prev_scissor) glDisable(GL_SCISSOR_TEST);
    if (!prev_stencil) glDisable(GL_STENCIL_TEST);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
}

} // namespace immune::gui
