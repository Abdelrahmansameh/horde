// game/level/RenderSdf.h — the smooth distance field the substrate is DRAWN
// from, baked straight off the level's geometry rather than off the sim's mask.
//
// WHY A SECOND DISTANCE FIELD
// The sim's DistanceField is an exact Euclidean transform of a BINARY mask on
// the sim grid (half a world unit per cell). That is the right thing for wall
// contact -- cheap, and it agrees exactly with the cells agents may occupy --
// but as a picture it is a stair-step: the zero crossing sits on cell centres,
// so every wall is a polyline of half-unit facets, and the corners a spline
// authors are exactly as sharp as the control points make them. tissue.frag
// used to hide the facets under a noise warp, which is what gave every lane a
// wobbling, hand-torn outline.
//
// This field is different in three ways, none of which the sim wants:
//
//   ANALYTIC. Every texel holds the true distance to the union of capsules the
//   vessel splines sweep out (and to the obstacle primitives carved back out of
//   them), evaluated per texel. There is no mask in the loop, so a straight
//   wall is straight to floating-point precision and a bend is a true arc.
//
//   ROUNDED. Concave corners of the lumen -- the inside of a bend, the crotch
//   of a fork, where an island meets a wall -- are filleted by a morphological
//   closing of radius `round_radius`. The closing is BOUNDED (it can only eat
//   wall within `round_depth_frac * R` of the lumen, so a sharp wedge is
//   blunted rather than pushed back by radii) and it PROTECTS thin structures
//   (a septum, a bar, an island thinner than 2R is found on the wall's medial
//   axis and left whole). This is what turns the near-cusp at the inside of a
//   tight bend into the soft inner radius the target look has.
//
//   PADDED. It covers a margin around the level so a lane that reaches the
//   edge of the world runs cleanly off the screen on a wide display instead of
//   smearing its last texel column, and a vessel whose end sits at the edge is
//   extended along its tangent rather than capped.
//
// Not merely cosmetic: LevelLoader::bake_geometry() bakes this and writes its
// sign back into the TissueMask's walkability, so the wall the horde presses
// against IS the wall on screen, fillets included. Baked once per level load
// (~60-150 ms on the shipped levels) and handed to render::TissueDecor.
#pragma once

#include "core/Types.h"

#include <vector>

namespace immune::game {

struct LevelDef;

struct RenderSdfDesc {
    /// Upper bound on texels; the cell size grows to fit large levels under it.
    usize texel_budget = 1'000'000;
    /// Never finer than this fraction of the level's own sim cell size --
    /// past that point the bilinear reconstruction is already sub-pixel.
    f32 min_cell_frac = 0.7f;
    /// Padding on every side, as a fraction of the level's extent on that axis.
    f32 margin_frac = 0.5f;
    /// Fillet radius for concave lumen corners, as a fraction of the narrowest
    /// vessel width in the level, and its absolute cap in world units.
    f32 round_frac = 0.30f;
    f32 round_max = 14.0f;
    /// Bounded-closing depth: wall deeper than this fraction of the fillet
    /// radius is never removed, which keeps an acute wedge (a fork's crotch)
    /// from retreating by several radii. 0.5 rounds a right-angle corner
    /// completely (its fillet reaches 0.41 R deep) and merely blunts anything
    /// sharper. Structures thinner than 2 R are protected separately (see the
    /// .cpp), so this bound is only ever about corners.
    f32 round_depth_frac = 0.5f;
    /// A vessel endpoint closer than this many widths to the world edge (or
    /// past it) is treated as leaving the level and extended off the grid.
    f32 edge_extend_widths = 0.5f;
};

struct RenderSdf {
    i32 width = 0;
    i32 height = 0;
    f32 cell_size = 0.5f;
    /// World rectangle the grid covers; texel (0,0) is centred at
    /// bounds.min + cell_size/2.
    Rect bounds;
    /// Fillet radius actually applied, world units (0 = none).
    f32 round_radius = 0.0f;
    /// Row-major, `width * height`, world units, POSITIVE inside the lumen.
    std::vector<f32> distance;

    bool valid() const { return width > 0 && height > 0 && !distance.empty(); }
    /// Bilinear sample; clamps to the grid.
    f32 sample(Vec2 world) const;
};

/// Bakes the field for `def`. Never fails: a level with no vessels yields a
/// grid of solid wall.
RenderSdf bake_render_sdf(const LevelDef& def, const RenderSdfDesc& desc = {});

/// Rewrites `field` (row-major w*h, world units per `cell`) with the exact
/// distance to its own zero contour, keeping each texel's sign. The contour is
/// located to sub-texel precision from the field's gradient, so the result is
/// smooth even though it went through a grid. Exposed for tests.
void resample_to_contour(std::vector<f32>& field, i32 w, i32 h, f32 cell);

} // namespace immune::game
