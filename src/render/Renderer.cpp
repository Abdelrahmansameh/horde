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
#include "sim/ecs/NamedAgents.h"
#include "sim/flowfield/FlowField.h"
#include "sim/projectile/Projectiles.h"
#include "sim/spatial/SpatialHash.h"
#include "vfx/Particles.h"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
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

/// Per-instance data for the field-VFX pass (Wave 4G). NOT a shared contract —
/// mirrored only in assets/shaders/field.vert, since no other consumer reads
/// it (unlike ChaffInstance/EntityInstance, this struct is a private
/// implementation detail of this .cpp).
///
/// Shape-specific field meaning (see field.vert's header comment):
///   Circle/Chain: scale = diameter, rotation = 0.
///   Rect:         scale = full width/height, rotation = 0 (sim::Rect is
///                 always axis-aligned).
///   Cone:         scale = diameter, rotation = direction angle, arc_cos =
///                 cos(arc_radians).
struct FieldGpuInstance {
    f32 x, y;
    f32 scale_x, scale_y;
    f32 rotation;
    f32 arc_cos;
    f32 falloff;
    f32 intensity;
    u32 tint_rgba8;
    u32 shape_id;
};

/// Bound generously above SimWorld's default `max_damage_fields` (512, see
/// sim/SimWorld.h) so a full load never silently truncates; excess is dropped
/// with a warning, same policy as the chaff batcher.
constexpr u32 kMaxFieldInstances = 1024;

/// Per-round GPU layout, mirrored in projectile.vert. Unlike ParticleInstance
/// this is NOT a frozen contract — it is a Renderer.cpp implementation detail,
/// the same arrangement FieldGpuInstance has, so the SoA-to-GPU mapping can
/// change here and in the shader together without touching sim/.
struct ProjectileGpuInstance {
    f32 x, y;
    f32 vx, vy;
    f32 radius;
    f32 phase;
    f32 r, g, b, a;
    u32 visual_id;
};

/// Elite death-burst timing. The renderer has no access to the spawning
/// entity's archetype's `ArchetypeBehavior::death_fade` (sim/ecs internals,
/// not exposed through the frozen Components.h/NamedAgents.h contracts this
/// file may include) so this is a fixed approximation shared by every
/// archetype rather than an exact match: the burst always fades over this
/// many seconds of `AiBrain::state_timer`, regardless of when the entity is
/// actually destroyed. Harmless either way — if the real death_fade is
/// shorter the entity (and its burst) simply disappears mid-fade; if longer,
/// the burst finishes fading a little before the entity is destroyed.
constexpr f32 kDeathBurstWindow = 0.5f;
constexpr f32 kDeathBurstScale = 2.4f;

