# IMMUNE architecture

This is a map of the implemented engine and its ownership boundaries. Read it
before changing a system that crosses modules. Source and tests establish
current behavior; [DESIGN.md](../DESIGN.md) explains the intended experience.
Historical implementation waves and `FROZEN CONTRACT` comments in headers
describe how the project was assembled, not a separate approval process for
current contributors.

The engine guides are [SIMULATION.md](SIMULATION.md),
[RENDERING.md](RENDERING.md), and [AUDIO_PLATFORM.md](AUDIO_PLATFORM.md). Start
from [the documentation index](README.md) for gameplay, content, UI, and tools.

## Module map

The application is C++20. [CMakeLists.txt](../CMakeLists.txt) lists exact sources
and link dependencies. `immune_common` supplies include paths, the C++ standard,
and GLM. The remaining libraries are:

| Target / directory | Responsibility | Direct project dependencies |
| --- | --- | --- |
| `immune_core` / `src/core` | Types, math, clocks, PCG RNG, arena, jobs, logging, profiling | common |
| `immune_platform` / `src/platform` | SDL window/GL context, frame input, filesystem | core |
| `immune_config` / `src/config` | Strict JSON/schema/registry/config-store machinery | core, platform |
| `immune_sim` / `src/sim` | World, chaff, ECS, flow, squads, combat, fluid, scars | core |
| `immune_vfx` / `src/vfx` | Cosmetic particles and family death-effect tables | core, sim |
| `immune_render` / `src/render` | Camera, GL wrappers, shaders, world passes, PNG capture | core, platform, sim, vfx |
| `immune_game` / `src/game` | Levels, waves, deployed cells, abilities, economy, progression, editor, gym, autoplay, telemetry, config adapters | core, config, sim, render |
| `immune_gui` / `src/gui` | Retained widgets, layout, hit testing, SDF drawing, text/icons | core, platform, render |
| `immune_ui` / `src/ui` | Player screens and developer panels | core, platform, render, game, gui |
| `immune_audio` / `src/audio` | Procedural synthesis and mixing | core |
| `immune_app` / `src/app` | Boot, state machine, input routing, wiring, command-line modes | core, platform, sim, render, game, ui, audio |

External dependencies include SDL2, glad, GLM, EnTT, nlohmann JSON, Dear ImGui,
stb, nanosvg, and Catch2. This is a dependency graph, not a single linear chain.
`gui` links render for GL infrastructure but its source stays independent of
game rules. Preserve these source boundaries:

- Simulation receives plain data and explicit commands. It does not read SDL,
  widgets, JSON files, or renderer state.
- The renderer consumes simulation state; drawing must not change gameplay.
- VFX receives sim outputs and cannot feed results back into it.
- `game/config` translates JSON-backed game configuration into structs accepted
  by sim/render/VFX. Generic `config` does not know enemy or tower rules.
- UI screens consume models and return intents. App translates them into game
  calls. Gym/editor panels use their documented shared game APIs.
- `game/session` reports a level outcome; `app` decides which screen to enter.

## Runtime ownership and level lifetime

[App](../src/app/App.h) owns the interactive window, clock, jobs, simulation,
renderer, audio, UI, game systems, configuration, and progression. Boot loads
configuration, establishes GL-dependent subsystems, and starts the front end.
See [App.cpp](../src/app/App.cpp) for initialization and teardown order.

[GameStateMachine](../src/app/GameState.h) has Boot, MainMenu, LevelSelect,
StrengthenImmunity, InLevel, Paused, Editor, LevelComplete, LevelFailed, and
Quitting states. Only `InLevel` makes `sim_running()` true. The normal frame loop
polls input, applies pending transitions, consumes fixed ticks while running,
renders, and updates the audio listener/music state. Pending transitions prevent
routine changes from destroying a screen during its own update; editor helpers
also have explicit transition paths.

In a non-running state, App calls `FixedClock::drop_accumulated()`. Menu time
therefore does not become a burst of gameplay ticks on resume. Fast-forward
changes the accumulator's time scale, not the fixed simulation `dt`.

A level load initializes the world, bakes tissue/flow/lane data, registers named
and tower systems, attaches game systems, and applies configuration.
`SimWorld::init()` is destructive: it resets agents, ECS entities, ECS systems
and registry context, combat stores, counters, and seeded state. Re-register
per-level systems afterwards. Clearing entities alone can leave old systems
behind and cause a second level to tick twice.

Normal play calls `TowerSystem::validate_deploy()` / `deploy()` to create one
persistent immune cell in the swarmer store, with no ECS tower/spawner entity.
`deploy_cells()` validates and pays for requested quantities. Legacy `place()`
creates a spawner tower for compatibility/debug uses, and its release systems
remain installed. Do not infer current player behavior from older spawner
comments in public headers. A direct deployment's high-range owner ID groups
attribution/scars; it is not an ECS entity to dereference.

The editor owns a `LevelDoc` and separate baked preview. It does not reinitialize
the active world on every gesture. `LevelLoader::bake_geometry()` is shared by
the editor and real loading; Play loads the in-memory document including unsaved
edits. See [LEVEL_EDITOR.md](LEVEL_EDITOR.md).

## Two tick boundaries

[SimWorld::tick](../src/sim/SimWorld.cpp) advances engine stores: movement, ECS,
combat, fluid, retirement, rebakes, and the world tick counter. It does not own
wave scheduling, ATP, ability cooldowns, or level outcomes.

[game::step_level](../src/game/session/LevelSession.cpp) owns the outer sequence:

