# Testing and verification

IMMUNE has three complementary verification paths: Catch2 tests for system contracts, JSON simulation scripts for reproducible scenarios, and real GL captures for visual changes. [Building](BUILDING.md) describes the compiler setup; [Balance](BALANCE.md) describes the automatic player and performance comparisons.

## Catch2 and CTest

Run from the repository root:

```powershell
.\tools\build.bat all windows-release
$immuneTests = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune_tests.exe"
& $immuneTests "[level]" --order decl --rng-seed 1
& $immuneTests "[gym]" --order decl --rng-seed 1
& $immuneTests "[deployment]" --order decl --rng-seed 1
```

The default `all` wrapper configures, builds, and runs CTest. [tests/CMakeLists.txt](../tests/CMakeLists.txt) gathers `test_*.cpp`, links one executable with `Catch2::Catch2WithMain`, discovers individual cases at test time, and sets the repository root as their working directory. Direct invocations need that same directory for level/config/render assets. For the debug preset, use `ctest --preset windows-debug -C Debug` after building `Debug`.

Use `immune_tests.exe --list-tests` or `--list-tags` to inspect the current suite. A quoted Catch2 expression can select a tag or test-name pattern. Useful tags include `[core]`, `[sim]`, `[chaff]`, `[determinism]`, `[level]`, `[editor]`, `[gym]`, `[towers]`, `[deployment]`, `[swarm]`, `[abilities]`, `[meta]`, `[render]`, `[gui]`, and `[audio]`; verify names in the suite before treating a tag as coverage of an entire feature.

Run focused tests while iterating and the full required suite before handing off a system change. For a documentation-only change, check documented names, paths, examples, and links; compilation is only necessary if source or build configuration also changed. Record failing test names and actual output rather than preserving old claims about a fixed number of baseline failures.

### Which tests to read when changing a subsystem

| Area | Starting points in `tests/` |
|---|---|
| CLI/modes | [test_cli.cpp](../tests/test_cli.cpp), [test_autoplay.cpp](../tests/test_autoplay.cpp) |
| Level content/serialization | [test_level_content.cpp](../tests/test_level_content.cpp), [test_level_loader.cpp](../tests/test_level_loader.cpp), [test_level_writer.cpp](../tests/test_level_writer.cpp) |
| Authoring | [test_level_validate.cpp](../tests/test_level_validate.cpp), [test_level_edit.cpp](../tests/test_level_edit.cpp), [test_level_templates.cpp](../tests/test_level_templates.cpp) |
| Editor/gym front ends | [test_editor_panel.cpp](../tests/test_editor_panel.cpp), [test_gym_commands.cpp](../tests/test_gym_commands.cpp), [test_gym_edit.cpp](../tests/test_gym_edit.cpp), [test_gym_panel.cpp](../tests/test_gym_panel.cpp) |
| Geometry | [test_tissue_raster.cpp](../tests/test_tissue_raster.cpp), [test_level_obstacles.cpp](../tests/test_level_obstacles.cpp), [test_level_lanes.cpp](../tests/test_level_lanes.cpp) |
| Rendering/VFX | [test_render_screenshots.cpp](../tests/test_render_screenshots.cpp), [test_render_vfx.cpp](../tests/test_render_vfx.cpp), [test_chaff_death_vfx.cpp](../tests/test_chaff_death_vfx.cpp), [test_chaff_hit_flash.cpp](../tests/test_chaff_hit_flash.cpp) |

These are entry points, not an exhaustive test inventory. Search for the relevant API, component, behavior, or tag to find additional cases.

## JSON simulation scripts

```powershell
$immuneExe = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune.exe"
& $immuneExe --sim-test tests/scripts/spawn_point_reaches_goal.json --threads 1
```

The script harness in [Modes.cpp](../src/app/Modes.cpp) creates the world, runs actions at named ticks, evaluates every scheduled assertion, and emits one report on stdout. Exit 0 means its evaluated assertions passed; exit 1 means a setup/parse failure or an assertion failed. `--report` does not save this mode's report: redirect stdout if a file is needed.

### Script schema 1

