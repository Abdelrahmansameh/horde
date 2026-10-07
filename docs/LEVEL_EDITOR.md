# In-game level editor

The editor works on a `LevelDef`, renders the shared geometry bake, and can play unsaved content through the real level-session loop. Launch it from the repository root:

```powershell
$immuneExe = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune.exe"
& $immuneExe --editor
& $immuneExe --editor assets/levels/plaque_field.json
```

F4 opens a new/template editor document from the title screen or the currently played level from a live run. The current player title menu has no editor button. `--editor` without a file starts a straight-lane template; use **File → New** to choose another template.

See [Levels](LEVELS.md) for the schema, catalog, geometry pipeline, validator, and formatting command; [Building](BUILDING.md) covers launch/build requirements.

## A typical authoring session

1. Open an existing level or choose **File → New**. Starter shapes are Straight lane, Fork, Switchback, Convergent chamber, and Multi-lane trunk.
2. Set the name, region, world dimensions, cell size, lane width, and initial wave count. Template parameters generate real geometry, markers, zones, objective, and waves for further editing.
3. Use the canvas and outliner to arrange geometry; the inspector edits the selected element's data.
4. Inspect the Validation panel and viewport halos. Clicking an issue selects its element and moves the view to it.
5. Use **Play → Play** or **Play from selected wave** to test the in-memory document. Return with Escape or the playtest's **Back to Editor** action.
6. Save through **File → Save As** for a new file, then run `--level-check` and `--level-fmt --check` against the saved file.

Templates and the wave ramp generator materialize ordinary authored data. The game does not re-run those generators when loading the file.

## Canvas tools and navigation

| Key | Tool | Current behavior |
|---|---|---|
| V | Select | Click, Shift+click toggle selection, marquee, drag to move; Alt+drag duplicates the hit element |
| P | Pen | Click to add vessel points; click a vessel's final endpoint to extend it; Enter/right-click finishes |
| W | Width | Drag from a control point to its desired edge; Shift blends neighboring widths, Ctrl sets the whole vessel |
| B | Obstacle | Keys 1–5 select disc, capsule, box, polygon, ridge; drag the first three, click points and Enter/right-click for polygon/ridge |
| S | Spawn | Click to add a marker, snapping to a nearby centerline unless Alt is held |
| O | Objective | Click to add an oriented rectangular objective; edit half-extents/rotation in inspector |
| Z | Placement zone | Drag a rectangular buildable zone |
| Q | Squad path | Click polyline points; Enter/right-click finishes |

Middle-drag or Space+drag pans. The wheel zooms around the cursor. Grid/snap controls sit in the toolbar; Alt temporarily suspends snapping. Selected box obstacles/objectives have a rotation grip; angular snapping uses 15-degree increments, with Alt to suspend it. Handles are picked in screen pixels so they remain usable at different zoom levels.

Implemented keyboard shortcuts:

| Shortcut | Action |
|---|---|
| Ctrl+Z | Undo |
| Ctrl+Shift+Z or Ctrl+Y | Redo |
| Ctrl+D | Duplicate primary selection |
| Delete | Delete selection |
| F | Frame selection |
| Home | Frame play bounds |
| Alt+1 through Alt+8 | Toggle grid, world bounds, vessels, obstacles, spawn points, objectives, zones, squad paths |
| Escape | Cancel an in-progress canvas operation; remain in the editor |

Canvas shortcuts are disabled while a text input has focus. File and Play menu entries display shortcut labels such as Ctrl+S and F5, but the current code does not wire those labels to keyboard handlers. Use the menu actions for New/Open/Save and Play/Stop.

## Panels, camera, and wave editing

The dockspace leaves the central area empty for the world; `imgui.ini` stores the local panel layout. Camera framing compensates for panels covering the outer part of the framebuffer. **Capture current framing** in level settings stores the uncovered viewport as the game's starting camera, rather than copying a raw camera center hidden behind panels.

The outliner groups lanes/vessels, obstacles, spawn points, objectives, zones, and authored squad paths. Point handles take precedence over filled bodies in picking, and obstacle picking takes precedence over underlying vessel bodies. The inspector is shape-aware and shows JSON rotations in degrees.

The Waves panel supports wave selection, ordering/duplication, spawn-entry editing, a timeline/table view, modifier selection, markers, elite ids, squad sizes, and path filters. Timeline bars can move spawn-entry start times. The ramp generator controls first/last family counts, appearance waves, prep, reward, duration, and curve, then writes real entries into the document. Budget readouts summarize total agents, release span, and peak release rate; they do not model every replication descendant or prove the runtime capacity is sufficient.

Use **Level → Bake derived squad paths to authored** before editing a generated path spread. A lane with even one authored path no longer gets automatic derivation for that lane. The command copies the loader's resolved paths into document data; review the authored route set afterward.

## Playtest behavior

Playtesting calls `App::load_level_def()` on the in-memory document, then the normal `step_level()` session. It is sandboxed: all types/abilities are unlocked, progression bonuses are off, and no campaign payout is saved. Level economy and placement rules still apply through the interactive app.

**Play from selected wave** trims only the live director's table. Editing data remains intact. Playtest Restart replays the same document and starting wave; it does not reload the file, which lets an unsaved document be tested repeatedly. Escape returns from live play/results to editing; if the gym panel is open it consumes Escape first.

