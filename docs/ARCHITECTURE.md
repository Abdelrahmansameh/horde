# IMMUNE — Architecture

This document is the contract between waves. Every interface header listed here
is **frozen**: Waves 1–4 implement *against* these headers, in parallel, and an
agent that needs a signature changed requests it from the orchestrator rather
than editing another agent's header.

`DESIGN.md` is the authority on *what* the game is. This document is the
authority on *how the code is shaped*, and every section states the rationale so
a later agent can tell an intentional constraint from an accident.

---

## 0. The three forces that shape everything

1. **10,000 agents at 60 FPS.** DESIGN.md §8.2 makes data-oriented design in the
   hot path a hard constraint, not a preference. The chaff store is SoA, damage
   is aggregate, movement is field-sampled, and rendering is instanced. Any
   design that introduces a per-agent object, a per-agent virtual call, or a
   per-agent heap allocation is wrong by construction.
2. **Agents build this game, not humans at a screen.** The executable ships three
   headless modes from Wave 0 (`--bench`, `--sim-test`, `--screenshot`). They are
   the only way a sub-agent can prove its work. Their output formats are public
   API.
3. **Determinism.** Fixed 60 Hz tick, explicit seeded PRNG, no wall-clock reads
   inside the sim. Determinism is not a nicety here — it is the precondition for
   (2). If the sim is not reproducible, `--sim-test` and `--screenshot` are
   worthless.

---

## 1. Module map and ownership

One CMake library target per module. Directory ownership is exclusive.

| Target | Directory | Owner wave | Depends on |
|---|---|---|---|
| `immune_core` | `src/core/` | 0 | — |
| `immune_platform` | `src/platform/` | 0 | core, SDL2, glad |
| `immune_sim` | `src/sim/` | 1A/1B/1D, 2A | core, EnTT |
| `immune_render` | `src/render/` | 1C, 3D | core, platform, sim |
| `immune_game` | `src/game/` | 2B/2C/2D, 3A, 4A/4B | core, sim, render, json |
| `immune_ui` | `src/ui/` | 3B | core, platform, render, game, imgui |
| `immune_audio` | `src/audio/` | 3C | core, SDL2 |
| `immune_app` | `src/app/` | 0 | everything |

Dependencies point one way only: `core → platform → sim → render → game → ui →
app`. `sim` never includes `render`; `render` never mutates `sim`.

---

## 2. `core` — foundations

### `Types.h`
Scalar aliases, `Vec2/3/4` (glm), `Rect`, `EntityId`, and the two frozen enums
`PathogenFamily` and `TowerType`. **`PathogenFamily`'s enum order is the
renderer's draw-batch order** — reordering it silently changes rendering.

`kTicksPerSecond = 60`. Note the deliberate pair:
- `kFixedDt` (f32) — what sim math uses.
- `kFixedDtSeconds` (f64) — what the clock accumulator uses.

Accumulating in f32 loses one tick per simulated second to rounding. This bit us
during Wave 0 and is now covered by a test.

### `Rng.h` — PCG32
Trivially copyable, 16 bytes of state. `fork(stream_id)` produces an independent
deterministic sub-stream **without advancing the parent**, which is how parallel
work stays reproducible: a `parallel_for` body forks by range index rather than
sharing a generator.

> **Rule:** sim code never calls `rand()`, `std::random_device`, or a global
> generator. It takes an `Rng&`, sourced from `SimWorld::rng()`.

### `Clock.h`
`WallClock` / `ScopedTimer` for profiling (non-deterministic, never in sim
logic). `FixedClock` is the sim/render decoupler:

```
clock.begin_frame();
while (clock.consume_tick()) sim.tick();   // fixed 60 Hz
renderer.draw(clock.alpha());              // variable rate
```

The accumulator is stored **in tick units, not seconds**, so consuming a tick is
an exact `-= 1.0`. `max_frame_seconds` (default 0.25 s) caps catch-up so a
debugger pause cannot trigger a death spiral of ticks.

### `Arena.h`
Bump allocator. Reset is O(1); it never runs destructors, so `T` must be
trivially destructible (static_assert enforced). Per-tick scratch memory comes
from a frame arena so the hot path performs zero `malloc`s. `allocate()` returns
`nullptr` on overflow rather than growing — a hot-path caller must size the arena
so this cannot happen. Alignment is applied to the **absolute address**, not the
offset, because `malloc` only guarantees `max_align_t`.

