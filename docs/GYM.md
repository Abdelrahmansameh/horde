# The gym and developer command console

The gym combines [assets/levels/gym.json](../assets/levels/gym.json), the ImGui [GymPanel](../src/ui/GymPanel.cpp), and the reusable [GymCommands](../src/game/gym/GymCommands.cpp) executor. It is useful for movement, combat, effects, wave, UI, and tuning experiments.

```powershell
$immuneExe = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune.exe"
& $immuneExe --level assets/levels/gym.json --sandbox
```

The panel opens automatically when the loaded level's `name` is `gym`. Backtick or F2 toggles it on other levels, and Escape closes it before other Escape handling. F1 is the debug overlay. Gym runs are sandboxed and hold objective integrity by default; `invuln off` restores the loss consequence of leaks. Headless modes default invulnerability off.

## Panel and level

The shared target bar selects cursor, spawn marker, objective, or a literal world point. The Horde tab spawns/kills families and elites; Defense places legacy emitters, sells/fires them, casts abilities, and edits ATP; Waves inspects or jumps the director; World exposes time, stepping, integrity protection, camera, overlays, fields, effects, and statistics. Buttons issue command strings and the log shows results. The input line exposes commands not represented by a button.

The current gym has five lanes converging on one objective. Each lane includes a spawn chamber and a trunk sharing its lane id.

| Lane id | Vessel type | Spawn marker |
|---|---|---|
| `lymph_lane` | lymphatic | `p_lymph` |
| `artery_lane` | artery | `p_artery` |
| `vein_lane` | vein | `p_vein` |
| `nerve_lane` | nerve_adjacent | `p_nerve` |
| `mucosa_lane` | mucosal_fold | `p_mucosa` |

Five authored waves provide a mixed arrival, fever modifier, swarm modifier, all-lane arrival, and a larger horde. The first wave's historical name `gym_1_both_families` predates the parasite family; its current entries include all three families. Read the JSON or use `wave status` for current counts and timing. `spawn_points` prints current coordinates rather than relying on old screenshots.

## Command syntax

Commands tokenize on whitespace; shell-like quoting inside the command string is not supported. Command keywords are generally case-insensitive, but ids and widget paths should use their exact spellings. `help` lists the command table; `help spawn` explains an entry; `?` aliases `help`.

Scripts separate commands with semicolons or newlines. Blank lines and lines whose first token begins with `#` are no-ops. Execution stops at the first failed command, returning all messages up to that point. Commands execute between ticks, through supplied public systems/callbacks. Missing context is a reported failure.

Point-taking commands accept `at x,y`, `at x y`, `at <spawn-id>`, `at cursor`, `at objective`, or `at center`. `organ`/`goal` alias objective and `centre` aliases center. Cursor is available only when the caller supplies it. A default spawn uses the first marker; most other default targets use the cursor if available, otherwise the world center.

### Population and defense

| Command | Behavior |
|---|---|
| `spawn <virus\|bacteria\|parasite\|all> <count> [at target] [radius r]` | Spawn ordinary agents; `all` requests `count` for **each of three families** |
| `elite <name\|id\|all\|list> [at target]` | List or spawn roster elites |
| `flood [count]` | Request each family from every spawn marker; count is per family per marker |
| `kill [family\|all]` | Flag ordinary agents for next-tick removal with accounting; does not kill every named elite |
| `tower <type\|all\|list> [at target]` | Place stationary legacy emitter(s) for free; type names below |
| `sell [all]` | Sell last/all legacy emitters; credit refund if an economy is supplied |
| `fire` | Clear legacy emitter cooldowns so they can release on the next tick |
| `cast <complement\|histamine\|fever\|clot> [at target]` | Cast a player ability subject to unlocks/cooldown and placement constraints |
| `ready` | Reload ability defaults, including cleared cooldowns |
| `atp <amount\|+amount>` | Set or add ATP |
| `integrity <value>` | Set objective integrity; use values in the intended 0–100 range for HUD review |

