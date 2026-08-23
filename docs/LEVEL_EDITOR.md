# IMMUNE — Level Editor Plan

Status: proposed. Extends `Level.h`'s schema to v2; supersedes nothing.

---

## 1. The decision: an in-game editor, not an external tool

A level in this project is not the JSON file. It is what the JSON file *bakes into*:
`vessels[]` → `TissueMask` → `DistanceField` → `FlowField` → `LaneOwnershipMap`. Almost
every authoring mistake is invisible in the text and obvious in the bake — a spawn point
three units off the lumen, a switchback whose inner wall pinches shut at width 4, a lane
that never reaches the objective, a `placement_zone` sitting over solid tissue.

So the editor renders the bake, live, through the real `render::Renderer`, in the real
process. Two consequences drive the whole design:

- **What you see is the game.** No second rasterizer, no Python approximation, no
  "preview" that can disagree with what ships. The editor calls the same
  `rasterize_vessels` / `bake` path `LevelLoader::instantiate()` calls.
- **Playtest is instant.** The edited `LevelDef` is already in memory next to a live
  `SimWorld`. Hitting Play is an `instantiate()`, not a save-relaunch-navigate cycle.

Everything else follows the split `game/gym` already established: **the editing model is
headless and unit-testable; every line of ImGui lives in `ui/`.** That is what lets
`--level-check` run the exact validator the editor's red halos come from, in CI, with no
window.

---

## 2. Format changes

### 2.1 A writer, at last

`Level.cpp` has a parser and no serializer. Everything below depends on fixing that.

**`src/game/level/LevelWriter.{h,cpp}`**

```cpp
/// Canonical JSON for a LevelDef. Round-trip exact: parsing the result must
/// produce a LevelDef equal to the input.
std::string level_to_json(const LevelDef& def);

/// Field-by-field equality, for the round-trip test and for the editor's
/// dirty flag. Floats compared exactly -- canonical output makes that valid.
bool level_equal(const LevelDef& a, const LevelDef& b);
```

Rules the writer follows, because a level file is a git artifact before it is a game
asset:

- `nlohmann::ordered_json` with a fixed key order (`schema, name, display_name, region,
  world, camera, economy, vessels, spawn_points, objectives, placement_zones,
  squad_paths, ambient_drift, waves`). The current files are alphabetised by
  `nlohmann`'s default map, which puts `waves` in the middle of the geometry. Reading
  order should match authoring order.
- **Omit every field equal to its default.** A 90-line level should not carry
  `"elite_id": 0` on forty spawn entries.
- Floats rounded to 3 decimals on write. The shipped content already looks like this
  (`287.378`); making it a rule stops a drag gizmo from emitting
  `287.37799072265625` diffs.
- Vectors written inline (`"p": [43.998, 44.374]`) instead of exploded over six lines.
  Same information, a third of the file.

### 2.2 Schema v2 — the things a level cannot currently say

The loader accepts `schema` 1 **and** 2; every v2 field is optional with the current
behaviour as its default, so all thirteen shipped levels keep loading unchanged. The
writer emits `"schema": 2`.

| New field | Type | Why |
|---|---|---|
| `display_name`, `description`, `author`, `difficulty` | string / string / string / int | Level select shows a filename-derived name today. |
| `tags` | string[] | Grouping in level select and in the editor's file browser. |
| `economy.starting_atp` | u32 | Currently global in `economy.json`. A tutorial level and a floodplain cannot want the same opening bankroll. |
| `economy.income_multiplier` | f32 | A per-level difficulty lever that isn't "more enemies". |
| `allowed_towers` | string[] | Empty = all. The biggest missing design lever: a level that is *about* the Goblet Cell. Read by the HUD build menu. |
| `camera.view_height`, `camera.center`, `camera.min/max_view_height` | f32 / vec2 | Framing is currently "the whole level", which is wrong for a long capillary. |
| `win.survive_seconds` | f32 | Alternative to "clear the table". 0 = wave-clear, i.e. today's behaviour. |
| `editor` | object | Grid size, last camera, author notes. Editor-only; the loader ignores it. |

Two existing fields get straightened out at the same time:

- `Vessel::children` is parsed and **used by nothing**. Rather than delete a documented
  field, the editor gives it a job: dragging a parent vessel's terminal control point
  drags every child's first point with it, so a bifurcation stays welded. The validator
  flags dangling child ids.
- `LevelLoader::validate()`'s doc comment promises "every spawn point must reach at
  least one objective". The implementation does not check it. §3.2 makes that true.

### 2.3 Shared bake, so the preview cannot drift

`instantiate()` currently owns spline→mask→sdf→flow *and* needs a whole `SimWorld`. Split
the geometry half out, additively on the frozen header:

```cpp
/// Bakes def's geometry into caller-owned buffers. instantiate() is this plus
/// the SimWorld wiring; the editor is this alone.
LevelLoadResult bake_geometry(const LevelDef& def, sim::TissueMask& mask,
                              sim::DistanceField& sdf, sim::FlowField& flow) const;
```

No behaviour change, no new file, and now exactly one rasterizer in the project.

### 2.4 Worked example — exactly what Save writes

`assets/levels/elbow_turn.json` today is **328 lines**. Through the writer it is **95**,
with the same parsed `LevelDef` (verified: parse → write → parse is equal field for
field). Abridged to two waves here; the real output carries all eight.

```json
{
  "schema": 2,
  "name": "elbow_turn",
  "region": "capillary",
  "world": { "min": [0.0, 0.0], "max": [997.88, 997.88], "cell_size": 2.0 },
  "vessels": [
    {
      "id": "main",
      "points": [
        { "p": [42.0, 955.582], "w": 68.0 },
        { "p": [498.791, 955.582], "w": 68.0 },
        { "p": [901.842, 955.582], "w": 68.0 },
        { "p": [922.408, 951.492], "w": 68.0 },
        { "p": [939.841, 939.841], "w": 68.0 },
        { "p": [951.492, 922.408], "w": 68.0 },
        { "p": [955.582, 901.842], "w": 68.0 },
        { "p": [955.582, 498.791], "w": 68.0 },
        { "p": [955.582, 42.0], "w": 68.0 }
      ]
    }
  ],
  "spawn_points": [
    { "id": "p0", "pos": [42.0, 955.582], "radius": 26.444 }
  ],
  "objectives": [
    { "id": "organ", "pos": [955.582, 42.0], "radius": 17.0 }
  ],
  "placement_zones": [
    { "min": [42.0, 805.11], "max": [955.582, 997.88] },
    { "min": [810.484, 42.0], "max": [997.88, 955.582] },
    { "min": [686.881, 686.881], "max": [997.88, 997.88], "concentrated": true, "priority": 2.0 }
  ],
  "waves": [
    {
      "name": "capillary_wave_1", "prep_time": 8.0, "atp_reward": 50,
      "spawns": [
        { "family": "virus", "count": 135, "duration": 4.0 }
      ]
    },
    {
      "name": "capillary_wave_2", "prep_time": 7.43, "atp_reward": 60,
      "spawns": [
        { "family": "virus", "count": 210, "duration": 4.0 },
        { "family": "bacteria", "count": 65, "start_time": 1.0, "duration": 3.34 }
      ]
    }
  ]
}
```

What disappeared, and why it is safe: `"children": []` (default empty), `"integrity": 100`
(default), `"ambient_drift": [0, 0]` (default), `"concentrated": false` / `"priority": 1.0`
on the two plain zones (defaults), `"start_time": 0.0` on the leading spawn of each wave
(default). Every one of those is a `j.value(key, default)` call in the parser, so omitting
it and writing it produce the same `LevelDef`.

**The identity exception to default-omission.** `family` and `count` are always written
even when they equal their defaults. `{ "duration": 4.0 }` is a legal spawn entry that
means 0 viruses, and no author should have to know that. The rule is: omit defaults,
except for the fields that say *what the record is*.

Layout, as visible above: one line per vessel control point, one line per spawn entry, one
line per zone — because those are the things a diff should show one of when you drag one
of them. Everything else is inline.

A schema-2 level using the new fields adds only a header block; the geometry and waves
are unchanged:

```json
{
  "schema": 2,
  "name": "skin_1_breach",
  "display_name": "First Breach",
  "description": "A single artery, and nothing clever.",
  "difficulty": 1,
  "tags": ["tutorial", "skin"],
  "region": "skin",
  "world": { "min": [0.0, 0.0], "max": [256.0, 144.0], "cell_size": 0.5 },
  "camera": { "view_height": 110.0, "center": [128.0, 72.0] },
  "economy": { "starting_atp": 450 },
  "allowed_towers": ["macrophage", "neutrophil"],
  "vessels": [ "..." ]
}
```

**Unknown fields are dropped.** The parser ignores keys it does not know, and the writer
cannot re-emit what the `LevelDef` never held. I scanned all thirteen shipped levels: not
one carries an unrecognised key at any level of the tree, so this costs nothing today. It
is still worth stating, because it means the JSON has no room for hand-written
annotations that survive a Save — which is what the v2 `editor` block (author notes, grid
size) exists to absorb.

---

## 3. Modules

```
src/game/editor/            headless. no ImGui, no GL. unit-tested.
  LevelDoc.{h,cpp}          the document: LevelDef + selection + undo/redo + ops
  LevelValidate.{h,cpp}     rich validation with world-space anchors
  LevelTemplates.{h,cpp}    starter geometries and the wave-ramp generator
src/game/level/
  LevelWriter.{h,cpp}       §2.1
src/ui/editor/              every line of ImGui
  EditorCanvas.{h,cpp}      viewport: hit-testing, gizmos, tools, snapping
  EditorPanels.{h,cpp}      outliner, inspector, wave timeline, validation, settings
src/app/
  EditorMode.{h,cpp}        session: file lifecycle, live bake, play/stop
```

`game/editor` joins `immune_game`, `ui/editor` joins `immune_ui`. New
`GameStateId::Editor`; new CLI flag `--editor [level.json]`; a button on the main menu.

### 3.1 `LevelDoc` — the editing model

```cpp
enum class ElementKind : u8 {
    None, Vessel, VesselPoint, SpawnPoint, Objective,
    Zone, SquadPath, SquadPathPoint, Wave, SpawnEntry,
};
struct ElementRef { ElementKind kind = ElementKind::None; i32 index = -1; i32 sub = -1; };
```

- **Undo/redo is whole-`LevelDef` snapshots.** A level is a few kilobytes; two hundred
  snapshots is under a megabyte. Command objects with inverse operations would be the
  "proper" answer and would be the wrong one here — they are a class of bug per
  operation, paid to save memory nobody needs.
- **Gesture coalescing.** `begin_gesture("move point")` / `end_gesture()` brackets a drag
  so one snapshot covers it, not one per mouse-move.
- **Ops are free functions on the doc**, each one undo step: add/insert/delete/move
  control point, add vessel, split vessel, branch vessel, retype vessel, rename lane
  (rewrites every referring `lane_id`), add/delete spawn point / objective / zone /
  squad path, add/duplicate/reorder/delete wave, add/edit/delete spawn entry.
- **Id hygiene is the doc's job**, not the UI's: `unique_id("vessel")` → `vessel_3`;
  renaming a spawn point rewrites the `spawn_point_id` of every wave entry naming it. An
  editor that lets you break your own references is a text editor with extra steps.

### 3.2 `LevelValidate` — what "correct" means

One function, `std::vector<Issue> validate_level(const LevelDef&, const BakedGeometry&)`,
where each `Issue` carries severity (error / warning), a message, an `ElementRef`, and a
world-space anchor so the UI can draw a halo and fly the camera to it.

Errors (block Save unless overridden, fail `--level-check`):

- Schema, empty `waves`, no vessels, no spawn points, no objectives — the existing rules.
- Duplicate ids within any collection.
- Wave entry naming an unknown `spawn_point_id`; squad path naming an unknown `lane_id`.
- Non-positive vessel width, fewer than 2 points on a vessel or squad path, non-positive
  `half_width`.
- **Spawn point or objective not on tissue** (mask lookup) — promised by the doc comment
  today, unchecked.
- **Spawn point cannot reach any objective** — finite cost-to-goal in the baked
  `FlowField` at the spawn cell. This is the check that catches a pinched switchback, and
  it is only possible because the editor bakes for real.
