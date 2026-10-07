# Contributor conventions

[Documentation index](README.md) · [Architecture](ARCHITECTURE.md) · [Testing](TESTING.md)

These are development guidelines for the current repository. Historical
implementation-wave ownership tables and blanket frozen-header rules have
been retired. An active assignment can still define file ownership; coordinate
shared edits and interface changes with whoever is working on them.

## Naming and structure

| Item | Convention | Example |
|---|---|---|
| Namespace | Lower snake case under `immune` | `immune::sim` |
| Type / enum / enumerator | PascalCase | `ChaffBuffers`, `PathogenFamily::Virus` |
| Function / local / parameter | Lower snake case | `state_hash()`, `world_pos` |
| Private member | Trailing underscore | `count_` |
| Public stream | Lower snake case | `pos_x`, `density` |
| Constant | `kPascalCase` | `kFixedDt` |
| Macro | `IMMUNE_UPPER_SNAKE` | `IMMUNE_PROFILE_SCOPE` |
| File | Primary type's name | `SpatialHash.h`, `SpatialHash.cpp` |

Use `#pragma once`. Includes are rooted at `src/`, such as
`#include "sim/chaff/ChaffBuffers.h"`. Group the own header, other project
headers, third-party headers, and standard headers in that order. Prefer
forward declarations when they preserve module boundaries. Implementation
helpers belong in an anonymous namespace in the owning `.cpp`.

One CMake library target represents each major module. Follow the actual
dependency graph in [Architecture](ARCHITECTURE.md); simulation must not depend
on SDL, GUI, JSON loading, or renderer implementation. Generic `config/` owns
schema machinery, while `game/config/` translates authored gameplay data.

## Data and hot paths

Crowd-scale streams use structure-of-arrays storage. Keep the hottest position
and velocity axes separate; use packed flags and table-driven family behavior.
Avoid adding a per-pathogen object hierarchy or virtual dispatch.

Reserve bounded simulation pools at initialization. Check capacity before
spawning; define overflow behavior rather than accidentally reallocating at
horde scale. Some subsystems and diagnostic paths use other containers, so
this is a rule for new hot-path work, not a claim that every existing tick is
allocation-free. Measure changes to `SimWorld::tick` and render submission.

Chaff compaction changes indices. An index is not persistent identity; follow
the owning subsystem's handle/remapping protocol for references that survive
a tick. Preserve serial update/removal ordering where it affects behavior.
Use `immune::Arena` only when its reset lifetime matches the caller.

Do not introduce per-agent heap allocation, formatted logging, exceptions,
wall-clock gameplay decisions, or unordered behavior into hot loops. Use
preallocated scratch, explicit statuses, and profiling scopes. Loading,
screens, tools, and error reporting favor ordinary readable C++.

## Reproducibility

Pass seeded `Rng` explicitly for gameplay. Do not use `rand()`, random devices,
global generators, or render time to choose simulation behavior. Partition
parallel work predictably and use independent RNG streams as the subsystem
requires. Avoid shared floating-point accumulation dependent on scheduling.

The simulation advances with `Tick`/`kFixedDt`; player actions arrive as
explicit requests. Cosmetic particle/audio randomness must not perturb gameplay
RNG or simulation state.

Do not promise bit-identical runs on every machine or thread count. Chaff RNG
partitioning and elapsed-time flow rebake scheduling can affect results.
`state_hash` covers selected state, not the whole game. Preserve the same
executable, seed, commands, level, config, thread setting, and relevant tree
state when comparing runs; record limitations and repeat timing-sensitive
cases. See [Simulation](SIMULATION.md) and [Configuration](CONFIGURATION.md).

## Interfaces and errors

Headers should explain ownership, units, lifetime, capacity, and why a contract
has its shape. Changes to public signatures are allowed when the task requires
them: update callers, tests, and docs together and coordinate concurrent owners.
Enum values, GPU layouts, save keys, level schemas, and report fields can be
external contracts; inspect all consumers before changing them.

Use result structs for errors that need explanations and `optional` for absent
reads. Initialization that can fail generally returns status/error information.
Keep third-party parsing exceptions at data boundaries. Assert programmer
invariants; validate authored data and user input at runtime. Explain a retained
compatibility path as such rather than documenting it as normal gameplay.

## Tests and evidence

Catch2 v3 tests use `tests/test_<subject>.cpp`, discovered through the CMake
`test_*.cpp` glob. Add meaningful subject tags. Names must not begin with `-`
because CTest forwards names to Catch2. Test observable contracts, invariants,
boundaries, round trips, and regressions.

Use the shared [LevelSession](../src/game/session/LevelSession.h) when testing
full run behavior. Bare sim scripts and gameplay sessions have different scope.
Visual changes require a generated screenshot inspected by its author and a
check of shader logs. Performance changes need relevant benchmark evidence;
record machine/build/context, not just a naked timing.

Run checks appropriate to the change and any checks required by the active task.
Documentation-only changes need source/example/link checks, not a compulsory
full rebuild or unrelated performance sweep. Report actual commands and results,
including pre-existing failures. See [Testing](TESTING.md).

## Build and assets

C++20, MSVC `/W4 /permissive-`, and vcpkg manifest dependencies are the current
baseline. Build outside OneDrive through the provided presets. Do not suppress
warnings globally or vendor dependencies without a concrete project reason.

Runtime world art and audio are procedural. Authored JSON, GLSL `.vert/.frag`,
SVG UI assets, and OFL-licensed UI fonts are committed. Keep the font license
with distributions. Existing documentation reference images are not runtime
assets. Captures, generated reports, and experiments belong in ignored scratch
or external output, not the asset tree. See [Assets](ASSETS.md).

## Documentation upkeep

Update affected Markdown in the same change as behavior. Use relative links
inside repository docs so a clone remains navigable. Describe current defaults
from JSON separately from level overrides, meta effects, and illustrative test
fixtures. Label proposals and targets; never report a performance goal as a
verified measurement.

For a new subsystem, document its purpose, owner, inputs/outputs, lifetime/order,
configuration, extension points, relevant tests, and limitations. Register the
reference and source folder in [docs/README.md](README.md). Check commands
against their parser/handler, not only `--help` or stale header comments.

Comments explain reasons and invariants. Track unfinished work with a concrete
description and useful context; do not attach fictional historical wave owners.