Current type names are `neutrophil`, `macrophage`, `cytotoxic_t`, `goblet_cell`, and `fibroblast`. Use `tower list` and `elite list` against the running executable for current statistics/roster. `upgrade` and `tier` arguments are not supported.

**The gym `tower` command uses the retained stationary `TowerSystem::place()` path.** It creates an ECS emitter and is distinct from the player deck's directly deployed mobile cells. Free placement bypasses ATP payment, but still checks geometry, zones, type allowlists, and unlocks. It searches a few nearby Y offsets when the requested point is invalid. `tower all` spreads types across the world width at the requested row; it can place fewer than all types.

Screenshot `--towers`/`--tower` and simulation action `place_swarm`/`place_tower` instead call direct deployment. These cells are not in `placed_towers()`, so gym `sell`/`fire` and `ui select` do not operate on them. Use the appropriate path for the behavior under test.

Large gym spawns use tissue-aware burst capacity, place what fits immediately, and queue the rest for deterministic release on later ticks when a spawn queue is supplied. Counts in the result distinguish immediate and queued population. Grouped spawns may use squad paths; explicit targets retain the requested location. A tick-0 capture can show only the first released portion. `radius` is a spawn parameter rather than a guarantee that every requested agent fits in that disc.

Ability aliases include `cascade`/`burst` for complement, `flare` for histamine, and `fibrin`/`barrier` for clot. `ready` also resets definitions to defaults, so it can overwrite a live ability tuning experiment.

### Session, world, and diagnostics

| Command | Behavior |
|---|---|
| `wave [start\|next\|status\|n]` | Status, skip prep, or jump to a 1-based wave of the director's current table |
| `field <radius> <kill_rate> [duration] [at target]` | Submit a circular damage field; default duration 2 seconds; nonpositive duration becomes one tick |
| `vfx <event\|all\|list> [at target]` | Raise diagnostic combat events |
| `time [scale]` | Inspect/set interactive time scale; 0 pauses |
| `step [ticks]` | Tick the sim directly 1–100,000 times; default 1; does not advance the wave/economy session loop |
| `cam <target\|fit> [height]` | Move/fit interactive camera; `camera` alias |
| `overlay <debug\|threat\|squads> [on\|off]` | Set an interactive overlay; omitted state means on |
| `squads [on\|off\|list\|paths]` | Toggle squad behavior or inspect live squads/routes; off preserves membership |
| `invuln [on\|off]` | Hold integrity after normal tick processing; no argument toggles; `invulnerable`/`godmode` aliases |
| `autoplay [on\|off] [profile]` | Enable/disable the interactive bot; `bot` alias |
| `stats` | Sim, economy, wave, and pending-spawn summary |
| `spawn_points` | Marker ids/positions and objective location |
| `level <name\|path>` | Load discovered level by name/title/stem or a path |
| `restart` | Reload the current level |

`wave n` replaces the live director table with its tail starting at `n`, so later wave numbers refer to the trimmed table. Restart to recover the original table. Invulnerability restores integrity rather than suppressing arrivals: leak counters still move, which makes reachability experiments useful without ending the run.

Event names currently are `muzzle`, `impact`, `expired`, `explosion`, `beam`, `chain`, `cone`, `freeze`, `shatter`, `slash`, `splash`, `death`, and `swarmer`. `vfx all` spreads them along a row. These are diagnostic events, including legacy effect types, rather than proof that every effect is used by current gameplay.

### Configuration and editor commands

```text
config list [filter]
config get dotted.path
config set dotted.path value
config reload
config dump
```

`list` is capped at 40 matches; supply a filter to narrow it. Vector values use comma-separated numbers. Interactive `dump` writes live base config into the loaded config directory; it is different from CLI `--dump-config`, which exports compiled defaults. `--config DIR` pins the directory against automatic hot reload. See [Configuration](CONFIGURATION.md) for fields and initialization-only settings.