- Geometry outside `world.min/max`.
- `Vessel::children` naming an unknown vessel.

Warnings (shown, never block):

- A `placement_zone` overlapping no tissue, or lying outside the world.
- A lane with no spawn point feeding it; a spawn point whose resolved lane has no vessels.
- A wave whose concurrent total would exceed `sim.capacities.max_chaff`.
- Vessel width below ~2 cells at the current `cell_size` (rasterizes to a broken lumen).
- `cell_size` producing a grid over ~500k cells (bake and rebake cost).
- `atp_reward` or `prep_time` running backwards versus the previous wave.

### 3.3 `LevelTemplates`

Two generators, both of which exist to kill the tedium that stops people building levels:

- **Starter geometries**, parameterised: straight lane, fork, switchback, convergent
  chamber, multi-lane trunk. Each produces a complete, valid, playable `LevelDef` —
  vessels, spawn points, an objective, zones, and a default wave table.
- **Wave ramp generator.** Every shipped table is visibly a linear ramp (counts
  135/210/285…, prep 8.0→4.0, ATP 50→120). The dialog takes wave count, per-family
  start/end counts, prep start/end, ATP start/end, an optional curve exponent, and
  *materialises real `WaveDef`s* you then hand-edit. Not a runtime generator — the
  project deliberately deleted one of those (`Level.h`, AUTHORED WAVES). This is a
  typist for the table, exactly as `GymPanel` is a typist for the gym language.

---

## 4. The interface

```
┌ File  Edit  View  Level  Play ─────────────────────────── immune — skin_1_breach* ┐
├───────────────┬──────────────────────────────────────────────┬───────────────────┤
│ OUTLINER      │  [V][P][W][S][O][Z][Q]     grid 1.0  snap ✓  │ INSPECTOR         │
│ ▾ Lanes       │                                              │  Vessel  main     │
│   ▾ artery_a  │            (viewport: real tissue,           │   lane   artery_a │
│     • main    │             real flow field, real            │   type   artery ▾ │
│     • branch  │             lane hues, gizmos on top)        │   pts    7        │
│ ▾ Spawn pts   │                                              │   ▸ point 3       │
│ ▾ Objectives  │                                              │     x 156.5       │
│ ▾ Zones       │                                              │     y  44.4       │
│ ▾ Squad paths │                                              │     w  68.0       │
├───────────────┴──────────────────────────────────────────────┼───────────────────┤
│ WAVES   1 ▸ 2 ▸ 3 ▸ 4 ▸ 5 ▸ 6 ▸ 7 ▸ 8      + wave   ⚙ ramp…  │ VALIDATION        │
│  prep 8.0s   atp 50   modifier none ▾                        │ ⛔ p_b unreachable │
│  0s    1s    2s    3s    4s    5s    6s                      │ ⚠ zone 4 off      │
│  ▐virus 135 ██████████▏                                      │    tissue         │
│  ▐bacteria 65    ████████▏                                   │                   │
└──────────────────────────────────────────────────────────────┴───────────────────┘
```

Panels dock. That needs vcpkg's `imgui[docking-experimental]` feature — a one-line
`vcpkg.json` change, opt-in at runtime via `ImGuiConfigFlags_DockingEnable`, invisible to
the existing HUD. If it fights the current `imgui` pin, the fallback is a fixed
three-column layout; the panels themselves don't care.

### 4.1 Viewport overlays: ImGui draw lists, not a new render pass

The renderer has exactly one world-space line facility (`submit_flow_debug`'s shader,
shared by `submit_squad_debug`) and no filled shapes, no text, no thick antialiased
strokes. Rather than grow the frozen renderer interface, the gizmo layer draws into
`ImGui::GetBackgroundDrawList()`, projecting through `Camera::world_to_screen()` — which
is exact by design, "which is what grid-free continuous tower placement needs".

That buys antialiased polylines, filled handles, bezier previews, labels and dashes for
free. The only wrinkle is the camera tilt: a world circle projects to an ellipse, so
radius rings are built as N-gons through `world_to_screen` rather than as `AddCircle`.
Cheap — hundreds of primitives against a 10,000-agent renderer.

The real tissue, distance field, lane hues and flow arrows stay the renderer's job,
unchanged. The editor only turns overlays on and off.

