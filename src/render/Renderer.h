// render/Renderer.h — frame graph and draw submission. FROZEN CONTRACT.
// Owner: Wave 1C (core passes), Wave 3D (VFX passes).
//
// RATIONALE (DESIGN.md §8.5)
// Two facts drive this whole interface:
//
//  1. ONE INSTANCED DRAW CALL PER FAMILY. The CPU never builds per-agent
//     geometry or per-agent draw calls. `submit_chaff` takes the chaff SoA and
//     memcpys contiguous spans into a persistently-mapped instance buffer, one
//     range per PathogenFamily. Six draw calls cover ten thousand agents. This
//     is also why PathogenFamily's enum order is frozen: it is the batch order.
//
//  2. LOD BY DENSITY, NOT DISTANCE. The camera is fixed-ish, so distance LOD is
//     meaningless. Instead, where local occupancy (from SpatialHash::occupancy)
//     exceeds a threshold, those cells are *not* drawn as instances at all —
//     they are accumulated into a low-resolution density texture and drawn by
//     the blob pass as a shader-driven mass. A crossfade band between the two
//     thresholds makes the switch invisible: an agent near the boundary is drawn
//     at partial instance alpha and contributes partial blob density, so total
//     apparent mass is conserved. Without this, a floodplain level would try to
//     draw 10,000 overlapping sprites into a few hundred pixels.
//
//     This pass ships DISABLED -- see RendererDesc::lod_blob_enabled for the
//     measurements. The occupancy field it reads is still sampled every frame,
//     because the sprite shadow needs the same crowd signal.
//
// The renderer is strictly a *consumer* of sim state. It never mutates sim data
// and never runs during a sim tick — it reads the post-tick state plus an
// interpolation alpha.
#pragma once

#include "core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace immune::sim {
struct CombatEvent;
class ChaffBuffers;
class SpatialHash;
class EcsWorld;
class FlowField;
class SquadRegistry;
class TissueMask;
class DistanceField;
class ProjectileBuffers;
class SwarmerBuffers;
struct SlowZone;
class FluidBuffers;
struct DamageField;
}

namespace immune::vfx {
struct ParticleInstance;
enum class BlendMode : u8;
}

