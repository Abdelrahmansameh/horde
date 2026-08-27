# IMMUNE — In-Game Level Editor

**Status: built.** Single authoritative document; supersedes the two earlier
proposals, both deleted.

```bash
immune --editor                              # new level from a template
immune --editor assets/levels/plaque_field.json
```

Also: main menu → **Level Editor**, or **F4** from inside a running level to
edit the level you are playing.

---

## 1. Why in-game

A level is not the JSON file. It is what the JSON file *bakes into*:
`vessels[]` → `TissueMask` → carve `obstacles[]` → `DistanceField` → wall-cost
stamp → `FlowField` → `LaneOwnershipMap`. Almost every authoring mistake is
invisible in the text and obvious in the bake — a spawn point three units off
the lumen, a switchback that pinches shut at the current `cell_size`, a ridge
two units wider than intended sealing a lane.

So the editor renders the bake, live, through the real `render::Renderer`. What
you see is the game, and Play is an `instantiate()` rather than a
save-relaunch-navigate cycle.

---

## 2. The four decisions everything follows from

1. **The document is a `LevelDef` and nothing else.** Anything the editor wants
   to configure that `LevelDef` cannot hold went *into* `LevelDef` first (that
   is schema v2, §7) — never into private editor state the game cannot read.
2. **The editor bakes into its own buffers.** `instantiate()` needs a whole
   `SimWorld` and `SimWorld::init()` is destructive.
   `LevelLoader::bake_geometry()` gives the editor a private mask/SDF/flow that
   `Renderer::submit_tissue()` accepts directly — **zero renderer changes**, and
   editing never destroys a run.
3. **Headless model, ImGui front end.** `game/editor` has no ImGui and no GL, so
   the document, its operations and the validator unit-test with no screen and
   run in CI. `ui/editor` is a typist for them, and so is the `edit` gym command
   family — the GUI cannot become a second API.
4. **Gizmos draw into `ImGui::GetBackgroundDrawList()`.** The renderer has one
   world-space line facility and it is lines only. Projection goes through
   `Camera::world_to_screen()`, which is exact because the tilt is fixed. **A
   world circle is a screen ellipse**, so every ring is an N-gon.

---

## 3. Module map

```
src/game/level/
  LevelWriter.{h,cpp}      LevelDef -> canonical JSON; level_equal()   [immune_game]
src/game/editor/           headless: no ImGui, no GL                   [immune_game]
  LevelDoc.{h,cpp}         document + selection + undo + ops + id hygiene
  LevelValidate.{h,cpp}    Issue list with severity, ElementRef, anchor
  LevelTemplates.{h,cpp}   5 starter geometries + wave ramp + budget
src/ui/editor/                                                          [immune_ui]
  EditorGizmos.{h,cpp}     projection helpers: rings, ribbons, handles
  EditorCanvas.{h,cpp}     camera, tools, hit-test, drag, snap, gizmos
  EditorPanels.{h,cpp}     dockspace, outliner, inspector, waves, validation
src/app/
  EditorMode.{h,cpp}       file lifecycle, live bake, validation state
  LevelTools.{h,cpp}       --level-check / --level-fmt
```

---

## 4. The rasterizer fix, and why it mattered

Both earlier plans assumed the **flow field** was the expensive bake stage and
designed the whole live-edit cadence around deferring it. Measured, that was
wrong: `rasterize_vessels` was **50–90%** of every bake and the flow solve only
6–15%.

`rasterize_vessel` picked its sample count from arc length and cell size alone —
four samples per cell — while `stamp_disc` writes the disc's whole bounding box.
Cost was therefore `steps × (width / cell_size)²`, *independent of how much new
area each stamp covered*. Lanes are ~68 wide since they were widened to hold two
squads abreast (`ARCHITECTURE.md` §4.7), which put radius-34 stamps a quarter
unit apart. On `capillary_switchback`: **151 M cell-writes into a 421 k-cell
mask — every cell written ~360 times.** A single-constant model of exactly that
quantity predicted measured time across all 14 levels within 5% mean error.

