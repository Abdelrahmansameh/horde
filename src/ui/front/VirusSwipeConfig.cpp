#include "ui/front/VirusSwipeConfig.h"

#include "config/Json.h"
#include "platform/FileIO.h"

#include <array>
#include <cmath>
#include <exception>
#include <string_view>
#include <utility>

namespace immune::ui {
namespace {

f32 ranged_number(const config::Json& j, const char* key, const config::Ctx& ctx,
                  f32 min, f32 max) {
    const f32 value = config::require_f32(j, key, ctx);
    if (!std::isfinite(value) || value < min || value > max) {
        ctx.fail("field '" + std::string(key) + "' must be finite and between " +
                 std::to_string(min) + " and " + std::to_string(max));
    }
    return value;
}

gui::Color hex_color(const config::Json& j, const char* key, const config::Ctx& ctx) {
    const std::string text = config::require_string(j, key, ctx);
    gui::Color color{};
    if (!gui::parse_hex_color(text, color)) {
        ctx.fail("field '" + std::string(key) + "' must be a hex color such as #FFFFFF or #FFFFFFFF");
    }
    return color;
}

} // namespace

bool load_virus_swipe_config(const std::string& path, VirusSwipeConfig& out,
                             std::string& error) {
    const auto text = platform::read_text_file(path);
    if (!text) {
        error = "virus swipe: cannot read " + path;
        return false;
    }

    try {
        const config::Json j = config::Json::parse(*text);
        const config::Ctx ctx(path);
        if (!j.is_object()) ctx.fail("top level must be an object");
        constexpr std::array<std::string_view, 18> keys{
            "enabled", "virus_count", "density", "duration_seconds", "cover_fraction",
            "hold_seconds", "switch_fraction", "min_size", "max_size", "edge_irregularity", "drift", "spin",
            "seed", "from_left", "tint", "stagger_seconds", "speed_variation", "path_spread"
        };
        config::reject_unknown_keys(j, keys, ctx);

        VirusSwipeConfig staged;
        staged.enabled = config::require_bool(j, "enabled", ctx);
        staged.virus_count = config::require_i32(j, "virus_count", ctx);
        if (staged.virus_count < 1 || staged.virus_count > 50000) {
            ctx.fail("field 'virus_count' must be a whole number between 1 and 50000");
        }
        staged.density = ranged_number(j, "density", ctx, 0.1f, 4.0f);
        staged.duration_seconds = ranged_number(j, "duration_seconds", ctx, 0.1f, 10.0f);
        staged.cover_fraction = ranged_number(j, "cover_fraction", ctx, 0.1f, 0.9f);
        staged.hold_seconds = ranged_number(j, "hold_seconds", ctx, 0.0f,
                                            staged.duration_seconds * 0.5f);
        staged.switch_fraction = ranged_number(j, "switch_fraction", ctx, 0.0f, 1.0f);
        staged.min_size = ranged_number(j, "min_size", ctx, 12.0f, 300.0f);
        staged.max_size = ranged_number(j, "max_size", ctx, 12.0f, 300.0f);
        if (staged.max_size < staged.min_size) {
            ctx.fail("field 'max_size' must be greater than or equal to 'min_size'");
        }
        staged.edge_irregularity = ranged_number(j, "edge_irregularity", ctx, 0.0f, 400.0f);
        staged.drift = ranged_number(j, "drift", ctx, 0.0f, 100.0f);
        staged.stagger_seconds = ranged_number(j, "stagger_seconds", ctx, 0.0f, 1.0f);
        staged.speed_variation = ranged_number(j, "speed_variation", ctx, 0.0f, 0.8f);
        staged.path_spread = ranged_number(j, "path_spread", ctx, 0.0f, 300.0f);
        staged.spin = ranged_number(j, "spin", ctx, -12.0f, 12.0f);
        staged.seed = config::require_u32(j, "seed", ctx);
        staged.from_left = config::require_bool(j, "from_left", ctx);
        staged.tint = hex_color(j, "tint", ctx);

        out = std::move(staged);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = "virus swipe (" + path + "): " + e.what();
        return false;
    }
}

} // namespace immune::ui
