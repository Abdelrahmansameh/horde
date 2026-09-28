// gui/style/Theme.h — the UI's design tokens, loaded from JSON.
//
// assets/config/ui_theme.json holds every colour, text style and shape preset
// the screens use, taken from the design canvas. Widgets ask for them by name
// ("membrane.host", "label", "plum"), so a colour or a membrane's wobble is
// tuned by editing one file (hot-reloaded in play) rather than code.
//
// Colour values in the JSON are "#RRGGBB", "#RRGGBBAA", or the name of another
// colour, optionally with an alpha: "plum", "white@0.55".
#pragma once

#include "core/Types.h"
#include "gui/Color.h"
#include "gui/draw/Shape.h"
#include "gui/text/Text.h"

#include <map>
#include <string>
#include <string_view>

namespace immune::gui {

class Theme {
public:
    /// Parses a theme document. On failure returns false with a message and
    /// leaves the previous contents intact (so a typo during hot reload keeps
    /// the last good theme on screen).
    bool parse(std::string_view json, std::string* error = nullptr);
    bool load(const std::string& path, std::string* error = nullptr);

    /// Missing names log nothing and return an obvious fallback (magenta for
    /// colours) so a typo is visible on screen rather than silent.
    Color color(std::string_view name) const;
    const TextStyle& text(std::string_view name) const;
    const ShapeDesc& shape(std::string_view name) const;
    f32 number(std::string_view name, f32 fallback = 0.0f) const;

    bool has_color(std::string_view name) const { return colors_.count(std::string(name)) != 0; }
    bool has_text(std::string_view name) const { return texts_.count(std::string(name)) != 0; }
    bool has_shape(std::string_view name) const { return shapes_.count(std::string(name)) != 0; }

    /// Sets a colour at runtime (e.g. the pathogen family colours, which are
    /// owned by render::family_color and must not be duplicated in the JSON).
    void set_color(std::string_view name, Color c) { colors_[std::string(name)] = c; }

    /// Resolves a colour expression ("#hex", "name", "name@alpha").
    bool resolve_color(std::string_view expr, Color& out) const;

private:
    std::map<std::string, Color, std::less<>> colors_;
    std::map<std::string, TextStyle, std::less<>> texts_;
    std::map<std::string, ShapeDesc, std::less<>> shapes_;
    std::map<std::string, f32, std::less<>> numbers_;
};

} // namespace immune::gui