namespace immune::render {

class Camera;

struct RendererDesc {
    i32 framebuffer_width = 1600;
    i32 framebuffer_height = 900;
    /// Max instances per family batch; sized from SimDesc::max_chaff.
    u32 max_chaff_instances = 16384;
    u32 max_entity_instances = 4096;
    /// Live simulated rounds drawable in one frame.
    u32 max_projectile_instances = 16384;
    /// Live swarmers across every tower. See sim/swarm/Swarmers.h for how the standing
    /// cloud size is set; this only has to clear the equilibrium a full board
    /// of maxed T-cells reaches.
    u32 max_swarmer_instances = 49152;
    /// Cosmetic particles drawable per blend mode per frame. Sized for the
    /// Gunner's "continuous stream" brief; this is the single biggest instance
    /// buffer in the renderer and is expected to run near full at high tiers.
    u32 max_particle_instances = 262144;
    /// Whether the density-LOD blob pass runs at all. OFF by default.
    ///
    /// The pass is a perf LOD, and it was sized for a horde that could stack:
    /// it begins at 24 agents in a broadphase cell, which a 4-unit cell can
    /// only reach if the agents in it are inside each other. Since the contact
    /// pass stopped letting them (sim/chaff/ChaffSystem.cpp), a packed cell
    /// holds around eight, and the blob engaged only in wall jams.
    ///
    /// What it cost while it did engage was resolution. The density texture is
    /// 320x180 for the WHOLE visible world -- a handful of texels per agent,
    /// magnified roughly 5x to reach the screen and filtered on the way -- and
    /// blob.frag draws it as a bare exponential falloff with no silhouette. It
    /// cannot be sharp; there is nothing in the pass to be sharp with. Drawn
    /// over the sprites at up to 0.92 alpha, it reads as fog over the crowd.
    ///
    /// The sprite path carries the whole horde without it: 10k instances
    /// measure 0.505ms of submit_chaff, 1.51ms worst over 30 frames
    /// (tests/test_render_gl.cpp). The trade the LOD was making -- detail for a
    /// cost we are not paying -- is no longer a trade worth taking.
    ///
    /// Everything below stays live and tested; turn this on to get it back.
    bool lod_blob_enabled = false;
    /// Cell occupancy at which crossfade to the blob representation begins.
    u32 lod_blob_threshold = 24;
    /// Occupancy at which the cell is drawn purely as blob density.
    u32 lod_blob_full = 48;
    /// Resolution of the density texture backing the blob pass.
    i32 density_texture_width = 320;
    i32 density_texture_height = 180;
    bool hot_reload_shaders = false;
};

/// Per-instance vertex attributes. This layout is mirrored exactly in
/// assets/shaders/chaff.vert — changing it is a contract change.
/// 32 bytes: one instance per half cache line, 10k instances = 320 KB/frame.
struct ChaffInstance {
    f32 x, y;          ///< world position
    f32 scale;
    f32 rotation;
    u32 tint_rgba8;    ///< packed family colour x alpha (LOD crossfade)
    u32 flags;         ///< chaff_flags mirror; shader drives marked/slowed tells
    f32 anim_phase;    ///< per-agent animation offset, kills visual lockstep
    f32 pad;
};

struct EntityInstance {
    f32 x, y;
    f32 scale;
    f32 rotation;
    u32 tint_rgba8;
    u32 shape_id;      ///< procedural SDF shape selector
    f32 anim_phase;
    /// Extra per-shape parameter; its meaning is defined by `shape_id`, and it
    /// is 0 for shapes that don't declare one. Every tower body reads it as
    /// the tier, the clot bar as its aspect. Was dead padding
    /// before — the 32-byte layout is unchanged, so entity.vert's byte-for-byte
    /// contract still holds.
    f32 shape_param;
};

/// Optional extra inputs for the tissue pass (DESIGN.md §9.2's lane-identity
/// system, and the flow-aligned plasma treatment in §9.4).
///
/// Everything here is passed as raw arrays and a sim:: pointer rather than as
/// the `game::LaneOwnershipMap` it actually comes from: render/ deliberately
/// does not depend on game/, and that rule is worth more than the small amount
/// of unpacking a caller has to do (App::render / Modes.cpp, both one-liners).
///
/// Every field is optional. A null `decor`, or a decor with null members, makes
/// submit_tissue fall back to a single default hue and a straight-line flow
/// assumption — which is exactly what the bench/sim-test paths want.
struct TissueDecor {
    /// Drives the plasma streamlines (direction) and the travelling systolic
    /// pressure wave (cost-to-goal). Must have been baked from the same mask
    /// that is passed to submit_tissue, since its world extent is taken from
    /// that mask (FlowField does not expose its own origin).
    const sim::FlowField* flow = nullptr;

    /// game::LaneOwnershipMap::owner — `lane_width * lane_height` cells,
    /// row-major, each an index into `lane_type` or 0xFF for "unowned".
    const u8* lane_owner = nullptr;
    /// Per-lane game::VesselType ordinal; `lane_count` entries. Indexed by the
    /// values in `lane_owner`.
    const u8* lane_type = nullptr;
    u32 lane_count = 0;
    i32 lane_width = 0;
    i32 lane_height = 0;

    /// game::RenderSdf::distance -- the analytic, corner-rounded, padded
    /// distance field the substrate is actually drawn from when present
    /// (game/level/RenderSdf.h). `smooth_width * smooth_height` floats,
    /// row-major, world units, positive inside the lumen, covering
    /// `smooth_bounds` (which is generally LARGER than the mask's bounds).
    /// The renderer caches the texture it uploads on this pointer, so the
    /// backing array must outlive the level. Null falls back to the sim's
    /// own DistanceField, which is what every test and headless path passes.
    const f32* smooth_sdf = nullptr;
    i32 smooth_width = 0;
    i32 smooth_height = 0;
    Rect smooth_bounds;

