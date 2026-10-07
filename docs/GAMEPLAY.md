# Gameplay and run lifecycle

IMMUNE protects an objective from authored waves of pathogens by directly
deploying autonomous immune cells. This is the current behavior of
[App](../src/app/App.cpp), [LevelSession](../src/game/session/LevelSession.cpp),
and [WaveDirector](../src/game/wave/WaveDirector.cpp). Numbers quoted here are
the checked-in defaults; levels and permanent upgrades can modify them.

Related references: [immune cells](IMMUNE_CELLS.md), [pathogens](PATHOGENS.md),
[abilities and ATP](ABILITIES_ECONOMY.md), [progression](PROGRESSION.md), and
[design intent](../DESIGN.md).

## From menu to a run

The front end includes Main Menu, Strengthen Immunity, the campaign selector,
pause, victory, and defeat screens. Campaign level filenames beginning with
`campaign_` are ordered alphabetically; the first is open, and clearing a
level opens the next. Other checked-in levels are accessible through developer
tools and direct level launches. Campaign clearing is keyed by the level's
`name`, while `display_name` is its human-readable title.

At level load, the game starts with loaded configuration, applies permanent
tree purchases to a fresh copy for a campaign run, and applies the level's
economy and allowed-cell rules. Sandbox runs, the gym, and editor playtests
use the unrestricted baseline and do not pay persistent run rewards.

## Controls

Bindings originate in [Input.cpp](../src/platform/Input.cpp); dock order and
cursor handling live in [HudParts.cpp](../src/ui/hud/HudParts.cpp) and
[HudScreen.cpp](../src/ui/hud/HudScreen.cpp).

| Input | In-level behavior |
| --- | --- |
| `1`, `2`, `3`, `4`, `5` | Select Neutrophil, Cytotoxic T, Macrophage, Goblet Cell, Fibroblast respectively, if available. Selecting the armed type again cancels it. |
| Left click with a cell selected | Deploy one cell at the cursor if valid and affordable. |
| Hold left button | Repeat deployment every 0.18 seconds by default. The hold timer uses UI time and does not accumulate a backlog. |
| Shift with deployment | Request up to ten cells at that point per event; each successful cell costs ATP, and the batch stops on failure. |
| `Q`, `W`, `E`, `R` | Complement Cascade Burst, Histamine Flare, Fever Response, Fibrin Clot. Targeted abilities arm a cursor; Fever casts immediately. |
| Right click | Cancel an armed placement or ability cursor; otherwise clear legacy tower selection. |
| Escape | Cancel an armed cursor/selection first; otherwise open the pause menu. In editor playtests it returns to the editor. |
| Space | Send the current wave during Prep; otherwise toggle simulation time between paused and 1x. |
| `.` / `,` | Increase time to 2x then 4x / return to 1x. |
| Mouse wheel / middle drag | Zoom toward the cursor / pan within the level framing. |
| `F1`, `F4`, `F12` | Debug overlay, level editor, screenshot. |

The HUD provides matching deployment and ability buttons, wave controls,
simulation speed controls, and an Auto toggle. Gameplay clicks over the UI
do not deploy into the world. Locked or level-disallowed cell types cannot be
deployed through the underlying API either.

## Deployment rules and lifetime

Normal play creates one persistent entry in the swarmer store and no ECS tower.
`PlaceTower` is a retained intent name: App handles it with `deploy_cells()`.
The validation path is `TowerSystem::validate_deploy()`, distinct from the
legacy `validate()`/`place()` spawner path.

A direct deployment requires:

1. A valid cell type and a point inside the play bounds on walkable tissue.
2. Tissue clearance at least the cell's `swarm.size`.
3. A cell type allowed by the level and unlocked by the player's tree.
4. Enough ATP for the current build cost.
5. If the level authors placement rectangles, the entire cell footprint inside
   one rectangle. An empty list imposes no additional placement-zone limit.
6. No collision with a standing scar or Fibrin Clot, and a free swarmer slot.
7. For Fibroblast, a valid nearby construction or reinforcement goal.

Friendly cells do not reserve placement space: multiple cells may be deployed
at one point and then resolve their bodies during simulation. Their placement
does not carve terrain or rebake flow. Persistent cells skip timed lifetime
decrement, but can die to hostile damage, leave the simulation bounds, or
consume themselves by splashing mucus or completing a build. A run restart
creates a fresh world; deployed cells do not persist between attempts.

## Waves

Each level supplies its `WaveDef` table, including prep duration, spawn
entries, and ATP reward. The director does not generate region-based waves.

| Phase | Behavior |
| --- | --- |
| Prep | Deploy and inspect the preview. Passive ATP income is stopped. Normal App starts with Auto disabled; Send now/Space starts the wave, or Auto starts it after its timer. |
| Spawning | Each entry releases its count over its authored start time and duration. Entries may overlap. |
| Clearing | Spawning has finished; wait until total chaff density reaches zero or a 60-second clearing timeout expires. |
| Complete | The table has finished. |

The wave's ATP reward is credited when Clearing finishes. A clearing timeout
advances the schedule; it does not erase stragglers. Directly deployed cells
continue to simulate in Prep. The legacy spawner release gate pauses factories
in Prep but does not freeze persistent cells.

An entry naming a spawn point uses that marker. An empty spawn-point ID cycles
successive squads/bursts through all markers. Squad sizes and optional path
filters can be authored per entry; otherwise the simulation defaults apply.
Squads guide a coherent cohort along an authored path while local motion still
handles crowding, walls, damage, and obstacles. Replication can make the live
count exceed the authored count shown in a preview.

## Objective and result

The world starts with 100 objective integrity. Each ordinary pathogen reaching
the objective removes one integrity by default, regardless of family density;
Homeostasis reduces the per-agent leak cost. Chaff contact uses the first
authored objective's position, half-extents, and rotation. The loader also
creates ECS objective records, but their authored integrity values do not set
the scalar integrity used by the HUD and outcome check. Multiple records are
not currently separate defended health pools. An agent retiring out of bounds
is tracked separately from a leak or player kill.

`step_level()` evaluates loss first on every simulation tick: integrity at or
below zero loses even if a win condition becomes true on the same tick.
Ordinary victory requires the wave table to be complete and `chaff_count == 0`.
This currently tests ordinary chaff rather than named-agent survival. A level
with positive `win.survive_seconds` instead wins when its deterministic tick
clock reaches that duration; exhausting its waves early does not bypass the
survival timer. The timer counts simulation time, including Prep.

Terminal campaign wins and losses call `finish_run()` and save the persistent
reward described in [progression](PROGRESSION.md). Returning to the menu or
restarting an unfinished run does not run that payout path. Sandbox and
editor-playtest results show an outcome but do not alter campaign progression.

## Where to verify a change

- [Deployment tests](../tests/test_towers.cpp): persistent single-cell creation,
  batch charging, capacity, placement, and retained legacy spawners.
- [HUD tests](../tests/test_ui_hud.cpp): dock/cursor interaction and presentation.
- [Wave tests](../tests/test_wave_director.cpp): lifecycle, early start, spawn
  rotation, and reward handoff.
- [Level content tests](../tests/test_level_content.cpp) and
  [meta tests](../tests/test_meta_progression.cpp): authored content and
  persistent campaign rules.
