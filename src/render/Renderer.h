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
// The renderer is strictly a *consumer* of sim state. It never mutates sim data
// and never runs during a sim tick — it reads the post-tick state plus an
// interpolation alpha.
#pragma once

#include "core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace immune::sim {
class ChaffBuffers;
class SpatialHash;
class EcsWorld;
class FlowField;
class TissueMask;
class DistanceField;
struct DamageField;
}

namespace immune::render {

class Camera;

struct RendererDesc {
    i32 framebuffer_width = 1600;
    i32 framebuffer_height = 900;
    /// Max instances per family batch; sized from SimDesc::max_chaff.
    u32 max_chaff_instances = 16384;
    u32 max_entity_instances = 4096;
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
    f32 pad;
};

struct FrameStats {
    u32 draw_calls = 0;
    u32 chaff_instances_drawn = 0;
    u32 chaff_agents_in_blobs = 0;
    u32 entity_instances_drawn = 0;
    u32 vfx_fields_drawn = 0;
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

    /// Draws the tissue substrate layer (DESIGN.md §7 back layer) from the
    /// level's distance field, including the heartbeat pulse.
    void submit_tissue(const sim::TissueMask& mask, const sim::DistanceField& sdf, f32 heartbeat_phase);

    /// THE hot submission. Walks the chaff SoA once, partitions agents into
    /// per-family instance ranges and blob density accumulation using `hash`
    /// occupancy against the LOD thresholds, then issues one instanced draw per
    /// family plus one fullscreen blob pass.
    void submit_chaff(const sim::ChaffBuffers& chaff, const sim::SpatialHash& hash);

    /// Named agents, towers, projectiles — a few thousand at most.
    void submit_entities(const sim::EcsWorld& ecs);

    /// Damage/AoE fields as fluid shader effects (DESIGN.md §8.5): toxin clouds,
    /// histamine blooms, antibody tides, complement lightning.
    void submit_fields(const sim::DamageField* fields, usize count);

    /// Debug visualisation of the flow field vectors. Off in release play.
    void submit_flow_debug(const sim::FlowField& flow);

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

/// Packs a linear RGBA colour to the u32 layout ChaffInstance::tint_rgba8 uses.
u32 pack_rgba8(Vec4 color);

} // namespace immune::render
