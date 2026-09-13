// ui/editor/EditorGizmos.cpp — see EditorGizmos.h for why this draws into an
// ImGui draw list rather than through the renderer, and for the ellipse rule.
#include "ui/editor/EditorGizmos.h"

#include "core/Math.h"
#include "render/Camera.h"
#include "sim/flowfield/TissueRaster.h"   // eval_spline, so the drawn curve IS the baked curve

#include <imgui.h>

#include <cmath>

namespace immune::ui {

namespace gizmo_color {
// Deliberately NOT the game's family/tower hues: an editor overlay that reuses
// gameplay colours makes "this is selected" and "this is a virus" look alike.
u32 selection() { return IM_COL32(255, 214, 64, 255); }
u32 hover() { return IM_COL32(255, 255, 255, 200); }
u32 handle() { return IM_COL32(240, 240, 245, 235); }
u32 handle_hollow() { return IM_COL32(30, 32, 40, 235); }
u32 vessel() { return IM_COL32(120, 200, 255, 220); }
u32 vessel_ribbon() { return IM_COL32(120, 200, 255, 40); }
u32 obstacle() { return IM_COL32(255, 150, 90, 230); }
u32 spawn() { return IM_COL32(120, 255, 160, 230); }
u32 objective() { return IM_COL32(255, 120, 190, 235); }
u32 zone() { return IM_COL32(150, 160, 255, 170); }
u32 zone_concentrated() { return IM_COL32(200, 170, 255, 210); }
u32 squad_path() { return IM_COL32(255, 235, 140, 190); }
u32 world_bounds() { return IM_COL32(200, 200, 210, 120); }
u32 camera_frame() { return IM_COL32(90, 230, 220, 200); }
u32 grid() { return IM_COL32(255, 255, 255, 26); }
u32 error() { return IM_COL32(255, 80, 80, 255); }
u32 warning() { return IM_COL32(255, 190, 60, 255); }
u32 ghost() { return IM_COL32(180, 190, 210, 90); }
} // namespace gizmo_color

namespace {
ImVec2 iv(Vec2 v) { return ImVec2(v.x, v.y); }
}

Vec2 Gizmos::to_screen(Vec2 world) const { return cam_->world_to_screen(world); }
Vec2 Gizmos::to_world(Vec2 screen) const { return cam_->screen_to_world(screen); }

f32 Gizmos::world_per_pixel() const {
    // Measured along X, which the tilt does not foreshorten, so one number
    // describes handle size honestly in both axes' worst case.
    const Vec2 a = cam_->screen_to_world(Vec2{0.0f, 0.0f});
    const Vec2 b = cam_->screen_to_world(Vec2{1.0f, 0.0f});
    return math::max(std::fabs(b.x - a.x), 1e-6f);
}

void Gizmos::line(Vec2 a, Vec2 b, u32 color, f32 thickness) const {
    dl_->AddLine(iv(to_screen(a)), iv(to_screen(b)), color, thickness);
}

void Gizmos::dashed_line(Vec2 a, Vec2 b, u32 color, f32 dash_world, f32 thickness) const {
    const f32 len = math::length(b - a);
    if (len <= 1e-4f || dash_world <= 0.0f) return;
    const i32 steps = math::min(static_cast<i32>(len / dash_world), 4096);
    for (i32 i = 0; i < steps; i += 2) {
        const f32 t0 = static_cast<f32>(i) / static_cast<f32>(steps);
        const f32 t1 = math::min(static_cast<f32>(i + 1) / static_cast<f32>(steps), 1.0f);
        line(a + (b - a) * t0, a + (b - a) * t1, color, thickness);
    }
}

void Gizmos::polyline(const std::vector<Vec2>& pts, u32 color, f32 thickness, bool closed) const {
    if (pts.size() < 2) return;
    std::vector<ImVec2> sp;
    sp.reserve(pts.size());
    for (Vec2 p : pts) sp.push_back(iv(to_screen(p)));
    dl_->AddPolyline(sp.data(), static_cast<int>(sp.size()), color,
                     closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
}

void Gizmos::convex_fill(const std::vector<Vec2>& pts, u32 color) const {
    if (pts.size() < 3) return;
    std::vector<ImVec2> sp;
    sp.reserve(pts.size());
    for (Vec2 p : pts) sp.push_back(iv(to_screen(p)));
    dl_->AddConvexPolyFilled(sp.data(), static_cast<int>(sp.size()), color);
}

void Gizmos::ring(Vec2 center, f32 radius, u32 color, f32 thickness, i32 segments) const {
    if (radius <= 0.0f) return;
    // N-gon through world_to_screen, NOT AddCircle: world Y is foreshortened by
    // cos(tilt), so a world circle is a screen ellipse.
    std::vector<Vec2> pts;
    pts.reserve(static_cast<usize>(segments));
    for (i32 i = 0; i < segments; ++i) {
        const f32 a = (static_cast<f32>(i) / static_cast<f32>(segments)) * math::kTwoPi;
        pts.push_back(center + Vec2{std::cos(a), std::sin(a)} * radius);
    }
    polyline(pts, color, thickness, true);
}

void Gizmos::disc(Vec2 center, f32 radius, u32 color, i32 segments) const {
    if (radius <= 0.0f) return;
    std::vector<Vec2> pts;
    pts.reserve(static_cast<usize>(segments));
    for (i32 i = 0; i < segments; ++i) {
        const f32 a = (static_cast<f32>(i) / static_cast<f32>(segments)) * math::kTwoPi;
        pts.push_back(center + Vec2{std::cos(a), std::sin(a)} * radius);
    }
    convex_fill(pts, color);
}

void Gizmos::rect(const Rect& r, u32 color, f32 thickness) const {
    // Four projected corners rather than an axis-aligned screen box, so the
    // rect leans with the tilt like everything else in the world.
    const std::vector<Vec2> pts = {r.min, Vec2{r.max.x, r.min.y}, r.max, Vec2{r.min.x, r.max.y}};
    polyline(pts, color, thickness, true);
}

void Gizmos::rect_filled(const Rect& r, u32 color) const {
    const std::vector<Vec2> pts = {r.min, Vec2{r.max.x, r.min.y}, r.max, Vec2{r.min.x, r.max.y}};
    convex_fill(pts, color);
}

void Gizmos::handle(Vec2 world, u32 color, bool filled, f32 pixel_radius) const {
    const ImVec2 c = iv(to_screen(world));
    // Sized in PIXELS, so a control point stays grabbable whether you are
    // framed on the whole level or on one bend.
    if (filled) {
        dl_->AddCircleFilled(c, pixel_radius, color, 12);
        dl_->AddCircle(c, pixel_radius, gizmo_color::handle_hollow(), 12, 1.2f);
    } else {
        dl_->AddCircleFilled(c, pixel_radius, gizmo_color::handle_hollow(), 12);
        dl_->AddCircle(c, pixel_radius, color, 12, 1.6f);
    }
}

void Gizmos::diamond(Vec2 world, u32 color, bool filled, f32 pixel_radius) const {
    const Vec2 c = to_screen(world);
    const ImVec2 pts[4] = {ImVec2(c.x, c.y - pixel_radius), ImVec2(c.x + pixel_radius, c.y),
                           ImVec2(c.x, c.y + pixel_radius), ImVec2(c.x - pixel_radius, c.y)};
    if (filled) dl_->AddConvexPolyFilled(pts, 4, color);
    else dl_->AddPolyline(pts, 4, color, ImDrawFlags_Closed, 1.4f);
}

void Gizmos::capsule(Vec2 a, Vec2 b, f32 radius, u32 color, f32 thickness) const {
    const Vec2 d = b - a;
    const f32 len = math::length(d);
    if (len <= 1e-4f) {
        ring(a, radius, color, thickness);
        return;
    }
    const Vec2 n = Vec2{-d.y, d.x} / len;
    // Two offset sides plus two end caps -- built in world space so the tilt
    // handles the projection uniformly.
    std::vector<Vec2> outline;
    constexpr i32 kCap = 16;
    for (i32 i = 0; i <= kCap; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kCap);
        const f32 ang = std::atan2(n.y, n.x) - math::kPi * t;
        outline.push_back(b + Vec2{std::cos(ang), std::sin(ang)} * radius);
    }
    for (i32 i = 0; i <= kCap; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kCap);
        const f32 ang = std::atan2(-n.y, -n.x) - math::kPi * t;
        outline.push_back(a + Vec2{std::cos(ang), std::sin(ang)} * radius);
    }
    polyline(outline, color, thickness, true);
}

