// platform/Input.h — SDL event pump and frame-coherent input state. FROZEN CONTRACT.
//
// Input is a *render-rate* concern. It is sampled once per frame and translated
// into intents that the game layer converts into sim commands; the sim itself
// never reads InputState (that would break determinism).
#pragma once

#include "core/Types.h"

#include <array>

namespace immune::platform {

class Window;

/// Abstract game actions. Key bindings map SDL scancodes onto these so the sim
/// and UI never mention physical keys.
enum class Action : u8 {
    Pause = 0,
    SpeedUp,
    SpeedDown,
    CancelPlacement,
    ToggleDebugOverlay,
    ToggleThreatOverlay,
    SelectTower1, SelectTower2, SelectTower3, SelectTower4,
    SelectTower5, SelectTower6, SelectTower7, SelectTower8,
    Screenshot,
    Quit,
    Count
};

inline constexpr u32 kActionCount = static_cast<u32>(Action::Count);

enum class MouseButton : u8 { Left = 0, Right = 1, Middle = 2, Count = 3 };

/// Frame-coherent snapshot of keyboard/mouse. Edge queries ("pressed") are true
/// only on the frame the transition happened.
class InputState {
public:
    /// Drains the SDL event queue, updates state, and forwards window events
    /// (resize/close) to `window`. Call exactly once per rendered frame.
    void poll(Window& window);

    bool action_down(Action a) const { return down_[static_cast<u32>(a)]; }
    bool action_pressed(Action a) const { return down_[static_cast<u32>(a)] && !prev_down_[static_cast<u32>(a)]; }
    bool action_released(Action a) const { return !down_[static_cast<u32>(a)] && prev_down_[static_cast<u32>(a)]; }

    bool mouse_down(MouseButton b) const { return mouse_down_[static_cast<u32>(b)]; }
    bool mouse_pressed(MouseButton b) const { return mouse_down_[static_cast<u32>(b)] && !prev_mouse_down_[static_cast<u32>(b)]; }
    bool mouse_released(MouseButton b) const { return !mouse_down_[static_cast<u32>(b)] && prev_mouse_down_[static_cast<u32>(b)]; }

    /// Cursor position in window pixels, origin top-left.
    Vec2 mouse_pos() const { return mouse_pos_; }
    /// Pixel delta since the previous poll.
    Vec2 mouse_delta() const { return mouse_delta_; }
    /// Accumulated wheel ticks this frame (positive = away from user).
    f32 wheel() const { return wheel_; }

    /// True if the OS/SDL asked the app to quit.
    bool quit_requested() const { return quit_requested_; }

    /// When true, ImGui wants the cursor/keyboard and gameplay should ignore it.
    void set_ui_capture(bool mouse, bool keyboard) { ui_capture_mouse_ = mouse; ui_capture_keyboard_ = keyboard; }
    bool ui_capture_mouse() const { return ui_capture_mouse_; }
    bool ui_capture_keyboard() const { return ui_capture_keyboard_; }

    /// Rebinds an action to an SDL scancode. Bindings live here, not in game code.
    void bind(Action a, i32 sdl_scancode);
    void bind_defaults();

private:
    std::array<bool, kActionCount> down_{};
    std::array<bool, kActionCount> prev_down_{};
    std::array<i32, kActionCount> scancode_{};
    std::array<bool, static_cast<u32>(MouseButton::Count)> mouse_down_{};
    std::array<bool, static_cast<u32>(MouseButton::Count)> prev_mouse_down_{};
    Vec2 mouse_pos_{0.0f, 0.0f};
    Vec2 mouse_delta_{0.0f, 0.0f};
    f32 wheel_ = 0.0f;
    bool quit_requested_ = false;
    bool ui_capture_mouse_ = false;
    bool ui_capture_keyboard_ = false;
    bool bound_ = false;
};

} // namespace immune::platform
