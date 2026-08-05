// render/Gl.h — thin RAII wrappers over GL 4.5 direct-state-access objects.
// Owner: Wave 1C. Header-only on purpose: the module's .cpp list lives in the
// shared root CMakeLists.txt, which this wave does not own.
//
// These are deliberately not an abstraction layer. They are move-only handles
// that (a) guarantee deletion, and (b) force every call site through the DSA
// entry points, so no pass can accidentally rely on the bind-to-edit global
// state that the rest of the renderer assumes is untouched.
#pragma once

#include "core/Types.h"

#include <glad/glad.h>

#include <utility>

namespace immune::render::gl {

/// Move-only GL name holder. `Deleter` is a functor calling the right glDelete*.
template <typename Deleter>
class Handle {
public:
    Handle() = default;
    explicit Handle(GLuint id) : id_(id) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& o) noexcept : id_(o.id_) { o.id_ = 0; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { reset(); id_ = o.id_; o.id_ = 0; }
        return *this;
    }

    GLuint id() const { return id_; }
    bool valid() const { return id_ != 0; }
    void reset(GLuint id = 0) {
        if (id_ != 0) Deleter{}(id_);
        id_ = id;
    }
    GLuint release() { const GLuint v = id_; id_ = 0; return v; }

private:
    GLuint id_ = 0;
};

struct DeleteBuffer      { void operator()(GLuint id) const { glDeleteBuffers(1, &id); } };
struct DeleteTexture     { void operator()(GLuint id) const { glDeleteTextures(1, &id); } };
struct DeleteVertexArray { void operator()(GLuint id) const { glDeleteVertexArrays(1, &id); } };
struct DeleteFramebuffer { void operator()(GLuint id) const { glDeleteFramebuffers(1, &id); } };
struct DeleteRenderbuffer{ void operator()(GLuint id) const { glDeleteRenderbuffers(1, &id); } };

using TextureHandle      = Handle<DeleteTexture>;
using VertexArrayHandle  = Handle<DeleteVertexArray>;
using FramebufferHandle  = Handle<DeleteFramebuffer>;
using RenderbufferHandle = Handle<DeleteRenderbuffer>;

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------

/// An immutable-storage buffer, optionally persistently mapped.
///
/// The chaff instance buffer is persistent + coherent and N-buffered: the CPU
/// writes region `frame % regions` while the GPU may still be reading the
/// previous one. That is the whole reason this class exists — a per-frame
/// glBufferSubData of 320 KB would serialize the driver against the last draw.
class Buffer {
public:
    Buffer() = default;
    ~Buffer() { destroy(); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& o) noexcept { swap(o); }
    Buffer& operator=(Buffer&& o) noexcept { if (this != &o) { destroy(); swap(o); } return *this; }

    /// Immutable static storage, uploaded once.
    bool create_static(const void* data, usize bytes) {
        destroy();
        if (bytes == 0) return false;
        glCreateBuffers(1, &id_);
        if (id_ == 0) return false;
        glNamedBufferStorage(id_, static_cast<GLsizeiptr>(bytes), data, 0);
        bytes_ = bytes;
        return true;
    }

    /// Persistently mapped, coherent, write-only-from-CPU storage.
    bool create_persistent(usize bytes) {
        destroy();
        if (bytes == 0) return false;
        glCreateBuffers(1, &id_);
        if (id_ == 0) return false;
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glNamedBufferStorage(id_, static_cast<GLsizeiptr>(bytes), nullptr, flags);
        mapped_ = glMapNamedBufferRange(id_, 0, static_cast<GLsizeiptr>(bytes), flags);
        bytes_ = bytes;
        return mapped_ != nullptr;
    }

    void destroy() {
        if (id_ != 0) {
            if (mapped_ != nullptr) glUnmapNamedBuffer(id_);
            glDeleteBuffers(1, &id_);
        }
        id_ = 0;
        mapped_ = nullptr;
        bytes_ = 0;
    }

    GLuint id() const { return id_; }
    bool valid() const { return id_ != 0; }
    usize bytes() const { return bytes_; }
    void* mapped() const { return mapped_; }

    template <typename T>
    T* mapped_as() const { return static_cast<T*>(mapped_); }

private:
    void swap(Buffer& o) {
        std::swap(id_, o.id_);
        std::swap(mapped_, o.mapped_);
        std::swap(bytes_, o.bytes_);
    }
    GLuint id_ = 0;
    void* mapped_ = nullptr;
    usize bytes_ = 0;
};

// ---------------------------------------------------------------------------
// Fence ring — one sync object per in-flight buffer region.
// ---------------------------------------------------------------------------

/// Guards an N-buffered persistent mapping. `wait(i)` blocks until the GPU is
/// done with region `i`; `signal(i)` is called after the draws that read it.
template <u32 N>
class FenceRing {
public:
    ~FenceRing() { clear(); }