### `JobSystem.h`
Deliberately narrow: `parallel_for` over index ranges plus fire-and-forget
`dispatch` + `wait_idle`. No task graph, no dependencies, no cross-frame stealing
— every parallel workload in this game is "split N items across K workers".

The calling thread executes range 0, so `thread_count() == worker_count() + 1`.
`JobSystem(0)` is **explicitly serial**; auto-sizing uses the `kAutoWorkers`
sentinel. That distinction matters: headless modes pass 0 to get a reproducible
serial run.

> **Determinism rule for `parallel_for` bodies:** do not accumulate into shared
> floats (FP addition is not associative), and do not draw from a shared `Rng`.
> Fork per range index.

### `Profiler.h`
The `--bench` JSON schema lives here. The five canonical keys — `chaff_update`,
`spatial_hash`, `ecs_tick`, `render_submit`, `frame_total` — map directly onto
DESIGN.md §8.6's budget lines. Keys are always emitted, zero-filled if unsampled,
and in a fixed order, so the document is byte-stable and diffable across commits.
**Adding a key is a contract change.**

---

## 3. `platform`

`Window` (SDL2 + GL 4.5 core + glad), `InputState`, `FileIO`.

- `Window::create` requests a 4.5 core-profile context and loads glad. It never
  throws; check `ok()` / `error()`.
- `create_headless_gl()` makes a hidden-window GL context. This is what
  `--screenshot` uses: real GL, no visible window, no compositor.
- `InputState` maps SDL scancodes onto an abstract `Action` enum. Game code never
  mentions physical keys. Input is a **render-rate** concern; the sim never reads
  it (that would break determinism). Player actions reach the sim as explicit
  commands, which is also how `--sim-test` scripts drive the game.
- `FileIO::asset_root()` walks up from the executable looking for `assets/`,
  overridable via `$IMMUNE_ASSET_ROOT`. No exceptions cross this boundary.

---

## 4. `sim` — the deterministic simulation

### 4.1 `SimWorld` — one object, one canonical tick order

Everything a tick touches hangs off `SimWorld`, and the tick order is fixed:

```
1. spatial hash rebuild        [prof: spatial_hash]
1b. squad centroids + anchors  [prof: squad_update]
2. chaff update                [prof: chaff_update]
3. ECS systems                 [prof: ecs_tick]
4. damage fields apply
5. chaff compact + kill accounting
6. flow-field incremental rebake pump (budgeted)
7. tick counter advance
```

Changing this order is a contract change. `state_hash()` is an FNV-1a over the
chaff streams plus counters; `--sim-test` asserts on it to catch determinism
regressions that don't show up in aggregate counts.

`SimSnapshot` also carries per-family lifetime tallies —
`chaff_{spawned,killed,leaked,despawned}_by_family`. These were added for the
balance harness (`docs/BALANCE.md`) and are an **additive** change: every
pre-existing field keeps its meaning, including `chaff_killed_total`, which has
always counted every retirement rather than only damage kills. The split is made
where the cause is actually known — `ChaffSystem` knows a leak from an
out-of-bounds despawn, and `compact()` knows only that something retired — so
`killed = retired − leaked − out_of_bounds`, per family, per tick.

### 4.2 `sim/chaff` — SoA storage (**the most important contract in the project**)

There is no `ChaffAgent` class and there must never be one. Ten thousand agents
are parallel flat arrays:

```
pos_x, pos_y      f32   position, split per axis
vel_x, vel_y      f32   velocity, split per axis
family            u8    PathogenFamily; also the render batch key
density           f32   HP expressed as a density contribution
flags             u8    chaff_flags bitset
generation        u32   backs ChaffHandle across compaction
squad_id          u16   which squad, or kNoSquad (see 4.7)
```

**Why SoA, in order of weight:**

1. The per-tick movement kernel touches only pos/vel/flags. In SoA those streams
   are contiguous, so every cache line fetched is 100% useful. An AoS struct
   would drag family, density, and padding through L1 for nothing — that is the
   difference between hitting and missing the 4 ms budget.
2. Contiguous f32 streams auto-vectorize under MSVC, and hand-SIMD later is a
   drop-in because the layout already matches.
