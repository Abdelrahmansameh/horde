// render/ChaffBatcher.h — the density-LOD partition and instance packing.
// Owner: Wave 1C. Header-only (see Gl.h for why).
//
// This is the CPU half of `Renderer::submit_chaff`, factored out of the GL code
// so that the two things that are easy to get wrong — the crossfade band and the
// per-family batch layout — can be unit-tested with no GL context at all
// (tests/test_render_lod.cpp, tests/test_render_batch.cpp).
//
// THE CROSSFADE CONTRACT (docs/ARCHITECTURE.md §5.2)
// For a cell of occupancy `o`:
//   o <= threshold          -> instance_alpha = 1, blob_weight = 0
//   threshold < o < full    -> instance_alpha = 1 - s, blob_weight = s
//   o >= full               -> instance_alpha = 0, blob_weight = 1
// where s = smoothstep((o - threshold) / (full - threshold)).
//
// `instance_alpha + blob_weight == 1` for *every* occupancy, by construction.
// That identity is what makes the transition invisible: an agent in the band is
// drawn at partial sprite alpha AND deposits the complementary fraction of its
// density into the blob field, so the total apparent mass on screen is the same
// before, during, and after the switch. Weighting by `density` rather than by
// head-count keeps a half-dissolved agent from popping to full blob mass.
#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "render/Renderer.h"
#include "sim/chaff/ChaffBuffers.h"

#include <vector>

namespace immune::render {

// ---------------------------------------------------------------------------
// Family visual language (DESIGN.md §6: colour = family, silhouette size =
// threat tier, animation tempo = speed tier).
//
// These are three *separate* fields on purpose. Nothing in the renderer is
// allowed to derive one from another, because that is exactly how the visual
// language quietly drifts.
// ---------------------------------------------------------------------------

struct FamilyVisual {
    f32 silhouette;   ///< World-unit sprite diameter. THREAT tier.
    f32 tempo;        ///< Animation cycles per second. SPEED tier.
    f32 wobble;       ///< SDF deformation amount; family "texture", not tier.
};

/// Per-family look. Now DATA: assets/config/enemies.json owns these numbers and
/// pushes them down through set_family_visual() at load.
///
/// They live here rather than in game/ because render/ deliberately does not
/// depend on game/, and the chaff batcher needs them every frame. The values
/// below are the shipped defaults, so a renderer that is never handed a config
/// (a unit test, a bare screenshot harness) draws exactly what it always did.
///
/// `silhouette` is also the source of a chaff agent's COLLISION radius --
/// EnemyRoster::apply_to_tuning derives ChaffFamilyParams::radius from it --
/// which is what keeps what is drawn and what collides from ever disagreeing.
/// That is why enemy size is one number in one place rather than a visual one
/// and a physical one.
const FamilyVisual& family_visual(PathogenFamily family);
void set_family_visual(PathogenFamily family, const FamilyVisual& visual);

// ---------------------------------------------------------------------------
// Occupancy view — a non-owning window onto SpatialHash::occupancy().
// Decoupled from SpatialHash so tests can synthesize a grid while Wave 1B's
// rebuild() is still landing.
// ---------------------------------------------------------------------------

struct OccupancyGrid {
    Rect bounds{};
    f32 cell_size = 1.0f;
    IVec2 dims{0, 0};
    const u32* occupancy = nullptr;

    bool valid() const {
        return occupancy != nullptr && dims.x > 0 && dims.y > 0 && cell_size > 0.0f;
    }

    /// Occupancy of the cell containing `p`. 0 when the grid is unavailable,
    /// which makes every agent draw as an instance — the correct degradation.
    u32 at(Vec2 p) const {
        if (!valid()) return 0;
        const f32 inv = 1.0f / cell_size;
        const i32 cx = math::clamp(static_cast<i32>((p.x - bounds.min.x) * inv), 0, dims.x - 1);
        const i32 cy = math::clamp(static_cast<i32>((p.y - bounds.min.y) * inv), 0, dims.y - 1);
        return occupancy[static_cast<usize>(cy) * static_cast<usize>(dims.x) +
                         static_cast<usize>(cx)];
    }
};

// ---------------------------------------------------------------------------
// The crossfade split. THE subtle part; see the file header.
// ---------------------------------------------------------------------------

struct LodSplit {
    f32 instance_alpha = 1.0f;
    f32 blob_weight = 0.0f;
};

inline LodSplit lod_split(u32 occupancy, u32 threshold, u32 full) {
    if (occupancy <= threshold) return LodSplit{1.0f, 0.0f};
    if (full <= threshold || occupancy >= full) return LodSplit{0.0f, 1.0f};
    const f32 t = static_cast<f32>(occupancy - threshold) /
                  static_cast<f32>(full - threshold);
    const f32 s = math::smoothstep01(t);
    return LodSplit{1.0f - s, s};
}

// ---------------------------------------------------------------------------
// Low-resolution density field backing the blob pass.
// RGB is colour premultiplied by contributed mass; A is the mass itself, so the
// shader recovers a mass-weighted family colour with rgb / max(a, eps).
// ---------------------------------------------------------------------------

class DensityGrid {
public:
    void configure(i32 width, i32 height) {
        width_ = width > 0 ? width : 1;
        height_ = height > 0 ? height : 1;
        texels_.assign(static_cast<usize>(width_) * static_cast<usize>(height_), Vec4{0.0f});
    }