The rate is now **scallop-limited**: discs of radius `r` placed `d` apart bulge
inward by about `d²/8r`, so holding that under a quarter cell gives
`d = sqrt(2·r·cs)`, which grows with radius as the old rule failed to. Sub-cell
lumens fall back to the old rate, where the overlapping *chain* is what does the
covering.

| Level | before | after |
|---|--:|--:|
| capillary_switchback | 617 ms | **77 ms** |
| gym | 387 ms | 91 ms |
| plaque_field | 323 ms | 57 ms |
| elbow_turn | 35 ms | 21 ms |

Every level now bakes under 100 ms, which is what makes **re-bake on gesture
release** feel immediate instead of needing an explicit Apply button.
`tests/test_tissue_raster.cpp` asserts the properties the old rate got by brute
force: no interior gaps, no edge scalloping, thin lanes still contiguous,
hairpins watertight.

---

## 5. Interface

```
┌ File  Edit  View  Level  Play ─────────── IMMUNE editor - plaque_field* ┐
├──────────────┬──────────────────────────────────────────┬───────────────┤
│ OUTLINER     │ [V][P][W][B▾][S][O][Z][Q] grid 1.0 snap✓ │ INSPECTOR     │
│ ▾ Lanes      │                                          │ Level settings│
│   ▾ main     │      real tissue, real flow field,        │               │
│ ▾ Obstacles  │      real lane hues, gizmos on top        │               │
│ ▾ Spawn pts  │                                          │               │
│ ▾ Objectives │                                          │               │
│ ▾ Zones      │                                          │               │
│ ▾ Squad paths│                                          │               │
├──────────────┴──────────────────────────────────────────┼───────────────┤
│ WAVES │ VALIDATION                                      │               │
│ 1 2 3 4 5  + Ramp…  □table                              │               │
│ 140 agents over 4.0s, peak 35/s (cap 20000)             │               │
│ ▐virus 140 ████████████▏                                │               │
├─────────────────────────────────────────────────────────────────────────┤
│ x 156.5 y 44.4 | grid 1.00 | Select | 1 selected | bake 51.6 ms | ⚠2    │
└─────────────────────────────────────────────────────────────────────────┘
```

Panels dock (`imgui[docking-experimental]`, opt-in via
`ImGuiConfigFlags_DockingEnable`) around an **empty central node**, so the world
shows through and stays clickable; `imgui.ini` persists any rearrangement.
Frame-to-fit compensates for the panels, so "frame all" centres the level in the
part you can actually see rather than behind the outliner.

### Tools

| Key | Tool | Behaviour |
|---|---|---|
| `V` | Select | Click, Shift+click add, marquee, drag to move, Alt+drag duplicate, drag the rotation grip to turn an objective or box obstacle (15 deg steps while snap is on; Alt suspends) |
| `P` | Pen | Click to append control points; click an endpoint to extend it; Enter/right-click finishes |
| `W` | Width | Drag off a point sets its width; Shift smooths neighbours, Ctrl sets the whole vessel |
| `B` | Obstacle | `1`–`5` pick the shape. Disc/box/capsule drag to size; polygon and ridge click vertices then Enter |
| `S` | Spawn point | Click; snaps to the nearest lane centerline (Alt suspends) |
| `O` | Objective | Click; snaps to the centerline. The footprint is an oriented rectangle -- `half extents` + `rotation` in the inspector, or the canvas grip |
| `Z` | Zone | Drag a rect; 8 corner handles |
| `Q` | Squad path | Polyline; Enter finishes |

Global: **Ctrl+Z / Ctrl+Shift+Z** undo/redo · **Ctrl+D** duplicate · **Del**
delete · **F** frame selection · **Home** frame all · **Alt+1..8** view toggles ·
**Esc** cancel the in-progress gesture · **middle-drag** or **Space+drag** pan ·
**wheel** zoom to cursor · **Alt** suspends snapping · **F5** play,
**Shift+F5** stop.

### The details that decide whether it feels right

- **Vessels draw as their real Catmull-Rom**, sampled through the rasterizer's
  own `eval_spline`, so the curve you see is the curve that bakes. Midpoint
  diamonds subdivide; per-point rings show width.
