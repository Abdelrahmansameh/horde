# The Gym — a level and a console for testing everything

Two pieces, meant to be used together:

- **`assets/levels/gym.json`** — a sandbox level built to exercise every system
  at once rather than to be fun.
- **The gym panel** (`ui/GymPanel.h`) — an ImGui window of controls for making
  any of it happen right now, instead of waiting for a wave, saving up ATP, or
  building six towers by hand. It opens by itself on the gym level.
- **The command language underneath it** (`game/gym/GymCommands.h`) — every
  button in the panel builds a command string and runs it, and the log at the
  bottom echoes the line it ran, so the panel is a typist for the language
  rather than a second API that can drift from it.

The panel opens automatically on the gym level. Toggle it anywhere with
**`` ` ``** (backtick) or **F2**; Escape closes it. (F1 stays with the debug
overlay.)

## The window

```
TARGET  (o) cursor  ( ) portal [p_lymph v]  ( ) objective  ( ) point [x][y]
[ Horde ][ Defense ][ Waves ][ World ]
   ...controls for the selected tab...
------------------------------------------------------------------
> spawn all 300 at cursor
spawned 1800 agents (300 per family) at (30.0, 75.0) r=10.0
[ input line ................................ ] [Run] [help] [Clear]
```

The **target bar** at the top aims everything below it: pick cursor, a portal by
id, the objective, or a literal point once, and every spawn / cast / field /
vfx / camera control in every tab uses it.

| Tab | Holds |
|---|---|
| **Horde** | family + count spawn, spawn-every-family, flood every portal, kill by family or all, elite dropdown and spawn-every-elite, plus a live per-family census |
| **Defense** | tower type + tier place, place-one-of-each, upgrade/sell/fire, ability buttons that grey out on cooldown and show the seconds left, ATP set/add |
| **Waves** | director status, start-now, next, and the whole wave table with agent counts and modifier tags — click *Jump* on any row to run that wave immediately |
| **World** | time scale and step, **infinite objective integrity** (on by default here), camera, overlay toggles, a raw damage field with radius/rate/duration sliders, combat-event firing, and stats/portals/restart |

Hovering any button shows the command it runs. The input line takes the full
language for anything the widgets do not cover.

---

## Why this exists

Every subsystem here is already testable headlessly (`--bench`, `--sim-test`,
`--screenshot`) and reachable from the HUD during play. What was missing was the
middle: standing inside a running level and saying *"now show me the swarm
modifier / every elite / the Tesla arc / what 4,000 agents look like in one
lane."* Waiting out a prep timer to see wave 5 is not testing, it is
bookkeeping.

The command layer is a pure function of `(GymContext, line)` and owns no UI, so
the same command string runs from three places:

| Where | How |
|---|---|
| The in-game panel | its widgets, or its input line |
| A `--sim-test` script | `{"tick": 30, "type": "cmd", "cmd": "spawn virus 500"}` |
| A screenshot / CI | `immune --screenshot <level> --exec "spawn all 300; tower all"` |

Commands go through the same public APIs `app/` uses for player intents — there
is no back door into `SimWorld`. The one deliberate difference is that build
costs and placement gates are bypassed (`tower` places for free), because paying
100 ATP to look at a muzzle flash is bookkeeping too.

Commands run **between ticks**, never inside one, so nothing here weakens the
determinism contract: a fixed script of commands at fixed ticks replays
bit-identically.

---

## The level

Five lanes, one per `VesselType`, each with its own portal and its own spawn
chamber, all converging on a single organ at the right-hand side.

| Lane | Type | Portal | Character |
|---|---|---|---|
| `lymph_lane` | lymphatic | `p_lymph` | the wide middle highway — point big spawns here |
| `artery_lane` | artery | `p_artery` | long diagonal approach from the bottom-left |
| `vein_lane` | vein | `p_vein` | mirror of the artery, from the top-left |
| `nerve_lane` | nerve_adjacent | `p_nerve` | the narrow one, for clearance/chokepoint work |
| `mucosa_lane` | mucosal_fold | `p_mucosa` | fat and short, from the bottom |

Each lane is authored as **two vessels sharing one `lane_id`**: a wide spawn
chamber around the portal, then the trunk. The chamber is not decoration —
`spawn_burst()` sizes its spawn disc to the requested count, so a portal
authored on a normal 12-wide lane loses the outer half of any large burst off
the lumen. A gym whose `spawn 600` quietly yields 430 is a gym that lies.

The wave table is authored, and each wave isolates one thing:

| # | Name | Shows |
|---|---|---|
| 1 | `gym_1_one_of_each_family` | all six families, one per lane, side by side |
| 2 | `gym_2_every_elite` | all five elite archetypes at once |
| 3 | `gym_3_allergen_modifier` | the allergen curveball |
| 4 | `gym_4_fever_modifier` | the fever curveball |
| 5 | `gym_5_swarm_modifier` | the swarm curveball |
| 6 | `gym_6_all_lanes_at_once` | five portals firing together (lane threat overlay) |
| 7 | `gym_7_max_horde` | ~7,200 agents converging (the perf gate) |

The first prep window is two minutes, on purpose: a gym must not start shooting
at you while you are still setting up the thing you came to look at. `wave start`
skips any prep instantly; `wave 5` jumps straight to wave 5.

---

## Commands

The panel's buttons cover most of these. `help` lists them all and
`help <name>` explains one. Anywhere a command takes a target, these all work:

```
at 120,75        at 120 75        at p_lymph        at cursor
at objective     at center
```

| Command | What it does |
|---|---|
| `spawn <family\|all> <count> [at …] [radius <r>]` | Spawn chaff. Oversized counts stream in over the next ticks rather than spilling off the lane. |
| `elite <name\|id\|all\|list> [at …]` | Spawn a named elite. |
| `flood [count-per-portal]` | Every family out of every portal. The stress button. |
| `kill [family\|all]` | Flag chaff for removal, with real kill accounting. |
| `tower <type\|all\|list> [at …] [tier 1-3]` | Place towers for free; `all` spreads one of each. |
| `upgrade [all]` / `sell [all]` | Tier up, or refund. |
| `fire` | Trigger every placed tower's active ability. |
| `cast <complement\|histamine\|fever> [at …]` | Cast a player ability. |
| `ready` | Clear every ability cooldown. |
| `atp <amount\|+amount>` | Set or add ATP. |
| `wave [start\|next\|status\|<index>]` | Skip prep, jump waves, read the director. |
| `field <radius> <kill_rate> [duration] [at …]` | Submit a raw damage field — aggregate damage in isolation. |
| `vfx <event\|all\|list> [at …]` | Raise combat events so the particle layer draws them. |
| `time <scale>` / `step [ticks]` | Time scale (0 pauses); `step` advances while paused. |
| `cam <x,y\|portal\|objective\|fit> [height]` | Move the camera. |
| `invuln [on\|off]` | Hold the objective's integrity so a leak cannot end the run. Aliases: `godmode`. |
| `overlay <debug\|threat> [on\|off]` | Toggle a HUD overlay. |
| `stats` / `portals` | Print sim/economy/wave state, or the level's portals. |
| `level <name>` / `restart` | Load another level, or reload this one. |

Several commands can share a line, separated by `;`. Lines starting with `#` are
comments, so a session can be pasted in whole.

