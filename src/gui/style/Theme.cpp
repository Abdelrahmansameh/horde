#include "gui/style/Theme.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace immune::gui {

namespace {

// Document order matters: shapes name a "base" defined above them.
using json = nlohmann::ordered_json;

bool parse_font(std::string_view s, FontId& out) {
    static constexpr std::pair<std::string_view, FontId> kFonts[] = {
        {"fredoka-500", FontId::FredokaMedium},   {"fredoka-600", FontId::FredokaSemiBold},
        {"fredoka-700", FontId::FredokaBold},     {"nunito-600", FontId::NunitoSemiBold},
        {"nunito-700", FontId::NunitoBold},       {"nunito-800", FontId::NunitoExtraBold},
        {"nunito-900", FontId::NunitoBlack},
    };
    for (const auto& [name, id] : kFonts) {
        if (s == name) {
            out = id;
            return true;
        }
    }
    return false;
}

bool parse_kind(std::string_view s, ShapeKind& out) {
    if (s == "box") out = ShapeKind::Box;
    else if (s == "ellipse") out = ShapeKind::Ellipse;
    else if (s == "arc") out = ShapeKind::Arc;
    else if (s == "fluid") out = ShapeKind::Fluid;
    else if (s == "radial") out = ShapeKind::Radial;
    else return false;
    return true;
}

Vec2 vec2_of(const json& v) {
    if (v.is_array() && v.size() == 2) return Vec2{v[0].get<f32>(), v[1].get<f32>()};
    return Vec2{0.0f, 0.0f};
}

} // namespace

bool Theme::resolve_color(std::string_view expr, Color& out) const {
    f32 alpha = -1.0f;
    if (const usize at = expr.find('@'); at != std::string_view::npos) {
        alpha = std::strtof(std::string(expr.substr(at + 1)).c_str(), nullptr);
        expr = expr.substr(0, at);
    }
    Color c;
    if (!expr.empty() && expr.front() == '#') {
        if (!parse_hex_color(expr, c)) return false;
    } else {
        const auto it = colors_.find(expr);
        if (it == colors_.end()) return false;
        c = it->second;
    }
    if (alpha >= 0.0f) c.a = alpha;
    out = c;
    return true;
}