3. The renderer memcpys spans of these arrays into a mapped GPU instance buffer.
   No gather, no per-agent transform construction.
4. The spatial hash and damage fields both want to stream a dense `f32` position
   array, which is exactly what they get.

Position is split into `pos_x`/`pos_y` rather than a `Vec2` array on purpose:
separation and damage-field tests early-out on a single axis, and SoA-of-scalars
is what SIMD wants.

**Identity.** An index is not stable — `compact()` swap-removes dead agents.
Anything that must name a specific agent across ticks uses `ChaffHandle`
(index + generation). In practice almost nothing does; chaff is fought as a mass.
`squad_id` has to be carried across that swap for the same reason `generation`
is — it is the one other per-agent fact that outlives a slot.

### 4.3 `sim/spatial` — uniform grid

The only spatial queries the game makes are short-range over roughly uniform 2D
density: separation neighbours, "which chaff overlap this field", and tower
targeting. A uniform grid beats a tree: O(n) build in two linear passes, O(1)
arithmetic lookup, two flat integer arrays, no pointers, no per-frame allocation.

Layout is CSR-style counting sort: `cell_start` has `cell_count+1` entries, and
cell `c` owns `indices[cell_start[c] .. cell_start[c+1])`. Explicitly **not** a
per-cell `std::vector` — that would allocate thousands of times a frame.

Cell size should be ~2× the separation radius so a 3×3 block covers every
possible separation partner. Queries are conservative at cell granularity; the
caller does the exact distance test. `occupancy()` is exposed because the
renderer's density-LOD pass and the Mast Cell trigger both need it and neither
should pay for a second counting pass.

The hash stores **indices into the chaff SoA**, so `compact()` invalidates it.
Rebuild before use; never cache indices across ticks.

### 4.4 `sim/flowfield` — three-stage bake

```
TissueMask  ──►  DistanceField  ──►  FlowField
(walkable +      (clearance,          (cost-to-goal sweep,
 per-cell cost)   placement rules)     then negative gradient)
```

10,000 agents cannot each run A*. Baking a vector field once turns an agent's
entire pathfinding cost into one bilinear sample. Vessels get organic width and
branching for free because the field comes from a rasterized mask, not a corridor
graph.

**Incremental rebake is the load-bearing requirement.** Placing a tower must
reroute the horde visibly and immediately, but a full-level bake is far too slow
for a frame. `mark_dirty(rect)` records the region; `rebake_pending()` re-solves
it correctness-first (tests, `--sim-test`); `pump_rebake(budget_ms)` is the
gameplay path, spending a fixed millisecond budget per frame.

The dirty region is expanded by `rebake_margin_cells` and seeded from the
*existing* boundary costs. This is correct as long as the true shortest path from
any changed cell leaves and re-enters the region at most once — the margin is
what buys that. Agents flowing on a one-or-two-tick stale field read as momentum,
not as a bug.

`sample()` returns `(0,0)` outside the field or in an unreachable pocket. Callers
must treat zero as **"no guidance"**, not as "standing still is fine".

### 4.5 `sim/damage` — aggregate damage

Chaff is never hit individually. A tower publishes a `DamageField`: a region plus
a `kill_rate`. Each tick the field asks the spatial hash which cells it overlaps
and thins the chaff inside.

This is simultaneously the performance answer (cost scales with fields and the
cells they cover, not with 10,000 × towers pair tests) and the art direction
(mass removed at a field boundary *is* the "edge erosion" language of DESIGN.md
§7). The visual is the mechanic.

DESIGN.md §10 leaves the exact formula open, so both are implemented behind one
switch and the feel pass picks by playing:

- `DensityThinning` (default) — deterministic; subtracts `kill_rate * dt` from
  every overlapped agent's density. Smooth, predictable, reads as dissolving.
- `ProbabilisticRemoval` — each overlapped agent rolls to be removed whole.
  Grainier, cheaper per agent, order-sensitive, so it draws from a per-field
  forked stream to stay deterministic.

Field shape supports Circle / Rect / Cone / Chain (the Complement Cascade
resolves to a sequence of circle links at evaluation time). `family_mask` lets NK
Cells hit only hidden targets and Cytotoxic T favour elites. `friendly_fire`
marks fields that damage the player's own units/objective.