Editing and live simulation use separate geometry buffers. The editor document is not a mutable reference handed to the sim. Returning to editing re-frames and re-bakes the document.

## Validation and saving

Validation collects errors and warnings with an `ElementRef` and optional world anchor. Errors block ordinary Save; warnings remain advisory. **Save anyway (has errors)** is an explicit work-in-progress override. It does not bypass the loader/writer's serialization requirements or guarantee an invalid file will load later.

[EditorMode.cpp](../src/app/EditorMode.cpp) owns file lifecycle:

- Failed Open leaves the existing document intact; startup failure can fall back to a straight template with an error message.
- New/Open replaces the document and clears selection/undo history. Save needed edits before switching documents.
- Revert reloads the current source file and discards edits. An unsaved document has no file to revert to.
- Overwriting an existing file attempts a `<path>.bak` backup before writing. A backup failure warns but does not stop Save.
- Save uses the canonical writer, reloads the written file, and compares `level_equal()` before marking the document saved.
- The title's `*` means the document differs from its saved snapshot within the writer/equality tolerance.

CLI `--level-fmt` also round-trip checks before overwriting, but does not create the editor backup. Unknown JSON fields are discarded on loading and cannot be recovered by the writer. Persist new authoring data in the schema rather than relying on unrecognized keys.

The shared bake is synchronous when invalidated and normally deferred until a canvas gesture completes. Cost depends on geometry, cell size, simulation bounds, and hardware. The status bar reports measured bake time; `--level-check` reports cell count and stage timings. Historical fixed millisecond claims are not a current performance guarantee.

## Editor console commands

The gym `edit` family reaches the same document operations. It is available while editing (and when the editor document is being playtested). Ordinary `--sim-test` and `--screenshot` contexts do not supply a document and reject `edit`.

```text
edit new [straight|fork|switchback|convergent|multi-lane] [name]
edit list
edit vessel add <x,y> <x,y> [width]
edit point add <vessel-index> at <x,y>
edit point <vessel-index> <point-index> w <width>
edit obstacle <disc|capsule|box|polygon|ridge> at <x,y> [r|size <value>]
edit spawn [add] at <x,y>
edit objective [add] at <x,y>
edit zone <x,y> <x,y>
edit delete
edit undo
edit redo
edit validate
edit save [path] [force]
edit revert
```

Indices are zero-based. `edit delete`/`erase` acts on the current selection. Template selection is a case-insensitive prefix of its display label. Console tokens cannot contain whitespace, so use the UI for paths/names requiring spaces.

`edit validate` runs JSON-only validation without baked geometry. It returns a failed command result on structural errors, but is not the full reachability check. Use `--level-check` on the saved file for a complete CLI gate. `edit save PATH force` requests the save override; `edit save` uses the existing source path.

A startup recipe can create and save a template:

```powershell
& $immuneExe --editor --exec "edit new fork my_level; edit list; edit save assets/levels/my_level.json"
```

This launches a windowed editor and remains running. `--exec` failure is logged rather than converted into a process-level gate, so inspect the result before assuming the save happened. For automation of the document model, use the headless Catch2 tests or call its C++ APIs in a test fixture.

## Implementation map and extension rules

| Source | Responsibility |
|---|---|
| [LevelDoc.h](../src/game/editor/LevelDoc.h), [LevelDoc.cpp](../src/game/editor/LevelDoc.cpp) | Document, selection, hit tests, id/reference hygiene, operations, whole-document undo snapshots |
| [LevelValidate.cpp](../src/game/editor/LevelValidate.cpp) | Error/warning rules and bake-dependent reachability |
| [LevelTemplates.cpp](../src/game/editor/LevelTemplates.cpp) | Starter levels, wave ramp, release budgets |
| [LevelWriter.cpp](../src/game/level/LevelWriter.cpp) | Canonical output/equality |
| [EditorCanvas.cpp](../src/ui/editor/EditorCanvas.cpp), [EditorGizmos.cpp](../src/ui/editor/EditorGizmos.cpp) | Navigation, input, gestures, projection, overlays |
| [EditorPanels.cpp](../src/ui/editor/EditorPanels.cpp) | Docked panels, dialogs, inspector, waves |
| [EditorMode.cpp](../src/app/EditorMode.cpp) | File lifecycle, separate bake, validation state |
| [LevelTools.cpp](../src/app/LevelTools.cpp) | `--level-check`, `--level-fmt` |
| [App.cpp](../src/app/App.cpp) | Entry, playtest, stop, app-state transitions |

Undo stores up to 128 complete document snapshots. Bracket a drag/multi-field gesture with `begin_gesture()`/`end_gesture()` instead of pushing one operation per frame. Direct `mutable_def()` writes require that bracket. Renaming through document operations repairs relevant references; deleting the final vessel, spawn, objective, or wave is refused. A new persistent field needs loader/writer/equality coverage as well as inspector controls.

Relevant tests include [test_level_edit.cpp](../tests/test_level_edit.cpp), [test_level_writer.cpp](../tests/test_level_writer.cpp), [test_level_validate.cpp](../tests/test_level_validate.cpp), [test_level_templates.cpp](../tests/test_level_templates.cpp), [test_gym_edit.cpp](../tests/test_gym_edit.cpp), and [test_editor_panel.cpp](../tests/test_editor_panel.cpp). See [Testing](TESTING.md) for execution and GL requirements.
