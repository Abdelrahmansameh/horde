// render/Renderer.cpp — frame graph and draw submission. Owner: Wave 1C.
//
// See render/Renderer.h for the two facts that shape this file: one instanced
// draw call per PathogenFamily, and density-LOD crossfade instead of distance
// LOD. The CPU-side partition/crossfade math lives in render/ChaffBatcher.h
// (unit-tested with no GL context); this file is the GL half: persistently
// mapped, triple-buffered instance buffers, the density texture, and the
// shader passes that read them.
#include "render/Renderer.h"

#include "render/ChaffBatcher.h"

#include "core/Clock.h"
#include "core/Log.h"
#include "core/Math.h"
#include "render/Camera.h"
#include "render/Gl.h"
#include "render/Screenshot.h"
#include "render/Shader.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/Components.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/ecs/NamedAgents.h"
#include "sim/flowfield/FlowField.h"
#include "sim/squad/Squads.h"
#include "sim/projectile/Projectiles.h"
#include "sim/fluid/Fluid.h"
#include "sim/swarm/Swarmers.h"
#include "sim/spatial/SpatialHash.h"
#include "vfx/Particles.h"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace immune::render {

namespace {

/// The DESIGN.md §6 colour code, and the per-family look from ChaffBatcher.h.
/// Both are defaults now rather than constants: assets/config/enemies.json
/// overrides them at load through set_family_color/set_family_visual. Keeping
/// the compiled-in values here means a renderer that never sees a config --
/// a unit test, a bare screenshot harness -- looks exactly as it always did.
struct FamilyTables {
    Vec4 color[kFamilyCount];
    FamilyVisual visual[kFamilyCount];

    FamilyTables() {
        // Retuned when the substrate went vivid red. §9.3 fixes the family
        // coding as the constant that never shifts *with lane or region*; it
        // does not require the values to survive a change of floor unexamined,
        // and two of them did not. Measured against the lumen (#D23548,
        // luminance 0.34), the old Virus hue scored 0.01 luminance contrast
        // -- effectively invisible on the lane.
        //
        // VIRUS IS GREEN, not the red-purple DESIGN.md §6.2's table still
        // lists. No purple solved it: the Virus is the most numerous family and
        // a purple is always the nearest thing on the wheel to a red lane. The
        // specific green sits a clear hue apart from Bacteria's yellow-green
        // (hue 68 deg) rather than beside it.
        color[static_cast<u32>(PathogenFamily::Virus)]       = Vec4{0.20f, 0.94f, 0.38f, 1.0f};
        color[static_cast<u32>(PathogenFamily::Bacteria)]    = Vec4{0.72f, 0.80f, 0.22f, 1.0f};

        // silhouette = THREAT tier, tempo = SPEED tier, wobble = family
        // texture. Virus smaller and faster; bacteria bigger and slower.
        visual[static_cast<u32>(PathogenFamily::Virus)]       = FamilyVisual{1.53f, 3.4f, 0.55f};
        visual[static_cast<u32>(PathogenFamily::Bacteria)]    = FamilyVisual{2.25f, 1.5f, 0.30f};
    }
};

FamilyTables& family_tables() {
    static FamilyTables tables;
    return tables;
}

u32 family_slot(PathogenFamily family) {
    const u32 i = static_cast<u32>(family);
    return i < kFamilyCount ? i : 0u;
}

} // namespace

Vec4 family_color(PathogenFamily family) {
    return family_tables().color[family_slot(family)];
}

void set_family_color(PathogenFamily family, Vec4 color) {
    family_tables().color[family_slot(family)] = color;
}

const FamilyVisual& family_visual(PathogenFamily family) {
    return family_tables().visual[family_slot(family)];
}