Removed density is attributed to the owning tower and to the economy via
`DamageStats`. **Nothing may infer kills by diffing agent counts** — under
aggregate damage, only the damage system knows when a density threshold was
crossed.

**Renderers must read `rendered_fields()`, not `fields()`.** `fields()` is the
submission buffer, and `clear_transient()` runs at the *end* of `SimWorld::tick`,
dropping every persistent field on the grounds that its owner re-submits next
tick. That is right for the sim and wrong for the screen: the Cryo cone and the NK
rotor are both persistent, so anything drawing after the tick sees the
permanently-on AoEs as permanently absent.
`rendered_fields()` is the snapshot taken just before that cull.

### 4.6 `sim/ecs` — the small half

EnTT registry for named agents (≤200) and towers. Chaff never enters the
registry; adding a chaff entity violates §8.2.

The wrapper exists for exactly one reason: **explicit, stable system ordering**.
Systems declare a `SystemPhase` (PreUpdate → AI → Movement → Combat →
PostUpdate) and a sort key; the scheduler runs them in that order, always, on
every machine. Registration order never leaks into behaviour. The raw registry
stays reachable — this is a convenience layer, not an abstraction wall.

Components are plain trivially-copyable structs with no methods and no virtuals
so EnTT keeps them in dense pools.

---

### 4.7 `sim/squad` — squads, for readability

Every agent samples the same level-wide flow field, so the whole horde converges
on one shortest path and arrives as a single undifferentiated mass. That mass has
the fluid feel DESIGN.md §4.2 asks for, but at 10k agents there is no structure
in it to read. `SquadRegistry` partitions the horde into groups of ~60, each
following its own path across the lane.

Paths are **optional per-level data** (`squad_paths` in the level JSON). A lane
that authors none gets a spread derived from its vessel centerline at load, so
the feature required no edits to any shipped level. **How many** paths a lane
gets follows its narrowest lumen width, not a fixed count: paths are placed
`kMinPathSpacing` apart across the usable band, so a wide trunk carries three or
four squad columns and a capillary carries one. A fixed count is wrong at every
width but one — three paths in a 46-wide lane sit closer together than a squad
is across, and the horde reads as one mass however well the steering works.

Three forces, and only the third touches the hot neighbour loop:

- **Anchor** — a point sliding along the path, *leashed* to the squad's own
  centroid: monotonic, rate-limited, and always ~`anchor_lookahead` ahead. A
  squad jammed behind a tower keeps its anchor close instead of letting it sail
  off and drag stragglers into a wall. New squads on a path already occupied are
  pushed forward past it by `spawn_spacing`, so same-path squads form a column
  rather than spawning inside one another.
- **Cohesion** — a velocity impulse decomposed against the flow direction.
  *Across* the flow it steers toward the anchor (which line of the lane this
  squad rides); *along* the flow it pulls toward the squad's own centre of mass
  (so the squad does not string out into a ribbon). Splitting the axes is what
  lets the flow field keep all of its forward authority: the cross-flow term can
  never push an agent into a wall the field is routing around, and the
  along-flow term only speeds up stragglers and reins in leaders. Both ramp from
  **exactly zero** at the squad radius, which is what keeps a packed interior
  running the identical pre-squad kernel — the fluid feel is not traded for the
  grouping.
- **Repulsion** — in `gather_neighbours`, a neighbour from a *different* squad
  gets a wider separation radius and a stronger push, and is excluded from
  alignment. One `u16` compare per neighbour, short-circuited away entirely for
  ungrouped agents.

Two numbers are load-bearing and are *derived*, not chosen. `lateral_push` is a
velocity impulse in the same units as `separation_strength`, because the flow
term contributes ~0.33/tick while separation contributes up to ~16 — a cohesion
term expressed as an acceleration is two orders of magnitude quieter than the
crowd it is steering and simply loses. And `squad_radius_scale` is 0.75 because
that is exactly the radius `spawn_burst`'s phyllotaxis packs `n` agents into,
i.e. the radius a squad *physically occupies*; below it the target is smaller
than the bodies it describes, every member is permanently "outside", and the
promised free interior never exists.