/// Burst (lifetime > 0) DamageFields fade as their remaining lifetime runs
/// out. DamageField only exposes *remaining* lifetime, not elapsed/total
/// duration (frozen contract, sim/damage/DamageField.h), so a true fade-in at
/// spawn can't be reconstructed here — this fixed window shapes the fade-out
/// near expiry instead, which is what actually reads as "the nova is ending".
constexpr f32 kFieldBurstFadeWindow = 0.35f;

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

    // Field-VFX pass (Wave 4G). Small instance count (bounded by
    // max_damage_fields, in the hundreds at most) so a single triple-buffered
    // region is plenty; reuses the same persistent-mapping pattern as chaff/
    // entities purely for consistency, not because it's perf-critical here.
    gl::VertexArray field_vao;
    gl::Buffer field_instances;
    gl::FenceRing<kInstanceRegions> field_fence;
    u32 field_region = 0;

    // Projectile pass (Wave 6). Real simulated rounds, bounded by
    // SimDesc::max_projectiles.
    gl::VertexArray projectile_vao;
    gl::Buffer projectile_instances;
    gl::FenceRing<kInstanceRegions> projectile_fence;
    u32 projectile_region = 0;
    u32 max_projectile_instances = 0;

    // Particle pass (Wave 6). By far the largest instance buffer in the
    // renderer -- a quarter million instances per blend mode, triple buffered.
    // Sized from RendererDesc rather than a constant because it dominates VRAM
    // use and a smaller machine may want it cut.
    gl::VertexArray particle_vao;
    gl::Buffer particle_instances;
    gl::FenceRing<kInstanceRegions> particle_fence;
    u32 particle_region = 0;
    u32 max_particle_instances = 0;

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

    // ---- Field-VFX instanced pass (Wave 4G) --------------------------------
    if (!imp.field_vao.create()) { error_ = "failed to create the field VAO"; return false; }
    imp.field_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.field_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);

    const usize field_bytes =
        static_cast<usize>(kMaxFieldInstances) * sizeof(FieldGpuInstance) * kInstanceRegions;
    if (!imp.field_instances.create_persistent(field_bytes)) {
        error_ = "failed to allocate the persistently-mapped field instance buffer";
        return false;
    }
    imp.field_vao.bind_vertex_buffer(1, imp.field_instances, sizeof(FieldGpuInstance), 0, 1);
    imp.field_vao.attrib_float(1, 1, 2, GL_FLOAT, false, offsetof(FieldGpuInstance, x));
    imp.field_vao.attrib_float(2, 1, 2, GL_FLOAT, false, offsetof(FieldGpuInstance, scale_x));
    imp.field_vao.attrib_float(3, 1, 1, GL_FLOAT, false, offsetof(FieldGpuInstance, rotation));
    imp.field_vao.attrib_float(4, 1, 1, GL_FLOAT, false, offsetof(FieldGpuInstance, arc_cos));
    imp.field_vao.attrib_float(5, 1, 1, GL_FLOAT, false, offsetof(FieldGpuInstance, falloff));
    imp.field_vao.attrib_float(6, 1, 1, GL_FLOAT, false, offsetof(FieldGpuInstance, intensity));
    imp.field_vao.attrib_float(7, 1, 4, GL_UNSIGNED_BYTE, true, offsetof(FieldGpuInstance, tint_rgba8));
    imp.field_vao.attrib_int(8, 1, 1, GL_UNSIGNED_INT, offsetof(FieldGpuInstance, shape_id));

    // ---- Projectile pass. Attribute locations mirror projectile.vert. ------
    imp.max_projectile_instances = math::max(desc_.max_projectile_instances, 1u);
    if (!imp.projectile_vao.create()) { error_ = "failed to create the projectile VAO"; return false; }
    imp.projectile_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.projectile_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);
    const usize projectile_bytes = static_cast<usize>(imp.max_projectile_instances) *
                                   sizeof(ProjectileGpuInstance) * kInstanceRegions;
    if (!imp.projectile_instances.create_persistent(projectile_bytes)) {
        error_ = "failed to allocate the projectile instance buffer";
        return false;
    }
    imp.projectile_vao.bind_vertex_buffer(1, imp.projectile_instances,
                                          sizeof(ProjectileGpuInstance), 0, 1);
    imp.projectile_vao.attrib_float(1, 1, 2, GL_FLOAT, false, offsetof(ProjectileGpuInstance, x));
    imp.projectile_vao.attrib_float(2, 1, 2, GL_FLOAT, false, offsetof(ProjectileGpuInstance, vx));
    imp.projectile_vao.attrib_float(3, 1, 1, GL_FLOAT, false, offsetof(ProjectileGpuInstance, radius));
    imp.projectile_vao.attrib_float(4, 1, 1, GL_FLOAT, false, offsetof(ProjectileGpuInstance, phase));
    imp.projectile_vao.attrib_float(5, 1, 4, GL_FLOAT, false, offsetof(ProjectileGpuInstance, r));
    imp.projectile_vao.attrib_int(6, 1, 1, GL_UNSIGNED_INT,
                                  offsetof(ProjectileGpuInstance, visual_id));

    // ---- Particle pass. Attribute locations mirror particle.vert, and the
    // instance layout mirrors vfx::ParticleInstance byte for byte (that one IS
    // a frozen contract -- see vfx/Particles.h).
    imp.max_particle_instances = math::max(desc_.max_particle_instances, 1u);
    if (!imp.particle_vao.create()) { error_ = "failed to create the particle VAO"; return false; }
    imp.particle_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.particle_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);
    const usize particle_bytes = static_cast<usize>(imp.max_particle_instances) *
                                 sizeof(vfx::ParticleInstance) * kInstanceRegions;
    if (!imp.particle_instances.create_persistent(particle_bytes)) {
        error_ = "failed to allocate the particle instance buffer";
        return false;
    }
    imp.particle_vao.bind_vertex_buffer(1, imp.particle_instances,
                                        sizeof(vfx::ParticleInstance), 0, 1);
    imp.particle_vao.attrib_float(1, 1, 2, GL_FLOAT, false, offsetof(vfx::ParticleInstance, x));
    imp.particle_vao.attrib_float(2, 1, 2, GL_FLOAT, false, offsetof(vfx::ParticleInstance, vx));
    imp.particle_vao.attrib_float(3, 1, 1, GL_FLOAT, false, offsetof(vfx::ParticleInstance, size));
    imp.particle_vao.attrib_float(4, 1, 1, GL_FLOAT, false, offsetof(vfx::ParticleInstance, rotation));
    imp.particle_vao.attrib_float(5, 1, 4, GL_UNSIGNED_BYTE, true,
                                  offsetof(vfx::ParticleInstance, tint_rgba8));
    imp.particle_vao.attrib_int(6, 1, 1, GL_UNSIGNED_INT,
                                offsetof(vfx::ParticleInstance, kind_blend));
    imp.particle_vao.attrib_float(7, 1, 1, GL_FLOAT, false,
                                  offsetof(vfx::ParticleInstance, age_norm));
    imp.particle_vao.attrib_float(8, 1, 1, GL_FLOAT, false, offsetof(vfx::ParticleInstance, seed));

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
    imp.shaders.load_graphics("field", "field.vert", "field.frag");
    imp.shaders.load_graphics("projectile", "projectile.vert", "projectile.frag");
    imp.shaders.load_graphics("particle", "particle.vert", "particle.frag");
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
    // DESIGN.md §7.1/§9.1: "subtle heartbeat pulse" on the substrate layer.
    // tissue.frag already turns a phase into a low-amplitude brightness pulse
    // (`1.0 + 0.025*sin(phase)`); the current callers (app/Modes.cpp,
    // app/App.cpp) always pass a static 0.0f, which would otherwise freeze the
    // pulse at a constant brightness every frame. Driving it off the
    // renderer's own wall-clock `time` (the same clock chaff/entity animation
    // phases already use) is what actually makes it pulse; `heartbeat_phase`
    // stays additive so a future caller can still offset it per-lane (DESIGN.md
    // §9.2's "arterial lanes pulse faster") without this renderer-side default
    // going away.
    constexpr f32 kHeartbeatRate = 2.1f; // radians/sec; a relaxed resting pulse
    glUniform1f(5, heartbeat_phase + imp.time * kHeartbeatRate);

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

        // Elite death burst (DESIGN.md §9.5 tier 2: "individual pop/burst
        // VFX"). Detected generically off AiBrain::state == Dying rather than
        // a dedicated death event — none exists in the frozen NamedAgents.h
        // contract, and this reads the same state system_named_cleanup
        // (sim/ecs/NamedAgents.cpp) already uses to decide when to destroy the
        // entity, so it needs no new sim-side plumbing. Tier-agnostic on
        // purpose: a future boss (comp::NamedAgent::tier == 2) gets the same
        // hook, just scaled up, so Wave 5B's boss death has something to plug
        // into already.
        if (const auto* brain = registry.try_get<const sim::comp::AiBrain>(entity)) {
            if (brain->state == sim::comp::AiState::Dying && count < cap) {
                f32 burst_scale = kDeathBurstScale;
                if (const auto* na = registry.try_get<const sim::comp::NamedAgent>(entity)) {
                    if (na->tier >= 2) burst_scale *= 1.8f; // boss-tier: bigger pop
                }
                EntityInstance& burst = region_base[count++];
                burst.x = t.position.x;
                burst.y = t.position.y;
                burst.scale = sp.size * burst_scale;
                burst.rotation = 0.0f;
                // Flash the family colour toward white rather than reusing it
                // flat, so the burst still reads as "impact" and not just a
                // bigger silhouette of the same sprite.
                const Vec4 hot{sp.tint.r + (1.0f - sp.tint.r) * 0.35f,
                               sp.tint.g + (1.0f - sp.tint.g) * 0.35f,
                               sp.tint.b + (1.0f - sp.tint.b) * 0.35f, sp.tint.a};
                burst.tint_rgba8 = pack_rgba8(hot);
                burst.shape_id = 4; // entity.frag: death burst
                burst.anim_phase = math::saturate(brain->state_timer / kDeathBurstWindow);
                burst.pad = 0.0f;
            }
        }
    }

    // Telegraph overlay (DESIGN.md §9.5 tier 2: "a clearly telegraphed wind-up
    // beforehand... so an elite's attack is anticipated, not just suffered").
    // Reads NamedFrame::telegraphs directly rather than mutating comp::Sprite
    // on the source entity: that keeps this fully decoupled from the named-
    // agent sim code (sim/ecs/NamedAgents.cpp, Wave 1D's territory) at the
    // cost of one extra registry-context lookup per frame. frame_if_any
    // returns null when no named-agent systems are installed (bench scenarios
    // with named_count == 0, most sim-tests) — handled gracefully below.
    //
    // Two overlay instances per active telegraph: the pre-built diamond shape
    // (shape_id 2, already documented in entity.frag as "telegraphed/alert"
    // but never wired up before this) as a pulsing alert glyph, plus a new
    // closing countdown ring (shape_id 3) whose radius shrinks and flashes as
    // ActiveTelegraph::progress approaches 1.
    if (const sim::named::NamedFrame* frame = sim::named::frame_if_any(registry)) {
        const Vec4 diamond_tint{1.0f, 0.25f, 0.20f, 0.85f};
        const Vec4 ring_tint{1.0f, 0.55f, 0.15f, 0.9f};
        for (const sim::named::ActiveTelegraph& tg : frame->telegraphs) {
            if (count + 2 > cap) break;

            EntityInstance& diamond = region_base[count++];
            diamond.x = tg.point.x;
            diamond.y = tg.point.y;
            diamond.scale = tg.radius * 0.9f;
            diamond.rotation = 0.0f;
            diamond.tint_rgba8 = pack_rgba8(diamond_tint);
            diamond.shape_id = 2;
            diamond.anim_phase = imp.time * 6.0f; // fast alert pulse (shape 2's own sin pulse)
            diamond.pad = 0.0f;

            EntityInstance& ring = region_base[count++];
            ring.x = tg.point.x;
            ring.y = tg.point.y;
            ring.scale = tg.radius * 2.0f; // scale == diameter, matches entity.vert's convention
            ring.rotation = 0.0f;
            ring.tint_rgba8 = pack_rgba8(ring_tint);
            ring.shape_id = 3; // entity.frag: telegraph countdown ring
            ring.anim_phase = math::saturate(tg.progress); // repurposed as progress, not a phase
            ring.pad = 0.0f;
        }
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
    // DESIGN.md §9.5: tower AoEs render as literal fluid/chemical fields that
    // visibly reshape the pathogen river. See field.vert/field.frag for the
    // shape-specific SDF treatment; this function's job is purely the CPU-side
    // per-instance parameterization (persistent-vs-burst intensity, tint,
    // shape-specific transform) plus the single instanced draw call.
    stats_.vfx_fields_drawn = static_cast<u32>(count);
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;
    WallClock timer;

    if (count > 0 && fields == nullptr) count = 0;
    const u32 draw_count = math::min(static_cast<u32>(count), kMaxFieldInstances);
    if (count > draw_count) {
        IMMUNE_LOG_WARN("field VFX: dropped %zu fields (exceeds kMaxFieldInstances=%u)",
                        count - draw_count, kMaxFieldInstances);
    }

    imp.field_fence.wait(imp.field_region);
    FieldGpuInstance* region_base = imp.field_instances.mapped_as<FieldGpuInstance>() +
        static_cast<usize>(imp.field_region) * kMaxFieldInstances;

    // Cool blue/violet base palette per shape category (DESIGN.md §9.3: towers
    // and their effects are the "not part of the flow" temperature family),
    // distinct enough per shape that a standing toxin cloud (Circle), an
    // antibody wall (Rect), a directional spray (Cone), and a complement
    // cascade (Chain) don't all read as the same generic glow. friendly_fire
    // fields (the allergen overreaction mechanic) override to a hot warning
    // colour regardless of shape, since those damage the player, not the horde.
    const Vec4 kCircleTint{0.30f, 0.80f, 0.88f, 1.0f};
    const Vec4 kRectTint{0.58f, 0.46f, 0.95f, 1.0f};
    const Vec4 kConeTint{0.92f, 0.38f, 0.58f, 1.0f};
    const Vec4 kChainTint{0.78f, 0.95f, 1.0f, 1.0f};
    const Vec4 kFriendlyFireTint{1.0f, 0.32f, 0.15f, 1.0f};

    for (u32 i = 0; i < draw_count; ++i) {
        const sim::DamageField& f = fields[i];
        FieldGpuInstance inst{};

        switch (f.shape) {
        case sim::FieldShape::Rect: {
            const Vec2 c = f.rect.center();
            const Vec2 sz = f.rect.size();
            inst.x = c.x;
            inst.y = c.y;
            inst.scale_x = math::max(sz.x, 0.05f);
            inst.scale_y = math::max(sz.y, 0.05f);
            inst.rotation = 0.0f; // sim::Rect is always axis-aligned
            inst.arc_cos = -1.0f;
            inst.shape_id = 1;
            break;
        }
        case sim::FieldShape::Cone: {
            inst.x = f.origin.x;
            inst.y = f.origin.y;
            const f32 diameter = math::max(f.radius, 0.05f) * 2.0f;
            inst.scale_x = diameter;
            inst.scale_y = diameter;
            inst.rotation = std::atan2(f.direction.y, f.direction.x);
            inst.arc_cos = std::cos(math::max(f.arc_radians, 0.001f));
            inst.shape_id = 2;
            break;
        }
        case sim::FieldShape::Chain: {
            inst.x = f.origin.x;
            inst.y = f.origin.y;
            const f32 diameter = math::max(f.radius, 0.05f) * 2.0f;
            inst.scale_x = diameter;
            inst.scale_y = diameter;
            inst.rotation = 0.0f;
            inst.arc_cos = -1.0f;
            inst.shape_id = 3;
            break;
        }
        case sim::FieldShape::Circle:
        default: {
            inst.x = f.origin.x;
            inst.y = f.origin.y;
            const f32 diameter = math::max(f.radius, 0.05f) * 2.0f;
            inst.scale_x = diameter;
            inst.scale_y = diameter;
            inst.rotation = 0.0f;
            inst.arc_cos = -1.0f;
            inst.shape_id = 0;
            break;
        }
        }

        inst.falloff = f.falloff;

        // Persistent (lifetime <= 0, refreshed every tick by its owning
        // tower): a steady mid-brightness toxin-cloud read, with a slow
        // shader-driven breathing pulse. Per-field phase offset (hashed from
        // its array slot) keeps a bank of towers from breathing in lockstep,
        // same rationale as ChaffInstance::anim_phase.
        //
        // Burst (lifetime > 0, one-shot Histamine Flare / Complement Cascade
        // style effects): DamageField only exposes *remaining* lifetime, not
        // elapsed or total duration, so a true spawn-flash can't be
        // reconstructed here. Instead the burst starts bright and fades as it
        // approaches expiry, which reads correctly as "the nova is ending"
        // even without knowing when it began.
        if (f.lifetime <= 0.0f) {
            const u32 h = (i * 2654435761u) ^ 0x9E3779B9u;
            const f32 phase = static_cast<f32>(h & 0xFFFFu) * (math::kTwoPi / 65536.0f);
            const f32 breathe = 0.5f + 0.5f * std::sin(imp.time * 1.4f + phase);
            inst.intensity = 0.35f + 0.30f * breathe;
        } else {
            inst.intensity = math::saturate(f.lifetime / kFieldBurstFadeWindow);
        }

        Vec4 tint = kCircleTint;
        switch (f.shape) {
        case sim::FieldShape::Rect:  tint = kRectTint;  break;
        case sim::FieldShape::Cone:  tint = kConeTint;  break;
        case sim::FieldShape::Chain: tint = kChainTint; break;
        case sim::FieldShape::Circle: default: tint = kCircleTint; break;
        }
        if (f.friendly_fire) tint = kFriendlyFireTint;
        inst.tint_rgba8 = pack_rgba8(tint);

        region_base[i] = inst;
    }

    const ShaderProgram prog = imp.shaders.get("field");
    if (prog.valid() && draw_count > 0) {
        glUseProgram(prog.gl_id);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
        glUniform1f(1, imp.time);
        imp.field_vao.bind();
        const u32 base_instance = imp.field_region * kMaxFieldInstances;
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_FAN, 0, 4, static_cast<GLsizei>(draw_count),
                                          base_instance);
        ++stats_.draw_calls;
    }

    imp.field_fence.signal(imp.field_region);
    imp.field_region = (imp.field_region + 1) % kInstanceRegions;

    stats_.submit_ms += timer.elapsed_ms();
}

