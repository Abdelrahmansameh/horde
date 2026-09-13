// ui/editor/EditorGizmos.h — world-space overlay drawing. NEW MODULE.
//
// RATIONALE
// The renderer has exactly one world-space line facility (submit_flow_debug's
// shader, shared by submit_squad_debug) and it is LINES ONLY: no fills, no
// text, no thick antialiased strokes. Rather than grow the frozen renderer
// interface with a whole 2D vector-graphics pass that only the editor would
// use, the gizmo layer draws into ImGui::GetBackgroundDrawList(), projecting
// through Camera::world_to_screen().
//
// That is exact by design -- the camera's tilt is fixed, so world<->screen is
// affine (render/Camera.h) -- and it buys antialiased polylines, filled
// handles, dashes and labels for free.
//
// THE ONE WRINKLE: A WORLD CIRCLE IS A SCREEN ELLIPSE.
// World Y is foreshortened by cos(tilt), so ImDrawList::AddCircle is wrong for
// anything measured in world units. Every ring here is an N-gon built through
// world_to_screen, exactly as ui/Hud.cpp's draw_range_ring already does it.
// Cheap: hundreds of primitives against a renderer that draws ten thousand
// agents.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

struct ImDrawList;

namespace immune::render { class Camera; }

namespace immune::ui {

/// Colour palette for the editor overlay. Kept in one place because the editor
/// speaks a colour language of its own (selection, hover, error, warning) that
/// must not be confused with the game's family/tower hues.
namespace gizmo_color {
u32 selection();
u32 hover();
u32 handle();
u32 handle_hollow();
u32 vessel();
u32 vessel_ribbon();
u32 obstacle();
u32 spawn();
u32 objective();
u32 zone();
u32 zone_concentrated();
u32 squad_path();
u32 world_bounds();
u32 camera_frame();
u32 grid();
u32 error();
u32 warning();
u32 ghost();
} // namespace gizmo_color

/// Projection + drawing helpers bound to one camera and one draw list.
///
/// Everything takes WORLD coordinates and handles the projection internally,
/// so no caller has to remember the ellipse rule.
class Gizmos {
public:
    Gizmos(ImDrawList* dl, const render::Camera& cam) : dl_(dl), cam_(&cam) {}

    Vec2 to_screen(Vec2 world) const;
    Vec2 to_world(Vec2 screen) const;
    /// World-unit length of `pixels` on screen, along X (which the tilt does
    /// not foreshorten). How every handle stays a constant apparent size.
    f32 world_per_pixel() const;

    void line(Vec2 a, Vec2 b, u32 color, f32 thickness = 1.5f) const;
    void dashed_line(Vec2 a, Vec2 b, u32 color, f32 dash_world, f32 thickness = 1.5f) const;
    void polyline(const std::vector<Vec2>& pts, u32 color, f32 thickness = 1.5f,
                  bool closed = false) const;
    /// Filled polygon. Convex only -- ImGui's filler assumes it.
    void convex_fill(const std::vector<Vec2>& pts, u32 color) const;

    /// A world-radius circle, drawn as an N-gon so the tilt turns it into the
    /// ellipse it actually is.
    void ring(Vec2 center, f32 radius, u32 color, f32 thickness = 1.5f, i32 segments = 48) const;
    void disc(Vec2 center, f32 radius, u32 color, i32 segments = 48) const;

    /// An axis-aligned world rect (four projected corners, so it leans with the
    /// tilt rather than being an axis-aligned screen box).
    void rect(const Rect& r, u32 color, f32 thickness = 1.5f) const;
    void rect_filled(const Rect& r, u32 color) const;

    /// A grab handle at a world point, sized in PIXELS so it stays usable at
    /// any zoom. `filled` marks selection.
    void handle(Vec2 world, u32 color, bool filled, f32 pixel_radius = 4.5f) const;
    /// A diamond, for midpoint-insert affordances -- deliberately a different
    /// silhouette from a control point so the two do not read alike.
    void diamond(Vec2 world, u32 color, bool filled, f32 pixel_radius = 4.0f) const;

    /// A capsule outline: the swept-disc shape a vessel segment or a capsule
    /// obstacle actually is.
    void capsule(Vec2 a, Vec2 b, f32 radius, u32 color, f32 thickness = 1.5f) const;

    /// The translucent width band along a vessel or squad path.
    void ribbon(const std::vector<Vec2>& center, const std::vector<f32>& half_widths,
                u32 color) const;

    void label(Vec2 world, const std::string& text, u32 color, Vec2 pixel_offset = {8.0f, -8.0f}) const;

    /// A pulsing halo marking a validation issue.
    void halo(Vec2 world, f32 radius, u32 color, f32 phase) const;

private:
    ImDrawList* dl_ = nullptr;
    const render::Camera* cam_ = nullptr;
};

/// Samples a Catmull-Rom through `points` the same way the rasterizer does, so
/// the curve drawn in the viewport is the curve that gets baked. `out_widths`
/// receives the interpolated half-width at each sample when non-null.
void sample_vessel_curve(const std::vector<Vec2>& points, const std::vector<f32>& widths,
                         i32 samples_per_segment, std::vector<Vec2>& out,
                         std::vector<f32>* out_half_widths);

} // namespace immune::ui
