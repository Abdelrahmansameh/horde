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
#include "sim/chaff/HitFlash.h"
#include "sim/chaff/ReplicationSplit.h"

#include <cmath>
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

    /// Occupancy at `p` interpolated between the four surrounding cell CENTRES.
    ///
    /// THIS is what the crossfade must read, not `at()`. Occupancy is a
    /// per-cell integer, so `at()` is a step function of position: every agent
    /// inside one broadphase cell gets bit-identical LOD treatment, and an
    /// agent a hair across the boundary gets a different one. A band that is
    /// smooth in occupancy buys nothing when its INPUT jumps -- the horde tiles
    /// into hard-edged squares of "all sprite" and "all blob", axis-aligned to
    /// a grid that exists for broadphase reasons and is supposed to be
    /// invisible.
    ///
    /// Reading the field bilinearly makes the split continuous across cell
    /// borders: same numbers, same band, no seam. Four loads and three lerps
    /// per agent, against the one load `at()` does.
    f32 at_smooth(Vec2 p) const {
        if (!valid()) return 0.0f;
        const f32 inv = 1.0f / cell_size;
        // -0.5 puts the sample on the cell-CENTRE lattice: a point at a cell's
        // centre must read that cell's value and nothing else, or the
        // interpolation smears the whole field half a cell toward -x/-y.
        const f32 fx = (p.x - bounds.min.x) * inv - 0.5f;
        const f32 fy = (p.y - bounds.min.y) * inv - 0.5f;
        const f32 x0f = std::floor(fx);
        const f32 y0f = std::floor(fy);
        const i32 x0 = static_cast<i32>(x0f);
        const i32 y0 = static_cast<i32>(y0f);
        const f32 tx = fx - x0f;
        const f32 ty = fy - y0f;
        const auto load = [&](i32 x, i32 y) {
            // Clamped, not zeroed: outside the grid the nearest edge cell is
            // the honest answer, and it stops the border from reading as empty
            // and popping its agents back to full sprites.
            x = math::clamp(x, 0, dims.x - 1);
            y = math::clamp(y, 0, dims.y - 1);
            return static_cast<f32>(occupancy[static_cast<usize>(y) *
                                                  static_cast<usize>(dims.x) +
                                              static_cast<usize>(x)]);
        };
        const f32 lo = load(x0, y0) * (1.0f - tx) + load(x0 + 1, y0) * tx;
        const f32 hi = load(x0, y0 + 1) * (1.0f - tx) + load(x0 + 1, y0 + 1) * tx;
        return lo * (1.0f - ty) + hi * ty;
    }
};

// ---------------------------------------------------------------------------
// The crossfade split. THE subtle part; see the file header.
// ---------------------------------------------------------------------------

struct LodSplit {
    f32 instance_alpha = 1.0f;
    f32 blob_weight = 0.0f;
};