bool Theme::parse(std::string_view text, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return false;
    };
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::exception& e) {
        return fail(std::string("ui theme: ") + e.what());
    }
    if (!doc.is_object()) return fail("ui theme: top level must be an object");

    // Parse into a scratch theme so a bad document leaves this one intact.
    Theme next;
    if (doc.contains("colors")) {
        // Colours may name other colours; resolve in document order and keep
        // retrying forward references while that makes progress.
        const json& cols = doc["colors"];
        std::vector<std::pair<std::string, std::string>> pending;
        for (auto it = cols.begin(); it != cols.end(); ++it) {
            if (!it.value().is_string()) return fail("ui theme: colour " + it.key() + " must be a string");
            pending.emplace_back(it.key(), it.value().get<std::string>());
        }
        for (usize before = pending.size() + 1; !pending.empty() && pending.size() < before;) {
            before = pending.size();
            std::vector<std::pair<std::string, std::string>> retry;
            for (const auto& [name, value] : pending) {
                Color c;
                if (next.resolve_color(value, c)) next.colors_[name] = c;
                else retry.emplace_back(name, value);
            }
            pending.swap(retry);
        }
        if (!pending.empty()) return fail("ui theme: cannot resolve colour " + pending.front().first + " = " +
                                          pending.front().second);
    }
    auto color_field = [&](const json& obj, const char* key, Color& out) -> bool {
        if (!obj.contains(key)) return true;
        const json& v = obj[key];
        if (!v.is_string() || !next.resolve_color(v.get<std::string>(), out)) return false;
        return true;
    };
    auto num = [](const json& obj, const char* key, f32& out) {
        if (obj.contains(key) && obj[key].is_number()) out = obj[key].get<f32>();
    };

    if (doc.contains("text")) {
        for (auto it = doc["text"].begin(); it != doc["text"].end(); ++it) {
            const json& t = it.value();
            TextStyle st;
            if (t.contains("font") && !parse_font(t["font"].get<std::string>(), st.font)) {
                return fail("ui theme: text " + it.key() + ": unknown font " + t["font"].get<std::string>());
            }
            num(t, "size", st.size);
            num(t, "letter_spacing", st.letter_spacing);
            num(t, "line_height", st.line_height);
            num(t, "outline_width", st.outline_width);
            num(t, "shadow_blur", st.shadow_blur);
            if (t.contains("uppercase")) st.uppercase = t["uppercase"].get<bool>();
            if (t.contains("tabular")) st.tabular = t["tabular"].get<bool>();
            if (t.contains("shadow_offset")) st.shadow_offset = vec2_of(t["shadow_offset"]);
            if (!color_field(t, "color", st.color) || !color_field(t, "outline", st.outline) ||
                !color_field(t, "shadow", st.shadow)) {
                return fail("ui theme: text " + it.key() + ": bad colour");
            }
            next.texts_[it.key()] = st;
        }
    }

    if (doc.contains("shapes")) {
        for (auto it = doc["shapes"].begin(); it != doc["shapes"].end(); ++it) {
            const json& s = it.value();
            ShapeDesc d;
            // "base": start from another preset and override fields.
            if (s.contains("base")) {
                const auto b = next.shapes_.find(s["base"].get<std::string>());
                if (b == next.shapes_.end()) return fail("ui theme: shape " + it.key() + ": unknown base");
                d = b->second;
            }
            if (s.contains("kind") && !parse_kind(s["kind"].get<std::string>(), d.kind)) {
                return fail("ui theme: shape " + it.key() + ": unknown kind");
            }
            num(s, "radius", d.radius);
            num(s, "band", d.band);
            num(s, "stroke_width", d.stroke_width);
            num(s, "rim_width", d.rim_width);
            num(s, "shadow_blur", d.shadow_blur);
            num(s, "shadow_spread", d.shadow_spread);
            num(s, "bulge", d.bulge);
            num(s, "wobble_amp", d.wobble_amp);
            num(s, "wobble_wavelength", d.wobble_wavelength);
            num(s, "wobble_speed", d.wobble_speed);
            num(s, "dash_length", d.dash_length);
            num(s, "dash_gap", d.dash_gap);
            num(s, "liquid_inset", d.liquid_inset);
            num(s, "wave_amp", d.wave_amp);
            num(s, "radial_inner", d.radial_inner);
            if (s.contains("decor_count")) d.decor_count = s["decor_count"].get<u32>();
            if (s.contains("vertical_gradient")) d.vertical_gradient = s["vertical_gradient"].get<bool>();
            if (s.contains("shadow_offset")) d.shadow_offset = vec2_of(s["shadow_offset"]);
            if (s.contains("fill_axis")) {
                d.fill_axis = s["fill_axis"].get<std::string>() == "vertical" ? FillAxis::Vertical
                                                                              : FillAxis::Horizontal;
            }
            if (!color_field(s, "fill", d.fill) || !color_field(s, "fill2", d.fill2) ||
                !color_field(s, "stroke", d.stroke) || !color_field(s, "rim", d.rim) ||
                !color_field(s, "shadow", d.shadow) || !color_field(s, "decor", d.decor) ||
                !color_field(s, "liquid", d.liquid) || !color_field(s, "bubble", d.bubble)) {
                return fail("ui theme: shape " + it.key() + ": bad colour");
            }
            next.shapes_[it.key()] = d;
        }
    }

    if (doc.contains("numbers")) {
        for (auto it = doc["numbers"].begin(); it != doc["numbers"].end(); ++it) {
            if (!it.value().is_number()) return fail("ui theme: number " + it.key() + " must be a number");
            next.numbers_[it.key()] = it.value().get<f32>();
        }
    }

    // Runtime-set colours (family colours) survive a reload.
    for (const auto& [k, v] : colors_) {
        if (next.colors_.find(k) == next.colors_.end()) next.colors_[k] = v;
    }
    *this = std::move(next);
    return true;
}

bool Theme::load(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error != nullptr) *error = "ui theme: cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), error);
}

Color Theme::color(std::string_view name) const {
    const auto it = colors_.find(name);
    return it != colors_.end() ? it->second : Color{1.0f, 0.0f, 1.0f, 1.0f};
}

const TextStyle& Theme::text(std::string_view name) const {
    static const TextStyle kFallback{};
    const auto it = texts_.find(name);
    return it != texts_.end() ? it->second : kFallback;
}

const ShapeDesc& Theme::shape(std::string_view name) const {
    static const ShapeDesc kFallback = [] {
        ShapeDesc d;
        d.fill = Color{1.0f, 0.0f, 1.0f, 1.0f};
        return d;
    }();
    const auto it = shapes_.find(name);
    return it != shapes_.end() ? it->second : kFallback;
}

f32 Theme::number(std::string_view name, f32 fallback) const {
    const auto it = numbers_.find(name);
    return it != numbers_.end() ? it->second : fallback;
}

} // namespace immune::gui