void set_family_visual(PathogenFamily family, const FamilyVisual& visual) {
    family_tables().visual[family_slot(family)] = visual;
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
///   Circle/Chain: scale = diameter, rotation = 0, arc_cos unused.
///   Rect:         scale = full width/height, rotation = 0 (sim::Rect is
///                 always axis-aligned), arc_cos = width/height aspect.
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

/// Per-granule GPU layout, mirrored in swarmer.vert. Same status as
/// ProjectileGpuInstance: a Renderer.cpp implementation detail, not a contract.
struct SwarmerGpuInstance {
    f32 x, y;
    f32 vx, vy;
    f32 radius;
    f32 phase;
    f32 r, g, b, a;
    u32 flags;      ///< bit 0: attached to a host. Mirrors swarmer.frag.
};

/// Per-instance data for the fluid thickness pass. Mirrored only in
/// assets/shaders/fluid.vert.
///
/// `vx/vy` is the particle's motion over the last substep, not a normalized
/// heading: the vertex stage stretches the splat along it, which is what gives
/// fast mucus its streak and lets a jet read as moving even while every
/// individual particle is a featureless blob.
struct FluidGpuInstance {
    f32 x, y;
    f32 vx, vy;
    f32 radius;
    f32 density;   ///< Relaxed density / rest density. Surface vs body cue.
    f32 fade;      ///< 0..1 lifetime fade, already resolved on the CPU.
    f32 foam;      ///< 0..1 how churned this particle is (splashed / on a wall).
};

/// Divisor between the framebuffer and the fluid thickness target.
///
/// Half resolution is not a corner cut here, it is the correct filter. The
/// composite reads a screen-space GRADIENT of thickness to build its normals,
/// and a gradient taken at full resolution over a field made of overlapping
/// discs picks up the individual discs as bumps. Halving the resolution (and
/// sampling it back bilinearly) low-passes exactly the frequency the discs live
/// at, so the surface comes out smooth and continuous. It also quarters the
/// fill cost of the single most overdrawn pass in the frame.
constexpr i32 kFluidTargetDivisor = 2;

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
///
/// IT MUST BE SHORTER THAN THE SHORTEST BURST IN THE GAME, and at 0.35s it was
/// longer than every one of them. Because intensity is remaining/window, a
/// field whose ENTIRE life is under the window starts already faded and only
/// gets dimmer: the Tesla's chain (kTeslaArcSeconds, 0.12s) peaked at 34%
/// brightness and the Macrophage's shell (kMortarBurstSeconds, 0.30s) at 86%,
/// so the roster's two burst towers were the two whose AoE you could barely
/// see. At 0.10s both hold full brightness for most of their life and spend
/// only the last hundred milliseconds fading, which is what the curve was for.
constexpr f32 kFieldBurstFadeWindow = 0.10f;

// ---------------------------------------------------------------------------
// Tissue-pass side textures (DESIGN.md §9.2 lane identity, §9.4 fluid feel).
// See assets/shaders/tissue.frag for what the shader does with them.
// ---------------------------------------------------------------------------

/// Resolution cap for the flow texture. The flow field itself is as fine as the
/// level's tissue grid (0.5 world units — a 260x160 level is 520x320 cells),
/// which is far more than a visual needs: the plasma striations want a *smooth*
/// direction field, and downsampling is how they get one. Capping also bounds
/// the per-frame rebuild cost to a fixed ~37k bilinear samples regardless of
/// level size.
constexpr i32 kFlowTexMax = 192;

/// Resolution cap for the lane-hue texture. Coarser than the flow texture on
/// purpose: it is bilinearly filtered, so a coarse grid is what gives soft
/// hue transitions where two lanes converge instead of a hard seam.
constexpr i32 kLaneTexMax = 128;

/// DESIGN.md §9.2's fixed per-vessel-type hue and ambient tempo. Indexed by
/// game::VesselType's ordinal — mirrored, not included, since render/ does not
/// depend on game/ (see TissueDecor's rationale). Order: artery, vein,
/// lymphatic, nerve-adjacent, mucosal fold.
///
/// These are SATURATED. The substrate used to hold back on value — a dark,
/// desaturated floor that could not possibly compete with the horde. That is
/// no longer the strategy: the lanes are vivid, and foreground readability is
/// carried by hue separation and per-agent drop shadows instead (the pathogen
/// families are yellow-green, teal, brown and magenta; the towers are cool
/// blue/white/violet — none of them sit near a red substrate on the wheel).
///
/// Each type keeps the identity §9.2 assigns it, so a returning player still
/// recognises an artery at a glance; what changed is the intensity, not the
/// hue relationships between them.
struct LaneVisual {
    Vec4 hue;   ///< rgb only; a unused
    f32 tempo;  ///< ambient animation rate multiplier
};
// Desaturated one step from the first vivid pass. Each value was mixed 22%
// toward its OWN equal-luminance grey rather than being darkened or dulled by
// hand, so every lane holds exactly the brightness and hue it had and only the
// intensity comes off — the palette relationships between the five, and the
// contrast the foreground was measured against, are untouched.
constexpr LaneVisual kLaneVisuals[] = {
    {{0.820f, 0.227f, 0.289f, 1.0f}, 1.70f}, // Artery        — arterial crimson
    {{0.512f, 0.184f, 0.402f, 1.0f}, 0.85f}, // Vein          — deep wine-violet
    {{0.897f, 0.632f, 0.429f, 1.0f}, 0.55f}, // Lymphatic     — gold, slow drift
    {{0.600f, 0.288f, 0.756f, 1.0f}, 1.15f}, // NerveAdjacent — violet
    {{0.816f, 0.441f, 0.379f, 1.0f}, 0.70f}, // MucosalFold   — warm coral
};
constexpr u32 kLaneVisualCount = static_cast<u32>(sizeof(kLaneVisuals) / sizeof(kLaneVisuals[0]));

/// Encoded into the lane texture's alpha, so the shader can recover tempo as
/// `a * 2`. Every tempo above is comfortably inside [0, 2].
constexpr f32 kTempoEncodeScale = 0.5f;

/// Builds the RGBA8 lane-hue texture from a LaneOwnershipMap's owner grid.
///
/// Three steps, and the middle one is the interesting one:
///
///  1. DOWNSAMPLE to `out_w x out_h`. Each coarse cell takes the first owned
///     fine cell it covers. Coarse is not a compromise here — see kLaneTexMax.
///
///  2. FLOOD the unowned cells. `owner` is 0xFF both outside every vessel (most
///     of the map) and, per game::LaneOwnershipMap's documented first-claim-wins
///     caveat, wherever two lanes' lumens overlap — which is precisely where
///     lanes converge, i.e. the most visible place on the level. Sampling those
///     as "no lane" would put a hole of default hue right at the junction. So
///     unowned cells are iteratively filled from their owned 4-neighbours,
///     which both closes the junction hole (with a blend of the lanes meeting
///     there, which is the honest answer) and carries each lane's hue outward
///     into the surrounding flesh so the interstitium belongs to its vessel.
///     Ping-ponged so fill order cannot bias the result toward one direction.
///
///  3. BLUR twice, so lane boundaries are soft gradients rather than seams.
///     Combined with the texture's linear filtering this is what makes a
///     three-lane level read as three tinted regions of one organ instead of
///     three flat colour fields.
///
/// Level-load-time cost only (the caller caches on the source pointer), and
/// bounded by kLaneTexMax regardless of level size.
void build_lane_tint(const TissueDecor& decor, i32 out_w, i32 out_h, std::vector<u8>& out) {
    const usize n = static_cast<usize>(out_w) * static_cast<usize>(out_h);
    std::vector<Vec4> tint(n, Vec4{0.0f, 0.0f, 0.0f, 0.0f}); // rgb + encoded tempo
    std::vector<u8> filled(n, 0);

    const auto visual_for = [&decor](u8 lane_index) -> LaneVisual {
        u32 type = 0;
        if (decor.lane_type != nullptr && lane_index < decor.lane_count) {
            type = decor.lane_type[lane_index];
        }
        if (type >= kLaneVisualCount) type = 0;
        return kLaneVisuals[type];
    };

    // ---- 1. Downsample -----------------------------------------------------
    for (i32 oy = 0; oy < out_h; ++oy) {
        const i32 y0 = oy * decor.lane_height / out_h;
        const i32 y1 = math::max((oy + 1) * decor.lane_height / out_h, y0 + 1);
        for (i32 ox = 0; ox < out_w; ++ox) {
            const i32 x0 = ox * decor.lane_width / out_w;
            const i32 x1 = math::max((ox + 1) * decor.lane_width / out_w, x0 + 1);
            u8 owner = 0xFFu;
            for (i32 y = y0; y < y1 && owner == 0xFFu; ++y) {
                for (i32 x = x0; x < x1; ++x) {
                    const u8 o = decor.lane_owner[static_cast<usize>(y) *
                                                  static_cast<usize>(decor.lane_width) +
                                                  static_cast<usize>(x)];
                    if (o != 0xFFu) { owner = o; break; }
                }
            }
            if (owner == 0xFFu) continue;
            const LaneVisual lv = visual_for(owner);
            const usize i = static_cast<usize>(oy) * static_cast<usize>(out_w) +
                            static_cast<usize>(ox);
            tint[i] = Vec4{lv.hue.r, lv.hue.g, lv.hue.b, lv.tempo * kTempoEncodeScale};
            filled[i] = 1;
        }
    }

    // ---- 2. Flood ----------------------------------------------------------
    // The bound is the coarse grid's diagonal, so a level whose vessels occupy
    // one corner still fills completely; the early-out is what makes the usual
    // case (a few passes) cheap.
    const i32 kMaxPasses = out_w + out_h;
    std::vector<Vec4> next_tint = tint;
    std::vector<u8> next_filled = filled;
    for (i32 pass = 0; pass < kMaxPasses; ++pass) {
        bool any_hole = false;
        for (i32 y = 0; y < out_h; ++y) {
            for (i32 x = 0; x < out_w; ++x) {
                const usize i = static_cast<usize>(y) * static_cast<usize>(out_w) +
                                static_cast<usize>(x);
                if (filled[i] != 0) continue;
                Vec4 sum{0.0f, 0.0f, 0.0f, 0.0f};
                i32 hits = 0;
                const i32 dx[4] = {-1, 1, 0, 0};
                const i32 dy[4] = {0, 0, -1, 1};
                for (i32 k = 0; k < 4; ++k) {
                    const i32 nx = x + dx[k];
                    const i32 ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= out_w || ny >= out_h) continue;
                    const usize j = static_cast<usize>(ny) * static_cast<usize>(out_w) +
                                    static_cast<usize>(nx);
                    if (filled[j] == 0) continue;
                    sum.r += tint[j].r; sum.g += tint[j].g;
                    sum.b += tint[j].b; sum.a += tint[j].a;
                    ++hits;
                }
                if (hits == 0) { any_hole = true; continue; }
                const f32 inv = 1.0f / static_cast<f32>(hits);
                next_tint[i] = Vec4{sum.r * inv, sum.g * inv, sum.b * inv, sum.a * inv};
                next_filled[i] = 1;
            }
        }
        tint = next_tint;
        filled = next_filled;
        if (!any_hole) break;
    }
    // A level with no lanes at all leaves everything unfilled; fall back to the
    // arterial default rather than emitting black.
    for (usize i = 0; i < n; ++i) {
        if (filled[i] == 0) {
            tint[i] = Vec4{kLaneVisuals[0].hue.r, kLaneVisuals[0].hue.g, kLaneVisuals[0].hue.b,
                           kLaneVisuals[0].tempo * kTempoEncodeScale};
        }
    }

    // ---- 3. Blur -----------------------------------------------------------
    std::vector<Vec4> tmp(n);
    for (i32 pass = 0; pass < 2; ++pass) {
        for (i32 axis = 0; axis < 2; ++axis) {
            const i32 sx = (axis == 0) ? 1 : 0;
            const i32 sy = (axis == 0) ? 0 : 1;
            for (i32 y = 0; y < out_h; ++y) {
                for (i32 x = 0; x < out_w; ++x) {
                    Vec4 sum{0.0f, 0.0f, 0.0f, 0.0f};
                    f32 wsum = 0.0f;
                    for (i32 k = -1; k <= 1; ++k) {
                        const i32 nx = math::clamp(x + k * sx, 0, out_w - 1);
                        const i32 ny = math::clamp(y + k * sy, 0, out_h - 1);
                        const f32 w = (k == 0) ? 2.0f : 1.0f;
                        const Vec4& s = tint[static_cast<usize>(ny) * static_cast<usize>(out_w) +
                                             static_cast<usize>(nx)];
                        sum.r += s.r * w; sum.g += s.g * w; sum.b += s.b * w; sum.a += s.a * w;
                        wsum += w;
                    }
                    const f32 inv = 1.0f / wsum;
                    tmp[static_cast<usize>(y) * static_cast<usize>(out_w) +
                        static_cast<usize>(x)] =
                        Vec4{sum.r * inv, sum.g * inv, sum.b * inv, sum.a * inv};
                }
            }
            tint.swap(tmp);
        }
    }

    out.resize(n * 4);
    for (usize i = 0; i < n; ++i) {
        const u32 packed = pack_rgba8(tint[i]);
        out[i * 4 + 0] = static_cast<u8>(packed & 0xFFu);
        out[i * 4 + 1] = static_cast<u8>((packed >> 8) & 0xFFu);
        out[i * 4 + 2] = static_cast<u8>((packed >> 16) & 0xFFu);
        out[i * 4 + 3] = static_cast<u8>((packed >> 24) & 0xFFu);
    }
}

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

    // Swarmer pass. Mirrors the projectile pass exactly; kept as its own VAO,
    // buffer and fence ring rather than sharing the projectile ones so the two
    // submits in a frame can never contend for the same region.
    gl::VertexArray swarmer_vao;
    gl::Buffer swarmer_instances;
    gl::FenceRing<kInstanceRegions> swarmer_fence;
    u32 swarmer_region = 0;
    u32 max_swarmer_instances = 0;

    // Particle pass (Wave 6). By far the largest instance buffer in the
    // renderer -- a quarter million instances per blend mode, triple buffered.
    // Sized from RendererDesc rather than a constant because it dominates VRAM
    // use and a smaller machine may want it cut.
    gl::VertexArray particle_vao;
    gl::Buffer particle_instances;
    gl::FenceRing<kInstanceRegions> particle_fence;
    u32 particle_region = 0;
    u32 max_particle_instances = 0;

    // Fluid pass. Two stages with two very different shapes: an instanced
    // additive splat into `fluid_target`, then one fullscreen composite that
    // turns that thickness field into a shaded surface.
    gl::VertexArray fluid_vao;
    gl::Buffer fluid_instances;
    gl::FenceRing<kInstanceRegions> fluid_fence;
    u32 fluid_region = 0;
    u32 max_fluid_instances = 0;
    gl::ColorTarget fluid_target;
    i32 fluid_target_w = 0;
    i32 fluid_target_h = 0;

    // Blob + tissue passes both just need the shared quad's position attrib.
    gl::VertexArray screen_quad_vao;

    gl::Texture2D density_tex;
    DensityGrid density_grid;

    gl::Texture2D tissue_sdf_tex;
    i32 tissue_tex_w = 0;
    i32 tissue_tex_h = 0;
    /// World bounds the cached tissue side-textures were built for; a change
    /// means a different level was loaded.
    Rect tissue_world{{0.0f, 0.0f}, {0.0f, 0.0f}};

    // Flow texture: rebuilt every frame (the field reroutes whenever a tower
    // lands, and there is no version counter on FlowField to test against), but
    // capped at kFlowTexMax so that rebuild is a fixed small cost.
    gl::Texture2D tissue_flow_tex;
    i32 flow_tex_w = 0;
    i32 flow_tex_h = 0;
    std::vector<f32> flow_scratch; // rgba32f staging (dir.xy, cost, coherence)
    std::vector<u8> flow_valid;    // hole-fill mask, parallel to flow_scratch
    bool flow_tex_valid = false;
    // Change detector for the flow field; see submit_tissue.
    f64 flow_bake_ms = -1.0;
    u32 flow_cells_visited = 0xFFFFFFFFu;
    u32 flow_regions = 0xFFFFFFFFu;
    bool flow_rebuild_queued = false;

    // Lane-hue texture: built once per level. Lane ownership is a pure function
    // of the level file and never changes at runtime, so the source pointer
    // plus dimensions are a sufficient cache key.
    gl::Texture2D tissue_lane_tex;
    const u8* lane_src = nullptr;
    i32 lane_src_w = 0;
    i32 lane_src_h = 0;
    bool lane_tex_ready = false;

    // Flow-field debug overlay: rebuilt CPU-side each call (F1 overlay only,
    // never in the hot path), uploaded into a plain dynamic-storage buffer.
    GLuint flow_vao = 0;
    GLuint flow_vbo = 0;
    usize flow_vbo_capacity_bytes = 0;
    /// Rebuilt every frame the overlay is on; owned here so that rebuild is a
    /// clear() rather than an allocation of up to a megabyte per frame.
    std::vector<FlowDebugVertex> flow_verts;

    WallClock clock;
    f32 time = 0.0f;

    // Cached from the most recent begin_frame().
    glm::mat4 view_projection{1.0f};
    Rect visible_bounds{};
    f32 alpha = 0.0f;
};

