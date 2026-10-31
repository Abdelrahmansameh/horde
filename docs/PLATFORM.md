# Platform services

The platform layer owns OS-facing window/context, frame input, and filesystem
helpers. It does not supply gameplay input to a simulation tick. Audio is a
separate module; see [Audio](AUDIO.md). Core clocks/RNG/jobs/profiling are summarized in [ARCHITECTURE.md](ARCHITECTURE.md#core-services).

## Window and GL lifecycle

[Window](../src/platform/Window.h) wraps an SDL2 window and OpenGL 4.5 core
context. `create()` initializes video, requests a double-buffered context with
depth/stencil buffers, loads glad, records drawable dimensions and sets swap
interval. Creation returns false with `error()` on failure. Check that result
before making GL calls.

`WindowDesc` defaults to 1600×900, resizable, vsync enabled; optional debug mode
wires the GL debug callback into logging and filters routine notifications.
`swap()` presents the backbuffer. Input polling forwards resize/close events;
App updates the camera viewport and renderer targets from the new dimensions.

`create_headless_gl(window, width, height)` creates a hidden window at the
requested size with vsync/resizing disabled. Contrary to an older header note,
it is not forced to 1×1. It is real OpenGL and still requires a working driver.
Sim-test and direct simulation fixtures need no window. `--bench` attempts a
hidden GL context for render measurements and falls back to sim-only timing
with zero render samples if context/renderer setup fails.

Destroy GL consumers while their context is still valid. Current App teardown
resets screens, shuts down retained GUI and developer UI, shuts down
renderer, destroys window/context, then releases jobs. Window destruction
also runs from its destructor. `Window::destroy()` deletes the GL context and
window; it does not itself call a global `SDL_Quit()`.

GL checks live in [test_render_gl.cpp](../tests/test_render_gl.cpp) and
[test_gui_render.cpp](../tests/test_gui_render.cpp); startup failure should be
diagnosed from `Window::error()` and logs before investigating gameplay.

## Frame-coherent input

[InputState](../src/platform/Input.h) owns the single SDL event drain.
`poll(window)` runs once per rendered frame, updates key/mouse snapshots, forwards
window events, and optionally forwards every raw event to the developer UI's
event sink. Do not introduce a second competing `SDL_PollEvent` loop.

Bindings map SDL scancodes to abstract `Action`s. App/game code consumes actions
such as pause, speed, roster/ability selection, overlays, editor, screenshot and
quit. Default physical bindings are in
[Input.cpp](../src/platform/Input.cpp), and `bind()` supports overrides.
Mouse positions/deltas are top-left-origin window pixels; camera conversion
turns picks into world positions.

Pressed edges are latched from events as well as sampled state. A quick
press/release between polls still counts as a press; mouse press and release
may both be true while down is false. Wheel delta is accumulated per frame.
`shift_down()` reads either Shift key. UI capture flags allow App to suppress
gameplay input while widgets/developer tools own it.

SimWorld does not read InputState. App translates input/UI intents into explicit
game calls; headless commands can exercise those public APIs without OS events.
If adding an action, update its binding, routing and UI/command semantics rather
than poll physical keys inside the sim. GUI and input-routing coverage includes
[test_gui_core.cpp](../tests/test_gui_core.cpp),
[test_ui_hud.cpp](../tests/test_ui_hud.cpp), and
[test_menu.cpp](../tests/test_menu.cpp).

## Files, assets and user data

[FileIO](../src/platform/FileIO.h) has text/byte reads returning `optional`, writes
returning bool, existence/directory helpers, and sorted extension-filtered file
listing. Text/binary writes create parent directories and truncate their target;
they are not an atomic save-transaction primitive. Game save/config/editor code
adds its own policy above them.

In standalone publishing builds, the same helpers read embedded resources from
memory. Their `immune-embedded:/` paths are read-only, and directory listings
come from the embedded index. Explicit external files retain normal filesystem
behavior. See [Publishing](PUBLISHING.md) for the separate build mode.

In development builds, `asset_root()` is cached on its first call. Resolution is:

1. A non-empty `IMMUNE_ASSET_ROOT` environment value.
2. The first executable-directory ancestor containing `assets`, searching up to
   eight levels.
3. The first current-working-directory ancestor containing `assets`, also up to
   eight levels.
4. Current working directory as the fallback.

`asset_path("shaders/chaff.vert")` joins root + `assets` + the relative path.
The override is a repository/content root containing `assets`, not the `assets`
directory itself. Set it before first lookup; changing the environment after
the cached root is initialized will not redirect existing asset access.

`executable_dir()` uses SDL's base path and falls back to working directory.
`user_data_dir()` uses `SDL_GetPrefPath("IMMUNE", "IMMUNE")` for writable user
data, with executable-directory fallback. App saves to the explicit `--save`
path when supplied, otherwise `user_data_dir()/save.json`. Keep saves out of
asset paths. Sorted listings keep level/config processing order stable.

Configuration/save/content coverage is in
[test_config.cpp](../tests/test_config.cpp),
[test_meta_progression.cpp](../tests/test_meta_progression.cpp), and
[test_level_writer.cpp](../tests/test_level_writer.cpp).