void Gizmos::ribbon(const std::vector<Vec2>& center, const std::vector<f32>& half_widths,
                    u32 color) const {
    if (center.size() < 2 || half_widths.size() != center.size()) return;
    // One quad per segment rather than one big polygon: the band is not convex
    // in general, and AddConvexPolyFilled would fold it inside out on a bend.
    for (usize i = 0; i + 1 < center.size(); ++i) {
        const Vec2 d = center[i + 1] - center[i];
        const f32 len = math::length(d);
        if (len <= 1e-5f) continue;
        const Vec2 n = Vec2{-d.y, d.x} / len;
        const std::vector<Vec2> quad = {center[i] + n * half_widths[i],
                                        center[i + 1] + n * half_widths[i + 1],
                                        center[i + 1] - n * half_widths[i + 1],
                                        center[i] - n * half_widths[i]};
        convex_fill(quad, color);
    }
}

void Gizmos::label(Vec2 world, const std::string& text, u32 color, Vec2 pixel_offset) const {
    const Vec2 s = to_screen(world);
    dl_->AddText(ImVec2(s.x + pixel_offset.x, s.y + pixel_offset.y), color, text.c_str());
}

void Gizmos::halo(Vec2 world, f32 radius, u32 color, f32 phase) const {
    // Two rings breathing out of phase: legible against busy tissue without
    // needing a fill that would hide the thing it is pointing at.
    const f32 pulse = 0.5f + 0.5f * std::sin(phase * 3.0f);
    ring(world, radius * (1.0f + 0.15f * pulse), color, 2.5f, 32);
    ring(world, radius * (0.75f + 0.1f * pulse), color, 1.5f, 32);
}