1. Apply queued gym spawns, then advance the authored wave director.
2. Enable legacy tower release outside Prep; with no director, release continuously.
3. Tick the world, then apply gym toggles before checking outcomes.
4. Accrue passive ATP outside Prep, credit removed density, and credit pending
   wave rewards.
5. Advance active-ability timers.
6. Check objective destruction first, then the survival deadline or wave clear.

App and autoplay call this function. New headless gameplay runners should use
it rather than duplicate the order. Focused engine tests/benchmarks may tick
SimWorld directly, which intentionally represents a smaller workload.
`LevelSystems` contains non-owning pointers, skips absent optional systems, and
carries the level's `survive_seconds` setting.

## Data and determinism

Large populations use bounded structure-of-arrays stores: chaff, projectiles,
swarmers, fluid, and cosmetic particles each have separate streams. Named agents,
towers, and scars use EnTT components and explicit system phases. Do not
introduce a heap object, virtual behavior call, or GL draw per chaff agent.

Gameplay advances at `kTicksPerSecond == 60`, using `kFixedDt` for float math
and `kFixedDtSeconds` for double scheduling. Randomness uses explicitly seeded
[Rng](../src/core/Rng.h). Repeatable fixtures pin the build, configuration, seed,
commands, worker count, and rebake scheduling. Range-based RNG partitioning can
change when workers change; `pump_rebake()` uses elapsed time to choose how many
complete regions run. Fixed ticks do not establish cross-machine or
cross-thread bit equality. See [SIMULATION.md](SIMULATION.md#reproducibility-and-hashes).

`state_hash()` is a regression fingerprint of selected state, not a save format
or full-world serialization. Configuration has a separate `config_hash` in
deterministic modes; named-agent tests also use `named::state_hash()`. Compare
relevant counters/system-specific state as well as hashes when proving a change.

## Core services

| Service | Current behavior | Guidance / tests |
| --- | --- | --- |
| [Types.h](../src/core/Types.h), [Math.h](../src/core/Math.h) | Scalar/GLM aliases, rectangles, IDs, fixed ticks, family/tower enums | Enum order indexes config, streams, visuals, and roster tables. Update consumers together; use count constants. |
| [Rng.h](../src/core/Rng.h) | PCG32 with explicit seed/stream; `fork(id)` does not advance the parent | Forks need a changing input for fresh randomness each tick. [test_rng.cpp](../tests/test_rng.cpp) |
| [Clock.h](../src/core/Clock.h) | Double accumulator in tick units; default frame clamp 0.25 s; manual advancement, time scale, alpha | Clamped hitches discard excess catch-up time. [test_clock.cpp](../tests/test_clock.cpp) |
| [Arena.h](../src/core/Arena.h) | Bump allocator, absolute-address alignment, O(1) reset, null on overflow, peak tracking | Only trivially destructible objects; reset invalidates pointers. Availability does not mean every temporary uses it. [test_arena.cpp](../tests/test_arena.cpp) |
| [JobSystem.h](../src/core/JobSystem.h) | Fork/join contiguous ranges, dispatch/wait; caller participates | `JobSystem(0)` is serial; `kAutoWorkers` auto-sizes. Avoid shared RNG and order-sensitive float reductions. [test_jobsystem.cpp](../tests/test_jobsystem.cpp) |
| [Profiler.h](../src/core/Profiler.h) | Reserved samples reduced to avg/p50/p99/min/max/count | Five canonical JSON keys always emit; extras follow sorted. Key order is stable; timings are not. [test_profiler.cpp](../tests/test_profiler.cpp) |
| [Log.h](../src/core/Log.h) | Shared formatted diagnostics with selectable sink/level | Keep diagnostics out of machine-readable stdout. |

Reuse buffers and reserve at load. Avoiding allocation in bulk hot paths is a
design rule, not an allocation-profiler guarantee for every ECS spawn, query
temporary, or runtime construction path.

## Performance evidence

Historical targets guide measurement. They are not claims that every
hardware/build/configuration meets them, or that a single test enforces all of
them.

| Workload | Historical target |
| --- | --- |
| Chaff movement plus render submission at 10k | under 4 ms |
| Up to 200 named agents' ECS tick | under 2 ms |
| Spatial hash rebuild | under 1 ms |
| Full interactive frame | under 16.6 ms |

Canonical keys are `chaff_update`, `spatial_hash`, `ecs_tick`, `render_submit`,
and `frame_total`; sim extras include `squad_update`, `hostile_update`, and
`burrow_update`. `--bench` attempts hidden real GL and submits tissue, chaff,
entities and fields each tick. Context/renderer failure degrades to sim timing
with zero render samples; those zeros do not establish GPU performance. Several
combat passes contribute to total tick
cost without separate keys. Renderer `FrameStats::submit_ms` measures timed CPU
pass submissions. App's `render_submit` scope also includes `end_frame()`, which
currently calls `glFinish()` and can wait for GPU work. Neither figure is an
isolated GPU timestamp measurement.

Record scenario, ticks, seed, workers, build, hardware and config when comparing
results. The old Wave 0 stub list has been retired from this guide: current
functionality is documented by subsystem and tests.

## Choosing an extension seam

- Gameplay numbers belong in existing config schemas/adapters; see
  [CONFIGURATION.md](CONFIGURATION.md).
- Bulk-agent state needs spawn/clear/compaction, hash/counter decisions, and
  CPU/GPU packing updates if visible.
- Instantaneous visuals belong in `CombatEvent`; persistent things belong in
  stores read by render.
- Named behavior belongs in archetype tables/hooks and existing ECS phases.
- Player actions go through intents/commands and game validation/spending APIs.
- Editor rules belong in shared document/validator/bake paths so tools agree.

See [TESTING.md](TESTING.md) for validation commands and test selection.
