// gui/draw/DrawList.h — the per-frame draw stream every widget records into.
//
// ONE VERTEX FORMAT, ONE PROGRAM
// Shapes, stroked curves, text and icons all go through the same 24-byte
// vertex and the same uber-shader (assets/shaders/gui.{vert,frag}); each
// vertex carries its mode. A shape's parameters live in `records` (uploaded
// as an SSBO) and its four vertices point at them. So a panel, the text on
// it, the icon beside the text and the curve behind it all batch into one
// draw call, and the only things that split a draw are a change of scissor
// clip, a stencil clip push/pop, or an offscreen layer.
//
// Coordinates are LOGICAL pixels (the 1920x1080 reference the design canvas
// is drawn at). The backend scales to the framebuffer; `device_px` (logical
// units per framebuffer pixel) is what stroke fringes are sized from.
#pragma once

#include "core/Types.h"
#include "gui/Color.h"
#include "gui/draw/Affine2.h"
#include "gui/draw/Shape.h"

#include <span>
#include <string_view>
#include <vector>

namespace immune::gui {

enum class VertexMode : u32 {
    Solid = 0,  ///< Tessellated geometry; colour already carries the AA fringe.
    Shape = 1,  ///< SDF shape; uv = local position, record = its ShapeRecord.
    Text = 2,   ///< SDF glyph; uv into the font atlas, record = text style.
    Image = 3,  ///< Premultiplied RGBA from the icon atlas, tinted by colour.
    Layer = 4,  ///< Composite of an offscreen layer, tinted by colour.
    ImageGray = 5,  ///< Icon-atlas quad desaturated (an unaffordable card's icon).
};

struct Vertex {
    Vec2 pos;
    Vec2 uv;
    u32 color = 0;         ///< Premultiplied RGBA8.
    u32 mode_record = 0;   ///< mode << 28 | record index.
};
static_assert(sizeof(Vertex) == 24, "gui.vert reads a 24-byte vertex");

inline u32 pack_mode(VertexMode m, u32 record) {
    return (static_cast<u32>(m) << 28) | (record & 0x0FFFFFFFu);
}

enum class CmdKind : u8 {
    Draw,         ///< Draw `index_count` indices with the cmd's clip state.
    StencilPush,  ///< Write a clip shape into the stencil (increment).
    StencilPop,   ///< Remove it again (decrement).
    LayerBegin,   ///< Start drawing into offscreen layer `layer_depth`.
    LayerEnd,     ///< Composite it (the cmd's indices are the composite quad).
};

struct DrawCmd {
    CmdKind kind = CmdKind::Draw;
    u32 first_index = 0;
    u32 index_count = 0;
    Rect clip;               ///< Scissor, logical px.
    u32 stencil_depth = 0;   ///< Stencil value the draw must match.
    u32 layer_depth = 0;     ///< 0 = the default framebuffer.
};

enum class LineCap : u8 { Butt, Round };
enum class LineJoin : u8 { Miter, Round };

struct StrokeStyle {
    f32 width = 1.0f;
    Color color = kBlack;
    LineCap cap = LineCap::Round;
    LineJoin join = LineJoin::Round;
    f32 miter_limit = 4.0f;
    /// Dash pattern in logical px along the line; zero length = solid.
    /// `dash_offset` follows SVG's stroke-dashoffset.
    f32 dash_length = 0.0f;
    f32 dash_gap = 0.0f;
    f32 dash_offset = 0.0f;
};

/// A text style's GPU-side parameters, stored as a ShapeRecord so text and
/// shapes share one SSBO: geom[0] = outline width, geom[1] = softness (blur),
/// colors0[2] = outline colour.
struct TextStyleRecord {
    Color outline = kTransparent;
    f32 outline_width = 0.0f;  ///< Logical px.
    f32 softness = 0.0f;       ///< Logical px of extra edge blur (shadows).
};

class DrawList {
public:
    /// Starts a frame. `viewport` is the logical size of the target;
    /// `device_px` logical units per framebuffer pixel; `time` drives
    /// shader animation (wobble, slosh, dash flow) and hit tests.
    void reset(Vec2 viewport, f32 device_px, f32 time);

    Vec2 viewport() const { return viewport_; }
    f32 device_px() const { return device_px_; }
    f32 time() const { return time_; }

    // ---- State stacks ------------------------------------------------------
    void push_transform(const Affine2& t);   ///< Composes with the current one.
    void pop_transform();
    const Affine2& transform() const { return transforms_.back(); }

    /// Axis-aligned scissor, in the current transform's space, intersected
    /// with the enclosing clip. Rotations are ignored (bounding box).
    void push_clip_rect(Rect r);
    void pop_clip_rect();
    Rect clip_rect() const { return clips_.back(); }

    /// Clips following draws to the inside of `shape` via the stencil
    /// buffer. Nestable up to 8 deep.
    void push_clip_shape(const ShapeDesc& shape);
    void pop_clip_shape();