Determinism: centroids are accumulated in a **serial** pass in index order (float
addition is not associative, so a parallel reduction would make steering depend
on thread count), and `SquadRegistry::state_hash()` folds into
`SimWorld::state_hash()` so `--sim-test` actually covers the layer.

Measured at 10k agents in 167 squads: `chaff_update` 1.4 ms (budget 4 ms),
`squad_update` 0.09 ms.

**Level geometry.** Squads need room: one is ~16 world units across once the
crowd relaxes, so a lane must be roughly 50+ wide to hold two side by side.
Every shipped level was widened to ~68 units of lumen for this, with world
bounds and control points scaled to keep lanes from merging (and
`default_test_level()` matched, so the headless modes stay representative).
Lanes were 5-12 wide before, i.e. narrower than a single squad.

**Damage.** `apply_density_loss()` is the *only* way chaff takes damage. There is
no per-unit hit path. Density reaching zero sets `kPendingKill`; removal happens
in the once-per-tick `compact()`, so indices are stable within a tick.

**Capacity.** All streams are reserved once at level load. Spawning past capacity
fails and is reported; it never reallocates mid-tick.

**Invariants** (asserted in debug, checked by tests):
- I1 every index in `[0, count)` has `kAlive`
- I2 all streams have equal size, `>= count`
- I3 `count <= capacity` always; `spawn()` never grows an array
- I4 `density[i] > 0` for every live agent after `compact()`

`ChaffSystem` is the movement kernel. Per agent per tick, the entire "AI" is:

```
v += flow.sample(p) * speed          // one bilinear field fetch
v += separation(p) * k               // 3x3 spatial-hash cell scan
v  = clamp_length(v, max_speed)
p += v * dt
```

Family behaviour (replication, drift, hiding) is a variation on those
four lines gated by a flag bit — never a subclass.

## 5. `render`

Two facts drive the entire renderer interface:

**1. One instanced draw call per family.** The CPU never builds per-agent
geometry or per-agent draw calls. `submit_chaff` walks the chaff SoA once and
memcpys contiguous spans into a persistently-mapped instance buffer, one range
per `PathogenFamily`. Six draw calls cover ten thousand agents. This is why the
`PathogenFamily` enum order is frozen.

`ChaffInstance` is 32 bytes — half a cache line each, 320 KB/frame at 10k. Its
layout is mirrored exactly in `assets/shaders/chaff.vert`; changing one without
the other is a contract break.

**2. LOD by density, not distance.** The camera is fixed-ish, so distance LOD is
meaningless (DESIGN.md §8.5). Instead, where cell occupancy exceeds
`lod_blob_threshold`, agents are not drawn as instances at all — they accumulate
into a low-resolution density texture drawn by a shader-driven blob pass. A
crossfade band up to `lod_blob_full` makes the switch invisible: an agent near the
boundary draws at partial instance alpha *and* contributes partial blob density,
conserving apparent mass. Without this a floodplain level would try to draw
10,000 overlapping sprites into a few hundred pixels.

`Camera` is a fixed tilted-topdown (15–25°, no rotation). Because tilt is fixed,
world↔screen is affine and `screen_to_world` is exact — which is precisely what
grid-free continuous tower placement needs. World Y is foreshortened by
`cos(tilt)`; height above the plane is a constant vertical offset plus a drop
shadow. No 3D geometry anywhere.

`ShaderManager` hot-reloads from `assets/shaders/*.glsl`. Since the project ships
zero binary assets, shader source is the *only* on-disk art, so hot reload is the
entire art iteration loop. A failed recompile logs the GLSL error and **keeps the
previous working program**, so a typo never blanks the screen.

**Tower art lives in two shaders, and they have to agree.** A tower's *body* is a
procedural SDF in `entity.frag`, selected by `shape_id = 16 + TowerType` (16
GUNNER, 17 MORTAR, 18 CRYO, 19 TESLA, 20 HYDRO, 21 BLADE); its *attack* is a
`DamageField` drawn by `field.frag` — except the Goblet Cell, whose attack is
simulated fluid drawn by its own pass (see `sim/fluid/Fluid.h`). Three rules
hold body and attack together:

- **Identity hue is one colour per tower, everywhere.** `palette_for()` in
  `vfx/Particles.cpp` is the source; the body tints toward it, the particles use
  it, and `submit_fields` tints the AoE with it. Hue is the only channel that
  survives a glance at 60 fps (DESIGN.md §9.3), so a tower must never say two
  different things in two places.
