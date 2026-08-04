#include "platform/Input.h"

#include "platform/Window.h"

#include <SDL.h>

namespace immune::platform {

void InputState::bind(Action a, i32 sdl_scancode) {
    scancode_[static_cast<u32>(a)] = sdl_scancode;
}

void InputState::bind_defaults() {
    scancode_.fill(SDL_SCANCODE_UNKNOWN);
    bind(Action::Pause, SDL_SCANCODE_SPACE);
    bind(Action::SpeedUp, SDL_SCANCODE_PERIOD);
    bind(Action::SpeedDown, SDL_SCANCODE_COMMA);
    bind(Action::CancelPlacement, SDL_SCANCODE_ESCAPE);
    bind(Action::ToggleDebugOverlay, SDL_SCANCODE_F1);
    bind(Action::ToggleThreatOverlay, SDL_SCANCODE_TAB);
    bind(Action::SelectTower1, SDL_SCANCODE_1);
    bind(Action::SelectTower2, SDL_SCANCODE_2);
    bind(Action::SelectTower3, SDL_SCANCODE_3);
    bind(Action::SelectTower4, SDL_SCANCODE_4);
    bind(Action::SelectTower5, SDL_SCANCODE_5);
    bind(Action::SelectTower6, SDL_SCANCODE_6);
    bind(Action::SelectTower7, SDL_SCANCODE_7);
    bind(Action::SelectTower8, SDL_SCANCODE_8);
    bind(Action::Screenshot, SDL_SCANCODE_F12);
    bind(Action::Quit, SDL_SCANCODE_UNKNOWN);
    bound_ = true;
}

void InputState::poll(Window& window) {
    if (!bound_) bind_defaults();

    prev_down_ = down_;
    prev_mouse_down_ = mouse_down_;
    wheel_ = 0.0f;
    const Vec2 prev_pos = mouse_pos_;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                quit_requested_ = true;
                window.request_close();
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    window.on_resized(ev.window.data1, ev.window.data2);
                } else if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                    quit_requested_ = true;
                    window.request_close();
                }
                break;
            case SDL_MOUSEWHEEL:
                wheel_ += static_cast<f32>(ev.wheel.y);
                break;
            default:
                break;
        }
    }

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    for (u32 i = 0; i < kActionCount; ++i) {
        const i32 sc = scancode_[i];
        down_[i] = (sc != SDL_SCANCODE_UNKNOWN) && keys[sc] != 0;
    }

    int mx = 0, my = 0;
    const Uint32 buttons = SDL_GetMouseState(&mx, &my);
    mouse_pos_ = Vec2{static_cast<f32>(mx), static_cast<f32>(my)};
    mouse_delta_ = mouse_pos_ - prev_pos;
    mouse_down_[static_cast<u32>(MouseButton::Left)] = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    mouse_down_[static_cast<u32>(MouseButton::Right)] = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    mouse_down_[static_cast<u32>(MouseButton::Middle)] = (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0;
}

} // namespace immune::platform