```json
{
  "schema": 1,
  "name": "example_scenario",
  "seed": 42,
  "ticks": 120,
  "max_chaff": 16384,
  "level": "assets/levels/gym.json",
  "actions": [
    { "tick": 0, "type": "cmd", "cmd": "invuln on; spawn bacteria 20 at p_lymph" },
    { "tick": 30, "type": "place_swarm", "tower": "macrophage", "pos": [220, 395] }
  ],
  "assertions": [
    { "tick": 0, "metric": "tick", "op": "==", "value": 0 },
    { "tick": 120, "metric": "state_hash", "op": "!=", "value": 0 }
  ]
}
```

`schema` is required and must be 1. Current defaults are `name: unnamed`, 600 ticks, capacity 16,384, and the CLI seed. The script's `seed` and `level` override CLI values. Its `ticks` controls duration; CLI `--ticks` is not the script duration. Missing `level` uses CLI `--level`, then the built-in test lane.

The minimal assertions above illustrate the format, not a meaningful regression specification. For a real scenario, assert an observable behavior such as damage, successful traversal, a surviving objective, or a bounded count at the relevant ticks.

### Action timing and supported actions

Tick-0 actions run before tick-0 assertions. For each tick `t` from 1 through `ticks`, actions for `t` run first, then the gym spawn queue drains, `SimWorld::tick()` runs, invulnerability settings are applied, and assertions for `t` run. Array order is retained for actions sharing a tick.

| `type` | Fields | Behavior |
|---|---|---|
| `spawn_chaff` | `family`, `count`, `pos: [x,y]`, `radius` | One direct burst; defaults virus, count 0, position `[0,0]`, radius 3 |
| `place_swarm` | `tower`, `pos: [x,y]` | Deploy one direct cell through `TowerSystem::deploy()` without an ATP charge |
| `place_tower` | Same as `place_swarm` | Compatibility alias; now also direct cell deployment |
| `cmd` | `cmd: "gym commands"` | Run the console script; semicolons/newlines separate commands |

A direct burst is not streamed or arranged into squads the way the gym `spawn` command can be. Use `cmd` when those behaviors are part of the scenario. Unknown `spawn_chaff` family strings currently default to virus; spell the three supported families exactly.

Unknown action types, failed placements, and failed gym commands warn but do **not** fail the run by themselves. Assertions decide the result. A gym script stops at its first failed command; the harness continues to later actions. Avoid `--quiet` while authoring a scenario, because it hides those diagnostic logs.

### Assertion metrics

| Metric | Meaning |
|---|---|
| `tick` | Completed sim tick counter |
| `chaff_count`, `named_count`, `total_density` | Current population/density |
| `objective_integrity` | Current objective integrity |
| `chaff_killed_total` | Historical aggregate removal counter; includes leaks and out-of-bounds retirements |
| `chaff_leaked_total` | Objective arrivals |
| `active_squads`, `chaff_latched` | Current squad/latch state |
| `swarmers_killed_total`, `towers_lost_total` | Friendly-unit/legacy-tower loss counters |
| `scars_live`, `scars_built_total`, `scars_lost_total` | Current/cumulative scar construction |
| `state_hash` | Hash of sim state, converted to a floating-point value for assertions |
| `chaff_spawned.<family>` | Spawns, including replication descendants |
| `chaff_killed.<family>` | Damage kills |
| `chaff_leaked.<family>` | Objective arrivals |
| `chaff_despawned.<family>` | Out-of-bounds retirements |
| `chaff_alive.<family>` | Current agents of that family |

Replace `<family>` with `virus`, `bacteria`, or `parasite`. Operators are `==`, `!=`, `<`, `<=`, `>`, `>=`; an unknown metric or operator fails the assertion. Numeric equality is exact. Large integer hashes lose precision in an assertion's floating-point conversion; compare the top-level integer `state_hash` in separate reports when checking exact replay.

Assertions outside `0..ticks` are never evaluated, and an empty assertion set can pass. Always inspect `passed`, `failed`, and the emitted assertion list to confirm the scenario actually checked what was intended.

### Script scope and reports