    /// Multiplies the alpha of everything drawn until the matching pop
    /// (nested pushes multiply). Cheap per-widget opacity; overlapping parts
    /// of a translucent group show through each other — use push_layer when
    /// the group must fade as one.
    void push_alpha(f32 alpha);
    void pop_alpha();
    f32 alpha() const { return alphas_.back(); }

    /// Draws following content into an offscreen layer, composited at
    /// `opacity` on pop — for fading a group as one (no overlap seams).
    void push_layer(f32 opacity);
    void pop_layer();

    // ---- Primitives -------------------------------------------------------
    /// Returns the record index (so a caller can reuse it for hit testing).
    u32 shape(const ShapeDesc& d);

    void stroke_polyline(std::span<const Vec2> points, bool closed, const StrokeStyle& style);
    /// Convex polygon fill with an AA fringe.
    void fill_convex(std::span<const Vec2> points, Color color);

    /// Adds a text style record for glyph quads; returns its index.
    u32 text_style(const TextStyleRecord& s);
    /// One glyph quad (dst in current-transform space).
    void glyph(Rect dst, Vec2 uv0, Vec2 uv1, Color color, u32 style_record);
    /// One icon-atlas quad; `grayscale` desaturates it.
    void image(Rect dst, Vec2 uv0, Vec2 uv1, Color tint, bool grayscale = false);

    // ---- Output -----------------------------------------------------------
    const std::vector<Vertex>& vertices() const { return vertices_; }
    const std::vector<u32>& indices() const { return indices_; }
    const std::vector<ShapeRecord>& records() const { return records_; }
    const std::vector<DrawCmd>& commands() const { return cmds_; }
    u32 max_layer_depth() const { return max_layer_depth_; }
    bool empty() const { return indices_.empty(); }

    /// Records a shape added this frame, in target space, for hit tests.
    const ShapeRecord& record(u32 i) const { return records_[i]; }

private:
    void ensure_draw_cmd();
    void quad(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, Vec2 uv0, Vec2 uv1, Vec2 uv2, Vec2 uv3,
              u32 color, u32 mode_record);
    u32 push_record(const ShapeRecord& r);
    /// Emits the quad for record `r` (already in target space).
    void shape_quad(const ShapeRecord& r, u32 record_index, f32 margin);
    void stroke_single(std::span<const Vec2> pts, bool closed, f32 width, u32 color, LineCap cap,
                       LineJoin join, f32 miter_limit);
    void round_fan(Vec2 c, f32 radius, f32 a0, f32 a1, u32 core, u32 edge);

    Vec2 viewport_{1920.0f, 1080.0f};
    f32 device_px_ = 1.0f;
    f32 time_ = 0.0f;

    std::vector<Vertex> vertices_;
    std::vector<u32> indices_;
    std::vector<ShapeRecord> records_;
    std::vector<DrawCmd> cmds_;

    std::vector<Affine2> transforms_{Affine2{}};
    std::vector<f32> alphas_{1.0f};
    std::vector<Rect> clips_;

    struct StencilEntry {
        u32 first_index;
        u32 index_count;
    };
    std::vector<StencilEntry> stencil_stack_;
    /// Per-layer stencil depth: a layer starts with its own clean stencil.
    std::vector<u32> stencil_depth_stack_{0};
    std::vector<f32> layer_opacity_;
    u32 max_layer_depth_ = 0;
    std::vector<Vec2> scratch_;
};

/// Builds flattened polylines from path commands (beziers are subdivided to
/// `tolerance` logical px) — vessels, lane thumbnails, custom outlines.
class Path {
public:
    explicit Path(f32 tolerance = 0.25f) : tol_(tolerance) {}

    Path& move_to(Vec2 p);
    Path& line_to(Vec2 p);
    Path& quad_to(Vec2 c, Vec2 p);
    Path& cubic_to(Vec2 c1, Vec2 c2, Vec2 p);
    /// Arc around `center` from angle a0 to a1 (radians).
    Path& arc(Vec2 center, f32 radius, f32 a0, f32 a1);
    Path& close();

    /// Parses SVG path data (M L H V C S Q T Z, absolute and relative).
    /// False on malformed input; whatever parsed before the error is kept.
    bool svg(std::string_view d);

    struct Contour {
        std::vector<Vec2> points;
        bool closed = false;
    };
    const std::vector<Contour>& contours() const { return contours_; }

    void stroke(DrawList& dl, const StrokeStyle& style) const;
    /// Total length of every contour.
    f32 length() const;

private:
    Contour& current();
    std::vector<Contour> contours_;
    Vec2 cursor_{0.0f, 0.0f};
    Vec2 start_{0.0f, 0.0f};
    Vec2 last_ctrl_{0.0f, 0.0f};
    char last_cmd_ = 0;
    f32 tol_;
};

} // namespace immune::gui
