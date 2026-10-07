# UI framework and screens

[Documentation index](README.md) · [Assets](ASSETS.md) · [Gameplay](GAMEPLAY.md)

Player-facing screens use the custom retained framework in `src/gui`.
Dear ImGui remains for the gym, debug tools, and level editor. The implemented
screens and input routing are the reference for behavior; the HTML canvas is
a visual source snapshot and contains superseded interactions.

## Boundaries and frame flow

```text
app / game / sim -> app/UiBridge -> plain UI model -> ui screen -> widgets
app             <- Intent / MenuResult          <- ui screen
```

`gui/` provides reusable layout, drawing, text, icons, and input routing without
including game or sim headers. `ui/` presents game concepts from model structs;
it does not directly mutate simulation state. Some models include gameplay
enums, so this is a state boundary, not a prohibition on all game headers.

| Area | Responsibility |
|---|---|
| [UiBridge](../src/app/UiBridge.h) | Read state into HUD, campaign thumbnails, tree, and screenshot fixture models |
| [HudModel](../src/ui/hud/HudModel.h), [HudScreen](../src/ui/hud/HudScreen.h) | Match display, armed cell/ability, hold placement, HUD hotkeys |
| [FrontModel](../src/ui/front/FrontModel.h), [FrontEnd](../src/ui/front/FrontEnd.h) | Title, campaign selection, pause, results, screen transitions |
| [TreeModel](../src/ui/front/TreeModel.h), [TreeScreen](../src/ui/front/TreeScreen.h) | Strengthen Immunity presentation and tree camera |
| [Intent](../src/ui/Intent.h), [MenuResult](../src/ui/Menu.h) | Requests applied by App |
| [DevUi](../src/ui/DevUi.h), [GymPanel](../src/ui/GymPanel.h), [editor UI](../src/ui/editor/) | ImGui developer controls |

In the live match render path, App first renders the world, starts the developer
UI frame, synchronizes HUD/front models, then runs GUI layout/input/animation.
`HudScreen::handle_input` emits requests and App applies them. The gym panel is
built, the GUI draws over the world, ImGui draws last, and the window swaps.
World changes from these requests appear in subsequent world rendering.
Pause and terminal-result screens replace the interactive HUD over the world.
The editor has its own canvas/panel path.

## Retained widgets and layout

[Widget](../src/gui/core/Widget.h) owns children with `unique_ptr`; screens
construct trees and update properties rather than rebuild everything each frame.
The context owns layers in draw order:
`World`, `Hud`, `Popup`, `Modal`, `Tooltip`, `Toast`. Input searches in reverse.
Removed widgets are retired until the end of a frame, avoiding destruction
during event dispatch. Stable IDs form paths; anonymous containers are skipped.

[LayoutParams](../src/gui/core/Layout.h) implements row, column, and stack,
padding/gap, grow, alignment, justification, minimum/maximum sizes, and
fit/fixed/fill/percent sizing. A child can follow layout flow, anchor to a parent
point, or anchor to a projected world point. Layout is measured and arranged
every frame. Animation transforms change appearance without moving layout boxes.

Layout units are logical pixels on a 1920×1080 reference frame.
`Gui::set_viewport` contain-fits that frame to the framebuffer and multiplies
by UI scale, clamped to 0.75–1.5. Convert framebuffer input using `to_logical`;
the world projection callback must return logical pixels too. Test narrow,
wide, and scaled layouts rather than assuming larger UI scale cannot clip.

## Pointer routing and automation

[Gui](../src/gui/core/Gui.h) accepts per-frame pointer position, button edges,
held state, wheel, and presence. Events bubble from the nearest interactive
widget. A press captures its target; a movement threshold starts dragging;
release over the pressed target produces Click, or Deny for a disabled widget.
Shaped panels hit-test their CPU SDF outline, not just the bounding rectangle.
Disabled controls can still hover and show explanations. An outgoing screen
sets `accepts_pointer=false` while it fades.

`wants_pointer()` blocks world actions while over UI or during capture. App
combines that with ImGui mouse capture. ImGui keyboard capture also prevents
typing console commands from selecting cells or firing abilities. SDL input
latches action/button edges so short taps between frames can still be consumed.

`Gui::find`, `click`, `hover`, and `dump` use widget paths. The gym's `ui`
command exposes them through [UiBridge::run_ui_command](../src/app/UiBridge.cpp).

```text
ui dump
ui click hud/dock/neutrophil
ui hover hud/dock/neutrophil
ui cancel
```

Paths depend on visibility, unlocks, and the active screen. Use a dump from the
same context before scripting a path. A successful synthetic click means events
were delivered to a visible addressable widget; it does not require the widget
to mark them handled or prove a gameplay action happened. Disabled buttons can
receive Deny instead of Click.
See [Gym](GYM.md) for screenshot-only pointer/front-screen fixture commands.

## Current player screens

The HUD shows integrity, ATP/income, current/next wave composition, preparation
and critical banners, unlocked deployment cards, ability cooldown cells, speed,
pause, auto-start, and menu controls. Cell cards respect both permanent unlocks
and a level's allowed types. World overlays include placement range/ghosts,
target reticles, and preparation spawn composition.