The loop is a bare simulation loop. It registers gameplay systems but does **not** advance the wave director, economy, or ability cooldown clock as a complete session would. The harness's `WaveDirector` has no level wave table; `wave start` alone does not play authored waves here. Use `--autoplay` or `--screenshot --ui` for full session behavior, and Catch2 tests around `step_level()` for session contracts.

Gym commands have no camera, time-scale clock, UI, editor document, level-loading callback, or autoplay callback here. `config get/set/list` is available; `config reload/dump` is absent. Config setters reapply a subset of live systems, while some `SimDesc` initialization values require a fresh world. See [Gym](GYM.md) and [Configuration](CONFIGURATION.md).

The report includes `schema`, `name`, `script`, `seed`, `ticks`, `passed`, `failed`, `assertions`, `final_state`, integer `state_hash`, `config_hash`, `config_dir`, and `result: PASS|FAIL`. `final_state` includes per-family spawned/killed/leaked/despawned counters. Replication can make spawned counts exceed the original burst; compare the specific counter needed by the test.

## GL tests and screenshots

Headless rendering means a hidden SDL/OpenGL window, not a renderer without a graphics context. OpenGL 4.5 core is still required.

```powershell
& $immuneTests "[render]" --order decl --rng-seed 1
& $immuneTests "visual verification*" --order decl --rng-seed 1
```

Some GL cases skip or report a context problem when GL is unavailable. A skipped visual case is not evidence that its picture is correct. The screenshot tests write artifacts to the temporary directory (check their stderr for exact paths, commonly `render_verify_*.png`). Inspect the actual images as well as the numeric assertions.

For a real game capture:

```powershell
$shotPath = Join-Path $env:TEMP 'immune-gym-check.png'
& $immuneExe --screenshot assets/levels/gym.json --tick 240 --threads 1 --exec "spawn all 150 at p_lymph; tower all at 500,395" --out $shotPath
```

For the current player HUD and direct cell deployment:

```powershell
$shotPath = Join-Path $env:TEMP 'immune-hud-check.png'
& $immuneExe --screenshot assets/levels/campaign_01_first_bend.json --ui --width 1920 --height 1080 --tick 120 --towers --out $shotPath
```

Without `--ui`, captures tick only the sim: authored waves and ATP do not advance automatically. `--ui` uses the complete level-session loop and exposes the `ui` command bridge. `--scenario` can seed a benchmark population, and `--towers`/`--tower` deploy direct cells at available sites. These placements are diagnostics and may skip types when no legal site is found.

A screenshot writes JSON containing output path, framebuffer dimensions, requested level, tick, seed, state snapshot, and state hash. The level field echoes the requested path even if loading fell back to the built-in lane; inspect stderr for warnings. A failed `--exec` logs an error and the capture continues, so exit 0 alone does not prove every setup command succeeded.

Use `--focus x,y --view-height H` to review a small feature at useful scale. A whole-level capture can hide per-agent problems. Shader animation uses render time rather than solely sim tick, so equal simulation inputs do not promise byte-identical PNGs.

For shader edits, read stderr for `compile failed` and review the PNG. A build or simulation test cannot prove a shader compiled successfully at runtime. Record executable, level file, seed, threads, tick, config, setup commands, and the actual observed image when reporting visual verification.

## Content and performance checks

```powershell
& $immuneExe --level-check assets/levels
& $immuneExe --level-fmt assets/levels --check
python tools/extract_icons.py --check
python tools/gen_tree_layout.py --check
python tools/bench_report.py --no-history
```

Level checking reports semantic/baked errors and warnings; formatting checks canonical text without writing it. Generated icon/tree checks compare against their source inputs. They are independent of Catch2 and are not automatically added to CTest. See [Levels](LEVELS.md), [Tools](TOOLS.md), and [Balance](BALANCE.md) for interpretation.

## Reproducibility limits

Keep the executable revision, level, tuning, seed, commands, and thread count fixed. Prefer `--threads 1` for scripted comparisons. [Flow-field rebakes](../src/sim/SimWorld.cpp) have a wall-clock budget, so geometry-changing scenarios can depend on whether work completes within that budget. Run repeated baselines before calling a changed result a regression; do not infer exact determinism from seed alone. Cosmetic VFX and audio are outside the sim-state hash.