namespace {

/// (Re)creates the offscreen thickness target for a given framebuffer size.
/// Separate from init() because resize() has to run it again: the target is in
/// SCREEN space, so a window resize invalidates it outright. Takes the pieces
/// rather than the Impl so it can stay a file-local free function — Impl is a
/// private nested type and naming it out here would not compile.
void ensure_fluid_target(gl::ColorTarget& target, i32& cached_w, i32& cached_h,
                         i32 fb_width, i32 fb_height) {
    const i32 w = math::max(1, fb_width / kFluidTargetDivisor);
    const i32 h = math::max(1, fb_height / kFluidTargetDivisor);
    if (target.valid() && cached_w == w && cached_h == h) return;
    if (!target.create(w, h, GL_RGBA16F)) {
        IMMUNE_LOG_WARN("fluid: failed to create the %dx%d thickness target; "
                        "the fluid pass will not draw", w, h);
        cached_w = 0;
        cached_h = 0;
        return;
    }
    cached_w = w;
    cached_h = h;
}

} // namespace

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
    imp.entity_vao.attrib_float(7, 1, 1, GL_FLOAT, false, offsetof(EntityInstance, shape_param));

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

    // ---- Swarmer pass. Attribute locations mirror swarmer.vert. ------------
    imp.max_swarmer_instances = math::max(desc_.max_swarmer_instances, 1u);
    if (!imp.swarmer_vao.create()) { error_ = "failed to create the swarmer VAO"; return false; }
    imp.swarmer_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.swarmer_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);
    const usize swarmer_bytes = static_cast<usize>(imp.max_swarmer_instances) *
                                sizeof(SwarmerGpuInstance) * kInstanceRegions;
    if (!imp.swarmer_instances.create_persistent(swarmer_bytes)) {
        error_ = "failed to allocate the swarmer instance buffer";
        return false;
    }
    imp.swarmer_vao.bind_vertex_buffer(1, imp.swarmer_instances, sizeof(SwarmerGpuInstance), 0, 1);
    imp.swarmer_vao.attrib_float(1, 1, 2, GL_FLOAT, false, offsetof(SwarmerGpuInstance, x));
    imp.swarmer_vao.attrib_float(2, 1, 2, GL_FLOAT, false, offsetof(SwarmerGpuInstance, vx));
    imp.swarmer_vao.attrib_float(3, 1, 1, GL_FLOAT, false, offsetof(SwarmerGpuInstance, radius));
    imp.swarmer_vao.attrib_float(4, 1, 1, GL_FLOAT, false, offsetof(SwarmerGpuInstance, phase));
    imp.swarmer_vao.attrib_float(5, 1, 4, GL_FLOAT, false, offsetof(SwarmerGpuInstance, r));
    imp.swarmer_vao.attrib_int(6, 1, 1, GL_UNSIGNED_INT, offsetof(SwarmerGpuInstance, flags));

    // ---- Fluid pass. Attribute locations mirror fluid.vert. ---------------
    // Sized from the swarmer cap rather than given its own knob: both are
    // "however many small sim-owned bodies can be alive", and SimDesc caps the
    // fluid store an order of magnitude below the swarmer store anyway.
    imp.max_fluid_instances = math::max(desc_.max_swarmer_instances, 1u);
    if (!imp.fluid_vao.create()) { error_ = "failed to create the fluid VAO"; return false; }
    imp.fluid_vao.bind_vertex_buffer(0, imp.quad_vbo, sizeof(Vec2), 0, 0);
    imp.fluid_vao.attrib_float(0, 0, 2, GL_FLOAT, false, 0);
    const usize fluid_bytes = static_cast<usize>(imp.max_fluid_instances) *
                              sizeof(FluidGpuInstance) * kInstanceRegions;
    if (!imp.fluid_instances.create_persistent(fluid_bytes)) {
        error_ = "failed to allocate the fluid instance buffer";
        return false;
    }
    imp.fluid_vao.bind_vertex_buffer(1, imp.fluid_instances, sizeof(FluidGpuInstance), 0, 1);
    imp.fluid_vao.attrib_float(1, 1, 2, GL_FLOAT, false, offsetof(FluidGpuInstance, x));
    imp.fluid_vao.attrib_float(2, 1, 2, GL_FLOAT, false, offsetof(FluidGpuInstance, vx));
    imp.fluid_vao.attrib_float(3, 1, 1, GL_FLOAT, false, offsetof(FluidGpuInstance, radius));
    imp.fluid_vao.attrib_float(4, 1, 1, GL_FLOAT, false, offsetof(FluidGpuInstance, density));
    imp.fluid_vao.attrib_float(5, 1, 1, GL_FLOAT, false, offsetof(FluidGpuInstance, fade));
    imp.fluid_vao.attrib_float(6, 1, 1, GL_FLOAT, false, offsetof(FluidGpuInstance, foam));

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

    // ---- Fluid thickness target ------------------------------------------
    // RGBA16F, not RGBA8: thickness is an unbounded additive sum (a hundred
    // overlapping droplets is a legitimate value) and the velocity channels are
    // signed. An 8-bit target would clip both, and clipped thickness means a
    // flat-topped surface with no normals in the middle of every puddle.
    ensure_fluid_target(imp.fluid_target, imp.fluid_target_w, imp.fluid_target_h,
                        desc.framebuffer_width, desc.framebuffer_height);

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
    imp.shaders.load_graphics("swarmer", "swarmer.vert", "swarmer.frag");
    imp.shaders.load_graphics("fluid", "fluid.vert", "fluid.frag");
    imp.shaders.load_graphics("fluid_composite", "fluid_composite.vert",
                              "fluid_composite.frag");
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
    if (!ready_) return;
    glViewport(0, 0, width, height);
    // The fluid thickness target is a screen-space buffer, so it has to be
    // rebuilt at the new size or the composite would sample a stale aspect.
    if (impl_) {
        ensure_fluid_target(impl_->fluid_target, impl_->fluid_target_w,
                            impl_->fluid_target_h, width, height);
    }
}

