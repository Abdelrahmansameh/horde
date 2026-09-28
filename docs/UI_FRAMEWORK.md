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
| 2 | Core: widget tree, flex / anchored / world-anchored layout, `Gui` context and layers, input fan-out from `InputState` and capture merge with ImGui, hotkeys, animation (`Tween`, `Spring`, canvas loops beat/pulse/wobble/throb/halo/spin/flow), theme with hot reload, base widgets | Next |
| 3 | In-match HUD: `HudModel` + `app/UiBridge`, `HudController` (selection and cursors out of `Hud.cpp`'s statics), `HudScreen` for the five canvas states (wave, placing, inspect, prep, critical), world-anchored range ring, frame-order wiring, UI sounds, remove the ImGui HUD, `--ui` in screenshot mode, `ui.click/hover/dump` gym commands | |
| 4 | Out-of-match screens: main menu (with the macrophage mascot), level select with campaign gating, pause, victory and defeat, screen transitions; remove ImGui from `Menu.cpp` | |
| 5 | Skill tree: pan canvas, vessel edges, `TreeScreen` over the 75 nodes of `ImmunityTree.cpp` | |
| 6 | Polish: side-by-side pass against every artboard, motion tuning, UI scale option, perf (< 0.5 ms CPU per frame) | |

## Building and testing

- Unit tests: `tests/test_gui_draw.cpp`, `test_gui_text.cpp`,
  `test_gui_icons.cpp` (no GL); `test_gui_render.cpp` (headless GL) writes
  `gui_showcase.png` to the working directory.
- Regenerate fonts: `python tools/build_fonts.py` (needs fontTools).
- Refresh icons after the canvas changes: `python tools/extract_icons.py`;
  `--check` reports icons that are stale.