void Renderer::submit_projectiles(const sim::ProjectileBuffers& projectiles) {
    // The Gunner's real simulated rounds. Deliberately drawn BEFORE the
    // particle pass so the additive tracer storm layers on top of them: the
    // round is matter, the smear behind it is atmosphere.
    const usize count = projectiles.count();
    stats_.projectile_instances_drawn = static_cast<u32>(count);
    if (!ready_ || !impl_ || count == 0) return;
    Impl& imp = *impl_;
    WallClock timer;

    const u32 draw_count = math::min(static_cast<u32>(count), imp.max_projectile_instances);

    imp.projectile_fence.wait(imp.projectile_region);
    ProjectileGpuInstance* base = imp.projectile_instances.mapped_as<ProjectileGpuInstance>() +
        static_cast<usize>(imp.projectile_region) * imp.max_projectile_instances;

    // Warm white-yellow: the Gunner's identity hue, matching the palette the
    // VFX layer uses for the same tower (vfx/Particles.cpp's palette_for).
    // Rounds carry a visual_id, not a TowerType, so this is a constant here
    // rather than a per-round lookup -- today only the Gunner fires rounds.
    for (u32 i = 0; i < draw_count; ++i) {
        ProjectileGpuInstance inst{};
        inst.x = projectiles.pos_x[i];
        inst.y = projectiles.pos_y[i];
        inst.vx = projectiles.vel_x[i];
        inst.vy = projectiles.vel_y[i];
        inst.radius = math::max(projectiles.hit_radius[i], 0.12f);
        // Hashed off the slot so rounds don't shimmer in lockstep, same
        // rationale as ChaffInstance::anim_phase.
        const u32 h = (i * 2654435761u) ^ 0x85EBCA6Bu;
        inst.phase = static_cast<f32>(h & 0xFFFFu) * (math::kTwoPi / 65536.0f);
        inst.r = 1.0f; inst.g = 0.96f; inst.b = 0.68f; inst.a = 1.0f;
        inst.visual_id = projectiles.visual_id[i];
        base[i] = inst;
    }

    const ShaderProgram prog = imp.shaders.get("projectile");
    if (prog.valid()) {
        glUseProgram(prog.gl_id);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
        glUniform1f(1, imp.time);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        imp.projectile_vao.bind();
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_FAN, 0, 4, static_cast<GLsizei>(draw_count),
                                          imp.projectile_region * imp.max_projectile_instances);
        ++stats_.draw_calls;
    }

    imp.projectile_fence.signal(imp.projectile_region);
    imp.projectile_region = (imp.projectile_region + 1) % kInstanceRegions;
    stats_.submit_ms += timer.elapsed_ms();
}

