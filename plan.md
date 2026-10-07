# IMMUNE implementation status

This file replaces the initial five-wave bootstrap plan. The repository now
contains a playable game, tools, tests, and authored content; the original plan's
zero-code assumptions, eight-tower/six-family scope, and historical ownership
rules are not current requirements. This inventory was audited on 2026-10-06.

## Implemented areas

| Area | Current implementation | Reference |
|---|---|---|
| Application | State machine, campaign/sandbox/editor modes, common level session | [Architecture](docs/ARCHITECTURE.md) |
| Gameplay | Direct placement of five mobile immune cell types, hold/Shift deployment, ATP, four abilities | [Gameplay](docs/GAMEPLAY.md), [Immune cells](docs/IMMUNE_CELLS.md) |
| Enemy content | Virus, Bacteria, Parasite; replication, hostile pressure, parasite burrow/slither | [Pathogens](docs/PATHOGENS.md) |
| Campaign | Ten ordered campaign files; preceding-clear unlock gate | [Levels](docs/LEVELS.md) |
| Progression | Strengthen Immunity tree, two currencies, capstones, respec, versioned saves | [Progression](docs/PROGRESSION.md) |
| Simulation | Fixed 60 Hz, SoA chaff, ECS framework, flow/spatial/squad/hostile/fluid/scar systems | [Simulation](docs/SIMULATION.md) |
| Rendering | GL 4.5 instancing, procedural tissue/species/VFX, camera and screenshot output | [Rendering](docs/RENDERING.md) |
| UI | Custom retained player GUI; title/tree/campaign/HUD/pause/results; ImGui developer tools | [UI framework](docs/UI_FRAMEWORK.md) |
| Audio/platform | SDL window/input/file lookup and runtime synthesized sound/music | [Audio and platform](docs/AUDIO_PLATFORM.md) |
| Tuning | Six strict gameplay files, live registry/reload, separate UI theme | [Configuration](docs/CONFIGURATION.md) |
| Content tools | In-game editor, document validation/writer, CLI checks/formatting, level preview | [Level editor](docs/LEVEL_EDITOR.md), [Tools](docs/TOOLS.md) |
| Verification | Catch2, GL tests, sim scripts, benchmarks, autoplay telemetry/sweeps | [Testing](docs/TESTING.md), [Balance](docs/BALANCE.md) |

## Current limitations and retained code

These are observations for maintainers, not a commitment or newly prioritized
feature backlog.

- Elite/named-agent and boss infrastructure exists, but the shipped elite
  catalog is empty. The active family enum contains three identities; older
  six-family design proposals are not the shipped roster.
- Stationary ECS tower emitters, sell/fire paths, and inspection widgets remain
  for developer contexts. Player deployment creates cells instead; normal play
  has no spawner towers, selling, or in-run upgrades.
- Renderer blob/density LOD and shadow machinery exists but is disabled by
  default. Capacity and FPS goals must be measured on the actual workload.
- Fixed ticks and seeded RNG support repeatable comparisons, but thread-range
  RNG partitioning and elapsed-time flow rebake scheduling limit reproducibility.
  The state hash is a partial fingerprint, not complete game-state equivalence.
- Headless autoplay shares the session tick but does not mirror every interactive
  level-rule application. Sim-test drives bare simulation, not the full economy,
  wave, campaign, and payout lifecycle.
- Several editor menu shortcut labels are presentation hints rather than wired
  keyboard actions. The actual tool/canvas key handlers define supported keys.
- HTML UI artboards preserve earlier interactions. The live radial tree and
  direct-cell behavior are authoritative; preview labels/prices are illustrative.
- Save progression, live tuning, visual shader correctness, and campaign balance
  require their corresponding checks. A successful C++ build alone proves none
  of those properties.

Use [docs/README.md](docs/README.md) for the full source coverage map.
When adding features, describe actual delivered behavior and evidence in the
owning reference rather than restoring obsolete wave acceptance checklists.
