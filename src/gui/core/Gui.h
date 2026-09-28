// gui/core/Gui.h — the UI context: layers of widgets, input routing, services.
//
// One Gui per window. It owns the shared services (fonts, icons, theme), the
// widget layers, the pointer state machine (hover, press capture, drag,
// click/deny) and the GL backend, and it is the only thing app/ talks to:
//
//     gui.set_viewport(framebuffer_size);
//     gui.frame(pointer_input_from(input), dt);   // layout, events, update
//     if (gui.wants_pointer()) ...                 // keep clicks off the world
//     gui.render(fb_w, fb_h);                      // after the world, before ImGui
//
// Layers draw bottom to top and take the pointer top to bottom:
// World (markers over the battlefield) < Hud < Popup < Modal < Tooltip < Toast.
#pragma once

#include "core/Types.h"
#include "gui/backend/GlBackend.h"
#include "gui/core/Event.h"
#include "gui/core/Widget.h"
#include "gui/draw/DrawList.h"
#include "gui/icons/IconLibrary.h"
#include "gui/style/Theme.h"
#include "gui/text/Text.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace immune::gui {

class Label;
class Panel;

enum class LayerId : u8 { World = 0, Hud, Popup, Modal, Tooltip, Toast, Count };
inline constexpr usize kLayerCount = static_cast<usize>(LayerId::Count);

/// Pointer state for one frame, in LOGICAL px (see Gui::to_logical).
struct PointerInput {
    Vec2 pos{-1.0f, -1.0f};
    std::array<bool, 3> down{};
    std::array<bool, 3> pressed{};
    std::array<bool, 3> released{};
    f32 wheel = 0.0f;
    bool present = true;  ///< False when the cursor is outside the window.
};

class Gui {
public:
    /// The design canvas is drawn at 1920x1080; layout happens at that scale.
    static constexpr f32 kReferenceHeight = 1080.0f;

    struct Assets {
        std::string fonts_dir;   ///< assets/fonts
        std::string icons_dir;   ///< assets/ui/icons
        std::string theme_path;  ///< assets/config/ui_theme.json
    };

    Gui();
    ~Gui();
    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    /// Loads fonts, icons and theme (CPU only; no GL needed). False with
    /// error() on a missing font or a malformed theme; a missing icon
    /// directory only warns (icons draw nothing).
    bool init(const Assets& assets);
    /// Creates the GL backend. Needs a current GL 4.5 context.
    bool init_renderer();
    /// Releases GL objects; call while the context is still current.
    void shutdown();
    const std::string& error() const { return error_; }
    /// Re-reads the theme file; the previous theme stays on a parse error.
    bool reload_theme(std::string* error = nullptr);
    /// Reloads the theme only if the file's content changed since the last
    /// load. True when a new theme was applied (widgets built from the old
    /// one should be rebuilt).
    bool reload_theme_if_changed(std::string* error = nullptr);

    // ---- Viewport -------------------------------------------------------------
    /// `framebuffer` in pixels. The UI scale is framebuffer height / 1080
    /// unless `scale_override` > 0 (a player UI-scale setting).
    void set_viewport(Vec2 framebuffer, f32 scale_override = 0.0f);
    f32 scale() const { return scale_; }
    Vec2 viewport() const { return viewport_; }
    Vec2 to_logical(Vec2 framebuffer_px) const { return framebuffer_px / scale_; }

    /// Maps a world point to logical screen px, for World-positioned widgets.
    using Projection = std::function<Vec2(Vec2 world)>;
    void set_projection(Projection p) { projection_ = std::move(p); }
    Vec2 project(Vec2 world) const { return projection_ ? projection_(world) : Vec2{-1e6f, -1e6f}; }

    // ---- Tree -----------------------------------------------------------------
    Widget& layer(LayerId id) { return *layers_[static_cast<usize>(id)]; }
    /// Widget by path ("hud/dock/neutrophil"), searching every layer.
    Widget* find(std::string_view path);

    // ---- Frame ----------------------------------------------------------------
    /// Advances time, lays out, routes the pointer, updates widgets.
    void frame(const PointerInput& input, f32 dt);
    /// Records every layer into `dl` (tests and custom targets).
    void draw(DrawList& dl);
    /// Draws into the bound framebuffer. No-op before init_renderer().
    void render(i32 framebuffer_width, i32 framebuffer_height);
    const GuiFrameStats& render_stats() const { return backend_.stats(); }
    void poll_shader_reload() { backend_.poll_shader_reload(); }

    /// True while the pointer is over UI (or a UI press is in progress):
    /// the world must ignore it.
    bool wants_pointer() const { return wants_pointer_; }

    // ---- Pointer state (for widgets) -----------------------------------------
    Widget* hovered_widget() const { return hover_target_; }
    Widget* pressed_widget() const { return press_target_; }
    bool is_hovered(const Widget* w) const;
    Vec2 pointer() const { return pointer_; }

    // ---- Automation --------------------------------------------------------------
    /// Synthesizes a full press+release on the widget at `path` (gym
    /// `ui.click`, tests). False if it is missing or invisible.
    bool click(std::string_view path);
    /// Pretends the pointer rests on `path` until the next real input.
    bool hover(std::string_view path);
    /// One line per visible widget: path, rect, flags. For `ui.dump`.
    std::string dump() const;

    // ---- Services -------------------------------------------------------------
    FontLibrary& fonts() { return fonts_; }
    TextRenderer& text() { return text_; }
    IconLibrary& icons() { return icons_; }
    Theme& theme() { return theme_; }
    const Theme& theme() const { return theme_; }
    f32 time() const { return time_; }

    std::function<void(UiSound)> on_sound;
    void play(UiSound s) {
        if (on_sound) on_sound(s);
    }

    /// Destroys a removed widget at the end of the frame (safe mid-dispatch).
    void retire(std::unique_ptr<Widget> w) { retired_.push_back(std::move(w)); }

private:
    Widget* pick(Widget& w, Vec2 p);
    void dispatch(Event e, Widget* target);
    void update_tree(Widget& w, f32 dt);
    void update_tooltip(f32 dt);

    std::array<std::unique_ptr<Widget>, kLayerCount> layers_;
    FontLibrary fonts_;
    TextRenderer text_{fonts_};
    IconLibrary icons_;
    Theme theme_;
    GlBackend backend_;
    DrawList draw_list_;
    Assets assets_;
    std::string theme_text_;
    std::string error_;
    bool renderer_ready_ = false;

    Vec2 framebuffer_{1920.0f, 1080.0f};
    Vec2 viewport_{1920.0f, 1080.0f};
    f32 scale_ = 1.0f;
    f32 time_ = 0.0f;
    Projection projection_;

    Vec2 pointer_{-1.0f, -1.0f};
    Widget* hover_target_ = nullptr;
    Widget* press_target_ = nullptr;
    Vec2 press_origin_{0.0f, 0.0f};
    Vec2 last_drag_{0.0f, 0.0f};
    bool dragging_ = false;
    bool wants_pointer_ = false;
    /// Set by hover(): overrides the real pointer until it moves.
    Widget* forced_hover_ = nullptr;

    f32 hover_time_ = 0.0f;
    Panel* tooltip_ = nullptr;
    Label* tooltip_label_ = nullptr;

    std::vector<std::unique_ptr<Widget>> retired_;
};

} // namespace immune::gui
