// ui/DevUi.h — the ImGui context, for developer tools.
//
// The player-facing UI is the gui framework (src/gui, ui/hud, ui/front).
// ImGui stays for the tools a player never sees -- the gym panel and the
// level editor. This class owns ImGui's context and SDL2/GL3 backends, opens
// and draws its frame, and carries the debug-overlay switches the gym's
// `overlay` command flips.
#pragma once

namespace immune::platform { class InputState; class Window; }

namespace immune::ui {

class DevUi {
public:
    /// Sets up the ImGui context and the SDL2/GL3 backends. `window` must
    /// already own a live GL context. Also installs itself as InputState's
    /// raw-event sink so ImGui sees SDL events, since InputState::poll() owns
    /// the one SDL_PollEvent loop.
    bool init(platform::Window& window, platform::InputState& input);
    void shutdown();

    /// Begins an ImGui frame and publishes ImGui's capture flags into
    /// InputState (app/ then ORs in the gui's), so a click on a tool window
    /// does not also reach the game.
    void begin_frame(platform::InputState& input);
    /// Issues the ImGui draw data. Runs last, over the world and the gui.
    void render();

    void set_threat_overlay_visible(bool v) { threat_overlay_ = v; }
    bool threat_overlay_visible() const { return threat_overlay_; }
    void set_debug_overlay_visible(bool v) { debug_overlay_ = v; }
    bool debug_overlay_visible() const { return debug_overlay_; }
    /// Squad routes/anchors overlay. Separate from the debug (flow-arrow)
    /// overlay rather than folded into it: the two are drawn over the same
    /// ground and reading either one is much harder with the other on.
    void set_squad_overlay_visible(bool v) { squad_overlay_ = v; }
    bool squad_overlay_visible() const { return squad_overlay_; }

private:
    bool threat_overlay_ = true;
    bool debug_overlay_ = false;
    bool squad_overlay_ = false;
    bool initialized_ = false;
};

} // namespace immune::ui