    void set_extent(const Rect& world_extent) { extent_ = world_extent; }

    void clear() {
        for (Vec4& t : texels_) t = Vec4{0.0f};
        deposited_mass_ = 0.0f;
    }

    /// Bilinear splat of `mass` (and `mass * colour`) at a world position. The
    /// four corner weights sum to 1, so total deposited mass is exactly `mass`
    /// for any point inside the extent — which is what conserves apparent mass
    /// across the crossfade.
    void splat(Vec2 world, Vec4 color, f32 mass) {
        if (mass <= 0.0f) return;
        const Vec2 size = extent_.size();
        if (size.x <= 0.0f || size.y <= 0.0f) return;
        const f32 fx = (world.x - extent_.min.x) / size.x * static_cast<f32>(width_) - 0.5f;
        const f32 fy = (world.y - extent_.min.y) / size.y * static_cast<f32>(height_) - 0.5f;
        const i32 x0 = static_cast<i32>(std::floor(fx));
        const i32 y0 = static_cast<i32>(std::floor(fy));
        const f32 tx = fx - static_cast<f32>(x0);
        const f32 ty = fy - static_cast<f32>(y0);
        deposited_mass_ += mass;
        const f32 w[4] = {(1.0f - tx) * (1.0f - ty), tx * (1.0f - ty),
                          (1.0f - tx) * ty, tx * ty};
        const i32 xs[4] = {x0, x0 + 1, x0, x0 + 1};
        const i32 ys[4] = {y0, y0, y0 + 1, y0 + 1};
        for (int k = 0; k < 4; ++k) {
            const i32 x = math::clamp(xs[k], 0, width_ - 1);
            const i32 y = math::clamp(ys[k], 0, height_ - 1);
            Vec4& t = texels_[static_cast<usize>(y) * static_cast<usize>(width_) +
                              static_cast<usize>(x)];
            const f32 m = mass * w[k];
            t.x += color.x * m;
            t.y += color.y * m;
            t.z += color.z * m;
            t.w += m;
        }
    }

    /// Sum of the alpha channel — equals the total splatted mass (bilinear
    /// weights are a partition of unity). Tests assert on this.
    f32 total_mass() const {
        f32 sum = 0.0f;
        for (const Vec4& t : texels_) sum += t.w;
        return sum;
    }

