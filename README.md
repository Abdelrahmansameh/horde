# IMMUNE

IMMUNE is a C++20 game about defending tissue from a flowing pathogen horde.
Players spend ATP to deploy immune cells directly, use active abilities, and
grow permanent upgrades in the Strengthen Immunity tree between runs. The
checked-in campaign has ten levels, five immune cell types, and three active
pathogen families. The engine uses SDL2, OpenGL 4.5, and a fixed 60 Hz simulation.

## Start here

- [Documentation index](docs/README.md): references for every major subsystem.
- [Gameplay and controls](docs/GAMEPLAY.md): how a run works.
- [Build and launch](docs/BUILDING.md): prerequisites, Windows setup, and commands.
- [Architecture](docs/ARCHITECTURE.md): module boundaries and application flow.
- [Contributor conventions](docs/CONVENTIONS.md) and [agent brief](docs/AGENT_BRIEF.md).

## Build and run on Windows

From the repository root, with Visual Studio 2022 C++ tools, CMake, Ninja, and
vcpkg installed:

```powershell
.\tools\build.bat all
$immuneExe = Join-Path $env:LOCALAPPDATA 'horde-build/windows-release/bin/immune.exe'
& $immuneExe
```

The helper contains machine-specific Visual Studio, compiler, and vcpkg paths;
check [Building](docs/BUILDING.md) when setting up another machine. Build output
lives outside the repository and OneDrive. Run from the repository root so
assets can be discovered. To run elsewhere, set `IMMUNE_ASSET_ROOT` to the
repository root, which contains the `assets` directory.

For a sandbox with all cells and abilities available:

```powershell
& $immuneExe --level assets/levels/gym.json --sandbox
```

See [Testing](docs/TESTING.md) for unit tests and simulation scripts,
[Gym](docs/GYM.md) for console commands, and [Level editor](docs/LEVEL_EDITOR.md)
for authoring and playtesting levels.

## Repository map

| Path | Contents |
|---|---|
| `src/` | Application, engine, simulation, gameplay, and UI |
| `assets/config/` | Gameplay tuning and UI theme |
| `assets/levels/` | Campaign, sandbox, and test level JSON |
| `assets/shaders/` | World and UI shaders |
| `assets/ui/`, `assets/fonts/` | SVG icons, tree layout, and licensed UI fonts |
| `tests/` | Catch2 tests and scripted simulation scenarios |
| `tools/` | Build, tuning, level preview, balance, and asset generation helpers |
| `docs/` | Maintainer documentation and UI design snapshots |

[DESIGN.md](DESIGN.md) describes the current design;
[PROGRESSION.md](PROGRESSION.md) leads into the progression reference;
[plan.md](plan.md) records implementation status and remaining limitations.
Code and checked-in data are authoritative when a document disagrees.