/// `occupancy` is a float because the renderer feeds it
/// OccupancyGrid::at_smooth -- see there for why a stepped input makes this
/// band's smoothness irrelevant. Integer call sites are unaffected: the same
/// integers through the same formula.
inline LodSplit lod_split(f32 occupancy, u32 threshold, u32 full) {
    const f32 thr = static_cast<f32>(threshold);
    const f32 fll = static_cast<f32>(full);
    if (occupancy <= thr) return LodSplit{1.0f, 0.0f};
    if (full <= threshold || occupancy >= fll) return LodSplit{0.0f, 1.0f};
    const f32 t = (occupancy - thr) / (fll - thr);
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
    /// Whether the crossfade runs. False makes every agent a full-alpha
    /// instance that deposits nothing, which is how the game ships -- see
    /// RendererDesc::lod_blob_enabled for why. Defaults TRUE here because the
    /// crossfade is this file's own unit under test; the renderer passes its
    /// own value.
    bool lod_blob_enabled = true;
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
    /// Instances emitted that will VISIBLY flash -- i.e. whose packed flash
    /// byte is non-zero, not merely whose stored ramp is. Not used by the
    /// renderer; it exists so a test can assert that a damaged agent actually
    /// reaches the GPU lit up, which is the one link in the chain that no
    /// sim-side or shader-side assertion can cover.
    u32 agents_flashing = 0;
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
    const f32* flash = chaff.hit_flash.data();
    const f32* replication_pulse = chaff.replication_pulse.data();
    const f32* replication_origin_x = chaff.replication_origin_x.data();
    const f32* replication_origin_y = chaff.replication_origin_y.data();

    // Per-family cursors into the fixed-stride destination regions.
    u32 cursor[kFamilyCount];
    for (u32 f = 0; f < kFamilyCount; ++f) cursor[f] = 0;

    // Family constants hoisted out of the loop: six table lookups, not 10,000.
    Vec4 fam_color[kFamilyCount];
    FamilyVisual fam_vis[kFamilyCount];
    sim::HitFlashParams fam_flash[kFamilyCount];
    sim::ReplicationSplitParams fam_split[kFamilyCount];
    // Reciprocal of the occupancy at which this family's sprites cover a
    // broadphase cell: cell area over the area of one silhouette disc. A virus
    // (1.53 across) fills a 4-unit cell at ~8.7 agents, a bacterium (2.25) at
    // ~4.0 -- "packed" is not the same number for both, and a shadow term that
    // used one number for both would retire on the wrong one.
    f32 inv_crowd_full[kFamilyCount];
    const f32 cell_area = occ.cell_size * occ.cell_size;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        fam_color[f] = family_color(static_cast<PathogenFamily>(f));
        fam_vis[f] = family_visual(static_cast<PathogenFamily>(f));
        fam_flash[f] = sim::family_hit_flash(static_cast<PathogenFamily>(f));
        fam_split[f] = sim::family_replication_split(static_cast<PathogenFamily>(f));
        const f32 s = math::max(fam_vis[f].silhouette, 0.01f);
        const f32 disc = 0.25f * math::kPi * s * s;
        inv_crowd_full[f] = disc / math::max(cell_area, disc);
    }

    for (usize i = 0; i < n; ++i) {
        const Vec2 p{px[i], py[i]};
        if (params.cull_enabled && !params.cull.contains(p)) {
            ++out.agents_culled;
            continue;
        }

        const u32 f = fam[i] < kFamilyCount ? fam[i] : 0u;
        const f32 d = den[i];
        // Sampled even when the crossfade is off: this is also the crowd signal
        // the sprite shadow reads, and that is not part of the LOD.
        const f32 local_occ = occ.at_smooth(p);
        const LodSplit split =
            params.lod_blob_enabled
                ? lod_split(local_occ, params.lod_blob_threshold, params.lod_blob_full)
                : LodSplit{1.0f, 0.0f};

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

        // How white this agent is right now, 0..1. The sim stores a linear ramp
        // (sim/chaff/HitFlash.h) and ALL of the shaping happens here, once per
        // frame, so retuning the curve or the strength re-renders agents that
        // are already mid-flash instead of only the next ones to be hit.
        //
        // The pow() is why this sits behind a `> 0` test rather than being
        // written branch-free like the rest of the loop: in any normal frame a
        // low single-digit percentage of the horde is inside a fade window, and
        // paying a transcendental for the other ninety-odd percent to multiply
        // by zero is the wrong trade at ten thousand agents.
        const sim::HitFlashParams& flash_params = fam_flash[f];
        f32 flash_k = 0.0f;
        if (flash_params.enabled && flash[i] > 0.0f) {
            const f32 ramp = math::saturate(flash[i]);
            flash_k = math::saturate(flash_params.strength *
                                     std::pow(ramp, flash_params.curve));
        }

        const sim::ReplicationSplitParams& split_params = fam_split[f];
        const f32 split_pulse = replication_pulse[i];
        const bool splitting = split_params.enabled && std::fabs(split_pulse) > 0.0f;
        const f32 split_base = math::smoothstep01(1.0f - math::saturate(std::fabs(split_pulse)));
        const f32 split_t = std::pow(split_base, math::max(0.01f, split_params.pull_ease));
        const f32 split_reveal =
            std::pow(split_base, math::max(0.01f, split_params.reveal_ease));
        const Vec2 split_origin{replication_origin_x[i], replication_origin_y[i]};
        // Physics separates the pair immediately so neither blocks the other,
        // while rendering begins both halves at their common source and eases
        // them outward. This is the continuity the simulation alone cannot
        // express in a fixed tick.
        const Vec2 draw_p = splitting ? split_origin + (p - split_origin) * split_t : p;
        inst.x = draw_p.x;
        inst.y = draw_p.y;
        // Constant silhouette: damage does NOT shrink the sprite. A wounded
        // agent used to render smaller (the density term), which read as
        // "further away" rather than "hurt" and made a damaged horde look
        // thinner than it actually was. Damage feedback belongs to the tint
        // and the dissolve VFX, not to the size.
        //
        // The hit punch is the one exception and it is off by default -- see
        // HitFlashParams::scale_punch, which carries the argument for why a
        // transient puff does not violate the rule the paragraph above states.
        // During a viral split the fragment shader draws each descendant as
        // one complementary half of this full-sized silhouette, then reveals
        // the missing half as it moves away. Keep a shared local frame so the
        // two clipped halves meet as the original parent before separating.
        Vec2 split_delta = p - split_origin;
        const bool split_oriented = splitting && f == static_cast<u32>(PathogenFamily::Virus) &&
                                    math::length_sq(split_delta) > math::kEpsilon;
        // Both halves share one local frame while they overlap. Canonicalizing
        // the axis is what lets their clipped silhouettes meet perfectly at
        // the parent seam rather than appearing as two independently rotated
        // sprites that happen to be on top of one another.
        if (split_oriented && (split_delta.x < 0.0f ||
                               (split_delta.x == 0.0f && split_delta.y < 0.0f))) {
            split_delta = split_delta * -1.0f;
        }
        inst.scale = vis.silhouette * (1.0f + flash_params.scale_punch * flash_k);
        inst.rotation = split_oriented ? std::atan2(split_delta.y, split_delta.x)
                                       : std::atan2(vy[i], vx[i]);
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
        // Local crowding, 0..1 over [0, lod_blob_threshold], in bits 16..23.
        //
        // The sprite pass needs this for its drop shadow and nothing else. Every
        // agent lays down a near-black contact shadow -- that shadow is what
        // gives a lone virion local contrast against a vivid red lumen -- and an
        // agent buried inside a horde is casting it onto the BODIES of its
        // neighbours rather than onto the lane. Stacked dozens deep they
        // composite to black and the mass reads as a hole punched in the
        // screen. A shadow is a figure-ground cue, so it belongs to the crowd's
        // silhouette, not to each individual inside it; this byte is what lets
        // the fragment stage keep it at the rim and retire it in the interior.
        //
        // Fed from the SMOOTH occupancy, so it fades across the crowd instead
        // of switching per broadphase cell. Bits 0..7 are chaff_flags, 8..15 the
        // family id, 16..23 this, and 24..31 the hit flash below. Mirrored by
        // CHAFF_CROWD_SHIFT in chaff.frag.
        // Scaled against how many of THIS family's sprites cover a cell, not
        // against lod_blob_threshold. The question a shadow is asking is "is
        // there lane under me or another body?", which is a question about
        // drawn area -- so it is answered from the silhouette and the cell
        // size, and it keeps its meaning whether or not the LOD pass this used
        // to borrow its scale from is even enabled.
        const f32 crowd_norm = math::saturate(local_occ * inv_crowd_full[f]);
        const u32 crowd_bits = static_cast<u32>(crowd_norm * 255.0f + 0.5f) << 16;
        // The hit flash claims the last free byte of the instance's flags word,
        // bits 24..31, rather than widening a 32-byte layout that chaff.vert
        // mirrors attribute for attribute. Eight bits is plenty: this is a mix
        // weight on a sprite that covers under a dozen pixels, and the value it
        // quantizes is already fading past it at ~1/7 per tick.
        //
        // What does NOT ride here is the flash COLOUR, which is per-family and
        // would cost three more bytes an agent to say the same thing ten
        // thousand times. It goes up as u_hit_flash_color[] instead, indexed in
        // the fragment stage by the family id already in bits 8..15.
        // Mirrored by CHAFF_FLASH_SHIFT in chaff.frag.
        // The final byte carries a hit flash normally. A splitting virion gets
        // priority for those few frames: it instead carries its 0..1 split
        // reveal, while the two spare live-agent flag bits identify the effect
        // and its complementary half in chaff.frag.
        const u32 effect_byte = splitting
                                    ? static_cast<u32>(split_reveal * 255.0f + 0.5f)
                                    : static_cast<u32>(flash_k * 255.0f + 0.5f);
        const u32 flash_bits = effect_byte << 24;
        // Counted off the PACKED byte, not off flash_k, so the tally means
        // "instances that will visibly flash" rather than "instances carrying a
        // float that rounds to nothing". The tail of a fade spends a tick or
        // two below half a quantization step, and those are not flashes.
        if (flash_bits != 0u) ++out.agents_flashing;
        constexpr u32 kVisualSplitActive = 1u << 6;
        constexpr u32 kVisualSplitNegativeHalf = 1u << 7;
        const u32 split_bits = splitting ? kVisualSplitActive |
                                          (split_pulse > 0.0f ? kVisualSplitNegativeHalf : 0u)
                                         : 0u;
        inst.flags = static_cast<u32>(flg[i]) | split_bits | (f << 8) | crowd_bits | flash_bits;
        // A shared phase is as important as a shared local frame: at frame
        // zero the two complementary masks must reconstruct ONE capsid, not
        // two different spiky outlines drawn over each other.
        inst.anim_phase = splitting ? params.time * vis.tempo * math::kTwoPi
                                    : offset + params.time * vis.tempo * math::kTwoPi;
        inst.pad = vis.wobble;
    }

    for (u32 f = 0; f < kFamilyCount; ++f) {
        out.family_counts[f] = cursor[f];
        out.instances_total += cursor[f];
    }
    return out;
}

} // namespace immune::render
