# UI framework (`src/gui`) — design and build plan

The player-facing UI is being rebuilt on a custom C++ framework so it can match
the "Living Membrane" design: wobbly cell-membrane panels with plum outlines,
cell icons, a blood-filled organ bar, fluid-filled ability cells, a
world-anchored tower popup, a pannable lymphatic skill tree and a vessel level
map. The design lives in the canvas
(https://claude.ai/artifact/BmwSmwUEERL8X7rbVyfvWp; snapshot in
`docs/ui-concepts/canvas/project/`) and its rules in `DESIGN.md` §8.

Decisions: a **retained widget tree**; the design's **fonts ship** as static
TTFs (Fredoka 500/600/700, Nunito 600/700/800/900, OFL) in `assets/fonts/`;
**ImGui stays for developer tools only** (gym panel, level editor, debug); the
**in-match HUD is built first**, then menus and results, then the tree.

## Architecture

```
app/ ──fills──▶ ui/models (plain structs) ──sync──▶ ui/screens ──Intent / MenuResult──▶ app/
                                                        │ built from
                                                        ▼
gui/  core     Gui context · Widget tree · layout · layers · input routing · hotkeys
      anim     AnimatedValue · Tween · Spring · Loop presets
      style    Theme (assets/config/ui_theme.json, hot-reload)
      text     FontLibrary (stb_truetype SDF) · TextRenderer
      icons    IconLibrary (SVG via nanosvg → premultiplied atlas)
      draw     DrawList · SDF shapes · stroked paths · stencil clips · layers
      backend  GlBackend (GL 4.5, one program, batched)
```

- `gui/` is game-agnostic: it never includes `game/` or `sim/`. It links
  `render` only for the GL wrappers (`render/Gl.h`) and `ShaderManager`.
- `ui/` screens take plain model structs, never `SimWorld`/`TowerSystem`, so
  every screen can be tested and screenshotted with fake data.
- The existing contract is kept: the UI never mutates the sim; it emits
  `ui::Intent` / `ui::MenuResult` and `app/` applies them.

### Draw layer (built)

- **Shapes are shader-evaluated SDFs** (`gui/draw/Shape.h`, `gui.frag`). One
  quad per shape; the single distance value gives the anti-aliased edge, the
  outline, a soft or hard drop shadow, the membrane band with its cytoplasm
  fill and rim highlight, organelle dots, dashes, and clipping for fluid inside
  a cell. Kinds: `Box` (rounded rect / capsule / membrane with `bulge` and live
  `wobble`), `Ellipse`, `Arc` (rings, cooldown and progress arcs, spinning
  dashed halos), `Fluid` (blood bar and ability cells: liquid to a level with a
  sloshing surface and bubbles), `Radial` (glows, the critical vignette).
- **Hit testing** uses `gui/draw/ShapeSdf.h`, a CPU mirror of the shader's
  outline (same constants). `test_gui_render.cpp` checks they agree.
- **Free-form curves** (vessels, lane thumbnails, range rings) are CPU
  tessellated (`DrawList::stroke_polyline`, `Path` with SVG path parsing):
  round/miter joins, round/butt caps, an anti-aliasing fringe, dash arrays with
  SVG `stroke-dashoffset` semantics.
- **One vertex format, one program**: shapes, curves, SDF text and icons share a
  24-byte vertex; shape parameters live in an SSBO. Draw calls split only on a
  scissor change, a stencil clip (`push_clip_shape`, for content inside a
  membrane such as level thumbnails) or an offscreen layer (`push_layer`, to fade
  a group without overlap seams). The Kit showcase is 8 draws.
- **Text**: SDF glyphs baked lazily (one bake serves 13–72 px); CSS-like styles
  (em size, letter-spacing in em, uppercase, tabular digits, line-height with
  half-leading); outline = visible width outside the glyph (canvas
  `-webkit-text-stroke: 9px` + `paint-order: stroke` = 4.5); hard or soft
  offset shadows; wrapping and ellipsis.