void sample_vessel_curve(const std::vector<Vec2>& points, const std::vector<f32>& widths,
                         i32 samples_per_segment, std::vector<Vec2>& out,
                         std::vector<f32>* out_half_widths) {
    out.clear();
    if (out_half_widths) out_half_widths->clear();
    if (points.size() < 2) {
        out = points;
        if (out_half_widths) {
            for (f32 w : widths) out_half_widths->push_back(w * 0.5f);
        }
        return;
    }

    // Built through the RASTERIZER's own eval_spline, so the curve the author
    // sees is the curve that gets baked. A separate spline evaluator here would
    // be a second source of truth about level geometry, which is the exact
    // thing this editor exists to avoid.
    sim::VesselSpline spline;
    spline.points.reserve(points.size());
    for (usize i = 0; i < points.size(); ++i) {
        spline.points.push_back(
            sim::VesselPoint{points[i], i < widths.size() ? widths[i] : 4.0f, 1.0f});
    }

    const i32 segments = static_cast<i32>(points.size()) - 1;
    const i32 steps = math::max(segments * math::max(samples_per_segment, 2), 2);
    out.reserve(static_cast<usize>(steps) + 1);
    for (i32 k = 0; k <= steps; ++k) {
        const f32 u = static_cast<f32>(segments) * static_cast<f32>(k) / static_cast<f32>(steps);
        const sim::VesselPoint vp = sim::eval_spline(spline, u);
        out.push_back(vp.pos);
        if (out_half_widths) out_half_widths->push_back(vp.width * 0.5f);
    }
}

} // namespace immune::ui
