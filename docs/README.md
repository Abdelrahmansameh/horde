# IMMUNE documentation

This is the starting point for people working on the game. These references
were audited against the repository on **2026-10-06**. They describe the current
implementation; numerical defaults mean the checked-in JSON, before level
overrides and permanent upgrades. Source and data take precedence over prose.

## Learn the game

| Subject | Reference |
|---|---|
| Design pillars and implemented scope | [Game design](../DESIGN.md) |
| Run flow, controls, deployment, waves, and outcomes | [Gameplay](GAMEPLAY.md) |
| The five immune cell types and their mechanics | [Immune cells](IMMUNE_CELLS.md) |
| Pathogen families, replication, attacks, and burrowing | [Pathogens](PATHOGENS.md) |
| Active abilities, ATP income, and spending | [Abilities and economy](ABILITIES_ECONOMY.md) |
| Tree nodes, prices, rewards, capstones, respec, and saves | [Progression](PROGRESSION.md) |
| Campaign files, level schema, geometry, and wave authoring | [Levels](LEVELS.md) |

## Develop and verify

| Subject | Reference |
|---|---|
| Prerequisites, presets, build paths, and launch | [Building](BUILDING.md) |
| Catch2, simulation scripts, screenshots, and benchmarks | [Testing](TESTING.md) |
| Naming, hot paths, determinism, contracts, and documentation upkeep | [Conventions](CONVENTIONS.md) |
| First steps, parallel work, and reporting evidence | [Agent brief](AGENT_BRIEF.md) |
| Console commands and differences between execution contexts | [Gym](GYM.md) |
| In-game editing, validation, undo, save, and playtest | [Level editor](LEVEL_EDITOR.md) |
| Autoplay, attribution, reports, and balance sweeps | [Balance](BALANCE.md) |
| Python helpers, generation, and auxiliary build scripts | [Tools](TOOLS.md) |
| Implementation inventory and limitations | [Project status](../plan.md) |

## Understand or extend the engine

| Subject | Reference |
|---|---|
| Module dependencies, app states, core services, and shared level session | [Architecture](ARCHITECTURE.md) |
| Tick ordering, chaff, ECS, fields, movement, damage, and capacities | [Simulation](SIMULATION.md) |
| Draw passes, shaders, camera, instance data, visual effects, and LOD | [Rendering](RENDERING.md) |
| Window/platform, input, file lookup, and synthesized sound | [Audio and platform](AUDIO_PLATFORM.md) |
| Tuning schemas, registry, reload, dumps, and config hashes | [Configuration](CONFIGURATION.md) |
| Retained widgets, models, screens, events, theme, and GUI rendering | [UI framework](UI_FRAMEWORK.md) |
| Asset folders, licenses, generators, and visual reference workflow | [Assets](ASSETS.md) |

## Source coverage map

Use this map when a folder name is the only clue you have. References explain
the relevant headers and point to tests; they are not copies of every function.

| Source area | Primary documentation |
|---|---|
| `src/app/` | [Architecture](ARCHITECTURE.md), [Building](BUILDING.md), [UI](UI_FRAMEWORK.md) |
| `src/core/` | [Core services](ARCHITECTURE.md#core-services), [Simulation](SIMULATION.md) |
| `src/platform/` | [Audio and platform](AUDIO_PLATFORM.md), [Assets](ASSETS.md) |
| `src/config/`, `src/game/config/` | [Configuration](CONFIGURATION.md) |
| `src/game/session/`, `src/game/wave/` | [Gameplay](GAMEPLAY.md), [Architecture](ARCHITECTURE.md) |
| `src/game/towers/` | [Immune cells](IMMUNE_CELLS.md) |
| `src/game/enemies/` | [Pathogens](PATHOGENS.md) |
| `src/game/abilities/`, `src/game/economy/` | [Abilities and economy](ABILITIES_ECONOMY.md) |
| `src/game/meta/` | [Progression](PROGRESSION.md) |
| `src/game/level/`, `src/game/editor/` | [Levels](LEVELS.md), [Level editor](LEVEL_EDITOR.md) |
| `src/game/gym/` | [Gym](GYM.md) |
| `src/game/autoplay/`, `src/game/telemetry/` | [Balance](BALANCE.md) |
| `src/sim/SimWorld.*`, `src/sim/Immunity.h`, `src/sim/SizeJitter.h` | [Simulation](SIMULATION.md), [Progression](PROGRESSION.md) |
| `src/sim/chaff/`, `src/sim/spatial/`, `src/sim/squad/` | [Simulation](SIMULATION.md), [Pathogens](PATHOGENS.md) |
| `src/sim/flowfield/`, `src/game/level/RenderSdf.*` | [Simulation](SIMULATION.md), [Levels](LEVELS.md), [Rendering](RENDERING.md) |
| `src/sim/swarm/`, `src/sim/scar/` | [Immune cells](IMMUNE_CELLS.md), [Simulation](SIMULATION.md) |
| `src/sim/burrow/`, `src/sim/hostile/`, `src/sim/ecs/` | [Pathogens](PATHOGENS.md), [Simulation](SIMULATION.md) |
| `src/sim/damage/`, `src/sim/projectile/`, `src/sim/fluid/` | [Simulation](SIMULATION.md), [Immune cells](IMMUNE_CELLS.md) |
| `src/sim/Attribution.h`, `src/sim/CombatEvents.h` | [Balance](BALANCE.md), [Rendering](RENDERING.md) |
| `src/render/`, `src/vfx/`, `assets/shaders/` | [Rendering](RENDERING.md) |
| `src/audio/` | [Audio and platform](AUDIO_PLATFORM.md) |
| `src/gui/` | [UI framework](UI_FRAMEWORK.md) |
| `src/ui/hud/`, `src/ui/front/`, `src/ui/Intent.h`, `src/ui/Menu.h` | [UI framework](UI_FRAMEWORK.md) |
| `src/ui/editor/`, `src/ui/GymPanel.*`, `src/ui/DevUi.*` | [Level editor](LEVEL_EDITOR.md), [Gym](GYM.md), [UI](UI_FRAMEWORK.md) |
| `assets/config/` | [Configuration](CONFIGURATION.md) |
| `assets/levels/` | [Levels](LEVELS.md) |
| `assets/ui/`, `assets/fonts/`, `docs/ui-concepts/` | [Assets](ASSETS.md) |
| `tests/`, `tests/scripts/` | [Testing](TESTING.md) |
| `tools/` | [Tools](TOOLS.md) |

## Keep this set current

When changing a subsystem, update its reference and any affected commands,
tables, examples, and tests in the same change. Add new topics to this index.
Keep design goals visibly separate from shipped behavior and measurements.
Check actual call sites as well as headers: several interfaces retain names
such as `TowerSystem` from the older spawner model.

The UI canvas files are visual snapshots, not the gameplay specification.
The local run-immune skills link to these maintained references. The audit
replaced obsolete spawner, selling, upgrade, roster, build, and coordination
instructions and qualified reproducibility claims. Future audits should start
with [Conventions](CONVENTIONS.md) and use [Testing](TESTING.md) for evidence.