- **Handles are sized in pixels**, so picking feels identical framed on the
  whole level or on one control point.
- **Points beat bodies** in hit-testing, so a control point on top of a filled
  shape stays grabbable. Obstacles beat the vessel under them.
- **A gesture is one undo entry.** A drag opens the gesture only once it
  actually moves, so a click that merely selects leaves nothing on the stack.
- **The inspector is shape-aware**: `ObstacleDef` shares three geometry fields
  across five shapes, so it shows only the ones this shape reads — and box
  rotation is shown in **degrees**, as the file stores it.
- **Validation is ambient**: pulsing halos in the viewport; clicking a row
  selects the element and eases the camera to it.

---

## 6. What the validator catches

`game/editor/LevelValidate` returns a *list* of issues, each with a severity, an
`ElementRef` to select and a world anchor to fly to. Strictly a superset of
`LevelLoader::validate()`. New rules worth naming:

- **Spawn point / objective not on tissue.** `Level.h` promised this since the
  schema was written; the implementation never had it.
- **Spawn point cannot reach any objective.** Existed only inside
  `instantiate()`, gated on `!obstacles.empty()` so it could not reject existing
  content. Here it runs whenever a bake is supplied — the pinched-switchback
  catcher.
- **Duplicate ids** in any collection. Unchecked anywhere before, and a
  duplicate `spawn_point_id` silently binds waves to whichever one
  `resolve_spawn_point()` finds first.
- Dangling `children`, unknown lane references, out-of-bounds geometry,
  non-positive sizes.
- Warnings: zone over solid ground, lane with no spawn point, vessel narrower
  than two cells, grid over ~500 k cells, backwards ATP/prep ramp, and a lane
  authoring exactly one squad path (which silently disables the derived spread
  for the whole lane — the **Bake derived squad paths** menu item is the fix).

All 14 shipped levels: **0 errors, 5 warnings.**

---

## 7. Schema v2

The loader accepts 1 **and** 2. Every v2 field is optional with the pre-v2
behaviour as its default, and `LevelWriter` stamps whichever version the content
actually needs — so a level using no v2 field stays a v1 file.

| Field | What it does | Read by |
|---|---|---|
| `display_name`, `description`, `author`, `difficulty`, `tags[]` | Level select showed a filename-derived name | `ui::LevelEntry` |
| `camera.center` / `view_height` / `min` / `max_view_height` | Framing was "the whole level", wrong for a long capillary | `App::load_level_def` |
| `economy.starting_atp`, `income_multiplier` | Was global in `economy.json`; a tutorial and a floodplain cannot want the same bankroll | `App::apply_level_rules` |
| `allowed_towers[]` | A level that is *about* one tower | `TowerSystem::validate` |
| `win.survive_seconds` | Alternative to clearing the table; measured off the sim tick counter so it replays identically | `game::step_level` |
| `SpawnEntry.squad_size` | 900 as 15×60 vs 6×150 are different arrivals; this was a global | `WaveDirector::tick` |
| `SpawnEntry.squad_paths[]` | Commit an entry to the outer paths only — a flank | `SquadRegistry::next_path_for_lane_filtered` |
| Empty `SpawnEntry.spawn_point_id` | Cycle each squad through every authored spawn point, in marker order; squads are born at the marker itself, then follow their path. Select an id to pin an entry to one marker | `WaveDirector::tick` |
| `editor` block | The parser drops unknown keys, so no annotation survived a Save | editor only |

**`allowed_towers` is enforced in `TowerSystem::validate()`, not by hiding HUD
buttons**, so the gym console and the balance bot obey it too; the HUD greys the
button so you can *see* the restriction.

### `placement_zones` is now enforced

`TowerSystem.cpp` used to state outright that
`PlacementResult::OutsidePlacementZone` was never returned, because
`LevelDef::placement_zones` was never threaded onto `SimWorld` — the field was
authorable, validated, drawn, read by the balance bot, and enforced by nothing.
It is threaded now, next to `SpawnPointRuntime`, and the whole tower **footprint**
must lie inside a zone.