### A few sessions worth stealing

```
spawn all 200; tower all; time 4          # every family vs every tower, fast-forward
wave 3; wave start                        # jump straight to the allergen wave
elite all at p_nerve; cam p_nerve 60      # every elite, framed
vfx all at cursor                         # one of each combat effect, side by side
field 25 80 2 at cursor                   # aggregate damage with no tower involved
flood 1200; overlay threat on             # five-lane pressure, threat readout on
```

---

## Notes and limits

- **Oversized spawns stream.** `spawn virus 4000` places what fits on the tissue
  now and hands the rest to a queue that releases another burst each tick as the
  previous one flows away — the same thing the wave director does over a wave's
  duration. `stats` shows what is still queued.
- **`step` advances the sim, not the session.** It ticks `SimWorld` only; the
  wave director and the economy do not advance. That is what you want when
  inspecting one frame of movement, and surprising if you expected `time 1`.
- **`wave <n>` drops the earlier waves.** The director's index is only movable
  through `set_waves()`, so jumping re-seats it on the tail of the table.
- **Infinite integrity is ON for the gym level and off everywhere else.** The
  gym level enables it at load; other levels get the real loss condition, and
  every headless path (`--sim-test`, `--screenshot`, unit tests) defaults it off
  so a script asserting that integrity depletes still observes that. Turn it off
  with the World tab's checkbox or `invuln off` when the loss path *is* what you
  are testing.
- **It holds integrity, it does not stop leaks.** An agent that reaches the
  organ still despawns and still counts in `chaff_leaked_total` — only the
  consequence is suspended. Suppressing the despawn instead would pile the horde
  onto the objective and change the very behaviour under test.
- **The panel is not a player path.** It bypasses `ui::Intent` deliberately:
  intents are the auditable record of what the *player* did, and a debug panel
  masquerading as one would make that record lie.
- **It opens itself on the gym level only.** Any other level leaves it as you
  left it, so it never appears uninvited during real play.

## Tuning from the console

Every gameplay number lives in `assets/config/*.json` and is addressable by a
dotted path:

```
config get towers.macrophage.3.stats.damage
config set towers.macrophage.3.stats.damage 200
config set enemies.families.virus.visual.silhouette 3.0
config set enemies.elites.tumor_mass.stats.max_health 20000
config list enemies.families.virus
config reload            # re-read the files from disk
config dump              # write the live values back out
```

`config set` writes through to the same bytes the JSON loader fills and then
re-applies every affected system, so a value changed here and a value changed in
the file behave identically. `config dump` is how an experiment that worked gets
kept: retune in the console until it feels right, then dump and commit.

Editing a config file while the game runs picks the change up within half a
second, the same way a shader edit does. A file caught mid-save keeps the last
good config and logs the parse error rather than taking the game down.

`reload` and `dump` are unavailable in `--sim-test`, because re-reading the
files mid-run would change a determinism input. `get`, `set` and `list` work
there, and every sim-test report carries a `config_hash` so a run records which
tuning produced it.