    f32 deposited_mass() const { return deposited_mass_; }
    i32 width() const { return width_; }
    i32 height() const { return height_; }
    const Rect& extent() const { return extent_; }
    const Vec4* data() const { return texels_.data(); }
    bool empty() const { return texels_.empty(); }

private:
    i32 width_ = 1;
    i32 height_ = 1;
    Rect extent_{};
    std::vector<Vec4> texels_;
    f32 deposited_mass_ = 0.0f;
};

// ---------------------------------------------------------------------------
// The batching pass itself.
// ---------------------------------------------------------------------------

struct ChaffBatchParams {
    u32 lod_blob_threshold = 24;
    u32 lod_blob_full = 48;
    /// Instance slots reserved per family. The destination buffer is
    /// kFamilyCount * per_family_capacity entries, so family f owns
    /// [f * capacity, f * capacity + count_f) and needs no prefix-sum pass.
    u32 per_family_capacity = 16384;
    /// Seconds since renderer init. Folded into anim_phase so the shader needs
    /// no per-family tempo uniform.
    f32 time = 0.0f;
    /// Agents outside this rect are skipped entirely. Set it to the camera's
    /// visible bounds, padded by the largest silhouette.
    Rect cull{};
    bool cull_enabled = false;
};

struct ChaffBatchResult {
    u32 family_counts[kFamilyCount] = {};
    u32 instances_total = 0;
    u32 agents_in_blobs = 0;   ///< Agents contributing any blob mass.
    u32 agents_culled = 0;
    u32 instances_dropped = 0; ///< Overflowed a family's capacity.
    f32 instance_mass = 0.0f;  ///< Sum of instance_alpha * density.
    f32 blob_mass = 0.0f;      ///< Sum of blob_weight * density.
    /// instance_mass + blob_mass. Invariant across the whole crossfade band:
    /// it equals the total density of every non-culled agent.
    f32 total_mass() const { return instance_mass + blob_mass; }
};

/// Walks the chaff SoA exactly once. Per agent: one occupancy lookup, one
/// crossfade split, one 32-byte structured store into the mapped instance
/// buffer and/or one bilinear splat into the density grid. No allocation, no
/// per-agent branch on family type, no per-agent draw call.
///
/// `dest` must have room for kFamilyCount * per_family_capacity instances.
/// `density` may be null (blob pass disabled), in which case band agents still
/// fade out of the instance pass — so pass one unless you want mass to vanish.
inline ChaffBatchResult build_chaff_batches(const sim::ChaffBuffers& chaff,
                                            const OccupancyGrid& occ,
                                            const ChaffBatchParams& params,
                                            ChaffInstance* dest,
                                            DensityGrid* density) {
    ChaffBatchResult out;
    if (dest == nullptr) return out;

    const usize n = chaff.count();
    const f32* px = chaff.pos_x.data();
    const f32* py = chaff.pos_y.data();
    const f32* vx = chaff.vel_x.data();
    const f32* vy = chaff.vel_y.data();
    const u8* fam = chaff.family.data();
    const u8* flg = chaff.flags.data();
    const f32* den = chaff.density.data();
    const u32* gen = chaff.generation.data();

    // Per-family cursors into the fixed-stride destination regions.
    u32 cursor[kFamilyCount];
    for (u32 f = 0; f < kFamilyCount; ++f) cursor[f] = 0;

    // Family constants hoisted out of the loop: six table lookups, not 10,000.
    Vec4 fam_color[kFamilyCount];
    FamilyVisual fam_vis[kFamilyCount];
    for (u32 f = 0; f < kFamilyCount; ++f) {
        fam_color[f] = family_color(static_cast<PathogenFamily>(f));
        fam_vis[f] = family_visual(static_cast<PathogenFamily>(f));
    }

    for (usize i = 0; i < n; ++i) {
        const Vec2 p{px[i], py[i]};
        if (params.cull_enabled && !params.cull.contains(p)) {
            ++out.agents_culled;
            continue;
        }

        const u32 f = fam[i] < kFamilyCount ? fam[i] : 0u;
        const f32 d = den[i];
        const LodSplit split = lod_split(occ.at(p), params.lod_blob_threshold,
                                         params.lod_blob_full);

        out.instance_mass += split.instance_alpha * d;
        out.blob_mass += split.blob_weight * d;

        if (split.blob_weight > 0.0f) {
            ++out.agents_in_blobs;
            if (density != nullptr) density->splat(p, fam_color[f], split.blob_weight * d);
        }

        if (split.instance_alpha <= 0.0f) continue;

        if (cursor[f] >= params.per_family_capacity) {
            ++out.instances_dropped;
            continue;
        }

        ChaffInstance& inst = dest[static_cast<usize>(f) * params.per_family_capacity +
                                  cursor[f]];
        ++cursor[f];

        const FamilyVisual& vis = fam_vis[f];
        // A per-agent phase offset from the generation counter: stable for the
        // agent's whole life, uncorrelated between neighbours. Without it, ten
        // thousand sprites pulse in lockstep and the mass reads as a texture.
        const u32 h = (gen[i] * 2654435761u) ^ static_cast<u32>(i * 40503u);
        const f32 offset = static_cast<f32>(h & 0xFFFFu) * (math::kTwoPi / 65536.0f);

        inst.x = p.x;
        inst.y = p.y;
        // Density shrinks the silhouette as an agent dissolves in a damage
        // field — DESIGN.md §7's chaff VFX tier, with no per-unit animation.
        inst.scale = vis.silhouette * (0.62f + 0.38f * math::saturate(d));
        inst.rotation = std::atan2(vy[i], vx[i]);
        Vec4 tint = fam_color[f];
        tint.a = split.instance_alpha;
        inst.tint_rgba8 = pack_rgba8(tint);
        // chaff_flags is a u8 and ChaffInstance::flags is a u32, so the top 24
        // bits were dead. The family id rides in bits 8..15 rather than
        // widening a frozen 32-byte instance layout: the fragment shader needs
        // to know which pathogen it is drawing to pick a silhouette, and
        // inferring it from the tint would tie shape to colour and break the
        // "these are separate channels" rule this file's header states.
        // Mirrored by CHAFF_FAMILY_SHIFT in chaff.frag.
        inst.flags = static_cast<u32>(flg[i]) | (f << 8);
        inst.anim_phase = offset + params.time * vis.tempo * math::kTwoPi;
        inst.pad = vis.wobble;
    }

    for (u32 f = 0; f < kFamilyCount; ++f) {
        out.family_counts[f] = cursor[f];
        out.instances_total += cursor[f];
    }
    return out;
}

} // namespace immune::render
