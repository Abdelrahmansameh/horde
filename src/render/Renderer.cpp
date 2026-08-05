// render/Renderer.cpp — frame graph and draw submission. Owner: Wave 1C.
//
// See render/Renderer.h for the two facts that shape this file: one instanced
// draw call per PathogenFamily, and density-LOD crossfade instead of distance
// LOD. The CPU-side partition/crossfade math lives in render/ChaffBatcher.h
// (unit-tested with no GL context); this file is the GL half: persistently
// mapped, triple-buffered instance buffers, the density texture, and the
// shader passes that read them.
#include "render/Renderer.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/Math.h"
#include "render/Camera.h"
#include "render/ChaffBatcher.h"
#include "render/Gl.h"
#include "render/Screenshot.h"
#include "render/Shader.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstddef>
#include <vector>

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

namespace {

/// Simple position+colour vertex for the flow-field debug overlay. File-scope
/// so both `Renderer::init` (attribute format) and `submit_flow_debug` (data)
/// can name it.
struct FlowDebugVertex {
    Vec2 pos;
    Vec4 color;
};

/// Triple-buffered: the CPU may be writing region N+1 while the GPU is still
/// reading region N's draws from a couple of frames ago. Two would work for a
/// simple double-buffer; three gives slack against a hitch without meaningful
/// extra memory (a few MB at 10k agents).
constexpr u32 kInstanceRegions = 3;

} // namespace

struct Renderer::Impl {
    ShaderManager shaders;

    // Shared unit quad ([-0.5, 0.5]^2), reused by every quad-based pass.
    gl::Buffer quad_vbo;

    gl::VertexArray chaff_vao;
    gl::Buffer chaff_instances;
    gl::FenceRing<kInstanceRegions> chaff_fence;
    u32 chaff_region = 0;

    gl::VertexArray entity_vao;
    gl::Buffer entity_instances;
    gl::FenceRing<kInstanceRegions> entity_fence;
    u32 entity_region = 0;

    // Blob + tissue passes both just need the shared quad's position attrib.
    gl::VertexArray screen_quad_vao;

    gl::Texture2D density_tex;
    DensityGrid density_grid;

    gl::Texture2D tissue_sdf_tex;
    i32 tissue_tex_w = 0;
    i32 tissue_tex_h = 0;

    // Flow-field debug overlay: rebuilt CPU-side each call (F1 overlay only,
    // never in the hot path), uploaded into a plain dynamic-storage buffer.
    GLuint flow_vao = 0;
    GLuint flow_vbo = 0;
    usize flow_vbo_capacity_bytes = 0;

    WallClock clock;
    f32 time = 0.0f;

    // Cached from the most recent begin_frame().
    glm::mat4 view_projection{1.0f};
    Rect visible_bounds{};
    f32 alpha = 0.0f;
};

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

