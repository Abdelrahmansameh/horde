// ui/front/Backdrop.h — the living tissue behind the out-of-match screens.
//
// The canvas's Menu, Levels and Tree artboards sit on the same backdrop: a
// field of dark tissue cells (rounded blobs, each with a nucleus and a
// nucleolus) on the tissue red, crossed by a blood vessel. Here the cells are
// SDF membranes that wobble slowly, laid out on a jittered grid from a fixed
// seed, so the field fills any aspect ratio and looks the same every launch.
#pragma once

#include "gui/core/Widget.h"
#include "gui/draw/DrawList.h"

#include <vector>

namespace immune::ui {

/// Fills its rect with the tissue field.
class TissueBackdrop : public gui::Widget {
public:
    explicit TissueBackdrop(std::string id = "backdrop", u32 seed = 0x1A5E);

    void draw_self(gui::DrawList& dl) override;

private:
    struct Cell {
        Vec2 offset;        ///< From the grid point.
        Vec2 half;
        f32 rotation;
        f32 seed;
        Vec2 nucleus;       ///< From the cell centre.
        Vec2 nucleus_half;
        Vec2 nucleolus;
        f32 nucleolus_r;
        bool dot;
        Vec2 dot_pos;
        f32 dot_r;
    };
    /// One period of the pattern; the field tiles it.
    static constexpr i32 kCols = 15, kRows = 10;
    static constexpr f32 kPitchX = 128.0f, kPitchY = 113.0f;
    std::vector<Cell> cells_;
};

/// A curve stroked as a stack of layers (wall, rim, lumen, core; a flowing
/// dashed centre line), in the widget's own coordinates: the vessels on the
/// main menu and the level map, and the lymph vessels of the tree.
class VesselStroke : public gui::Widget {
public:
    struct Layer {
        gui::StrokeStyle style;
        /// Dash travel speed, px/s (the canvas's `flow` keyframe: -44 px per
        /// 1.8 s). 0 = still.
        f32 flow_speed = 0.0f;
    };

    explicit VesselStroke(std::string id = {});

    gui::Path path;
    std::vector<Layer> layers;

    void draw_self(gui::DrawList& dl) override;
};

} // namespace immune::ui