void Renderer::begin_frame(const Camera& camera, f32 alpha) {
    stats_ = FrameStats{};
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;

    imp.view_projection = camera.view_projection();
    imp.visible_bounds = camera.visible_bounds();
    imp.alpha = alpha;
    imp.time = static_cast<f32>(imp.clock.elapsed_seconds());

    // Host tissue substrate base colour (DESIGN.md §9.3 palette). submit_tissue
    // draws a proper vessel-shaped quad over this; the clear is the fallback
    // for anything the tissue quad doesn't cover (and for Wave-0-era callers
    // that never call submit_tissue at all). Kept in step with the darkest
    // interstitial value tissue.frag resolves to, so the seam at the level's
    // edge is invisible rather than a bright border.
    glClearColor(0.256f, 0.054f, 0.121f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::submit_tissue(const sim::TissueMask& mask, const sim::DistanceField& sdf,
                             f32 heartbeat_phase, const TissueDecor* decor) {
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;
    const ShaderProgram prog = imp.shaders.get("tissue");
    if (!prog.valid()) return;
    // Timed like every other pass: with `decor` supplied this function does
    // real per-frame CPU work (the flow-texture rebuild), and a cost that does
    // not show up in render_submit is a cost nobody notices growing.
    WallClock timer;

    const i32 w = sdf.width();
    const i32 h = sdf.height();
    if (w <= 0 || h <= 0) return;

    if (imp.tissue_tex_w != w || imp.tissue_tex_h != h) {
        if (!imp.tissue_sdf_tex.create(w, h, GL_R32F, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE)) return;
        imp.tissue_tex_w = w;
        imp.tissue_tex_h = h;
    }
    imp.tissue_sdf_tex.upload(sdf.data(), GL_RED, GL_FLOAT);

    const Rect world = mask.world_bounds();
    // A new level can reuse the same grid dimensions as the old one, in which
    // case nothing else below would notice the swap. Bounds are what actually
    // identify the level's geometry, so they are the invalidation trigger.
    if (world.min.x != imp.tissue_world.min.x || world.min.y != imp.tissue_world.min.y ||
        world.max.x != imp.tissue_world.max.x || world.max.y != imp.tissue_world.max.y) {
        imp.tissue_world = world;
        imp.flow_tex_valid = false;
        imp.lane_src = nullptr;
        imp.lane_tex_ready = false;
    }

    // ---- Flow texture ------------------------------------------------------
    // The flow field's own origin is private, but it is always baked from this
    // same mask, so the mask's world bounds are its extent, and its grid has
    // the same dimensions as the SDF's — which is why the downsample below can
    // index the raw arrays proportionally instead of sampling in world space.
    // That matters: bilinear FlowField::sample + sample_cost over 37k points
    // measured at 1.16 ms/frame, which would have made this pass the most
    // expensive thing in render_submit. Nearest reads off directions()/costs()
    // are a rounding error by comparison, and the GPU's own bilinear filter
    // plus the noise the result drives hide the difference completely.
    bool have_flow = false;
    if (decor != nullptr && decor->flow != nullptr && decor->flow->width() > 0 &&
        decor->flow->height() > 0) {
        const sim::FlowField& flow = *decor->flow;
        const i32 src_w = flow.width();
        const i32 src_h = flow.height();
        const i32 fw = math::min(src_w, kFlowTexMax);
        const i32 fh = math::min(src_h, kFlowTexMax);
        if (imp.flow_tex_w != fw || imp.flow_tex_h != fh) {
            if (imp.tissue_flow_tex.create(fw, fh, GL_RGBA16F, GL_LINEAR, GL_LINEAR,
                                           GL_CLAMP_TO_EDGE)) {
                imp.flow_tex_w = fw;
                imp.flow_tex_h = fh;
                imp.flow_tex_valid = false;
            }
        }

        // Rebuild only when the field actually changed. A FlowField is baked at
        // level load and re-solved only when a tower blocks or unblocks a lane,
        // so on the overwhelming majority of frames this whole block is a few
        // comparisons. It exposes no version counter, but pump_rebake() returns
        // before touching `stats` when nothing is dirty (sim/flowfield/
        // FlowField.cpp), which makes the stats triple a serviceable one:
        // a rebake always moves it.
        //
        // WAIT FOR THE REBAKE TO FINISH. A rebake is amortised across frames
        // against SimWorld's flow_rebake_budget_ms, which defaults to half a
        // millisecond -- so re-solving a floodplain-sized dirty region runs for
        // dozens of frames. This condition used to include has_pending_rebake()
        // directly, which meant every one of those frames rebuilt the whole
        // texture from a HALF-SOLVED Dijkstra sweep: costs and directions that
        // are partly new and partly stale, with a different `max_cost` each
        // time, so `to_goal` renormalised globally every frame. The lane's
        // striations and systolic banding boiled for the better part of a
        // second after every tower placement, and each of those frames paid the
        // multi-millisecond rebuild on top of the sim's own re-solve.
        //
        // Latch the request instead and service it once, on the first frame the
        // field is whole again. The plasma flows along the previous routing in
        // the meantime, which is exactly the "cosmetic, self-corrects" tradeoff
        // this cache was always documented as making.
        const sim::RebakeStats& rs = flow.stats();
        const bool pending = flow.has_pending_rebake();
        if (pending) imp.flow_rebuild_queued = true;
        const bool changed = !imp.flow_tex_valid ||
                             imp.flow_rebuild_queued ||
                             rs.last_bake_ms != imp.flow_bake_ms ||
                             rs.cells_visited != imp.flow_cells_visited ||
                             rs.regions_processed != imp.flow_regions;

        if (imp.flow_tex_w == fw && imp.flow_tex_h == fh && changed && !pending) {
            const usize texels = static_cast<usize>(fw) * static_cast<usize>(fh);
            imp.flow_scratch.resize(texels * 4);
            imp.flow_valid.assign(texels, 0);
            const Vec2* dirs = flow.directions();
            const f32* costs = flow.costs();
            f32 max_cost = 0.0f;
            for (i32 y = 0; y < fh; ++y) {
                // Centre-aligned pick. `y * src_h / fh` takes the top-left
                // corner of each coarse cell, which shifts the whole field half
                // a cell up and left against the SDF the shader samples with
                // the same uv -- small, but it is a systematic misregistration
                // between a lumen and the plasma flowing inside it.
                const i32 sy = math::min((2 * y + 1) * src_h / (2 * fh), src_h - 1);
                for (i32 x = 0; x < fw; ++x) {
                    const i32 sx = math::min((2 * x + 1) * src_w / (2 * fw), src_w - 1);
                    const usize src = static_cast<usize>(sy) * static_cast<usize>(src_w) +
                                      static_cast<usize>(sx);
                    const usize texel = static_cast<usize>(y) * static_cast<usize>(fw) +
                                        static_cast<usize>(x);
                    const usize dst = texel * 4;
                    imp.flow_scratch[dst + 0] = dirs[src].x;
                    imp.flow_scratch[dst + 1] = dirs[src].y;
                    // Unreachable cells hold infinity; park them at -1 and
                    // resolve to "maximally far" once the scale is known.
                    const f32 cost = costs[src];
                    const bool finite = cost < 1e30f && cost == cost;
                    imp.flow_scratch[dst + 2] = finite ? cost : -1.0f;
                    if (finite) max_cost = math::max(max_cost, cost);
                    const bool has_dir = math::length_sq(dirs[src]) > 1e-4f;
                    imp.flow_valid[texel] = (finite && has_dir) ? u8{1} : u8{0};
                    // Coherence seed: 1 where the field is real guidance, 0
                    // where there is none. It is blurred alongside everything
                    // else below, which turns it into a smooth "how far should
                    // the plasma trust this direction" weight instead of a
                    // binary mask with a hard edge in it.
                    imp.flow_scratch[dst + 3] = (finite && has_dir) ? 1.0f : 0.0f;
                }
            }

            // ---- Close the small holes a placed tower punches in the field --
            // A tower blocks a square of the mask, so after the rebake those
            // cells are *unreachable*: zero direction and infinite cost. That
            // is correct navigation data and completely wrong visual data — the
            // shader falls back to a fixed direction where the flow is zero, so
            // the plasma streamlines snap to +x and the systolic phase jumps,
            // painting a rounded square of visibly broken lane around every
            // tower. (Diagnosed the hard way: the SDF is baked once at level
            // load and never sees a footprint at all, so this pass's *only*
            // knowledge of a tower is through the flow field.)
            //
            // Fix: treat those cells as holes and dilate the surrounding field
            // into them, so the lane simply flows over the footprint and the
            // tower sprite is the sole thing marking it. The pass count is
            // deliberately small — it is sized to swallow a footprint (the
            // largest is ~4.8 world units across) and nothing more, so the
            // genuinely unreachable interstitium outside the vessels stays
            // unfilled. Nothing samples flow out there, and flooding the whole
            // map would be both pointless and much more expensive.
            constexpr i32 kHoleFillPasses = 6;
            std::vector<f32> next = imp.flow_scratch;
            std::vector<u8> next_valid = imp.flow_valid;
            for (i32 pass = 0; pass < kHoleFillPasses; ++pass) {
                bool filled_any = false;
                for (i32 y = 0; y < fh; ++y) {
                    for (i32 x = 0; x < fw; ++x) {
                        const usize texel = static_cast<usize>(y) * static_cast<usize>(fw) +
                                            static_cast<usize>(x);
                        if (imp.flow_valid[texel] != 0) continue;
                        f32 sx_sum = 0.0f, sy_sum = 0.0f, c_sum = 0.0f;
                        i32 hits = 0;
                        const i32 dx[4] = {-1, 1, 0, 0};
                        const i32 dy[4] = {0, 0, -1, 1};
                        for (i32 k = 0; k < 4; ++k) {
                            const i32 nx = x + dx[k];
                            const i32 ny = y + dy[k];
                            if (nx < 0 || ny < 0 || nx >= fw || ny >= fh) continue;
                            const usize nt = static_cast<usize>(ny) * static_cast<usize>(fw) +
                                             static_cast<usize>(nx);
                            if (imp.flow_valid[nt] == 0) continue;
                            sx_sum += imp.flow_scratch[nt * 4 + 0];
                            sy_sum += imp.flow_scratch[nt * 4 + 1];
                            c_sum += imp.flow_scratch[nt * 4 + 2];
                            ++hits;
                        }
                        if (hits == 0) continue;
                        const f32 inv = 1.0f / static_cast<f32>(hits);
                        next[texel * 4 + 0] = sx_sum * inv;
                        next[texel * 4 + 1] = sy_sum * inv;
                        next[texel * 4 + 2] = c_sum * inv;
                        // Invented, not measured. The cell joins the field so
                        // the blur has something continuous to work with, but
                        // it carries zero coherence, so the striations fade
                        // across a footprint rather than being drawn
                        // confidently along a direction nobody computed.
                        next[texel * 4 + 3] = 0.0f;
                        next_valid[texel] = 1;
                        filled_any = true;
                    }
                }
                imp.flow_scratch = next;
                imp.flow_valid = next_valid;
                if (!filled_any) break;
            }

            // ---- Smooth it into something fluid -----------------------------
            // The flow field is a *pathfinding* product and looks like one. Its
            // directions come from the gradient of a Dijkstra sweep on an
            // 8-connected grid, so they are quantised into staircases; and
            // placing a tower re-solves only a dirty region, which FlowField.h
            // documents as an approximation ("correct as long as the true
            // shortest path leaves and re-enters the region at most once").
            // Around a footprint that approximation leaves a wedge of cells
            // pointing sideways or briefly backwards, plus a near-zero
            // stagnation seam.
            //
            // None of that hurts steering — an agent crossing a few odd cells
            // just curves — but the LIC integrates along these vectors, so it
            // renders that wedge as a hard chevron scratched into the lane
            // beside every tower. Plasma does not need cell-accurate direction;
            // it needs a smooth, plausible one. So blur, using only in-lane
            // samples (the valid mask stops the wall's zeroes bleeding in) and
            // renormalising each pass so a convergence cannot cancel the field
            // to nothing. This also dissolves the 8-way staircase, which is
            // worth having on its own.
            //
            // Twelve adjacent 3-taps is a Gaussian of sigma ~2.4 texels, and
            // that is the wrong order of magnitude for what is actually wrong
            // with this field. The 8-connected gradient does not produce
            // per-texel noise; it produces BROAD regions that all agree on one
            // of eight compass directions, meeting along a sharp crease. A
            // 2-texel blur turns that crease into a 2-texel ramp, which the LIC
            // still renders as a visible fold running across the lane. The
            // defect is tens of texels wide, so the filter has to be too.
            //
            // Reaching sigma ~10 with adjacent taps costs ~200 sweeps. A-trous
            // gets there in twelve by doubling the tap spacing (the same trick
            // the agreement estimate below uses, and for the same reason): the
            // strides ascend so every strided pass reads an already-smoothed
            // field, then descend again so the last passes fill in the detail
            // the wide ones stepped over.
            //
            // The one hazard a strided tap has and an adjacent one does not is
            // that it can step clean OVER a vessel wall and average a lane with
            // its neighbour flowing the other way. `imp.flow_valid` alone does
            // not catch that -- both endpoints are perfectly valid lane cells,
            // it is the wall in between that matters -- so a strided tap also
            // has to check that the cells it skipped are in-lane. Walls are ~3
            // texels thick at this resolution, so this rejects every jump that
            // leaves the vessel while costing only a handful of array reads.
            const i32 kSmoothStrides[] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 2, 1, 1};
            const auto path_clear = [&](i32 x, i32 y, i32 dx, i32 dy, i32 stride) {
                for (i32 step = 1; step < stride; ++step) {
                    const i32 mx = x + dx * step;
                    const i32 my = y + dy * step;
                    if (mx < 0 || my < 0 || mx >= fw || my >= fh) return false;
                    if (imp.flow_valid[static_cast<usize>(my) * static_cast<usize>(fw) +
                                       static_cast<usize>(mx)] == 0) {
                        return false;
                    }
                }
                return true;
            };
            for (const i32 stride : kSmoothStrides) {
                for (i32 axis = 0; axis < 2; ++axis) {
                    const i32 sx = (axis == 0) ? 1 : 0;
                    const i32 sy = (axis == 0) ? 0 : 1;
                    for (i32 y = 0; y < fh; ++y) {
                        for (i32 x = 0; x < fw; ++x) {
                            const usize texel = static_cast<usize>(y) * static_cast<usize>(fw) +
                                                static_cast<usize>(x);
                            f32 ax = 0.0f, ay = 0.0f, ac = 0.0f, ak = 0.0f, wsum = 0.0f;
                            if (imp.flow_valid[texel] != 0) {
                                for (i32 k = -1; k <= 1; ++k) {
                                    const i32 nx = x + k * sx * stride;
                                    const i32 ny = y + k * sy * stride;
                                    if (nx < 0 || ny < 0 || nx >= fw || ny >= fh) continue;
                                    const usize nt = static_cast<usize>(ny) *
                                                     static_cast<usize>(fw) +
                                                     static_cast<usize>(nx);
                                    if (imp.flow_valid[nt] == 0) continue;
                                    if (k != 0 && !path_clear(x, y, k * sx, k * sy, stride)) {
                                        continue;
                                    }
                                    const f32 kw = (k == 0) ? 2.0f : 1.0f;
                                    ax += imp.flow_scratch[nt * 4 + 0] * kw;
                                    ay += imp.flow_scratch[nt * 4 + 1] * kw;
                                    ac += imp.flow_scratch[nt * 4 + 2] * kw;
                                    ak += imp.flow_scratch[nt * 4 + 3] * kw;
                                    wsum += kw;
                                }
                            }
                            if (wsum <= 0.0f) {
                                // Invalid, or fully surrounded by invalid: pass
                                // it through untouched, with no coherence.
                                next[texel * 4 + 0] = imp.flow_scratch[texel * 4 + 0];
                                next[texel * 4 + 1] = imp.flow_scratch[texel * 4 + 1];
                                next[texel * 4 + 2] = imp.flow_scratch[texel * 4 + 2];
                                next[texel * 4 + 3] = 0.0f;
                            } else {
                                const f32 inv = 1.0f / wsum;
                                next[texel * 4 + 0] = ax * inv;
                                next[texel * 4 + 1] = ay * inv;
                                next[texel * 4 + 2] = ac * inv;
                                next[texel * 4 + 3] = ak * inv;
                            }
                        }
                    }
                    // Every texel was written, so this cannot carry stale data.
                    imp.flow_scratch.swap(next);
                }
                for (usize t = 0; t < texels; ++t) {
                    if (imp.flow_valid[t] == 0) continue;
                    const f32 vx = imp.flow_scratch[t * 4 + 0];
                    const f32 vy = imp.flow_scratch[t * 4 + 1];
                    const f32 len = std::sqrt(vx * vx + vy * vy);
                    if (len < 1e-4f) continue; // leave a true convergence alone
                    imp.flow_scratch[t * 4 + 0] = vx / len;
                    imp.flow_scratch[t * 4 + 1] = vy / len;
                }
            }

            // ---- Directional agreement ---------------------------------
            // Some places have no single plausible flow direction at all, and
            // no amount of smoothing invents one: the objective, which the
            // whole field points AT from every side; a vessel's dead-end cap;
            // the wedge an incremental re-solve leaves beside a tower. The LIC
            // in tissue.frag integrates along whatever vector it is handed, so
            // at those points it drew a starburst of streamlines radiating out
            // of one texel -- easily the loudest artifact on an untouched lane.
            //
            // Detect them by blurring a COPY of the unit directions WITHOUT
            // renormalising and taking the magnitude. Averaging unit vectors
            // gives ~1 where they agree and collapses to ~0 where they fan out,
            // and because the estimate is built by repeated blurring, a defect
            // spreads a wide, soft halo of low agreement around itself rather
            // than a single dark texel. That halo is exactly the region the
            // striations have to give up on, so it is what the coherence
            // channel needs to carry.
            //
            // Note this is *not* the same as reading the length back out of the
            // smoothing loop above, which renormalises after every pass and so
            // throws the measurement away as fast as it accumulates.
            {
                // A-trous strides. The defect this has to find is not a single
                // bad texel -- a cap fans its directions across a good 15-texel
                // radius -- and a stack of adjacent 3-taps grows its support
                // only as sqrt(passes), so reaching that radius directly would
                // take of order two hundred sweeps on every tower placement.
                // Doubling the tap spacing instead reaches a ~9-texel sigma in
                // eight, and a coherence estimate is exactly the kind of smooth
                // low-frequency quantity that does not care about the aliasing
                // a strided kernel would introduce in an image.
                const i32 kAgreeStrides[] = {1, 2, 4, 8, 1, 2, 4, 8};
                std::vector<f32> agree(texels * 2);
                for (usize t = 0; t < texels; ++t) {
                    agree[t * 2 + 0] = imp.flow_scratch[t * 4 + 0];
                    agree[t * 2 + 1] = imp.flow_scratch[t * 4 + 1];
                }
                std::vector<f32> agree_next(texels * 2);
                for (const i32 stride : kAgreeStrides) {
                    for (i32 axis = 0; axis < 2; ++axis) {
                        const i32 sx = (axis == 0) ? stride : 0;
                        const i32 sy = (axis == 0) ? 0 : stride;
                        for (i32 y = 0; y < fh; ++y) {
                            for (i32 x = 0; x < fw; ++x) {
                                const usize t = static_cast<usize>(y) *
                                                static_cast<usize>(fw) + static_cast<usize>(x);
                                f32 ax = 0.0f, ay = 0.0f, wsum = 0.0f;
                                if (imp.flow_valid[t] != 0) {
                                    for (i32 k = -1; k <= 1; ++k) {
                                        const i32 nx = x + k * sx;
                                        const i32 ny = y + k * sy;
                                        if (nx < 0 || ny < 0 || nx >= fw || ny >= fh) continue;
                                        const usize nt = static_cast<usize>(ny) *
                                                         static_cast<usize>(fw) +
                                                         static_cast<usize>(nx);
                                        if (imp.flow_valid[nt] == 0) continue;
                                        const f32 kw = (k == 0) ? 2.0f : 1.0f;
                                        ax += agree[nt * 2 + 0] * kw;
                                        ay += agree[nt * 2 + 1] * kw;
                                        wsum += kw;
                                    }
                                }
                                const f32 inv = (wsum > 0.0f) ? 1.0f / wsum : 0.0f;
                                agree_next[t * 2 + 0] = ax * inv;
                                agree_next[t * 2 + 1] = ay * inv;
                            }
                        }
                        agree.swap(agree_next);
                    }
                }
                // Shaped here rather than in the shader so the thresholds sit
                // next to the process that produced the number. A lane's body
                // measures ~0.95 even through a bend; a fan measures well under
                // 0.6, and the band between is where the striations hand over.
                // The band is high and narrow, because the numbers here are
                // high and close together. Direction spread maps to mean length
                // as sin(t)/t, so even a hard bend that swings 40 degrees across
                // the whole window still measures 0.98 -- while a cap, whose
                // directions fan across 120 degrees or more, measures 0.83. It
                // is the region above 0.97 that is "a lane", not the region
                // above a half.
                for (usize t = 0; t < texels; ++t) {
                    const f32 a = std::sqrt(agree[t * 2 + 0] * agree[t * 2 + 0] +
                                            agree[t * 2 + 1] * agree[t * 2 + 1]);
                    const f32 shaped = math::saturate((a - 0.85f) / 0.12f);
                    imp.flow_scratch[t * 4 + 3] *= shaped * shaped * (3.0f - 2.0f * shaped);
                }
            }

            const f32 inv_cost = 1.0f / math::max(max_cost, 1e-4f);
            for (usize i = 2; i < imp.flow_scratch.size(); i += 4) {
                const f32 c = imp.flow_scratch[i];
                imp.flow_scratch[i] = (c < 0.0f) ? 1.0f : math::saturate(c * inv_cost);
            }
            imp.tissue_flow_tex.upload(imp.flow_scratch.data(), GL_RGBA, GL_FLOAT);
            imp.flow_tex_valid = true;
            imp.flow_rebuild_queued = false;
            imp.flow_bake_ms = rs.last_bake_ms;
            imp.flow_cells_visited = rs.cells_visited;
            imp.flow_regions = rs.regions_processed;
        }
        have_flow = imp.flow_tex_valid;
    }

    // ---- Lane-hue texture (built once per level; see build_lane_tint) ------
    if (decor != nullptr && decor->lane_owner != nullptr && decor->lane_width > 0 &&
        decor->lane_height > 0 && decor->lane_owner != imp.lane_src) {
        const i32 lw = math::min(decor->lane_width, kLaneTexMax);
        const i32 lh = math::min(decor->lane_height, kLaneTexMax);
        std::vector<u8> pixels;
        build_lane_tint(*decor, lw, lh, pixels);
        imp.lane_tex_ready =
            imp.tissue_lane_tex.create(lw, lh, GL_RGBA8, GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE);
        if (imp.lane_tex_ready) imp.tissue_lane_tex.upload(pixels.data(), GL_RGBA, GL_UNSIGNED_BYTE);
        imp.lane_src = decor->lane_owner;
        imp.lane_src_w = decor->lane_width;
        imp.lane_src_h = decor->lane_height;
    }
    const bool have_lane = imp.lane_tex_ready && decor != nullptr &&
                           decor->lane_owner == imp.lane_src;

    glUseProgram(prog.gl_id);
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
    const Rect vis = imp.visible_bounds;
    glUniform2f(1, vis.min.x, vis.min.y);
    glUniform2f(2, vis.size().x, vis.size().y);
    glUniform2f(3, world.min.x, world.min.y);
    glUniform2f(4, world.size().x, world.size().y);
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
    // Raw seconds, kept separate from the phase above: the plasma advection and
    // the drifting corpuscles want a linear time, not something a caller may
    // have offset per-lane.
    glUniform1f(6, imp.time);
    glUniform1f(7, have_flow ? 1.0f : 0.0f);
    glUniform1f(8, have_lane ? 1.0f : 0.0f);

    imp.tissue_sdf_tex.bind_unit(0);
    if (have_flow) imp.tissue_flow_tex.bind_unit(1);
    if (have_lane) imp.tissue_lane_tex.bind_unit(2);
    imp.screen_quad_vao.bind();
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    ++stats_.draw_calls;
    stats_.submit_ms += timer.elapsed_ms();
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
    params.lod_blob_enabled = desc_.lod_blob_enabled;
    params.lod_blob_threshold = desc_.lod_blob_threshold;
    params.lod_blob_full = desc_.lod_blob_full;
    params.per_family_capacity = per_family_cap;
    params.time = imp.time;
    params.cull_enabled = false;

    imp.density_grid.set_extent(imp.visible_bounds);
    imp.density_grid.clear();

    // Null density grid when the pass is off: the batcher then skips the splat
    // rather than filling a texture nobody is going to sample.
    const ChaffBatchResult result =
        build_chaff_batches(chaff, occ, params, region_base,
                            desc_.lod_blob_enabled ? &imp.density_grid : nullptr);

    if (result.instances_dropped > 0) {
        IMMUNE_LOG_WARN("chaff render: dropped %u instances (a family exceeded "
                        "max_chaff_instances=%u)",
                        result.instances_dropped, per_family_cap);
    }

    // 320x180 RGBA32F is ~900 KB across the bus every frame; not worth paying
    // for a pass that is not going to draw.
    if (desc_.lod_blob_enabled) {
        imp.density_tex.upload(imp.density_grid.data(), GL_RGBA, GL_FLOAT);
    }

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
    if (desc_.lod_blob_enabled && blob_prog.valid() && result.blob_mass > 0.0f) {
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
        inst.shape_param = 0.0f;

        // Every tower body spends its tier on a countable feature — the
        // Macrophage's phagosomes, the Interferon crystal's reach, the
        // Cytotoxic T's microvilli, the Goblet Cell's granules, the NK Cell's
        // blades — so an upgrade is legible from the silhouette alone rather
        // than only from the stat panel.
        //
        // What travels is the RAW tier, not any one shape's derived count.
        // entity.frag turns it into blades (2 + tier) or antibodies (2 + tier)
        // or whatever else at the point of use, which keeps the "what does a
        // tier look like" decision in the shader that draws it — the NK Cell's
        // blade count in particular has to agree with the rotor-sweep particle
        // burst in vfx/Particles.cpp, and one owner for that formula is one
        // fewer place for the two to drift apart.
        //
        // Sourced from comp::Tower every frame rather than baked into the
        // sprite at upgrade time, for the same no-drift reason.
        if (const auto* tower = registry.try_get<const sim::comp::Tower>(entity)) {
            inst.shape_param = static_cast<f32>(tower->tier);
        }

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
                burst.shape_param = 0.0f;
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
            diamond.shape_param = 0.0f;

            EntityInstance& ring = region_base[count++];
            ring.x = tg.point.x;
            ring.y = tg.point.y;
            ring.scale = tg.radius * 2.0f; // scale == diameter, matches entity.vert's convention
            ring.rotation = 0.0f;
            ring.tint_rgba8 = pack_rgba8(ring_tint);
            ring.shape_id = 3; // entity.frag: telegraph countdown ring
            ring.anim_phase = math::saturate(tg.progress); // repurposed as progress, not a phase
            ring.shape_param = 0.0f;
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

    // A field is tinted with the IDENTITY HUE OF THE TOWER THAT CASTS IT, so a
    // tower's body, its particles and the AoE it puts on the ground are all one
    // colour. These values mirror palette_for()'s `primary` in
    // vfx/Particles.cpp entry for entry.
    //
    // Before this they were an unrelated per-shape palette, and it actively
    // fought the roster's own legibility rule (DESIGN.md §9.3): the Interferon
    // is the cyan tower and its cone rendered PINK, and the tower in slot 4 was
    // green while its attack rendered VIOLET. The player's only cheap "who is
    // shooting" channel is hue, and half the roster was spending it saying
    // something different in two places at once.
    //
    // Shape maps to tower one-to-one across the current roster, so the tower
    // does not have to be looked up: only the Interferon casts Cones, and Chain
    // is the Cytotoxic T (the Complement Cascade
    // ABILITY also resolves through Chain, and reading as a T-Cell discharge is
    // the right answer there — it is the same mechanism fired by the player).
    // Circle is the one genuine ambiguity and is split below by lifetime.
    const Vec4 kMortarTint{1.00f, 0.66f, 0.24f, 1.0f};  // Macrophage — amber
    const Vec4 kBladeTint{1.00f, 0.52f, 0.86f, 1.0f};   // NK Cell    — magenta
    // No tower casts a Rect any more -- the Goblet Cell that replaced the old
    // beam publishes no field at all. Kept because DamageField::Rect is still a
    // shape a future caster (or a scripted hazard) may submit, and an unhandled
    // shape would render untinted.
    const Vec4 kRectTint{0.62f, 1.00f, 0.80f, 1.0f};    // unclaimed  — pale green
    const Vec4 kConeTint{0.52f, 0.84f, 1.00f, 1.0f};    // Interferon — cyan
    const Vec4 kChainTint{0.76f, 0.66f, 1.00f, 1.0f};   // Cytotoxic T— violet
    // friendly_fire fields override to a hot warning colour regardless of
    // shape, since those damage the player.
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
            // arc_cos is dead weight for a Rect, so it carries the box's
            // ASPECT instead: field.frag needs to know which of the two axes
            // the beam runs along to put its hot centreline down the right one,
            // and the normalized local frame it shades in has lost that.
            inst.arc_cos = inst.scale_x / inst.scale_y;
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
            // Circle is the one shape two towers share, so it splits by
            // lifetime — the only thing that distinguishes them here, and it
            // happens to distinguish them cleanly. A PERSISTENT circle is the
            // NK Cell's rotor disc, permanently on and pinned to a tower; a
            // TIMED one is a Macrophage shell landing (or a Histamine Flare,
            // which is a nova and should read like one). Drawing both as the
            // same steady toxin cloud was why a mortar hit had no punch.
            inst.shape_id = (f.lifetime <= 0.0f) ? 4u : 0u;
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

        Vec4 tint = kMortarTint;
        switch (f.shape) {
        case sim::FieldShape::Rect:  tint = kRectTint;  break;
        case sim::FieldShape::Cone:  tint = kConeTint;  break;
        case sim::FieldShape::Chain: tint = kChainTint; break;
        case sim::FieldShape::Circle:
        default:
            // Same persistent-vs-timed split the shape id above makes.
            tint = (f.lifetime <= 0.0f) ? kBladeTint : kMortarTint;
            break;
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


void Renderer::submit_swarmers(const sim::SwarmerBuffers& swarmers) {
    // The Cytotoxic T's granules. Drawn after the projectile pass and before
    // the particles, for the same reason rounds are: these are matter, and the
    // additive cosmetic layer should composite on top of them.
    const usize count = swarmers.count();
    stats_.swarmer_instances_drawn = static_cast<u32>(count);
    if (!ready_ || !impl_ || count == 0) return;
    Impl& imp = *impl_;
    WallClock timer;

    const u32 draw_count = math::min(static_cast<u32>(count), imp.max_swarmer_instances);

    imp.swarmer_fence.wait(imp.swarmer_region);
    SwarmerGpuInstance* base = imp.swarmer_instances.mapped_as<SwarmerGpuInstance>() +
        static_cast<usize>(imp.swarmer_region) * imp.max_swarmer_instances;

    // The Cytotoxic T's violet, matching both the tower body (entity.frag) and
    // the palette the VFX layer uses for the same tower. Granules carry a tier
    // in visual_id but not a TowerType, and today only this tower releases
    // them, so the hue is a constant here rather than a per-granule lookup.
    for (u32 i = 0; i < draw_count; ++i) {
        SwarmerGpuInstance inst{};
        inst.x = swarmers.pos_x[i];
        inst.y = swarmers.pos_y[i];
        inst.vx = swarmers.vel_x[i];
        inst.vy = swarmers.vel_y[i];

        // Size tracks tier, and every granule is small on purpose: the read is
        // "there are a lot of them", which a bigger sprite actively destroys.
        const f32 tier = static_cast<f32>(math::clamp<u16>(swarmers.visual_id[i], 1u, 3u));
        inst.radius = 0.20f + 0.035f * tier;

        // Hashed off the slot so the cloud does not pulse in lockstep, same
        // rationale as ChaffInstance::anim_phase and the projectile pass.
        const u32 h = (swarmers.seed[i] * 2654435761u) ^ 0x85EBCA6Bu;
        inst.phase = static_cast<f32>(h & 0xFFFFu) * (math::kTwoPi / 65536.0f);

        // Fade the last half-second of life instead of popping out. Granules
        // dissolve constantly, and a cloud where dozens blink out per second
        // reads as flicker rather than as turnover.
        const f32 fade = math::saturate(swarmers.life[i] * 2.0f);
        inst.r = 0.78f; inst.g = 0.68f; inst.b = 1.0f;
        inst.a = 0.55f + 0.45f * fade;

        inst.flags = (swarmers.flags[i] & sim::swarmer_flags::kAttached) != 0 ? 1u : 0u;
        base[i] = inst;
    }

    const ShaderProgram prog = imp.shaders.get("swarmer");
    if (prog.valid()) {
        glUseProgram(prog.gl_id);
        glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
        glUniform1f(1, imp.time);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        imp.swarmer_vao.bind();
        glDrawArraysInstancedBaseInstance(GL_TRIANGLE_FAN, 0, 4, static_cast<GLsizei>(draw_count),
                                          imp.swarmer_region * imp.max_swarmer_instances);
        ++stats_.draw_calls;
    }

    imp.swarmer_fence.signal(imp.swarmer_region);
    imp.swarmer_region = (imp.swarmer_region + 1) % kInstanceRegions;
    stats_.submit_ms += timer.elapsed_ms();
}
void Renderer::submit_fluid(const sim::FluidBuffers& fluid, f32 particle_radius) {
    const usize count = fluid.count();
    stats_.fluid_instances_drawn = static_cast<u32>(count);
    if (!ready_ || !impl_ || count == 0) return;
    Impl& imp = *impl_;
    if (!imp.fluid_target.valid()) return;
    const ShaderProgram splat = imp.shaders.get("fluid");
    const ShaderProgram composite = imp.shaders.get("fluid_composite");
    if (!splat.valid() || !composite.valid()) return;
    WallClock timer;

    const u32 draw_count = math::min(static_cast<u32>(count), imp.max_fluid_instances);

    imp.fluid_fence.wait(imp.fluid_region);
    FluidGpuInstance* base = imp.fluid_instances.mapped_as<FluidGpuInstance>() +
        static_cast<usize>(imp.fluid_region) * imp.max_fluid_instances;

    const f32 radius = math::max(particle_radius, 0.01f);
    for (u32 i = 0; i < draw_count; ++i) {
        FluidGpuInstance inst{};
        inst.x = fluid.pos_x[i];
        inst.y = fluid.pos_y[i];
        // The last substep's actual displacement, not the velocity: at 180 Hz
        // substeps that is already the per-substep motion, which is exactly the
        // length a motion streak should be. Deriving it from velocity would
        // need the substep count, which render/ has no business knowing.
        inst.vx = fluid.pos_x[i] - fluid.prev_x[i];
        inst.vy = fluid.pos_y[i] - fluid.prev_y[i];
        inst.radius = radius;
        inst.density = fluid.density[i];

        // Fade over the last stretch of life. The solver stops depositing
        // damage over the same window (see kFadeWindow in Fluid.cpp), so what
        // the player sees dissolving really has stopped burning.
        inst.fade = math::saturate(fluid.life[i] * 3.0f);

        // Foam: fluid that has been churned. A particle that has hit something
        // stays marked, and one currently scraping along tissue counts double,
        // because that is where a real splash goes white and bubbly. This is
        // the whole reason a splash reads differently from the clean beam that
        // caused it.
        f32 foam = (fluid.flags[i] & sim::fluid_flags::kSplashed) != 0 ? 0.55f : 0.0f;
        if ((fluid.flags[i] & sim::fluid_flags::kOnWall) != 0) foam = 1.0f;
        inst.foam = foam;
        base[i] = inst;
    }

    // ---- Pass 1: accumulate thickness into the offscreen target ------------
    // Pure addition, no alpha: this is a field being summed, not a picture
    // being composited, and every particle must contribute its full weight
    // regardless of draw order.
    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    imp.fluid_target.bind();
    glViewport(0, 0, imp.fluid_target_w, imp.fluid_target_h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    glUseProgram(splat.gl_id);
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(imp.view_projection));
    glUniform1f(1, imp.time);
    imp.fluid_vao.bind();
    glDrawArraysInstancedBaseInstance(GL_TRIANGLE_FAN, 0, 4, static_cast<GLsizei>(draw_count),
                                      imp.fluid_region * imp.max_fluid_instances);
    ++stats_.draw_calls;

    // ---- Pass 2: threshold it into a surface and shade it ------------------
    gl::ColorTarget::bind_default();
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

    // PREMULTIPLIED alpha, and this matters: it lets the specular highlight
    // write light beyond what the surface's own opacity would allow, so a thin
    // sheet of mucus can still throw a hard glint. Straight alpha blending caps
    // every highlight at the coverage that carries it, and the surface goes
    // flat and plastic exactly where it should look wettest.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(composite.gl_id);
    glUniform2f(0, 1.0f / static_cast<f32>(imp.fluid_target_w),
                1.0f / static_cast<f32>(imp.fluid_target_h));
    glUniform1f(1, imp.time);
    imp.fluid_target.color().bind_unit(0);
    imp.screen_quad_vao.bind();
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    ++stats_.draw_calls;

    // Leave the pipeline on standard alpha so a later pass never inherits
    // premultiplied blending by accident.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    imp.fluid_fence.signal(imp.fluid_region);
    imp.fluid_region = (imp.fluid_region + 1) % kInstanceRegions;
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

    const Rect vis = imp.visible_bounds;
    const Vec2 span = vis.size();
    if (span.x <= 0.0f || span.y <= 0.0f || flow.cell_size() <= 0.0f) return;

    // SPACING IS IN PIXELS, NOT CELLS. Stepping by a multiple of the cell size
    // ties arrow density to the level's grid resolution, so a 0.5-unit-cell
    // level drew thousands of overlapping 6-pixel arrows -- a hatch pattern,
    // not a field. Deriving the step from the viewport keeps the overlay
    // legible at any zoom, and clamping to the cell size stops it from
    // claiming more resolution than the data has.
    constexpr f32 kSpacingPx = 26.0f;
    constexpr usize kMaxArrows = 6000;
    const f32 world_per_px = span.y / static_cast<f32>(math::max(desc_.framebuffer_height, 1));
    f32 step = math::max(kSpacingPx * world_per_px, flow.cell_size());

    // Snap the lattice to world space rather than to the visible rect, or every
    // arrow slides continuously under a panning camera and the whole field
    // shimmers. Halving density (rather than clipping the list) keeps a
    // zoomed-out view an honest, if coarser, picture of the same field.
    auto lattice_count = [&](f32 s) {
        return (static_cast<usize>(span.x / s) + 2) * (static_cast<usize>(span.y / s) + 2);
    };
    while (lattice_count(step) > kMaxArrows) step *= 2.0f;

    const f32 x0 = std::floor(vis.min.x / step) * step;
    const f32 y0 = std::floor(vis.min.y / step) * step;

    // Head geometry: two barbs swept back from the tip. The overlay previously
    // emitted one, which read as a stray tick rather than an arrowhead and was
    // most of why the field looked like noise.
    const f32 arrow_len = step * 0.62f;
    const f32 head_len = arrow_len * 0.34f;
    const f32 head_half = arrow_len * 0.20f;
    constexpr Vec4 kColor{0.55f, 0.95f, 1.0f, 0.75f};
    /// Below this much of the bilinear stencil the sample is mostly outside the
    /// lumen; its direction is an extrapolation, so it is not drawn at all.
    constexpr f32 kMinSupport = 0.35f;

    std::vector<FlowDebugVertex>& verts = imp.flow_verts;
    verts.clear();
    verts.reserve(lattice_count(step) * 6);

    for (f32 y = y0; y <= vis.max.y + step; y += step) {
        for (f32 x = x0; x <= vis.max.x + step; x += step) {
            const Vec2 p{x, y};
            f32 support = 0.0f;
            const Vec2 dir = flow.sample_with_support(p, support);
            if (math::length_sq(dir) < 1e-6f) continue;
            if (support < kMinSupport) continue;

            // Fade the last stretch to the wall instead of stopping dead: an
            // abrupt cutoff at the lumen edge is itself read as an artifact.
            Vec4 color = kColor;
            color.a *= math::saturate((support - kMinSupport) / (1.0f - kMinSupport));

            // Centred on the sample point, so the lattice reads as a field
            // rather than as ticks hanging off their own grid corners.
            const Vec2 perp{-dir.y, dir.x};
            const Vec2 tip = p + dir * (arrow_len * 0.5f);
            const Vec2 tail = p - dir * (arrow_len * 0.5f);
            const Vec2 base = tip - dir * head_len;

            verts.push_back(FlowDebugVertex{tail, color});
            verts.push_back(FlowDebugVertex{tip, color});
            verts.push_back(FlowDebugVertex{tip, color});
            verts.push_back(FlowDebugVertex{base + perp * head_half, color});
            verts.push_back(FlowDebugVertex{tip, color});
            verts.push_back(FlowDebugVertex{base - perp * head_half, color});
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
    // Explicit, because the per-vertex alpha fade at the lumen edge is only a
    // fade if blending is on -- and this pass inherits whatever state the last
    // one happened to leave behind.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(imp.flow_vao);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size()));
    ++stats_.draw_calls;
}

void Renderer::submit_squad_debug(const sim::SquadRegistry& squads) {
    if (!ready_ || !impl_) return;
    Impl& imp = *impl_;
    const ShaderProgram prog = imp.shaders.get("flow_debug");
    if (!prog.valid()) return;
    if (squads.paths().empty()) return;

    std::vector<FlowDebugVertex> verts;

    // Distinct hues per squad so two adjacent groups are separable at a glance
    // while tuning. Golden-ratio hue stepping for the same reason the squad
    // lateral offsets use it: consecutive ids land far apart on the wheel.
    auto squad_color = [](u32 id, f32 alpha) {
        const f32 h = std::fmod(static_cast<f32>(id) * 0.61803398875f, 1.0f) * 6.0f;
        const i32 sector = static_cast<i32>(h);
        const f32 frac = h - static_cast<f32>(sector);
        const f32 q = 1.0f - frac;
        switch (sector % 6) {
            case 0: return Vec4{1.0f, frac, 0.0f, alpha};
            case 1: return Vec4{q, 1.0f, 0.0f, alpha};
            case 2: return Vec4{0.0f, 1.0f, frac, alpha};
            case 3: return Vec4{0.0f, q, 1.0f, alpha};
            case 4: return Vec4{frac, 0.0f, 1.0f, alpha};
            default: return Vec4{1.0f, 0.0f, q, alpha};
        }
    };

    // Routes, dim: they are static scenery next to the moving anchors.
    const Vec4 path_color{0.45f, 0.55f, 0.70f, 0.40f};
    for (const sim::SquadPath& path : squads.paths()) {
        for (usize i = 0; i + 1 < path.points.size(); ++i) {
            verts.push_back(FlowDebugVertex{path.points[i], path_color});
            verts.push_back(FlowDebugVertex{path.points[i + 1], path_color});
        }
    }

    // Anchors and radii.
    constexpr u32 kRingSegments = 24;
    const std::vector<sim::Squad>& all = squads.squads();
    for (u32 id = 0; id < static_cast<u32>(all.size()); ++id) {
        const sim::Squad& sq = all[id];
        if (!sq.active || sq.member_count == 0) continue;
        const Vec4 c = squad_color(id, 0.9f);

        const f32 arm = 1.0f;
        verts.push_back(FlowDebugVertex{sq.anchor - Vec2{arm, 0.0f}, c});
        verts.push_back(FlowDebugVertex{sq.anchor + Vec2{arm, 0.0f}, c});
        verts.push_back(FlowDebugVertex{sq.anchor - Vec2{0.0f, arm}, c});
        verts.push_back(FlowDebugVertex{sq.anchor + Vec2{0.0f, arm}, c});

        // The radius ring is drawn around the CENTROID, not the anchor: the
        // radius describes where the members are, and seeing the gap between
        // ring and cross is exactly how you read whether the leash is too long.
        const Vec4 ring = squad_color(id, 0.45f);
        for (u32 k = 0; k < kRingSegments; ++k) {
            const f32 a0 = math::kTwoPi * (static_cast<f32>(k) / kRingSegments);
            const f32 a1 = math::kTwoPi * (static_cast<f32>(k + 1) / kRingSegments);
            verts.push_back(FlowDebugVertex{
                sq.centroid + Vec2{std::cos(a0), std::sin(a0)} * sq.radius, ring});
            verts.push_back(FlowDebugVertex{
                sq.centroid + Vec2{std::cos(a1), std::sin(a1)} * sq.radius, ring});
        }

        // A tether from the mass to its anchor, so a squad being dragged is
        // visually distinct from one sitting on its anchor.
        verts.push_back(FlowDebugVertex{sq.centroid, ring});
        verts.push_back(FlowDebugVertex{sq.anchor, ring});
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
