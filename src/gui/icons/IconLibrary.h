// gui/icons/IconLibrary.h — the UI's icons: SVG text files, rasterized on demand.
//
// Every icon in the design canvas is a small SVG (64x64 viewBox: paths,
// circles, ellipses, rounded rects, strokes with round caps and dash arrays,
// opacity, transforms). They are extracted verbatim into assets/ui/icons/ by
// tools/extract_icons.py, so what ships is exactly what was designed, and a
// designer can edit one in any vector tool.
//
// Each icon is parsed once (nanosvg) and rasterized (nanosvgrast) into a
// premultiplied RGBA atlas the first time it is drawn at a given pixel size.
// Drawing it is then a single textured quad, and fading a whole icon (a dimmed
// locked node) is exact, because the icon's overlapping parts were already
// composited when it was baked.
#pragma once

#include "core/Types.h"
#include "gui/Atlas.h"
#include "gui/Color.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace immune::gui {

class DrawList;

struct IconSprite {
    Vec2 uv0{0, 0}, uv1{0, 0};
    /// The baked cell covers the viewBox expanded by `pad` on every side (in
    /// viewBox units) because the canvas icons draw past their viewBox.
    f32 pad = 0.0f;
    Vec2 view_size{64.0f, 64.0f};
    bool valid = false;
};

class IconLibrary {
public:
    /// Fraction of the viewBox added on every side when baking. An icon's
    /// root <svg data-pad="..."> overrides it.
    static constexpr f32 kPadFraction = 0.25f;
    /// Largest bake, framebuffer px along the longer side (the menu mascot at
    /// 4K is about this big).
    static constexpr i32 kMaxBakeSize = 1100;
    static constexpr i32 kAtlasSize = 2048;

    IconLibrary();
    ~IconLibrary();
    IconLibrary(const IconLibrary&) = delete;
    IconLibrary& operator=(const IconLibrary&) = delete;

    /// Parses every *.svg in `dir`; the icon's name is the file stem. Returns
    /// the number loaded. Files that fail to parse are listed in errors().
    usize load_dir(const std::string& dir);
    /// Parses one SVG document from memory under `name` (tests, generated icons).
    bool load_svg(const std::string& name, std::string svg_text);

    bool has(std::string_view name) const;
    std::vector<std::string> names() const;
    const std::vector<std::string>& errors() const { return errors_; }
    /// Shape count of a parsed icon (0 when unknown) — a parse sanity check.
    usize shape_count(std::string_view name) const;

    /// The icon baked for a `pixel_size` x `pixel_size` on-screen box
    /// (framebuffer pixels), baking it on first request.
    const IconSprite& sprite(std::string_view name, i32 pixel_size);

    /// The pixel size an icon drawn `on_screen_px` big is baked at: exact up
    /// to 32 px, then in steps of ~6% so animated sizes share bakes.
    static i32 bake_size(f32 on_screen_px);

    /// Draws `name` fitted into `dst` (logical px; the viewBox maps onto dst,
    /// overflow draws outside it as in the canvas). `device_scale` is
    /// framebuffer px per logical px so the bake is pixel-exact.
    void draw(DrawList& dl, std::string_view name, Rect dst, Color tint = kWhite, f32 device_scale = 1.0f,
              bool grayscale = false);

    Atlas& atlas() { return atlas_; }
    const Atlas& atlas() const { return atlas_; }
    /// Drops every baked sprite (e.g. after the UI scale changes).
    void clear_bakes();

private:
    struct Icon;
    std::map<std::string, std::unique_ptr<Icon>, std::less<>> icons_;
    std::vector<std::string> errors_;
    Atlas atlas_;
    IconSprite missing_{};
    void* rasterizer_ = nullptr;
};

} // namespace immune::gui
