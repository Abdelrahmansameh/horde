---
name: run-immune
description: How to build, launch, drive and screenshot the IMMUNE game (horde repo) — headless --screenshot captures with --exec gym commands, framing with --focus/--view-height, --sim-test scripts, --bench, the GL screenshot unit tests, and interactive play/gym/editor. Use when asked to run the game, take or debug a screenshot, check a visual change, reproduce a bug from the console, or verify a change in the real exe.
---

# Running IMMUNE and taking debug screenshots

Everything runs **from the repo root** (`C:\Users\Abdel\OneDrive\Documents\horde`).
From anywhere else `assets/` does not resolve: shaders fail to load and the
capture comes out wrong. If you must run elsewhere, set `IMMUNE_ASSET_ROOT=<repo>/assets`.

## 1. Pick the binary

| Exe | When |
|---|---|
| `%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe` | Default shared build |
| `%LOCALAPPDATA%\horde-verify-build\bin\immune.exe` | Isolated build: use it when the user has the game open |

The user often has `immune.exe` running on the desktop. Check with
`tasklist | findstr /I immune`. **Never kill the user's game.** While it runs,
the shared build cannot relink `immune.exe` (`LNK1104`/`LNK1168`), so build the
exe in the verify dir instead.

In Bash:

```bash
E="$LOCALAPPDATA/horde-build/windows-release/bin/immune.exe"
SCRATCH="<this session's scratchpad dir>"   # never write captures into the repo
```

## 2. Build (needs vcvars)

A plain shell cannot compile: `cl.exe` can't find `cstddef` until vcvars is loaded. Use the wrappers with `cmd //c`:

```bash
cmd //c "%LOCALAPPDATA%\\horde-build\\claude_build.bat" immune_tests
```

```bash
cmd //c "%LOCALAPPDATA%\\horde-build\\claude_verify_build.bat"
```

- `claude_build.bat <target>` builds the shared `windows-release` preset (targets: `immune`, `immune_tests`).
- `claude_verify_build.bat` builds `immune` in `%LOCALAPPDATA%\horde-verify-build`, about 30 s incremental.
- `tools\build.bat configure|build|test|all [preset]` is the full configure+build+ctest cycle.
- A new `tests/test_*.cpp` needs a reconfigure. The first build after a reconfigure often fails in vcpkg ("vswhere.exe is not recognized"). Run the same command again.
- `C1041 cannot open program database`: run `taskkill /IM mspdbsrv.exe /F` and retry.
- Other Claude sessions may relink the verify exe while you use it. If you need a stable binary, copy it out and put `SDL2.dll` beside the copy.

## 3. Headless screenshot: the main debug tool

```bash
"$E" --screenshot assets/levels/gym.json --tick 240 \
     --exec "spawn all 300 at p_lymph; tower all" \
     --out "$SCRATCH/shot.png" 2> "$SCRATCH/err.txt"
```

Then **Read the PNG** and describe what you actually see. Always check stderr too:

```bash
grep -iE "error|warn|compile failed|--exec" "$SCRATCH/err.txt"
```

stderr also prints `screenshot: placed N/M towers` and `screenshot: N live rounds, N live swarmers, N particles…`. Those lines quickly tell you whether combat actually happened. stdout is a JSON report with `level`, `tick`, `seed`, `state` (chaff counts per family, kills, leaks, integrity…) and `state_hash`.

### Flags

| Flag | Meaning |
|---|---|
| `--screenshot <PATH>` | Level **file path**. A bare name like `gym` silently renders the built-in test lane, and the JSON still echoes the name you passed. The only clue is a stderr warning, so don't throw stderr away. |
| `--tick N` | Sim ticks (60/s) to run before the capture. Default 0, so nothing has moved yet. |
| `--out PATH` | Required. Write captures to the scratchpad, not the repo. |
| `--exec "a; b; c"` | Gym commands run before ticking (see §5). It stops at the first failing command, logs the failure and captures anyway. |
| `--focus x,y` `--view-height h` | Camera center and zoom in world units. The default frames the whole level, where one agent is about 6 px. Use `--view-height 9` to 30 to inspect sprite or shader art. |
| `--towers [--tower <name>]` | Auto-place one of every tower type (or just `<name>`) along the level's mid-line. |
| `--scenario <bench name>` | Pre-populate agents from a bench scenario. |
| `--ui` | Draw the in-match HUD (src/ui/hud) over the capture. The level then runs as in play: waves spawn and ATP flows (step_level, not a bare sim tick), and `ui …` gym commands work in `--exec` (see §5). Use `--width 1920 --height 1080` to match the design canvas. |
| `--config <dir>` | Use a copied and edited `assets/config` for tuning experiments. |
| `--width/--height` | Framebuffer size. Default 1600×900. |
| `--seed N`, `--threads N` | `state_hash` only matches across runs that use the same `--threads`. |

### Gotchas