The `edit` family requires an open `LevelDoc`. Its complete syntax and save/validation behavior are in [Level Editor](LEVEL_EDITOR.md). Ordinary sim-test and screenshot contexts do not supply a document.

## Which contexts support which commands

| Capability | Interactive play | `--sim-test` | Screenshot | Screenshot `--ui` | `--editor` |
|---|---|---|---|---|---|
| Spawn, legacy towers, fields, effects, stats | Yes | Yes | Yes | Yes | Needs a usable live world |
| Full authored wave/economy ticking | Yes | No | No | Yes | During playtest |
| Cursor, camera, time scale, overlays | Yes | No | No | No; use framing flags/UI pointer | Camera/editor tools available |
| `config get/set/list` | Yes | Yes | No | No | Yes |
| `config reload/dump` | Yes | No | No | No | Yes |
| `ui` bridge | Yes | No | No | Yes | Game UI only, not ImGui editor widgets |
| `level`, `restart`, `autoplay` callbacks | Yes | No | No | No | App callbacks exist; prefer editor lifecycle controls |
| `edit` document commands | No | No | No | No | Yes; also while that document is playtested |

A context can provide an empty director without ticking it; successful `wave start` does not imply waves will spawn. Headless setup failures and unsupported commands may be logged while a screenshot/script still completes. Keep diagnostic logs while developing recipes.

## Driving the game UI

`ui dump` lists current visible widget paths. Common subcommands are:

```text
ui click <path>
ui hover <path>
ui select <n>
ui cancel
ui level <n>
ui scale <0.75..1.5>
```

`ui select` selects the 1-based legacy tower model entry; `ui level` selects an unlocked 1-based campaign slot. Clicking a deck widget arms the player deployment cursor. Paths depend on screen visibility; a nonexistent/hidden widget fails. Examples include `hud/dock/neutrophil`, `hud/abilities/histamine/cell/button`, `hud/prep/send`, `tree/view/zoom_in`, and `levels/play`. Read `ui dump` before scripting paths after a UI change.

Screenshot-only setup adds:

```text
ui pointer <x> <y>
ui screen none|menu|tree|levels|pause|victory|defeat
ui tree new|sample|full
```

Pointer coordinates use logical pixels in the 1920 by 1080 design frame and move the placement/aim preview; they are not world coordinates or a world-click command. Front-end screenshots use synthetic progression/result data. Set `ui tree full` before `ui screen tree` so the view frames the chosen state. These commands do not edit the user's save.

## Useful recipes

```powershell
# Inspect movement/collision in the gym interactively.
& $immuneExe --level assets/levels/gym.json --sandbox --exec "spawn bacteria 1000 at p_lymph; squads paths; time 4"

# Capture legacy emitter combat at a known gym row.
& $immuneExe --screenshot assets/levels/gym.json --tick 240 --threads 1 --exec "spawn all 150 at p_lymph; tower all at 500,395" --out "$env:TEMP\immune-gym.png"

# Capture the current direct cell path and HUD.
& $immuneExe --screenshot assets/levels/campaign_01_first_bend.json --ui --tick 120 --towers --out "$env:TEMP\immune-cells.png"

# Exercise front-end tree presentation with a synthetic full tree.
& $immuneExe --screenshot assets/levels/campaign_04_twin_channels.json --ui --width 1920 --height 1080 --exec "ui tree full; ui screen tree" --out "$env:TEMP\immune-tree.png"
```

Inspect stderr, state JSON, and the resulting PNG. See [Testing](TESTING.md) for what each headless mode can prove and for repeatability limits.

## Extending the console

Add a command-table entry and a matching executor branch in `GymCommands.cpp`. Keep nullable-context checks, readable failure messages, between-tick mutations, and system-level APIs. Add command tests in [test_gym_commands.cpp](../tests/test_gym_commands.cpp), and panel coverage when adding a control. A new callback must be wired into each intended context separately; listing a command in `help` does not make it available everywhere.