### 4.2 Tools

Modal, single-key, paint-app conventions:

| Key | Tool | Behaviour |
|---|---|---|
| `V` | Select / move | Click select, shift-click add, drag marquee, drag to move, alt-drag to duplicate |
| `P` | Pen (vessel) | Click to append control points; click an existing endpoint to extend it; `B` on a point branches a child vessel; Enter/Esc to finish |
| `W` | Width | Drag ↕ on a point (or scroll over it) sets `w`; shift-drag smooths widths along the run |
| `S` | Spawn point | Click to place; snaps to the nearest lane centerline by default |
| `O` | Objective | Click to place; drag the ring for radius |
| `Z` | Zone | Drag a rect; 8 resize handles; `C` toggles `concentrated` |
| `Q` | Squad path | Polyline; drag the band edge for `half_width` |
| `Del` | Delete selection | |

The details that decide whether this feels good:

- **Vessels draw as their real Catmull-Rom**, with a translucent width ribbon, over the
  live rasterized tissue. Midpoint diamonds on each segment insert a control point.
- **Snapping**: to grid (default 1.0 world unit, configurable), to other control points,
  and — for spawn points and objectives — to the nearest vessel centerline. Hold `Alt` to
  suspend all of it. A spawn point off the lumen is the most common authoring error;
  making it hard to do beats reporting it afterwards.
- **Live bake with the right cadence.** Mask and SDF rebake on every edit (cheap). The
  flow field rebakes on gesture *end*, on a worker via the existing `JobSystem`, and the
  overlay swaps when it lands. During a drag you see the ribbon move at 60fps; a beat
  after you release, the tissue and the reachability check catch up.
- **View toggles**, each a checkbox and a number key: tissue mask, distance field, flow
  arrows, lane ownership tint, cost-to-goal heatmap, unreachable-tissue highlight,
  placement zones, squad paths (derived ones ghosted, plus a "bake derived to authored"
  button when you want to hand-tune them).
- **Validation is ambient**: offending elements wear a red or amber halo; clicking a row
  in the validation panel selects the element and eases the camera to it.

### 4.3 The wave timeline

Where authoring time actually goes, and the part the JSON serves worst.

