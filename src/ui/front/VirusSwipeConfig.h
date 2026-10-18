#pragma once

#include "core/Types.h"
#include "gui/Color.h"

#include <string>

namespace immune::ui {

/// Main-menu Play transition tuning. The JSON is reloaded before each swipe.
/// Distances are logical UI pixels; density scales the sprite sizes by sqrt.
struct VirusSwipeConfig {
    bool enabled = true;
    i32 virus_count = 24000;
    f32 density = 1.0f;
    f32 duration_seconds = 1.095f;
    f32 cover_fraction = 0.52f;
    f32 hold_seconds = 0.015f;
    f32 switch_fraction = 0.5f; ///< Screen switch within the fully spread hold, 0=start, 1=end.
    f32 min_size = 32.0f;
    f32 max_size = 70.0f;
    f32 edge_irregularity = 100.0f;
    f32 drift = 22.0f;
    f32 stagger_seconds = 0.14f; ///< Maximum individual start delay in each pass.
    f32 speed_variation = 0.35f; ///< Individual speed-curve variation, 0..0.8.
    f32 path_spread = 90.0f; ///< Vertical curve amplitude while crossing, logical px.
    f32 spin = 1.8f;
    u32 seed = 1337;
    bool from_left = true;
    gui::Color tint = gui::kWhite;
};

/// Loads a complete, strictly validated JSON file. On failure, out is unchanged
/// and error describes the file/field. On success, error is cleared.
bool load_virus_swipe_config(const std::string& path, VirusSwipeConfig& out,
                             std::string& error);

} // namespace immune::ui