**This changes gameplay.** Empty means "anywhere", so a level authoring no zones
is unaffected; measured against all 14 shipped levels the only difference is
`elbow_turn`, where the bot places 30 towers instead of 32.

---

## 8. Playtest

**F5** plays the in-memory document — unsaved edits included — through the real
`step_level()` loop with the real HUD. `App::load_level(path)` split into
`load_level(path)` + `load_level_def(def, source_path)` to make that possible.
**Play from wave N** trims the live director's table without touching the
document. **Shift+F5** or **Esc** returns to editing with the document untouched;
the sim never gets a mutable reference to it.

A playtest that runs to an end — cleared, or objective destroyed — gets its own
results screen: **Restart** replays the same document from the same wave, and
**Back to Editor** (also **Esc**) returns to editing. Neither one goes to the
front end, and Restart deliberately does not re-read the file: for an unsaved
document there is no path to re-read, which used to drop the editor into the
built-in test level.

---

## 9. Headless parity

```bash
immune --level-check <file|dir>          # validate; JSON report; exit 0/1
immune --level-fmt   <file|dir> [--check]# canonical rewrite / CI gate
immune --editor [file] --exec "edit ..."  # drive the editor from a command line
```

`--level-fmt` re-parses its own canonical output and compares before
overwriting, so a writer bug is caught on the file it is about to replace.
Normalising the shipped content took it from **5621 lines to 1837** with an
identical validator report.

The `edit` gym family (`edit new|list|vessel|point|obstacle|spawn|objective|
zone|undo|redo|validate|save|revert`) makes editor operations reachable from
`--exec`, and therefore from `--sim-test` and screenshot regressions. `edit
validate` *fails* on errors, so it gates a script without grepping:

```bash
immune --editor --exec "edit new switchback lvl; edit obstacle ridge 240,130 r 18; edit validate; edit save assets/levels/lvl.json"
```

---

## 10. Tests

| File | Covers |
|---|---|
| `test_level_writer` | Round-trip + idempotence over every shipped level; the four serialization traps; omit-defaults and its identity exception |
| `test_level_validate` | One fixture per rule, clean and failing; sealed lane; off-lumen spawn |
| `test_level_edit` | `undo(op(x)) == x` for every op; gesture coalescing; reference rewriting; refusals; hit-test precedence |
| `test_level_templates` | Every template validates clean (**no warnings either**), bakes, is reachable, and round-trips; ramp monotonicity; budget peak |
| `test_tissue_raster` | Watertightness and scallop bounds at every width |
| `test_gym_edit` | Every `edit` subcommand, headless |
| `test_editor_panel` | ImGui smoke over every tool, inspector branch and template, on a headless GL context; save/reload; save refusal |

81 new cases. Run from the repo root; the build needs a developer-prompt
environment. Three `test_towers`/`test_config` failures are pre-existing
`towers.json` drift on `master`.

---

## 11. Frozen headers touched

| Header | Change |
|---|---|
| `Level.h` | `GeometryBakeDesc`, `GeometryBakeStats`, `bake_geometry()`, schema-2 fields |
| `TissueRaster.h` | width-aware sample rate (§4) |
| `GameState.h` | `GameStateId::Editor` |
| `Cli.h` | `Mode::Editor`/`LevelCheck`/`LevelFmt`, `level_path`, `check_only` |
| `App.h` | `load_level_def()`, editor members |
| `SimWorld.h` | `placement_zones()` |
| `TowerSystem.h` | `set_allowed_towers()`, `PlacementResult::TowerNotAllowed` |
| `Input.h` | `Action::OpenEditor` (F4) |
| `WaveDirector.h` | `SpawnEntry::squad_size`, `squad_paths` |
| `Squads.h` | `next_path_for_lane_filtered()` |
| `Camera.h` | `bounds()` accessor |

All additive except `TissueRaster.h`'s sample rate, which changes the lumen edge
by sub-cell amounts (obstacle interiors are pixel-identical), and
`placement_zones` enforcement, which is the one deliberate gameplay change.