Number keys follow **dock order**, not `TowerType` enum order: Neutrophil,
Cytotoxic T, Macrophage, Goblet Cell, Fibroblast. Q/W/E/R select the four
abilities; Fever fires immediately because it needs no target. Hold placement
uses render/UI time and the configured interval, with no accumulated backlog;
Shift requests up to ten cells per pulse. Escape/right click cancel armed
cursors. See [Gameplay](GAMEPLAY.md) for the full control reference.

The HUD retains tower-selection/popup/Sell widgets and `SellTower` intent for
legacy ECS emitter fixtures. `UiBridge` populates board entries from ECS towers,
while normal deployment creates swarmers. App does not apply a normal-play
selling intent. These retained widgets and their old unit fixtures do not make
cell selling a current gameplay feature.

The title's Play action opens Strengthen Immunity, then Campaign. Campaign
files are discovered by filename and sorted; clearing the previous level opens
the next. Thumbnails are sampled from actual level vessels/obstacles/spawns,
normalized with world Y flipped and clipped to the level cell membrane.
Pause offers resume/restart/menu; results distinguish campaign payouts from
sandbox and editor playtest. F4 opens the editor from title or live play.

## Strengthen Immunity view

`ImmunityTree.cpp` owns keys, parents, purchasing rules, effects, and costs;
`UiBridge::make_tree_model` supplies levels, state, prices, requirements, wallet,
and branch points. `TreeModel::revealed` shows the root, owned nodes, their
ancestors, and children whose parent is owned. The screen does not invent
purchase eligibility.

[tree_layout.json](../assets/ui/tree_layout.json) stores coordinates and glyphs.
[gen_tree_layout.py](../tools/gen_tree_layout.py) derives radial wedges and ring
depths from catalog parents and child order. Edges use model parent links.
Run its `--check` after catalog changes. The original canvas Tree artboard is
not the current radial layout.

Drag pans, wheel zooms around the pointer, and zoom/recenter buttons frame the
revealed tree. The view eases toward its target and survives revisits. Offscreen
nodes are culled. Hover cards explain effects and requirements; clicks report
`PurchaseNode`, and App buys through `MetaProgression`. Hidden nodes cannot be
addressed on screen. See [Progression](PROGRESSION.md) for rule and save details.

## Drawing, theme, text, and icons

[DrawList](../src/gui/draw/DrawList.h) shares a 24-byte vertex format for solid
paths, SDF shapes, text, icons, and offscreen layer composites. Parameters live
in records uploaded as an SSBO. [GlBackend](../src/gui/backend/GlBackend.h) uses
`gui.vert`/`gui.frag`. Scissor changes, stencil clips, and offscreen groups split
batches; draw count is scene-dependent, not guaranteed to be one.

- [Shape](../src/gui/draw/Shape.h): boxes/membranes, ellipses, arcs, liquid
  meters, radial glows; [ShapeSdf](../src/gui/draw/ShapeSdf.h) mirrors hit geometry.
- Stroked polylines and SVG paths are CPU tessellated with caps, joins, dashes,
  and antialias fringes. Static strokes can be cached as `SolidMesh`.
- [Text](../src/gui/text/Text.h): lazy SDF glyphs from shipped Fredoka/Nunito,
  style measurement, wrapping, ellipsis, outline/shadow, tabular digits.
- [IconLibrary](../src/gui/icons/IconLibrary.h): NanoSVG rasterization into a
  premultiplied atlas; zoom buckets reuse bakes as the tree view changes.
- [Anim](../src/gui/anim/Anim.h): tweens, springs, and looping pulses on render
  time, separate from gameplay tick time.

[Theme](../src/gui/style/Theme.h) reads color names/hex/`name@alpha`, text styles,
shape presets with `base` inheritance, and numeric tokens from `ui_theme.json`.
Family colors are injected from renderer data. Missing tokens have visible
fallbacks; rejected parses such as invalid JSON syntax retain the previous theme.
Some wrongly typed theme values can throw outside the current parser's catch;
the recovery guarantee does not cover every malformed value. App rebuilds HUD and
front-end widgets after a successful theme reload because styles are copied.
This may reset local presentation state. `--config` pinning disables automatic
theme polling along with gameplay polling.

## Extending and checking UI

1. Add the displayed value to a plain model and populate it in UiBridge.
2. Build/update widgets in the relevant screen; assign stable, meaningful IDs.
3. Emit an intent or menu action and apply it in App if it changes the game.
4. Verify capture, disabled state, logical/world coordinates, and transitions.
5. Add tokens/assets through the maintained generators and update this reference.

Use [test_gui_core](../tests/test_gui_core.cpp),
[test_gui_draw](../tests/test_gui_draw.cpp), [test_gui_text](../tests/test_gui_text.cpp),
[test_gui_icons](../tests/test_gui_icons.cpp),
[test_ui_hud](../tests/test_ui_hud.cpp), and [test_menu](../tests/test_menu.cpp)
for model/layout/input contracts. GL tests in
[test_gui_render](../tests/test_gui_render.cpp) and menu tests generate PNGs.
Some fixtures intentionally contain legacy tower models and arbitrary prices;
they are not balance data. For visual changes generate and inspect real
screenshots at representative sizes/states; inspect shader logs too.

The UI CPU budget is a design goal of roughly 0.5 ms/frame, not a universal
measurement. `test_menu.cpp` reports local timings. Font generation, SVG
extraction, tree layout checks, and canvas snapshots are documented in
[Assets](ASSETS.md) and [Tools](TOOLS.md).