    void wait(u32 slot) {
        GLsync& s = sync_[slot % N];
        if (s == nullptr) return;
        // A 1 s cap: a longer stall means something is very wrong, and hanging
        // a headless screenshot run forever is worse than a torn frame.
        while (true) {
            const GLenum r = glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED || r == GL_WAIT_FAILED) break;
            break;
        }
        glDeleteSync(s);
        s = nullptr;
    }

    void signal(u32 slot) {
        GLsync& s = sync_[slot % N];
        if (s != nullptr) glDeleteSync(s);
        s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }

    void clear() {
        for (GLsync& s : sync_) {
            if (s != nullptr) glDeleteSync(s);
            s = nullptr;
        }
    }

    static constexpr u32 kRegions = N;

private:
    GLsync sync_[N] = {};
};

// ---------------------------------------------------------------------------
// Texture2D
// ---------------------------------------------------------------------------

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D() { destroy(); }
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    bool create(i32 width, i32 height, GLenum internal_format,
                GLenum min_filter = GL_LINEAR, GLenum mag_filter = GL_LINEAR,
                GLenum wrap = GL_CLAMP_TO_EDGE) {
        destroy();
        if (width <= 0 || height <= 0) return false;
        glCreateTextures(GL_TEXTURE_2D, 1, &id_);
        if (id_ == 0) return false;
        glTextureStorage2D(id_, 1, internal_format, width, height);
        glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min_filter));
        glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(mag_filter));
        glTextureParameteri(id_, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap));
        glTextureParameteri(id_, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap));
        width_ = width;
        height_ = height;
        return true;
    }

    void upload(const void* data, GLenum format, GLenum type) {
        if (id_ == 0 || data == nullptr) return;
        glTextureSubImage2D(id_, 0, 0, 0, width_, height_, format, type, data);
    }

    void bind_unit(u32 unit) const { if (id_ != 0) glBindTextureUnit(unit, id_); }

    void destroy() {
        if (id_ != 0) glDeleteTextures(1, &id_);
        id_ = 0;
        width_ = height_ = 0;
    }

    GLuint id() const { return id_; }
    bool valid() const { return id_ != 0; }
    i32 width() const { return width_; }
    i32 height() const { return height_; }

private:
    GLuint id_ = 0;
    i32 width_ = 0;
    i32 height_ = 0;
};

// ---------------------------------------------------------------------------
// Framebuffer with a single colour attachment (the HDR scene target).
// ---------------------------------------------------------------------------

class ColorTarget {
public:
    ColorTarget() = default;
    ~ColorTarget() { destroy(); }
    ColorTarget(const ColorTarget&) = delete;
    ColorTarget& operator=(const ColorTarget&) = delete;

    bool create(i32 width, i32 height, GLenum internal_format = GL_RGBA16F) {
        destroy();
        if (!color_.create(width, height, internal_format)) return false;
        glCreateFramebuffers(1, &fbo_);
        if (fbo_ == 0) return false;
        glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0, color_.id(), 0);
        const GLenum draw_buf = GL_COLOR_ATTACHMENT0;
        glNamedFramebufferDrawBuffers(fbo_, 1, &draw_buf);
        return glCheckNamedFramebufferStatus(fbo_, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    void destroy() {
        if (fbo_ != 0) glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
        color_.destroy();
    }

    void bind() const { glBindFramebuffer(GL_FRAMEBUFFER, fbo_); }
    static void bind_default() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

    GLuint fbo() const { return fbo_; }
    const Texture2D& color() const { return color_; }
    bool valid() const { return fbo_ != 0 && color_.valid(); }

private:
    GLuint fbo_ = 0;
    Texture2D color_;
};

// ---------------------------------------------------------------------------
// VertexArray
// ---------------------------------------------------------------------------

class VertexArray {
public:
    VertexArray() = default;
    ~VertexArray() { destroy(); }
    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;

    bool create() {
        destroy();
        glCreateVertexArrays(1, &id_);
        return id_ != 0;
    }

    void destroy() {
        if (id_ != 0) glDeleteVertexArrays(1, &id_);
        id_ = 0;
    }

    void bind_vertex_buffer(u32 binding, const Buffer& buf, i32 stride, i32 offset = 0,
                            u32 divisor = 0) const {
        glVertexArrayVertexBuffer(id_, binding, buf.id(), offset, stride);
        glVertexArrayBindingDivisor(id_, binding, divisor);
    }

    /// Float attribute (optionally normalized from an integer source type).
    void attrib_float(u32 location, u32 binding, i32 components, GLenum type,
                      bool normalized, u32 relative_offset) const {
        glEnableVertexArrayAttrib(id_, location);
        glVertexArrayAttribFormat(id_, location, components, type,
                                  normalized ? GL_TRUE : GL_FALSE, relative_offset);
        glVertexArrayAttribBinding(id_, location, binding);
    }

    /// Integer attribute, delivered to the shader as int/uint without conversion.
    void attrib_int(u32 location, u32 binding, i32 components, GLenum type,
                    u32 relative_offset) const {
        glEnableVertexArrayAttrib(id_, location);
        glVertexArrayAttribIFormat(id_, location, components, type, relative_offset);
        glVertexArrayAttribBinding(id_, location, binding);
    }

    void bind() const { glBindVertexArray(id_); }
    GLuint id() const { return id_; }
    bool valid() const { return id_ != 0; }

private:
    GLuint id_ = 0;
};

} // namespace immune::render::gl