void Renderer::submit_particles(const vfx::ParticleInstance* instances, usize count,
                                vfx::BlendMode blend) {
    // One instanced draw for the whole span. The CPU never builds per-particle
    // geometry -- vfx::ParticleSystem::build_instances already packed a
    // contiguous array in exactly the layout particle.vert reads, so this is a
    // memcpy into the mapped region plus one draw call.
    //
    // Called once per blend mode per frame, so this accumulates rather than
    // assigns the counter; the first call of the frame is the additive one.
    if (blend == vfx::BlendMode::Additive) stats_.particle_instances_drawn = 0;
    stats_.particle_instances_drawn += static_cast<u32>(count);
    if (!ready_ || !impl_ || count == 0 || instances == nullptr) return;
    Impl& imp = *impl_;
    WallClock timer;

    const u32 draw_count = math::min(static_cast<u32>(count), imp.max_particle_instances);
    if (count > draw_count) {
        IMMUNE_LOG_WARN("particles: dropped %zu instances (exceeds max_particle_instances=%u)",
                        count - draw_count, imp.max_particle_instances);
    }

    imp.particle_fence.wait(imp.particle_region);
    vfx::ParticleInstance* base = imp.particle_instances.mapped_as<vfx::ParticleInstance>() +
        static_cast<usize>(imp.particle_region) * imp.max_particle_instances;
    std::memcpy(base, instances, static_cast<usize>(draw_count) * sizeof(vfx::ParticleInstance));

    const ShaderProgram prog = imp.shaders.get("particle");
    if (prog.valid()) {
        glUseProgram(prog.gl_id);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
        glUniform1f(1, imp.time);
        glEnable(GL_BLEND);
        if (blend == vfx::BlendMode::Additive) {
            // Energy stacks toward white -- this is what makes a dense Gunner
            // stream read as one continuous bright river rather than a cloud
            // of separate dots.
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        } else {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        imp.particle_vao.bind();
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_FAN, 0, 4, static_cast<GLsizei>(draw_count),
                                          imp.particle_region * imp.max_particle_instances);
        ++stats_.draw_calls;
        // Leave the pipeline on standard alpha so a later pass never inherits
        // additive blending by accident.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    imp.particle_fence.signal(imp.particle_region);
    imp.particle_region = (imp.particle_region + 1) % kInstanceRegions;
    stats_.submit_ms += timer.elapsed_ms();
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
