#include "gui/Color.h"

namespace immune::gui {

bool parse_hex_color(std::string_view text, Color& out) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    auto nibble = [](char c, u32& v) {
        if (c >= '0' && c <= '9') { v = static_cast<u32>(c - '0'); return true; }
        if (c >= 'a' && c <= 'f') { v = static_cast<u32>(c - 'a' + 10); return true; }
        if (c >= 'A' && c <= 'F') { v = static_cast<u32>(c - 'A' + 10); return true; }
        return false;
    };
    u32 digits[8] = {};
    if (text.size() != 3 && text.size() != 6 && text.size() != 8) return false;
    for (usize i = 0; i < text.size(); ++i) {
        if (!nibble(text[i], digits[i])) return false;
    }
    if (text.size() == 3) {
        out = Color{static_cast<f32>(digits[0] * 17) / 255.0f, static_cast<f32>(digits[1] * 17) / 255.0f,
                    static_cast<f32>(digits[2] * 17) / 255.0f, 1.0f};
        return true;
    }
    auto byte = [&](usize i) { return static_cast<f32>(digits[i] * 16 + digits[i + 1]) / 255.0f; };
    out = Color{byte(0), byte(2), byte(4), text.size() == 8 ? byte(6) : 1.0f};
    return true;
}

} // namespace immune::gui