- **Icons**: the canvas's own SVGs, extracted by `tools/extract_icons.py` into
  `assets/ui/icons/` (towers, abilities, pathogens, elite marker, ATP, Memory
  Cell, Antibody, the tree's stat glyphs, control glyphs), rasterized on first
  use per pixel size into a premultiplied atlas: one quad per icon.

## Phases

| # | Phase | Status |
|---|---|---|
| 1 | Foundation: fonts, draw layer (SDF shapes, paths, stencil clips, layers), text, icons, GL backend, showcase test | **Done** |
| 2 | Core: widget tree, flex / anchored / world-anchored layout, `Gui` context and layers, pointer state machine (hover, capture, click/deny, drag, wheel, tooltips), animation (`Tween`, `Spring`, the canvas loops beat/pulse/wobble/throb/halo/spin/flow), theme (`assets/config/ui_theme.json`), base widgets (`Panel`, `Label`, `Icon`, `Button`, `Meter`, `Ring`, `Spacer`) | **Done** |
| 3 | In-match HUD: `HudModel` + `app/UiBridge`, `HudScreen` (with the selection and armed cursors that were `Hud.cpp`'s statics) for the five canvas states, world overlays, `Gui` in `App`, the ImGui HUD removed (`Hud` became `DevUi`), `--screenshot --ui`, gym `ui` and `integrity` commands | **Done** |
| 4 | Out-of-match screens: main menu (with the macrophage mascot), level select with campaign gating, pause, victory and defeat, screen transitions | **Done** |
| 5 | Skill tree: `TreeScreen` over the 75 nodes of `ImmunityTree.cpp`, laid out from the canvas; ImGui `Menu.cpp` removed | **Done** |
| 6 | Polish: side-by-side pass against every artboard, motion tuning, UI scale option, perf (< 0.5 ms CPU per frame) | Next |

### Core (built)

- **Retained tree** (`gui/core/Widget.h`): screens build widgets once and set
  properties as the game changes. Every widget has an id; `path()` joins them
  ("hud/dock/neutrophil", anonymous containers skipped) and `Gui::find`,
  `Gui::click`, `Gui::hover`, `Gui::dump` address widgets that way, for tests
  and the planned `ui.*` gym commands.
- **Layout** (`gui/core/Layout.h`) is a small flexbox: row, column or stack;
  padding, gap, cross-axis align, main-axis justify, grow; sizes fit / px /
  fill / percent with min and max. Children can instead be anchored to a point
  of the parent, or to a world point projected through the game camera each
  frame (`Gui::set_projection`). Layout is recomputed every frame.
- **Pointer routing** (`gui/core/Gui.h`): layers take the pointer top-down;
  events go to the nearest interactive widget and bubble to its ancestors;
  press capture, drags with a 4 px threshold, Click on release over the
  pressed widget, **Deny** instead of Click on a disabled one (the shake and
  the "can't afford" sound), right-click, wheel, tooltips after a delay.
  Panels block the pointer by their drawn outline, not their rect;
  `wants_pointer()` tells the game to keep that click off the world.
- The UI needs no raw SDL events (it has no text fields), so `InputState`
  stays as it is: app/ fills a `PointerInput` from its mouse API.
- **Theme**: colours (hex, names, `name@alpha`), text styles, shape presets
  with `base` inheritance, and numbers, from `assets/config/ui_theme.json`. A
  malformed reload keeps the last good theme. Pathogen family colours are set
  at runtime from `render::family_color`, never duplicated.
- **Animation** runs on the render clock: `Tween`, `Spring`, and the canvas's
  CSS keyframes ported one to one (`gui/anim/Anim.h`).

### In-match HUD (built)

- `ui/hud/HudModel.h` is everything the HUD shows as plain data;
  `app/UiBridge.cpp` fills it from the live game each frame, and
  `ui/hud/HudScreen` never includes `SimWorld`, `TowerSystem` or the wave
  director — which is what lets `tests/test_ui_hud.cpp` drive every state from
  a fixture.
- Per frame in `App::render_frame`: `sync_hud` (model → widgets) →
  `run_gui_frame` (layout, pointer, animation; its pointer capture is OR-ed
  into `InputState` so a HUD click never reaches the world or the camera) →
  `HudScreen::handle_input` (world clicks, hotkeys, queued button intents) →
  `apply_intents` → world passes → `gui_.render` → ImGui (`DevUi`) on top.
- Layout, sizes and colours follow the canvas artboards (Main, Placing,
  Inspect, Prep, Critical). The whole HUD is one draw call.
- Input: 1–5 arm towers in dock order, Q W E R abilities, Space sends the
  wave during prep, Escape disarms or closes the popup before it opens the
  pause menu. `InputState` latches button presses and action key-downs from
  SDL events, so a tap whose press and release fall between two frames still
  counts.
- Verification: `--screenshot <level> --ui` with `ui …` gym commands (see the
  run-immune skill for the five canvas states).

### Out-of-match screens (built)

- `ui/front/FrontEnd` owns the main menu, the campaign level select, pause and
  the results (level cleared / failed, with the editor-playtest and sandbox
  variants), from the Menu, Levels, Victory and Defeat artboards. app/ picks
  the screen from `GameStateId` (`App::front_screen`) and fills a
  `ui::FrontModel` (`App::front_model`, `UiBridge`); a click comes back as the
  same `ui::MenuResult` the ImGui menus reported, applied by
  `App::apply_menu_result`.
- Flow, as in the canvas: title → Strengthen Immunity → Campaign → level;
  results offer Strengthen Immunity, then Next level / Replay or Retry /
  Levels. Escape walks back the same chain. F4 on the title opens the editor.
- **Campaign**: the `assets/levels/campaign_NN_*.json` files in name order;
  a level opens when the one before it is cleared (`ui::campaign_unlocked`).
  Other level files stay reachable through the gym `level` command and the
  editor. The map opens on the next level to play (gold halo), draws the
  opened stretch of vessel in blood colours with plasma flowing, and a second
  click on a selected cell plays it.
- **Level thumbnails** are the real level: `UiBridge::make_level_thumb`
  samples each vessel's Catmull-Rom and turns obstacles into polygons, fitted
  into the cell; the cell stencil-clips them to its membrane.
- **Backdrop**: `TissueBackdrop` lays out wobbling SDF tissue cells on a
  seeded, staggered grid (any aspect ratio); `VesselStroke` strokes a path as
  layered bands with an optional flowing dash.
- **Mascot**: the macrophage is cut from the Menu artboard as an illustration
  icon (`mascot_macrophage.svg`, `data-pad` keeps its bake tight); the
  bacterium in its grip wobbles on its own.
- **Transitions**: the outgoing screen fades out and takes no pointer
  (`Widget::accepts_pointer`); the incoming one buds in from 97% scale. Both
  fade as a group through an offscreen layer (`Widget::group_opacity`).

### Strengthen Immunity (built)

- `ui/front/TreeScreen`, shown by `FrontEnd` for `FrontScreen::Tree`. The
  canvas's Tree artboard places every node, vessel and label by hand;
  `tools/extract_tree_layout.py` writes that to `assets/ui/tree_layout.json`
  (keyed by the game's node keys), and the screen draws from it. No pan or
  zoom: the tree fits the 1920x1080 frame, as designed.
- `ui::TreeModel` (`app/UiBridge::make_tree_model`) carries each node's
  level, state (`MetaProgression::check_purchase`: locked, short, available,
  maxed), next price and missing prerequisite, the wallet and branch points.
  The screen syncs in place every frame, so a purchase keeps the selection.
- Nodes are SDF cells: fill and rim by state, a white halo when buyable, a
  spinning dashed ring when selected, level pips round the lower rim, and an
  8-lobed star with a gold ring for capstones. A vessel's lumen lights when
  its node is owned; the trunks carry flowing plasma.
- Top bar: the selected node (glyph, name, level, effect, what it needs, its
  price, Grow), the wallet, Respec (not in the canvas; kept from the old
  screen), Play (to the campaign). Grow reports `MenuAction::PurchaseNode`;
  app/ buys through `MetaProgression`, so the screen cannot disagree with
  the rules.

## Building and testing

- Unit tests: `tests/test_gui_draw.cpp`, `test_gui_text.cpp`,
  `test_gui_icons.cpp`, `test_gui_core.cpp`, `test_ui_hud.cpp`, the logic
  half of `test_menu.cpp` (no GL); `test_gui_render.cpp` and the GL half of
  `test_menu.cpp` (headless GL) write `gui_showcase.png` and `front_*.png`
  to the working directory.
- Regenerate fonts: `python tools/build_fonts.py` (needs fontTools).
- Refresh icons after the canvas changes: `python tools/extract_icons.py`;
  `--check` reports icons that are stale. The tree layout likewise:
  `python tools/extract_tree_layout.py [--check]`.