bool Renderer::init(const RendererDesc& desc) {
    desc_ = desc;
    error_.clear();
    impl_ = std::make_unique<Impl>();
    Impl& imp = *impl_;

    glViewport(0, 0, desc.framebuffer_width, desc.framebuffer_height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- Shared unit quad --------------------------------------------------
    const Vec2 quad_verts[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    if (!imp.quad_vbo.create_static(quad_verts, sizeof(quad_verts))) {
        error_ = "failed to create the shared unit-quad VBO";
        return false;
    }

    // ---- Chaff instanced pass ----------------------------------------------
    if (!imp.chaff_vao.create()) { error_ = "failed to create the chaff VAO"; return false; }
    imp.chaff_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.chaff_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);

    const usize chaff_bytes = static_cast<usize>(kFamilyCount) * desc.max_chaff_instances *
                              sizeof(ChaffInstance) * kInstanceRegions;
    if (!imp.chaff_instances.create_persistent(chaff_bytes)) {
        error_ = "failed to allocate the persistently-mapped chaff instance buffer";
        return false;
    }
    // Bound once at offset 0; per-family/per-region addressing happens via
    // glDrawArraysInstancedBaseInstance so no per-draw rebind is needed.
    imp.chaff_vao.bind_vertex_buffer(1, imp.chaff_instances, sizeof(ChaffInstance), 0, 1);
    imp.chaff_vao.attrib_float(1, 1, 2, GL_FLOAT, false, offsetof(ChaffInstance, x));
    imp.chaff_vao.attrib_float(2, 1, 1, GL_FLOAT, false, offsetof(ChaffInstance, scale));
    imp.chaff_vao.attrib_float(3, 1, 1, GL_FLOAT, false, offsetof(ChaffInstance, rotation));
    imp.chaff_vao.attrib_float(4, 1, 4, GL_UNSIGNED_BYTE, true, offsetof(ChaffInstance, tint_rgba8));
    imp.chaff_vao.attrib_int(5, 1, 1, GL_UNSIGNED_INT, offsetof(ChaffInstance, flags));
    imp.chaff_vao.attrib_float(6, 1, 1, GL_FLOAT, false, offsetof(ChaffInstance, anim_phase));
    imp.chaff_vao.attrib_float(7, 1, 1, GL_FLOAT, false, offsetof(ChaffInstance, pad));

    // ---- Entity instanced pass ---------------------------------------------
    if (!imp.entity_vao.create()) { error_ = "failed to create the entity VAO"; return false; }
    imp.entity_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.entity_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);

    const usize entity_bytes = static_cast<usize>(desc.max_entity_instances) *
                               sizeof(EntityInstance) * kInstanceRegions;
    if (!imp.entity_instances.create_persistent(entity_bytes)) {
        error_ = "failed to allocate the persistently-mapped entity instance buffer";
        return false;
    }
    imp.entity_vao.bind_vertex_buffer(1, imp.entity_instances, sizeof(EntityInstance), 0, 1);
    imp.entity_vao.attrib_float(1, 1, 2, GL_FLOAT, false, offsetof(EntityInstance, x));
    imp.entity_vao.attrib_float(2, 1, 1, GL_FLOAT, false, offsetof(EntityInstance, scale));
    imp.entity_vao.attrib_float(3, 1, 1, GL_FLOAT, false, offsetof(EntityInstance, rotation));
    imp.entity_vao.attrib_float(4, 1, 4, GL_UNSIGNED_BYTE, true, offsetof(EntityInstance, tint_rgba8));
    imp.entity_vao.attrib_int(5, 1, 1, GL_UNSIGNED_INT, offsetof(EntityInstance, shape_id));
    imp.entity_vao.attrib_float(6, 1, 1, GL_FLOAT, false, offsetof(EntityInstance, anim_phase));
    imp.entity_vao.attrib_float(7, 1, 1, GL_FLOAT, false, offsetof(EntityInstance, pad));

    // ---- Blob / tissue shared screen quad -----------------------------------
    if (!imp.screen_quad_vao.create()) { error_ = "failed to create the screen-quad VAO"; return false; }
    imp.screen_quad_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.screen_quad_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);

    // ---- Density-LOD texture -------------------------------------------------
    imp.density_grid.configure(desc.density_texture_width, desc.density_texture_height);
    if (!imp.density_tex.create(desc.density_texture_width, desc.density_texture_height,
                                GL_RGBA32F, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE)) {
        error_ = "failed to create the density-LOD texture";
        return false;
    }

    // ---- Flow-field debug overlay VAO (buffer allocated lazily) -----------
    glCreateVertexArrays(1, &imp.flow_vao);
    glEnableVertexArrayAttrib(imp.flow_vao, 0);
    glVertexArrayAttribFormat(imp.flow_vao, 0, 2, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(FlowDebugVertex, pos)));
    glVertexArrayAttribBinding(imp.flow_vao, 0, 0);
    glEnableVertexArrayAttrib(imp.flow_vao, 1);
    glVertexArrayAttribFormat(imp.flow_vao, 1, 4, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(offsetof(FlowDebugVertex, color)));
    glVertexArrayAttribBinding(imp.flow_vao, 1, 0);

    // ---- Shaders --------------------------------------------------------
    // Since this project ships no binary assets, shader source is the only
    // on-disk art: a load failure is logged loudly but does not fail init —
    // the affected pass just draws nothing until the source is fixed and hot
    // reload picks it up (or the process is restarted).
    imp.shaders.load_graphics("chaff", "chaff.vert", "chaff.frag");
    imp.shaders.load_graphics("blob", "blob.vert", "blob.frag");
    imp.shaders.load_graphics("tissue", "tissue.vert", "tissue.frag");
    imp.shaders.load_graphics("entity", "entity.vert", "entity.frag");
    imp.shaders.load_graphics("flow_debug", "flow_debug.vert", "flow_debug.frag");
    if (!imp.shaders.get("chaff").valid()) {
        IMMUNE_LOG_ERROR("chaff shader failed to load: %s", imp.shaders.last_error().c_str());
    }

    imp.clock.reset();
    imp.time = 0.0f;

    ready_ = true;
    return true;
}

