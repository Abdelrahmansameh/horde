# Audio and platform services

The platform layer owns OS-facing window/context, frame input, and filesystem
helpers. Audio is a separate procedural mixer with its own SDL device/callback.
Neither supplies gameplay input to a simulation tick. Core clocks/RNG/jobs/
profiling are summarized in [ARCHITECTURE.md](ARCHITECTURE.md#core-services).

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
resets screens, shuts down retained GUI and developer UI, closes audio, shuts
down renderer, destroys window/context, then releases jobs. Window destruction
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

`asset_root()` is cached on its first call. Actual resolution is:

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

## Audio API and current wiring

[AudioEngine](../src/audio/Audio.h) synthesizes sounds at runtime; there is no
WAV loading path. `AudioEvent` carries SoundId, world position, gain, pitch and
intensity. Events should have finite numeric values. `post()` is non-blocking
from the game producer thread. `update()` supplies listener position, smoothed
horde/music intensity and frame `dt`; `set_master_gain()` and `set_paused()`
publish mixer controls.

Defaults are 48 kHz, 512 buffer frames, 64 configured voices and master gain
0.8. Sample rate/buffer sizes are constrained during initialization, and voice
count is capped by 128 preallocated slots. `music_gain` / `sfx_gain` exist in
`AudioConfig`, but the current mixer does not read them as separate gain controls;
do not document those fields as working sliders without implementing wiring.

App currently posts events from selected gameplay/UI callbacks and calls audio
update each rendered frame. Its music input is binary (`total_density > 0`),
smoothed in the mixer, rather than a calibrated continuous density ratio.
Not every declared SoundId is necessarily posted by a gameplay caller. A new
audible cue needs both a synth patch and a real producer call; adding an enum
entry alone produces no sound.

Audio is cosmetic. Device absence, dropped sounds, callback timing and noise
seeds must not change wave scheduling, damage, ATP, or sim hashes.

## Threading, queue and voice allocation

[AudioInternal.h](../src/audio/AudioInternal.h) contains SDL-free mixer logic:

| Component | Contract |
| --- | --- |
| `EventRing` | Fixed 256-event SPSC ring with atomic read/write indices; exactly one producer and one consumer |
| Voice pool | Fixed arrays, configured limit clamped to 128; no per-sample allocation |
| Patch table | Compile-time mapping from SoundId to synth family, priority, frequency, envelope/filter/gain |
| Listener/master/pause/music controls | Atomic scalars shared between game and callback |
| `pump()` | Consume queued events, update voice envelopes/pan/sustained beds |
| `render_block()` | Write interleaved stereo float PCM, synthesize adaptive sine layers, clamp final output |

Queue overflow drops the new sound and increments `dropped_events()` rather than
stall the game. Do not call `post()` concurrently from multiple sim workers; the
queue is SPSC, not a many-producer queue. A future parallel producer needs a
merge/dispatch point on the game thread.

Priorities are Ambient, Normal, High and Protected. The allocator prefers free
slots, otherwise steals the lowest-priority non-Protected active voice,
breaking ties by oldest age. Incoming priority does not impose a separate
lower/equal-priority eligibility test. Protected warning/telegraph voices are
not stolen; a fully Protected pool can prevent a new voice from starting.
ChaffDissolve updates a sustained intensity bed
rather than trigger one voice per dead agent. Unrefreshed beds fade/release.

Synth families are noise, sine, FM, subtractive and granular. Panning derives
world x relative to the latest listener x. The adaptive bed blends detuned sine
layers with smoothed intensity. This is a cosmetic approximation, not a 3D
acoustic scene or a replayable PCM recording system.

The SDL callback touches only its preallocated Mixer and atomic controls. It
must not allocate, lock, read sim state, load files, or call UI/game methods.
The callback receives Mixer directly as SDL userdata; it does not search engine
registrations on each audio block.

## Real device, null device and failure fallback

[Audio.cpp](../src/audio/Audio.cpp) keeps implementation state in a fixed
eight-entry atomic owner/implementation registry keyed by the AudioEngine
address. The public class lacks a pimpl member. This is implementation detail,
but it explains why concurrent engine instances have a small limit and why
explicit shutdown matters.

With a real device, SDL's callback is the only ring/voice consumer. It pumps by
block duration then renders stereo PCM. `AudioEngine::update()` only publishes
listener/music controls in this mode, avoiding a second consumer race.

With `null_device=true`, initialization never opens SDL audio hardware. `update()`
on the caller thread pumps the same ring/voice logic; there is no background
callback and no PCM playback. Null mode tests plumbing/envelopes/overflow, not
audible output. Headless runners may omit audio entirely rather than construct
a null engine.

If device opening fails, initialization succeeds as a silent mixer with a
diagnostic in `error()` and `ready()==true`. Distinguish a functioning silent
fallback from a real device when debugging. Registry exhaustion instead fails
initialization. `set_paused()` causes PCM rendering to output silence; pumping
still runs and ages envelopes, so it is not a gameplay clock pause.

`shutdown()` closes the SDL device before unregistering/freeing Mixer state so
the callback cannot use freed storage. Call shutdown before reinitialization
and before an AudioEngine owner is discarded. The current public class does not
provide an automatic destructor shutdown.

## Adding or verifying a sound

1. Add the semantic SoundId and a compile-time patch with appropriate priority.
2. Post one bounded event at the gameplay/UI boundary, aggregating mass events.
3. Keep producer ownership on one thread; pass finite position/gain/pitch data.
4. Test queue limits, voice pressure, priority, envelope/pan, silence and fallback
   behavior using the shared mixer.
5. Listen on a real device for timing/clarity; unit tests cannot establish the
   artistic quality of the sound.

[test_audio.cpp](../tests/test_audio.cpp) exercises queue wrap/overflow,
voice limits/priority, intensity bed, pan/envelopes, null-device behavior and
real-device/fallback initialization. These tests do not establish absence of
every possible concurrency race. See [TESTING.md](TESTING.md) for selecting
the audio tests.
