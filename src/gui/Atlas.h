// gui/Atlas.h — CPU-side texture atlas with a shelf packer and a dirty rect.
//
// Fonts (R8 SDF) and icons (RGBA8, premultiplied) both bake lazily, the first
// time something is drawn at a given size, into one of these. The backend
// uploads only the dirty rectangle, so a new glyph costs a few hundred bytes of
// upload, not the whole page.
#pragma once

#include "core/Math.h"
#include "core/Types.h"

#include <vector>

namespace immune::gui {

class Atlas {
public:
    void create(i32 width, i32 height, i32 channels) {
        width_ = width;
        height_ = height;
        channels_ = channels;
        pixels_.assign(static_cast<usize>(width) * static_cast<usize>(height) * static_cast<usize>(channels), 0);
        shelves_.clear();
        next_y_ = kPad;
        dirty_ = Dirty{0, 0, width, height};
        generation_ += 1;
    }

    /// Reserves a w x h cell. False when the page is full.
    bool pack(i32 w, i32 h, i32& out_x, i32& out_y) {
        for (Shelf& s : shelves_) {
            if (h <= s.height && s.x + w + kPad <= width_) {
                out_x = s.x;
                out_y = s.y;
                s.x += w + kPad;
                return true;
            }
        }
        if (next_y_ + h + kPad > height_ || w + 2 * kPad > width_) return false;
        shelves_.push_back(Shelf{next_y_, h, kPad + w + kPad});
        out_x = kPad;
        out_y = next_y_;
        next_y_ += h + kPad;
        return true;
    }

    /// Copies a tightly packed `w x h x channels` block to (x, y).
    void blit(i32 x, i32 y, i32 w, i32 h, const u8* src) {
        const usize row = static_cast<usize>(w) * static_cast<usize>(channels_);
        for (i32 j = 0; j < h; ++j) {
            u8* dst = pixels_.data() + (static_cast<usize>(y + j) * static_cast<usize>(width_) + static_cast<usize>(x)) *
                                           static_cast<usize>(channels_);
            const u8* s = src + static_cast<usize>(j) * row;
            for (usize k = 0; k < row; ++k) dst[k] = s[k];
        }
        mark_dirty(x, y, w, h);
    }

    struct Dirty {
        i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool empty() const { return x1 <= x0 || y1 <= y0; }
    };
    const Dirty& dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = Dirty{}; }

    i32 width() const { return width_; }
    i32 height() const { return height_; }
    i32 channels() const { return channels_; }
    const u8* pixels() const { return pixels_.data(); }
    /// Bumped by create(): the backend re-creates its texture when it changes.
    u32 generation() const { return generation_; }

    Vec2 uv(i32 x, i32 y) const {
        return Vec2{static_cast<f32>(x) / static_cast<f32>(width_), static_cast<f32>(y) / static_cast<f32>(height_)};
    }

private:
    static constexpr i32 kPad = 2;
    struct Shelf {
        i32 y;
        i32 height;
        i32 x;
    };
    void mark_dirty(i32 x, i32 y, i32 w, i32 h) {
        if (dirty_.empty()) {
            dirty_ = Dirty{x, y, x + w, y + h};
            return;
        }
        dirty_.x0 = math::min(dirty_.x0, x);
        dirty_.y0 = math::min(dirty_.y0, y);
        dirty_.x1 = math::max(dirty_.x1, x + w);
        dirty_.y1 = math::max(dirty_.y1, y + h);
    }

    i32 width_ = 0, height_ = 0, channels_ = 1;
    std::vector<u8> pixels_;
    std::vector<Shelf> shelves_;
    i32 next_y_ = kPad;
    Dirty dirty_{};
    u32 generation_ = 0;
};

} // namespace immune::gui