- **Silhouette is the fallback channel, so no two bodies share one.** Five of the
  six are amoeboid blobs; the Interferon is deliberately the hard-edged crystal,
  the Goblet Cell the only vessel-shaped one, and the NK Cell the only rotor.
  Each also carries a *directional* feature aligned to local +x — the
  Macrophage's maw, the Cytotoxic T's electrode, the Goblet Cell's open apical
  mouth — which `entity.vert` has already rotated onto the aim.
- **Tier is spent on something countable.** `EntityInstance::shape_param` carries
  the raw tier, and each body turns it into phagosomes / crystal reach /
  microvilli / mucin granules / blades, so an upgrade shows in the silhouette
  rather than only in the stat panel.

The one field shape two towers share is Circle, split by lifetime: persistent is
the NK Cell's rotor disc, timed is a Macrophage shell or a Histamine nova.

`Screenshot` writes PNGs via `stb_image_write` and handles the GL bottom-up →
PNG top-down flip. This is the project's primary visual verification channel.

---

## 6. `game`

- **`towers/`** — grid-free continuous placement validated against the distance
  field (clearance) and reachability (a placement that walls off every lane is
  rejected, not allowed-then-exploited). A successful placement edits the tissue
  mask and marks the flow field dirty over the footprint only. Targeting goes
  through the spatial hash; a tower asks the grid for cells in range and never
  iterates agents. Anti-chaff towers don't target at all — they publish a
  `DamageField`.
- **`enemies/`** — the DESIGN.md §6 readability rule (colour = family, silhouette
  size = threat tier, tempo = speed tier) is enforced *structurally*: those are
  three separate fields sourced from tables, so no archetype can quietly break the
  visual language. `render::family_color()` is the single source of truth for
  pathogen colour, shared by UI, VFX, and instance tints.
- **`level/`** — levels are **splines with per-point width in JSON**, not painted
  masks. Agents can author and diff text; nobody can author a mask PNG without a
  visual editor, and this project ships no binary assets. At load, splines
  rasterize into TissueMask → DistanceField → FlowField. Schema v1 is documented
  in `Level.h`; a missing `"schema"` is an error, not a default.
- **`wave/`** — the director is a pure function of (tick, wave table, RNG). No
  wall-clock, no background spawning, so a wave sequence replays identically. It
  schedules a table; it never builds one. Tables come from the level file only.
- **`economy/`** — ATP ledger with fractional carry so income is exact. Kill
  income is credited from `DamageStats`, never inferred from count deltas.
- **`meta/`** — versioned JSON save. Loading an older version must migrate;
  loading a *newer* version must fail loudly rather than silently drop fields.
- **`session/`** — `step_level()`, the one authoritative order of operations for
  a level tick (queued spawns → wave director → sim → gym toggles → economy →
  abilities → win/loss). It used to live inside `App::tick_sim`, which made it
  unavailable to anything without a window. That order *is* gameplay — move the
  economy credit before the sim tick and towers pay for kills a frame late — so
  the balance harness must run it rather than a plausible imitation. `App` calls
  it and `--autoplay` calls it; there is no second copy to drift. It owns
  nothing (every pointer in `LevelSystems` belongs to the caller) and reports
  the outcome rather than acting on it, because `app/GameState.h`'s transitions
  are an app-layer concern.
- **`autoplay/`** — the bot that plays a level for balance measurement. Where to
  build is derived from the level's own geometry (vessel widths, the flow
  field's cost-to-goal, lane ownership, `PlacementZoneTag`), never from an
  authored per-level plan: a plan file is one more thing to keep in step with
  every level edit, and a stale one measures the plan rather than the level.
  Purchases go through `Economy::spend` and `TowerSystem::validate` — the bot
  has no path into the sim a player lacks. Deterministic: no RNG, no wall clock.
- **`telemetry/`** — `RunTelemetry`, which turns a played level into the JSON a
  balance pass reads (per-tower earnings, per-family outcomes, per-wave
  pressure, an economy timeline). Rates and ratios are derived at report time
  from raw tallies, so a new metric never needs a re-run. See `docs/BALANCE.md`.