void Renderer::shutdown() {
    if (impl_) {
        if (impl_->flow_vbo != 0) glDeleteBuffers(1, &impl_->flow_vbo);
        if (impl_->flow_vao != 0) glDeleteVertexArrays(1, &impl_->flow_vao);
    }
    impl_.reset();
    ready_ = false;
}

void Renderer::resize(i32 width, i32 height) {
    desc_.framebuffer_width = width;
    desc_.framebuffer_height = height;
    if (ready_) glViewport(0, 0, width, height);
}

void Renderer::begin_frame(const Camera& camera, f32 alpha) {
    stats_ = FrameStats{};
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;

    imp.view_projection = camera.view_projection();
    imp.visible_bounds = camera.visible_bounds();
    imp.alpha = alpha;
    imp.time = static_cast<f32>(imp.clock.elapsed_seconds());

    // Host tissue substrate base colour (DESIGN.md §7 palette). submit_tissue
    // draws a proper vessel-shaped quad over this; the clear is the fallback
    // for anything the tissue quad doesn't cover (and for Wave-0-era callers
    // that never call submit_tissue at all).
    glClearColor(0.129f, 0.086f, 0.106f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::submit_tissue(const sim::TissueMask& mask, const sim::DistanceField& sdf,
                             f32 heartbeat_phase) {
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;
    const ShaderProgram prog = imp.shaders.get("tissue");
    if (!prog.valid()) return;

    const i32 w = sdf.width();
    const i32 h = sdf.height();
    if (w <= 0 || h <= 0) return;

    if (imp.tissue_tex_w != w || imp.tissue_tex_h != h) {
        if (!imp.tissue_sdf_tex.create(w, h, GL_R32F, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE)) return;
        imp.tissue_tex_w = w;
        imp.tissue_tex_h = h;
    }
    imp.tissue_sdf_tex.upload(sdf.data(), GL_RED, GL_FLOAT);

    glUseProgram(prog.gl_id);
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
    const Rect vis = imp.visible_bounds;
    glUniform2f(1, vis.min.x, vis.min.y);
    glUniform2f(2, vis.size().x, vis.size().y);
    const Rect mb = mask.world_bounds();
    glUniform2f(3, mb.min.x, mb.min.y);
    glUniform2f(4, mb.size().x, mb.size().y);
    glUniform1f(5, heartbeat_phase);

    imp.tissue_sdf_tex.bind_unit(0);
    imp.screen_quad_vao.bind();
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    ++stats_.draw_calls;
}

void Renderer::submit_chaff(const sim::ChaffBuffers& chaff, const sim::SpatialHash& hash) {
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;
    WallClock timer;

    // Wait for the GPU to finish reading the region we're about to overwrite
    // before touching it from the CPU — the whole point of persistent+coherent
    // mapping is that this is the *only* synchronisation needed.
    imp.chaff_fence.wait(imp.chaff_region);

    const u32 per_family_cap = desc_.max_chaff_instances;
    ChaffInstance* region_base = imp.chaff_instances.mapped_as<ChaffInstance>() +
        static_cast<usize>(imp.chaff_region) * kFamilyCount * per_family_cap;

    OccupancyGrid occ;
    occ.bounds = hash.bounds();
    occ.cell_size = hash.cell_size();
    occ.dims = hash.grid_dims();
    occ.occupancy = hash.occupancy();

    ChaffBatchParams params;
    params.lod_blob_threshold = desc_.lod_blob_threshold;
    params.lod_blob_full = desc_.lod_blob_full;
    params.per_family_capacity = per_family_cap;
    params.time = imp.time;
    params.cull_enabled = false;

    imp.density_grid.set_extent(imp.visible_bounds);
    imp.density_grid.clear();

    const ChaffBatchResult result =
        build_chaff_batches(chaff, occ, params, region_base, &imp.density_grid);

    if (result.instances_dropped > 0) {
        IMMUNE_LOG_WARN("chaff render: dropped %u instances (a family exceeded "
                        "max_chaff_instances=%u)",
                        result.instances_dropped, per_family_cap);
    }

    imp.density_tex.upload(imp.density_grid.data(), GL_RGBA, GL_FLOAT);

    const ShaderProgram chaff_prog = imp.shaders.get("chaff");
    if (chaff_prog.valid() && result.instances_total > 0) {
        glUseProgram(chaff_prog.gl_id);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
        glUniform1f(1, imp.time);
        imp.chaff_vao.bind();
        const u32 region_base_instance = imp.chaff_region * kFamilyCount * per_family_cap;
        for (u32 f = 0; f < kFamilyCount; ++f) {
            const u32 n = result.family_counts[f];
            if (n == 0) continue;
            const u32 base_instance = region_base_instance + f * per_family_cap;
            glDrawArraysInstancedBaseInstance(GL_TRIANGLE_FAN, 0, 4, static_cast<GLsizei>(n),
                                              base_instance);
            ++stats_.draw_calls;
        }
    }

    // Density-LOD blob pass: one fullscreen-ish draw, skipped entirely when
    // nothing crossed into the crossfade band this frame.
    const ShaderProgram blob_prog = imp.shaders.get("blob");
    if (blob_prog.valid() && result.blob_mass > 0.0f) {
        glUseProgram(blob_prog.gl_id);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
        const Rect vis = imp.visible_bounds;
        glUniform2f(1, vis.min.x, vis.min.y);
        glUniform2f(2, vis.size().x, vis.size().y);
        const f32 mass_scale = 1.0f / math::max(1.0f, static_cast<f32>(desc_.lod_blob_threshold) * 0.6f);
        glUniform1f(3, mass_scale);
        imp.density_tex.bind_unit(0);
        imp.screen_quad_vao.bind();
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        ++stats_.draw_calls;
    }

    imp.chaff_fence.signal(imp.chaff_region);
    imp.chaff_region = (imp.chaff_region + 1) % kInstanceRegions;

    stats_.chaff_instances_drawn = result.instances_total;
    stats_.chaff_agents_in_blobs = result.agents_in_blobs;
    stats_.submit_ms += timer.elapsed_ms();
}

void Renderer::submit_entities(const sim::EcsWorld& ecs) {
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;
    WallClock timer;

    imp.entity_fence.wait(imp.entity_region);
    const u32 cap = desc_.max_entity_instances;
    EntityInstance* region_base = imp.entity_instances.mapped_as<EntityInstance>() +
        static_cast<usize>(imp.entity_region) * cap;

    const entt::registry& registry = ecs.registry();
    u32 count = 0;
    auto view = registry.view<const sim::comp::Transform, const sim::comp::Sprite>();
    for (auto entity : view) {
        if (count >= cap) break;
        const auto& t = view.get<const sim::comp::Transform>(entity);
        const auto& sp = view.get<const sim::comp::Sprite>(entity);

        f32 tempo = 1.0f;
        if (const auto* na = registry.try_get<const sim::comp::NamedAgent>(entity)) {
            tempo = family_visual(na->family).tempo;
        }

        EntityInstance& inst = region_base[count++];
        inst.x = t.position.x;
        inst.y = t.position.y;
        inst.scale = t.scale * sp.size;
        inst.rotation = t.rotation;
        inst.tint_rgba8 = pack_rgba8(sp.tint);
        inst.shape_id = sp.atlas_index;
        // Same "don't pulse in lockstep" trick as the chaff batcher, keyed off
        // the entity id since named agents have no generation counter exposed.
        const u32 h = static_cast<u32>(entt::to_integral(entity)) * 2654435761u;
        inst.anim_phase =
            static_cast<f32>(h & 0xFFFFu) * (math::kTwoPi / 65536.0f) + imp.time * tempo * math::kTwoPi;
        inst.pad = 0.0f;
    }

    const ShaderProgram prog = imp.shaders.get("entity");
    if (prog.valid() && count > 0) {
        glUseProgram(prog.gl_id);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
        imp.entity_vao.bind();
        const u32 base_instance = imp.entity_region * cap;
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_FAN, 0, 4, static_cast<GLsizei>(count),
                                          base_instance);
        ++stats_.draw_calls;
    }

    imp.entity_fence.signal(imp.entity_region);
    imp.entity_region = (imp.entity_region + 1) % kInstanceRegions;

    stats_.entity_instances_drawn = count;
    stats_.submit_ms += timer.elapsed_ms();
}

void Renderer::submit_fields(const sim::DamageField* fields, usize count) {
    // Wave 3D owns the actual toxin-cloud / histamine-bloom / antibody-tide
    // shaders (DESIGN.md §7's field VFX). This wave only plumbs the count
    // through so FrameStats and the HUD have a real number before that pass
    // exists, per the brief: "build the pass plumbing, do not implement the
    // VFX themselves."
    (void)fields;
    if (!ready_) return;
    stats_.vfx_fields_drawn = static_cast<u32>(count);
}

void Renderer::submit_flow_debug(const sim::FlowField& flow) {
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;
    const ShaderProgram prog = imp.shaders.get("flow_debug");
    if (!prog.valid()) return;

    std::vector<FlowDebugVertex> verts;
    const Rect vis = imp.visible_bounds;
    const f32 step = math::max(flow.cell_size() * 3.0f, 1.0f);
    const f32 arrow_len = step * 0.4f;
    const Vec4 color{0.55f, 0.95f, 1.0f, 0.65f};

    for (f32 y = vis.min.y; y <= vis.max.y; y += step) {
        for (f32 x = vis.min.x; x <= vis.max.x; x += step) {
            const Vec2 p{x, y};
            const Vec2 dir = flow.sample(p);
            if (math::length_sq(dir) < 1e-6f) continue;
            const Vec2 tip = p + dir * arrow_len;
            verts.push_back(FlowDebugVertex{p, color});
            verts.push_back(FlowDebugVertex{tip, color});
            const Vec2 perp{-dir.y, dir.x};
            const Vec2 back = tip - dir * (arrow_len * 0.35f);
            verts.push_back(FlowDebugVertex{tip, color});
            verts.push_back(FlowDebugVertex{back + perp * (arrow_len * 0.2f), color});
        }
    }
    if (verts.empty()) return;

    const usize bytes = verts.size() * sizeof(FlowDebugVertex);
    if (bytes > imp.flow_vbo_capacity_bytes) {
        if (imp.flow_vbo != 0) glDeleteBuffers(1, &imp.flow_vbo);
        glCreateBuffers(1, &imp.flow_vbo);
        glNamedBufferStorage(imp.flow_vbo, static_cast<GLsizeiptr>(bytes), nullptr,
                             GL_DYNAMIC_STORAGE_BIT);
        imp.flow_vbo_capacity_bytes = bytes;
        glVertexArrayVertexBuffer(imp.flow_vao, 0, imp.flow_vbo, 0, sizeof(FlowDebugVertex));
    }
    glNamedBufferSubData(imp.flow_vbo, 0, static_cast<GLsizeiptr>(bytes), verts.data());

    glUseProgram(prog.gl_id);
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
    glBindVertexArray(imp.flow_vao);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size()));
    ++stats_.draw_calls;
}

void Renderer::end_frame() {
    // A full CPU/GPU sync point. Screenshot correctness only needs command
    // ordering (glReadPixels already waits for prior draws in the same
    // context), but forcing completion here keeps `--bench`/`--screenshot`
    // timings honest across runs and avoids ever reading a torn frame.
    glFinish();
}

bool Renderer::read_pixels(std::vector<u8>& out_rgba, i32& out_width, i32& out_height) const {
    out_width = desc_.framebuffer_width;
    out_height = desc_.framebuffer_height;
    return read_framebuffer_rgba(out_rgba, out_width, out_height);
}

void Renderer::poll_shader_reload() {
    if (!ready_ || !impl_ || !desc_.hot_reload_shaders) return;
    impl_->shaders.poll_reload();
}

} // namespace immune::render
