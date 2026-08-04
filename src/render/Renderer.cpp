// Wave 0 stub. Passes are owned by Wave 1C (core) and Wave 3D (VFX).
// begin_frame/end_frame are real so --screenshot produces a valid image today.
#include "render/Renderer.h"

#include "core/Log.h"
#include "core/Math.h"
#include "render/Camera.h"
#include "render/Screenshot.h"

#include <glad/glad.h>

namespace immune::render {

Vec4 family_color(PathogenFamily family) {
    // DESIGN.md §6 colour code. Single source of truth.
    switch (family) {
        case PathogenFamily::Virus:       return Vec4{0.72f, 0.20f, 0.62f, 1.0f}; // red-purple
        case PathogenFamily::Bacteria:    return Vec4{0.72f, 0.80f, 0.22f, 1.0f}; // yellow-green
        case PathogenFamily::FungalSpore: return Vec4{0.48f, 0.33f, 0.18f, 1.0f}; // brown
        case PathogenFamily::Parasite:    return Vec4{0.16f, 0.70f, 0.68f, 1.0f}; // teal
        case PathogenFamily::CancerCell:  return Vec4{0.70f, 0.55f, 0.58f, 1.0f}; // grey-pink
        case PathogenFamily::Allergen:    return Vec4{1.00f, 0.86f, 0.10f, 1.0f}; // warning yellow
        default:                          return Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    }
}

u32 pack_rgba8(Vec4 c) {
    const auto q = [](f32 v) { return static_cast<u32>(math::saturate(v) * 255.0f + 0.5f); };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
}

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(const RendererDesc& desc) {
    desc_ = desc;
    // Wave 1C: create DSA VAOs, persistently-mapped instance buffers, the blob
    // density texture, and the shader set.
    glViewport(0, 0, desc.framebuffer_width, desc.framebuffer_height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ready_ = true;
    error_.clear();
    return true;
}

void Renderer::shutdown() { ready_ = false; }

void Renderer::resize(i32 width, i32 height) {
    desc_.framebuffer_width = width;
    desc_.framebuffer_height = height;
    if (ready_) glViewport(0, 0, width, height);
}

void Renderer::begin_frame(const Camera&, f32) {
    stats_ = FrameStats{};
    // Host tissue substrate: warm, low-saturation (DESIGN.md §7 palette).
    glClearColor(0.129f, 0.086f, 0.106f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::submit_tissue(const sim::TissueMask&, const sim::DistanceField&, f32) {}

void Renderer::submit_chaff(const sim::ChaffBuffers&, const sim::SpatialHash&) {
    // Wave 1C: partition by SpatialHash occupancy into per-family instance
    // ranges vs blob density, then one instanced draw per family.
}

void Renderer::submit_entities(const sim::EcsWorld&) {}
void Renderer::submit_fields(const sim::DamageField*, usize) {}
void Renderer::submit_flow_debug(const sim::FlowField&) {}

void Renderer::end_frame() {
    glFinish();
}

bool Renderer::read_pixels(std::vector<u8>& out_rgba, i32& out_width, i32& out_height) const {
    out_width = desc_.framebuffer_width;
    out_height = desc_.framebuffer_height;
    return read_framebuffer_rgba(out_rgba, out_width, out_height);
}

void Renderer::poll_shader_reload() {}

} // namespace immune::render