- **`gym/`** — the debug/authoring command language (`spawn`, `tower`, `wave`,
  `cast`, `vfx`, …) behind the gym level's control panel (`ui/GymPanel.h`),
  `--sim-test`'s `cmd` action, and `--exec`. A pure function of (context, line) with no UI of its own, so the same
  command string runs interactively, in a script, and in a test. Reaches the sim
  only through the same public APIs `app/` uses for player intents. See
  `docs/GYM.md` and `assets/levels/gym.json`.

---

## 7. `ui` and `audio`

`ui` emits **intents**, it does not mutate the sim. `app/` translates intents
into sim commands so every state change goes through one auditable path — which
is also the path `--sim-test` scripts drive. At 10k agents the player cannot read
individual units, so the HUD carries the readability load via per-lane threat
indicators computed from aggregate data (occupancy + family counts), never by
iterating agents.

`audio` synthesizes everything at runtime (no WAV files). The SDL2 audio callback
runs on its own thread and must never allocate, lock, or call into the sim;
gameplay pushes immutable `AudioEvent` values through a lock-free ring buffer.
Dropping a sound when the queue is full is always preferable to stalling the sim.
Mass events (`ChaffDissolve`) scale one voice by intensity rather than triggering
one voice per agent.

---

## 8. `app` — the loop and the three modes

```
clock.begin_frame();
input.poll();
while (clock.consume_tick()) sim.tick();   // fixed 60 Hz
renderer.draw(clock.alpha());              // variable rate
```

The sim's tick count over a wall-clock span is identical regardless of frame
rate, so a 144 Hz machine and a 30 Hz machine play the same game.

`GameStateMachine` keeps exactly one state live with explicit transitions applied
at the top of a frame, so a state never destroys itself mid-update — and so the
headless modes can construct just `InLevel` without dragging in menus, an audio
device, or a window.

### `--bench <scenario> --ticks N`

Runs the sim headless and prints the `Profiler` JSON to stdout. Logging goes to
stderr so stdout stays machine-parsable. Scenarios are registered in
`app/Modes.cpp`; `--list-scenarios` prints them as JSON.

| Scenario | Content | Purpose |
|---|---|---|
| `empty` | nothing | fixed per-tick overhead |
| `chaff1k` | 1,000 chaff | smoke scale |
| `chaff10k` | 10,000 chaff | **the Wave 1 gate** (<4 ms) |
| `chaff10k_towers` | + 16 damage fields | Wave 2 gate |
| `named200` | 200 named agents | the <2 ms §8.6 budget |
| `mixed` | 10k + 200 + 16 fields | floodplain-like worst case |

### `--sim-test <script.json>`

Runs a scripted scenario and asserts invariants. Exit 0 pass / 1 fail. Every
assertion is evaluated (no early exit) so one run reports every failure. Script
schema v1 is documented at the top of the `--sim-test` section in
`app/Modes.cpp`; assertable metrics are `chaff_count`, `named_count`,
`total_density`, `objective_integrity`, `chaff_killed_total`,
`chaff_leaked_total`, `tick`, `state_hash`, plus the per-family forms
`chaff_{spawned,killed,leaked,despawned,alive}.<family>` (e.g.
`chaff_leaked.bacteria`). Per-family assertions exist because "the bacteria
got through" is a regression a total-only metric hides the moment the level
also kills more viruses.

Actions are `spawn_chaff`, `place_tower`, and `cmd` — the last runs a gym
command (`docs/GYM.md`), so anything reachable from the in-game console is
scriptable as a regression test without inventing a new action type first.

### `--screenshot <level> --tick N --out <file.png>`

Advances the deterministic sim to tick N, creates a headless GL 4.5 context,
renders one frame, and writes a PNG. Also prints a JSON metadata block including
`state_hash`, so a visual diff can be correlated with a sim-state diff.
`--exec "<gym commands>"` sets the world up first, so a look found by hand in the
console can be reproduced as a capture.

---

## 9. Performance budgets (DESIGN.md §8.6) — acceptance criteria

| Subsystem | Bench key | Budget @ 10k |
|---|---|---|
| Chaff: flow sample + separation + instanced render | `chaff_update` + `render_submit` | **< 4 ms** |
| ≤200 named agents, ECS tick | `ecs_tick` | **< 2 ms** |
| Spatial hash rebuild | `spatial_hash` | **< 1 ms** |
| Whole frame | `frame_total` | **< 16.6 ms** |

