#include "gui/icons/IconLibrary.h"

#include "core/Math.h"
#include "gui/draw/DrawList.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "gui/icons/NanoSvg.h"

namespace immune::gui {

struct IconLibrary::Icon {
    NSVGimage* image = nullptr;
    f32 pad_fraction = kPadFraction;
    std::map<i32, IconSprite> bakes;
    ~Icon() {
        if (image != nullptr) nsvgDelete(image);
    }
};

IconLibrary::IconLibrary() {
    atlas_.create(kAtlasSize, kAtlasSize, 4);
    rasterizer_ = nsvgCreateRasterizer();
}

IconLibrary::~IconLibrary() {
    if (rasterizer_ != nullptr) nsvgDeleteRasterizer(static_cast<NSVGrasterizer*>(rasterizer_));
}

usize IconLibrary::load_dir(const std::string& dir) {
    namespace fs = std::filesystem;
    usize loaded = 0;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".svg") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (load_svg(e.path().stem().string(), std::move(text))) ++loaded;
    }
    if (ec) errors_.push_back("cannot list " + dir + ": " + ec.message());
    return loaded;
}

bool IconLibrary::load_svg(const std::string& name, std::string svg_text) {
    // data-pad on the root <svg> overrides the bake padding (an illustration
    // whose viewBox already holds the whole drawing needs almost none).
    f32 pad_fraction = kPadFraction;
    if (const usize root = svg_text.find("<svg"); root != std::string::npos) {
        const usize root_end = svg_text.find('>', root);
        const usize attr = svg_text.find("data-pad=\"", root);
        if (attr != std::string::npos && attr < root_end) {
            pad_fraction = math::clamp(std::strtof(svg_text.c_str() + attr + 10, nullptr), 0.0f, 1.0f);
        }
    }
    // nsvgParse tokenizes in place, so it needs a mutable, terminated buffer.
    svg_text.push_back('\0');
    NSVGimage* image = nsvgParse(svg_text.data(), "px", 96.0f);
    if (image == nullptr || image->shapes == nullptr) {
        if (image != nullptr) nsvgDelete(image);
        errors_.push_back("icon " + name + ": no drawable shapes");
        return false;
    }
    auto icon = std::make_unique<Icon>();
    icon->image = image;
    icon->pad_fraction = pad_fraction;
    icons_[name] = std::move(icon);
    return true;
}

bool IconLibrary::has(std::string_view name) const { return icons_.find(name) != icons_.end(); }

std::vector<std::string> IconLibrary::names() const {
    std::vector<std::string> out;
    for (const auto& [n, _] : icons_) out.push_back(n);
    return out;
}

usize IconLibrary::shape_count(std::string_view name) const {
    const auto it = icons_.find(name);
    if (it == icons_.end()) return 0;
    usize n = 0;
    for (const NSVGshape* s = it->second->image->shapes; s != nullptr; s = s->next) ++n;
    return n;
}

void IconLibrary::clear_bakes() {
    for (auto& [_, icon] : icons_) icon->bakes.clear();
    atlas_.create(kAtlasSize, kAtlasSize, 4);
}

const IconSprite& IconLibrary::sprite(std::string_view name, i32 pixel_size) {
    const auto it = icons_.find(name);
    if (it == icons_.end()) return missing_;
    Icon& icon = *it->second;
    pixel_size = math::clamp(pixel_size, 4, kMaxBakeSize);
    if (auto b = icon.bakes.find(pixel_size); b != icon.bakes.end()) return b->second;

    IconSprite sp;
    const f32 vw = math::max(icon.image->width, 1.0f);
    const f32 vh = math::max(icon.image->height, 1.0f);
    sp.view_size = Vec2{vw, vh};
    sp.pad = math::max(vw, vh) * icon.pad_fraction;
    const f32 scale = static_cast<f32>(pixel_size) / math::max(vw, vh);
    const i32 w = static_cast<i32>(std::ceil((vw + 2.0f * sp.pad) * scale));
    const i32 h = static_cast<i32>(std::ceil((vh + 2.0f * sp.pad) * scale));
    std::vector<u8> pixels(static_cast<usize>(w) * static_cast<usize>(h) * 4u, 0);
    nsvgRasterize(static_cast<NSVGrasterizer*>(rasterizer_), icon.image, sp.pad * scale, sp.pad * scale, scale,
                  pixels.data(), w, h, w * 4);
    // nanosvgrast writes straight alpha; the gui pipeline is premultiplied.
    for (usize i = 0; i < pixels.size(); i += 4) {
        const u32 a = pixels[i + 3];
        pixels[i + 0] = static_cast<u8>((pixels[i + 0] * a + 127) / 255);
        pixels[i + 1] = static_cast<u8>((pixels[i + 1] * a + 127) / 255);
        pixels[i + 2] = static_cast<u8>((pixels[i + 2] * a + 127) / 255);
    }
    i32 x = 0, y = 0;
    if (atlas_.pack(w, h, x, y)) {
        atlas_.blit(x, y, w, h, pixels.data());
        sp.uv0 = atlas_.uv(x, y);
        sp.uv1 = atlas_.uv(x + w, y + h);
        sp.valid = true;
    } else {
        errors_.push_back("icon atlas full baking " + std::string(name));
    }
    return icon.bakes.emplace(pixel_size, sp).first->second;
}

void IconLibrary::draw(DrawList& dl, std::string_view name, Rect dst, Color tint, f32 device_scale,
                       bool grayscale) {
    const Vec2 size = dst.size();
    const f32 on_screen = math::max(size.x, size.y) * device_scale * dl.transform().uniform_scale();
    const IconSprite& sp = sprite(name, static_cast<i32>(std::ceil(on_screen)));
    if (!sp.valid) return;
    // Map the viewBox onto dst, then grow by the baked padding.
    const Vec2 k{size.x / sp.view_size.x, size.y / sp.view_size.y};
    const Rect quad{dst.min - Vec2{sp.pad * k.x, sp.pad * k.y}, dst.max + Vec2{sp.pad * k.x, sp.pad * k.y}};
    dl.image(quad, sp.uv0, sp.uv1, tint, grayscale);
}

} // namespace immune::gui