- The `--screenshot` exec context has **no camera and no config**, so `cam …` and `config set …` fail there. Frame with `--focus`/`--view-height`, and tune with `--config <dir>`.
- **Shader animation can't be stepped.** Shader time comes from the wall clock, so every capture lands at about the same phase whatever `--tick` is. To tune motion, port the shader into a WebGL page in the scratchpad, serve it through `.claude/launch.json` and `preview_start`, then take one confirming game shot.
- **A broken shader is silent.** The build and all tests still pass. The pass just vanishes, e.g. every swarmer disappears. After any `assets/shaders/` edit, grep stderr for `compile failed`. GLSL reserved words like `packed`, `input`, `output`, `filter`, `sample`, `common`, `active` are the usual cause.
- `spawn` grows its disc to fit the count, so 600 agents at radius 4 really covers a disc about 56 units wide. To pack a spot, stream small bursts.
- Death flashes are replayed from the last 30 ticks, so kills show up. Particles are stepped once per tick, the same as in live play.

### Useful recipes

```bash
# one tower type in isolation vs a horde, zoomed in
"$E" --screenshot assets/levels/gym.json --tick 300 --towers --tower neutrophil \
     --exec "spawn all 200" --view-height 60 --focus 200,396 --out "$SCRATCH/n.png"

# every combat VFX at one spot
"$E" --screenshot assets/levels/gym.json --tick 20 --exec "vfx all at center" --out "$SCRATCH/vfx.png"

# before/after a tuning change: copy the config, edit it, compare
cp -r assets/config "$SCRATCH/cfg" && <edit> && "$E" --screenshot ... --config "$SCRATCH/cfg" --out "$SCRATCH/after.png"
```

To find coordinates, use `--exec "spawn_points"` or read the level JSON (`spawn_points`, `objectives`, vessel points).

## 4. Other headless modes

```bash
"$E" --sim-test tests/scripts/<name>.json --quiet ; echo "exit=$?"   # 0 pass / 1 fail; JSON on stdout
"$E" --bench chaff10k --ticks 600 --quiet                            # perf JSON (budgets in docs/AGENT_BRIEF.md)
"$E" --list-scenarios --quiet
"$E" --autoplay --level assets/levels/campaign_01_first_bend.json --report "$SCRATCH/r.json"
"$E" --level-check assets/levels                                    # validate every level
"$E" --dump-config "$SCRATCH/cfg"                                   # live tuning -> JSON
```

- The sim-test schema, metrics and ops are in `docs/AGENT_BRIEF.md`. Steps can also run gym commands: `{"tick": 30, "type": "cmd", "cmd": "spawn virus 500"}`. `--sim-test` ignores `--report`, so read the report from stdout.
- Some `tests/scripts/*.json` are stale and fail on master, `smoke.json` included. Check the baseline before calling a failure a regression.
- `chaff_spawned` tracks survival (viruses bloom into bits). Compare `chaff_leaked_total` instead.
- The flow-field rebake has a wall-clock budget, so tower+tick results can flip from run to run. Run it several times before concluding anything.

## 5. Gym commands (for `--exec`, the console and sim-tests)

Full reference: `docs/GYM.md`, or run `help` / `help <cmd>`. Families: `virus`, `bacteria`, `parasite`, `all`. Targets: `at x,y`, `at <spawn_point_id>`, `at objective`, `at center`, `at cursor`.

```
spawn <family|all> <count> [at …] [radius r]   flood [n]        kill [family|all]
tower <type|all|list> [at …] [tier 1-3]        upgrade [all]    sell [all]   fire
cast <complement|histamine|fever|clot> [at …]  ready            atp <n|+n>
integrity <0-100>                              (organ integrity: the critical HUD state)
wave [start|next|status|<i>]                   field <r> <rate> [dur] [at …]
vfx <event|all|list> [at …]                    time <scale>     step [ticks]
cam <x,y|spawn_pt|objective|fit> [h]              invuln [on|off]  overlay <debug|threat> [on|off]
stats   spawn_points   restart   level <name>  autoplay [on|off] [profile]
config get|set|list <dotted.path> [v]          (interactive / sim-test only)
ui dump | click <path> | hover <path> | pointer <x> <y> | select <n> | cancel
ui level <n>                                   (level select: pick campaign level n)
ui screen none|menu|levels|pause|victory|defeat   (--screenshot --ui only)
                                               (the rest: interactive and --screenshot --ui)
```

`ui` drives the HUD by widget path (`ui dump` lists them): `hud/dock/<tower>`
(`neutrophil`, `cytotoxic_t`, `macrophage`, `goblet_cell`, `fibroblast`),
`hud/abilities/<cascade|histamine|fever|clot>/cell/button`,
`hud/controls/<pause|speed1|speed2|menu>`, `hud/prep/send`, `inspect/popup/sell`.
`ui pointer x y` (logical px in the 1920x1080 frame, `--screenshot` only) places
the pointer the placement ghost and aim reticle follow; `ui select n` opens the
popup for the n-th placed tower. The five canvas HUD states on campaign_01:

```bash
L=assets/levels/campaign_01_first_bend.json
T="tower neutrophil at 30,110; tower cytotoxic_t at 84,80; tower macrophage at 84,50; tower goblet_cell at 130,36"
S="$E --screenshot $L --ui --width 1920 --height 1080"
$S --tick 60  --exec "$T" --out "$SCRATCH/prep.png"                                   # prep
$S --tick 400 --exec "$T; wave start" --out "$SCRATCH/wave.png"                       # wave on
$S --tick 30  --exec "$T; ui click hud/dock/cytotoxic_t; ui pointer 1000 560" --out "$SCRATCH/placing.png"
$S --tick 400 --exec "$T; wave start; ui select 2" --out "$SCRATCH/inspect.png"
$S --tick 460 --exec "$T; wave start; integrity 18; ui click hud/abilities/histamine/cell/button; ui pointer 1100 560" --out "$SCRATCH/critical.png"
```

stderr prints `--ui: N draw calls, N shapes, N vertices`; the whole HUD is one
draw call unless a stencil clip or a layer is in use.

The out-of-match screens (src/ui/front) show over the same capture with
`ui screen <name>`. The results use a sample first-clear payout; the level
map treats every campaign level before `$L` as cleared, so pick `$L` to set
how far along the campaign looks. Their widget paths: `menu/play`,
`menu/quit`, `levels/cell<n>`, `levels/play`, `levels/back`,
`<victory|defeat>/panel/buttons/<tree|next|replay|retry|levels>`,
`pause/panel/buttons/<resume|restart|menu>`.

```bash
L=assets/levels/campaign_04_twin_channels.json
S="$E --screenshot $L --ui --width 1920 --height 1080"
$S --tick 10  --exec "ui screen menu" --out "$SCRATCH/menu.png"
$S --tick 10  --exec "ui screen levels" --out "$SCRATCH/levels.png"      # 1-3 cleared, 4 next
$S --tick 900 --exec "ui screen victory" --out "$SCRATCH/victory.png"    # over the level
$S --tick 900 --exec "ui screen defeat" --out "$SCRATCH/defeat.png"
```

## 6. Interactive play

```bash
"$E" --level assets/levels/gym.json --sandbox
"$E" --level assets/levels/campaign_01_first_bend.json --save "$SCRATCH/save.json" --exec "autoplay on; time 8"
"$E" --editor assets/levels/<f>.json
```

Launch windowed runs with `run_in_background`. They don't exit on their own, so stop **only the process you started** (for example the one whose path contains `horde-verify-build`), never the user's game.

- **Never touch the user's real save** (`%APPDATA%/IMMUNE/IMMUNE/save.json`). Pass `--save <scratch>` or `--sandbox`.
- `--exec` also runs in interactive play, right after the level loads.
- Keys: **F1** debug overlay · **TAB** threat overlay · **` / F2** gym panel (opens by itself on `gym.json`) · **F4** level editor (from a level, or a blank one from the title) · **SPACE** pause (during prep: send the wave now) · **, / .** speed · **1–5** arm a tower in dock order (Neutrophil, Cytotoxic T, Macrophage, Goblet Cell, Fibroblast) · **Q W E R** abilities · **Esc** cancel the armed cursor / close the tower popup, else the pause menu; in the front end it walks back levels → Strengthen Immunity → title · **F12** screenshot.
- To force a loss quickly on campaign_01: `--exec "spawn all 400 at 200,31 radius 4; time 4"`.

You can't see or click the live window. For proof, use a headless `--screenshot` of the same state, the log, or `stats`.

## 7. Unit tests, including GL screenshot tests

```bash
"$LOCALAPPDATA/horde-build/windows-release/bin/immune_tests.exe" "[render]"
"$LOCALAPPDATA/horde-build/windows-release/bin/immune_tests.exe" "visual verification*"
```

- Run them from the repo root. Launched from the build dir they report `draw_calls=0` and fail falsely.
- `test_render_screenshots.cpp`, `test_render_vfx.cpp`, `test_chaff_death_vfx.cpp` and others write PNGs to `%TEMP%` (`render_verify_*.png`). Read them back.
- Handy tags: `[render] [vfx] [sim] [gym] [towers] [swarm] [determinism] [level] [meta]`. Use `--order decl --rng-seed 1` for reproducible ordering.
- A GL test can leave `immune_tests.exe` running and block the next link. Kill **that** process, never `immune.exe`.
- Four baseline failures exist on master: config equality, squad cohort, autoplay attribution and hit-flash colour. Don't report them as yours.

## 8. Reporting visual work

Say which exe, level, tick and exec line you used, include the JSON counts that matter, and **describe the PNG you read**. Send key captures with `SendUserFile`. If stderr showed a warning, a failed exec or a shader compile error, say so.