These are pass/fail, not aspirations. If a change pushes `--bench` over budget,
that is a failure to report, not a footnote.

---

## 10. Wave 0 status — what is real vs. stubbed

*(Historical — this section describes the project immediately after Wave 0,*
*kept for context on how the contracts were bootstrapped. For the current,*
*up-to-date real-vs-stubbed status, see*
*[`HANDOFF_TECHNICAL.md` §4](HANDOFF_TECHNICAL.md#4-whats-real-vs-stubbed--module-by-module)*
*— nearly everything listed as stubbed below is real now.)*

**Real and tested:** `core` (clock, RNG, arena, job system, profiler),
`platform` (window/GL/input/file IO), `render::Camera`, `render::Screenshot`,
`ChaffBuffers` SoA storage semantics, the ECS scheduler's ordering guarantee,
`Economy`'s ledger, the CLI, and all three headless modes end to end.

**Stubbed (compiling, returning defaults), pending their owning wave:**
`FlowField` / `DistanceField` bake and sampling (1A), `SpatialHash::rebuild` and
queries (1B), `ChaffSystem::update` (1B), `Renderer`'s draw passes and
`ShaderManager` (1C), ECS behaviour systems (1D), `DamageSystem::apply` (2A),
`TowerSystem` (2B), `EnemyRoster` spawning (2C), `LevelLoader` JSON parsing (2D),
`WaveDirector` (3A), `Hud` (3B), `AudioEngine` (3C), `MetaProgression`
save/load (4B).

A Wave 0 `--screenshot` therefore produces a correctly-formed, uniformly cleared
frame in the warm tissue-substrate colour. That is the expected result, not a
bug: the clear path, the GL context, the readback, the flip, and the PNG encode
are all proven, and Wave 1C only has to add draws.

---

## 11. `config/` — the tuning surface

Every gameplay-numeric value — tower stats and per-role mechanics, enemy family
size/speed/health and elite stats, crowd physics, economy, abilities, meta
rewards — lives in `assets/config/*.json` and is loaded at startup. Wave tables
are the deliberate exception: they are authored per level in
`assets/levels/*.json`, not tuned globally (see `game/level/Level.h`).

**Module placement.** `immune_config` (`src/config/`) is generic machinery only:
strict parsing, a field registry, path-addressed get/set, dump, and file
polling. It links `core` + `platform` + nlohmann_json and knows nothing about
towers or enemies. The six schemas live in `src/game/config/`, inside
`immune_game`, because filling `sim::ChaffTuning` and pushing values into
`render` both need modules `config/` sits below. Order is unchanged:
`core → platform → config → sim → render → game → ui → app`.

**One declaration, four behaviours.** A config struct declares its fields once
as a `Schema` of `{name, kind, offsetof, doc}`. Parse, dump, get-by-path and
set-by-path are all derived from that list, so a dumped file is guaranteed to
round-trip and every field that exists is guaranteed to be reachable from
`config set`. Document *structure* is still hand-written per file.

**Authoritative.** A missing field is an error; an unknown key is an error with
a did-you-mean suggestion. The shipped files are therefore always a complete,
self-documenting list of every knob.

**Frozen headers.** Adopting the config changed no frozen contract's public
surface. Values reach their systems through seams that already existed
(`TowerSystem::set_stats`, `ChaffSystem::set_tuning`) or through new,
non-frozen headers (`TowerMechanics.h`, `EnemyConfigApply.h`,
`AbilityConfigApply.h`). The two exceptions are additive: `Cli.h` gained
`--config`/`--dump-config`, and `EnemyRoster.h` gained one friend declaration.
`WaveDirector.h` later lost its `generate()`, when wave tables moved to the
level files — the one frozen surface this project has deliberately narrowed.

**Determinism.** The config is a determinism input, so `--sim-test` and
`--bench` never hot-reload, `--config <dir>` pins it, and every sim-test report
carries a `config_hash` beside `state_hash`.

**Bootstrap.** `immune --dump-config <dir>` writes the live values out as a
complete file set. `assets/config` was generated that way rather than
transcribed, and `tests/test_config.cpp` asserts the shipped files still equal
the compiled-in defaults.