    /// World-unit size of the substrate's pattern features (cells, pebbles,
    /// wall thickness) relative to the look's reference framing. Levels the
    /// camera frames from further away pass a larger value so the pattern
    /// keeps its on-screen size; see tissue_pattern_scale(). 1 = reference.
    f32 pattern_scale = 1.0f;
};

/// The pattern scale for a level framed at `view_height` world units tall:
/// the look was authored at a 143-unit framing.
inline f32 tissue_pattern_scale(f32 view_height) {
    constexpr f32 kReferenceViewHeight = 142.8f;
    if (!(view_height > 0.0f)) return 1.0f;
    const f32 s = view_height / kReferenceViewHeight;
    return s < 0.25f ? 0.25f : (s > 12.0f ? 12.0f : s);
}

struct FrameStats {
    u32 draw_calls = 0;
    u32 chaff_instances_drawn = 0;
    u32 chaff_agents_in_blobs = 0;
    u32 entity_instances_drawn = 0;
    u32 vfx_fields_drawn = 0;
    u32 projectile_instances_drawn = 0;
    u32 swarmer_instances_drawn = 0;
    u32 fluid_instances_drawn = 0;
    u32 particle_instances_drawn = 0;
    f64 submit_ms = 0.0;
};

class Renderer {
public:
    // Both defined out-of-line in Renderer.cpp (even though defaulted): the
    // pimpl'd Impl type is incomplete here, and MSVC needs it complete
    // wherever unique_ptr<Impl>'s special members are actually emitted.
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Creates GL objects. Requires a current GL 4.5 context. False on failure.
    bool init(const RendererDesc& desc);
    void shutdown();
    bool ready() const { return ready_; }
    const std::string& error() const { return error_; }

    void resize(i32 width, i32 height);

    // ---- Frame ------------------------------------------------------------

    /// Clears targets and sets up per-frame uniforms. `alpha` is
    /// FixedClock::alpha() for sim-state interpolation.
    void begin_frame(const Camera& camera, f32 alpha);

    /// Draws the tissue substrate layer (DESIGN.md §9.1 back layer) from the
    /// level's distance field, including the heartbeat pulse. `decor` is
    /// optional (see TissueDecor); without it the pass still draws, just with
    /// one default lane hue and no flow-aligned plasma.
    void submit_tissue(const sim::TissueMask& mask, const sim::DistanceField& sdf,
                       f32 heartbeat_phase, const TissueDecor* decor = nullptr);

    /// THE hot submission. Walks the chaff SoA once, partitions agents into
    /// per-family instance ranges and blob density accumulation using `hash`
    /// occupancy against the LOD thresholds, then issues one instanced draw per
    /// family plus one fullscreen blob pass.
    void submit_chaff(const sim::ChaffBuffers& chaff, const sim::SpatialHash& hash);

    /// Hands this frame's combat events to the death-flash pass, which keeps
    /// drawing the BODY of each killed agent -- white and fading -- for
    /// HitFlashParams::death_linger seconds after the sim retired it.
    ///
    /// WHY THE RENDERER HAS TO OWN THIS
    /// Chaff has no wounded state: at shipped tower rates an agent loses all of
    /// its density inside one tick, so the killing blow is the only hit it ever
    /// takes and it is gone before a frame is drawn. A hit flash on the body
    /// therefore has to outlive the body, and the only layer that can hold
    /// something the sim has already deleted is this one. See
    /// sim/chaff/HitFlash.h's death_linger for the measurements.
    ///
    /// Non-ChaffDeath events are ignored, so callers can pass the whole frame's
    /// buffer -- the same span they hand vfx::ParticleSystem::emit_for_events.
    /// Call it BEFORE submit_chaff in the same frame; corpses are appended to
    /// the per-family instance batches there, so they cost no extra draw call.
    ///
    /// `age_seconds` is how long ago these deaths happened, and exists for the
    /// screenshot harness, which runs its entire sim before a GL context is
    /// created and so has to hand over deaths that are already several ticks
    /// old. Interactive callers draining once per frame pass 0.
    void submit_chaff_deaths(const sim::CombatEvent* events, usize count,
                             f32 age_seconds = 0.0f);

    /// Named agents, towers, clots — a few thousand at most. Towers are
    /// emitted last so they draw over every enemy standing in their footprint
    /// (towers are not obstacles; the horde passes through them).
    void submit_entities(const sim::EcsWorld& ecs);

    /// Damage/AoE fields as fluid shader effects (DESIGN.md §8.5): toxin clouds,
    /// histamine blooms, antibody tides, complement lightning. The Interferon's
    /// slow circles (sim/zone/SlowZones.h) ride the same pass as their own
    /// shape, so pass them alongside; they share the instance buffer and the
    /// draw call.
    void submit_fields(const sim::DamageField* fields, usize count,
                       const sim::SlowZone* zones = nullptr, usize zone_count = 0);