- Wave strip along the top; the selected wave's spawn entries are horizontal bars on a
  seconds axis. Bar start = `start_time`, length = `duration`, colour = family (from
  `render::family_color`, so the editor speaks the game's colour language), label =
  count.
- Drag a bar to move it, drag its edges to restretch, drag vertically to reorder,
  alt-drag to duplicate. Right-click for family / elite / spawn point.
- A **table view** toggle for when you want to type exact numbers.
- Family and elite pickers enumerate the live `EnemyRoster`, the way `GymPanel`'s tabs
  already do — no hardcoded name lists.
- A **budget readout** per wave: total agents, estimated peak concurrent, agents/sec, each
  against `sim.capacities.max_chaff`. You should know you are going to blow the chaff cap
  before you play the wave, not during it.

### 4.3.1 Squads — currently emergent, not authored

There is no squad field anywhere in the wave schema, and this is deliberate: a
`SpawnEntry` is a **count, not a group** (`WaveDirector.h`). Squads fall out of three
inputs, none of them per-wave:

1. **`sim.squads.target_squad_size`** — global, 60. `WaveDirector::tick()` chops each
   entry's per-tick release into bursts of this size and opens a new squad each time one
   fills.
2. **The spawn point's lane** — resolved via `resolve_spawn_point_lane_id()`.
3. **That lane's `SquadPath` list** — each new squad takes the *next* path on the lane,
   round-robin (`SquadRegistry::next_path_for_lane`, a `lane_cursor_ % lane_count_`).

Since no shipped level authors `squad_paths`, every lane's paths are derived by
`build_squad_paths()`: offset copies of the lane centerline, count taken from the lane's
**narrowest** width, `1 + floor((min_width - 2*8) / 22)`, capped at
`auto_paths_per_lane` (3). `elbow_turn`'s 68-wide lane therefore gets exactly 3.

So `{ "family": "virus", "count": 900 }` on that lane means: 15 squads of 60, cycling
paths 0 → 1 → 2 → 0 …, each spawned on its own path's anchor rather than in the spawn
point disc. Two consequences the editor must surface, because neither is visible in the
JSON:

- **Elites are never squadded.** The squad branch is `if (e.elite_id == 0)`; an entry with
  an elite spawns ungrouped out of the spawn point disc.
- **Authoring one path disables derivation for that whole lane** — "a lane that authored
  even one path is left entirely to the author". Hand-drawing a single path on a 3-path
  lane silently drops it to 1 and changes how every wave on it reads. The editor warns on
  this, and the *bake derived to authored* button (§4.2) is the intended fix: materialise
  all three, then edit one.

**What the editor shows, with no format change.** Each timeline bar carries a derived
readout — `900 → 15 squads × 60 · paths a_0 → a_1 → a_2` — and hovering it highlights
those paths in the viewport. That alone closes most of the gap, because the information
exists, it is just currently only discoverable by reading `WaveDirector.cpp`.

**What is worth adding to v2.** Two optional `SpawnEntry` fields, both defaulting to
today's behaviour:

```json
{ "family": "bacteria", "count": 300, "duration": 4.0,
  "squad_size": 150,
  "squad_paths": ["cap_a_0", "cap_a_2"] }
```

- `squad_size` (0 = use `sim.squads.target_squad_size`). The single most expressive knob
  in the whole wave table and currently a global: 900 as 15 squads of 60 and 900 as 6
  squads of 150 are different silhouettes arriving at different times, and no level can
  ask for the second one.
- `squad_paths` (empty = round-robin the lane's paths, as now). Lets an entry commit to
  the outer paths only — a flank — which round-robin cannot express at any count.

Both are small reads in the chop loop (`target_squad_size` becomes the entry's value; a
filtered variant of `next_path_for_lane`). `SpawnEntry` gains two fields and
`WaveDirector`'s behaviour is unchanged when they are absent. A third, `"grouped": false`,
would buy a deliberately formless flood; cheap, but wait until a level wants one.

### 4.4 Playtest in place

The whole point of being in-process.

- **Play** instantiates the in-memory `LevelDef` into the App's `SimWorld` and runs the
  real `step_level()` loop with the real HUD. **Stop** returns to editing with the
  document untouched — the sim never gets a mutable reference to it.
- **Play from wave N**, so you don't replay waves 1–6 to test wave 7.
- The time-scale slider and the gym panel are both available during playtest; the gym
  language already does everything a designer wants mid-test (`spawn`, `atp`, `tower`,
  `time`).
- App gets `load_level_def(const LevelDef&, std::string restart_path)`; the existing
  `load_level(path)` becomes parse-then-call-that.
- **Bot balance run** (phase 5): run `game/autoplay` against the current document on a
  background thread and show the report — outcome, waves survived, ATP curve, leaks —
  without leaving the editor. The fastest balance loop this project can have, built
  almost entirely from parts that already exist.

### 4.5 File lifecycle

New (template wizard: name, region, world size, starter shape) · Open (the
`discover_levels()` list, with per-level validation status) · Save · Save As · Duplicate ·
Revert. Save writes `assets/levels/<name>.json` and copies the previous contents to
`<name>.json.bak`. Save is **blocked on errors**, with an explicit "Save anyway" for
work-in-progress; warnings never block. The title bar carries the dirty marker; closing
dirty prompts.

---

## 5. Headless parity

The project's rule is that everything is verifiable without a screen, and the editor gets
no exemption. Two new modes, both thin wrappers over `game/editor`:

```
immune --level-check <file.json|dir>   # full validator incl. reachability; JSON out; exit 0/1
immune --level-fmt   <file.json|dir>   # canonical rewrite through LevelWriter; --check for dry run
```

`--level-check assets/levels` becomes a CI step. `--level-fmt --check` keeps hand-written
and editor-written JSON from diverging in style.

### Tests

| File | Covers |
|---|---|
| `tests/test_level_writer.cpp` | Round-trip every shipped level: parse → write → parse → `level_equal`. Plus v1-file → v2-write → reload equivalence. |
| `tests/test_level_edit.cpp` | Every op; `undo(op(x)) == x` for each; id uniqueness and reference rewriting; gesture coalescing. |
| `tests/test_level_validate.cpp` | One fixture per rule, error and clean case each. Reachability against a deliberately pinched switchback. |
| `tests/test_level_templates.cpp` | Every starter template validates clean and instantiates. Ramp generator monotonicity. |
| `tests/test_editor_panel.cpp` | ImGui smoke test on the headless GL context, following `test_gym_panel.cpp` exactly: drive frames through every panel and every tool to catch Begin/End mismatches. |

---

## 6. Phasing

Each phase ends somewhere useful; none needs the next one to be worth having.

| Phase | Build | You can then |
|---|---|---|
| **0. Foundations** | `LevelWriter`, `level_equal`, `bake_geometry` extraction, `LevelValidate`, `--level-check` / `--level-fmt`, their tests | Catch every broken level in CI, including unreachable spawns. No UI yet. |
| **1. Shell** | `GameStateId::Editor`, `--editor`, `LevelDoc` + undo, outliner, inspector, level settings, open/save | Edit any level numerically, with real validation and a live view of the bake. Already better than a text editor. |
| **2. Canvas** | `EditorCanvas`: gizmos, the seven tools, snapping, debounced rebake, view toggles | Draw a level with the mouse. This is the phase that makes it an editor. |
| **3. Waves** | Timeline, table view, ramp generator, budget readout, roster pickers | Author a full 8-wave table in a minute instead of an afternoon. |
| **4. Playtest** | Play / Stop / play-from-wave, `load_level_def`, in-editor gym panel and time scale | Draw → play → fix, without leaving the process. |
| **5. Schema v2 + polish** | v2 fields and their UI (allowed towers, starting ATP, camera, metadata), templates and the new-level wizard, docking layout persistence, autoplay balance report | Set up *everything* about a level in one place, and balance it against the bot. |

Phase 0 is a prerequisite for all of it and is worth doing on its own merits. Phases 2 and
3 are independent of each other.

---

## 7. Coverage check — can it set up everything?

| Level data | Editor surface |
|---|---|
| `world.min/max`, `cell_size` | Level Settings, plus a draggable world rect in the viewport |
| `name`, `region`, v2 metadata | Level Settings |
| `vessels[]`: id, lane_id, type, points (pos + width), children | Outliner tree, Pen and Width tools, Inspector |
| `spawn_points[]`: id, pos, radius, lane_id | Spawn tool, gizmo, Inspector |
| `objectives[]`: id, pos, radius, integrity | Objective tool, ring gizmo, Inspector |
| `placement_zones[]` + tags (concentrated, priority) | Zone tool, Inspector |
| `squad_paths[]`: id, lane_id, points, half_width | Path tool; derived paths ghosted with a bake-to-authored button |
| `ambient_drift` | Level Settings, 2D drag pad plus numerics |
| `waves[]`: name, prep_time, atp_reward, modifier | Wave timeline gutter |
| `spawns[]`: family, elite_id, count, start_time, duration, spawn_point_id | Timeline bars and table view |
| v2: starting ATP, income multiplier, allowed towers, camera framing, win condition | Level Settings |

---

## 8. Risks

- **`imgui[docking-experimental]`** changes the vcpkg imgui branch. Low risk (docking is
  opt-in per config flag and the API is compatible), but it touches every UI target.
  Verify early in phase 1; the fixed-column fallback costs nothing to fall back to.
- **Flow rebake latency** on the largest levels. Mitigated by the gesture-end cadence and
  the worker thread; if a 500k-cell level still stutters, the fallback is a coarser
  editor-only bake at 2× `cell_size` for the overlay, with the real one on demand.
- **Frozen-header edits.** Three additive changes (`bake_geometry`, `load_level_def`, and
  the v2 fields on `LevelDef`) touch files marked FROZEN CONTRACT. All are additions with
  unchanged existing behaviour — the same shape as the amendments already in those files
  — but each needs its rationale written into the header comment, per house style.
- **Scope.** The editor must not become a second place gameplay rules live. It authors
  `LevelDef` and nothing else; anything it wants to configure that isn't in `LevelDef`
  goes into `LevelDef` first (that is what §2.2 is), never into private editor state the
  game cannot read.