    /// The Goblet Cell's live mucus (sim/fluid/Fluid.h), surfaced as a real
    /// liquid rather than as a cloud of sprites.
    ///
    /// WHY THIS IS TWO PASSES AND NOT ONE
    /// Every other particle-ish pass in this renderer draws one blended sprite
    /// per element and is done. That is exactly wrong for a fluid: a thousand
    /// overlapping soft discs read as fog, and the one thing that makes liquid
    /// look like liquid is a SURFACE — a hard, continuous, curved boundary with
    /// a highlight on it. A surface cannot be produced by any single sprite,
    /// because it is a property of where the particles are *together*.
    ///
    /// So the particles are accumulated additively into an offscreen thickness
    /// buffer (pass one), and a fullscreen pass then thresholds that field into
    /// a surface and shades it — normals from the screen-space gradient of
    /// thickness, specular and fresnel off those normals, depth tint from the
    /// thickness itself (pass two). This is the standard screen-space fluid
    /// approach and it is the reason two adjacent droplets MERGE into one body
    /// with a single unbroken outline instead of showing a seam.
    ///
    /// `particle_radius` comes from FluidSystem::draw_radius(): the solver owns
    /// how far apart particles sit, so it also owns how big they must be drawn
    /// for the field to close up between them.
    void submit_fluid(const sim::FluidBuffers& fluid, f32 particle_radius);

    /// Live projectile rounds, as one instanced draw over the SoA store. These
    /// are the Gunner's actual simulated rounds — the cosmetic tracer trails
    /// that follow them are particles, submitted separately below.
    void submit_projectiles(const sim::ProjectileBuffers& projectiles);

    /// Every tower's live swarmers (sim/swarm/Swarmers.h). Its own pass rather
    /// than part of submit_projectiles: a round is a streaked slug and a
    /// swarmer is a wobbling body, and at swarm density the two looks cannot
    /// share a shader without one of them losing. Tinted by the tower that
    /// released each one, sized by its profile.
    void submit_swarmers(const sim::SwarmerBuffers& swarmers);

    /// One instanced draw of an already-built particle instance span, for one
    /// blend mode. Called once per blend mode per frame, additive first so
    /// alpha-blended mist composites over the glow rather than under it.
    ///
    /// Takes a raw span rather than the ParticleSystem itself so the renderer
    /// stays a pure consumer and never reaches into vfx state — same rule the
    /// rest of this interface follows for sim.
    void submit_particles(const vfx::ParticleInstance* instances, usize count,
                          vfx::BlendMode blend);

    /// Debug visualisation of the flow field vectors. Off in release play.
    void submit_flow_debug(const sim::FlowField& flow);

    /// Draws the squad layer's routes and live anchors: each path as a
    /// polyline, each squad as a cross at its anchor plus a ring at its
    /// current radius, coloured per squad.
    ///
    /// Shares submit_flow_debug's shader, VAO and vertex buffer -- it is the
    /// same "coloured world-space line list" problem, and giving it a private
    /// pipeline would duplicate the upload path for no benefit. Debug overlay
    /// only; nothing about the chaff instance layout or the family colour
    /// language (DESIGN.md 6.2) is touched, so squads stay a SPATIAL read in
    /// the shipped game and a coloured one only while this overlay is on.
    void submit_squad_debug(const sim::SquadRegistry& squads);

    /// Resolves post-processing and leaves the result in the default framebuffer.
    void end_frame();

    const FrameStats& stats() const { return stats_; }

    // ---- Capture ----------------------------------------------------------

    /// Reads the current framebuffer back to RGBA8. Used by --screenshot.
    /// Rows are returned top-down (already flipped from GL's bottom-up order).
    bool read_pixels(std::vector<u8>& out_rgba, i32& out_width, i32& out_height) const;

    /// Reloads any shader whose source file changed. No-op unless the desc
    /// enabled hot reload.
    void poll_shader_reload();

private:
    RendererDesc desc_{};
    FrameStats stats_{};
    bool ready_ = false;
    std::string error_;

    // GL objects, the shader manager, and per-frame scratch (persistently
    // mapped instance buffers, the density grid, etc.) all live behind a
    // pimpl. This keeps GL/shader headers out of every translation unit that
    // merely calls into the renderer (app/, game/), and keeps this frozen
    // header stable while the pass implementations iterate underneath it.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Canonical family colours (DESIGN.md §6). The single source of truth for
/// pathogen colour — UI, VFX, and the instance tint all read this so the
/// "colour = family" rule cannot drift between systems.
Vec4 family_color(PathogenFamily family);
/// Overrides the family colour code. assets/config/enemies.json calls this at
/// load; the compiled-in values remain the defaults.
void set_family_color(PathogenFamily family, Vec4 color);

/// Packs a linear RGBA colour to the u32 layout ChaffInstance::tint_rgba8 uses.
u32 pack_rgba8(Vec4 color);

} // namespace immune::render
