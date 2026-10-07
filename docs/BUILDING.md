# Building and running IMMUNE

This repository builds a C++20 Windows game with CMake and vcpkg. Run the commands below from the repository root. See [Testing](TESTING.md) for verification, [Tools](TOOLS.md) for Python helpers, and [Gym](GYM.md) for the developer console.

## Prerequisites

- CMake 3.21 or newer and Git.
- Visual Studio 2022 with the x64 C++ toolchain and Windows SDK.
- Ninja for the `windows-release` preset, available on `PATH` in the developer environment.
- A vcpkg checkout, with `VCPKG_ROOT` pointing to it. The wrapper defaults to `C:\dev\vcpkg` when the variable is unset.
- A driver supporting OpenGL 4.5 core for interactive play, screenshots, and GL tests. Simulation scripts and level validation do not create a rendering window.

The [vcpkg manifest](../vcpkg.json) pins a baseline and declares SDL2, glad, GLM, EnTT, nlohmann JSON, Dear ImGui with SDL/OpenGL/docking features, stb, NanoSVG, and Catch2 3. CMake installs dependencies in manifest mode; no separate manual package list is required.

## Supported presets

The names and paths below come from [CMakePresets.json](../CMakePresets.json).

| Preset | Generator | Build configuration | Build directory |
|---|---|---|---|
| `windows-release` | Ninja | `RelWithDebInfo` | `%LOCALAPPDATA%\horde-build\windows-release` |
| `windows-debug` | Visual Studio 17 2022 | Multi-configuration; request `Debug` explicitly | `%LOCALAPPDATA%\horde-build\windows-debug` |

`windows-release` is optimized and retains debugging information; its name does not mean the CMake `Release` configuration. The shared base sets `x64-windows`, C++20, the vcpkg toolchain, and the local triplet overlay. The Visual Studio generator ignores `CMAKE_BUILD_TYPE`; the debug build/test presets do not specify a configuration, so use `--config Debug` and `-C Debug` explicitly when invoking them directly.

### Standard wrapper

In PowerShell:

```powershell
.\tools\build.bat all windows-release
```

The [wrapper](../tools/build.bat) supports `configure`, `build`, `test`, or `all`, followed by an optional preset. Defaults are `all windows-release`.

```powershell
.\tools\build.bat configure windows-release
.\tools\build.bat build windows-release
.\tools\build.bat test windows-release
```

It changes to the repository root, initializes the MSVC environment, then executes the corresponding CMake/CTest preset. Configure and build are separate operations: `build` assumes configuration already exists; `test` assumes the test executable already exists.

The checked-in wrapper is currently machine-specific: it calls `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat` and pins vcpkg to that installation and toolset `14.44.35207`. On a different workstation, use a matching Visual Studio developer prompt and the direct commands below, or adjust the wrapper to the installed toolchain. [The triplet overlay](../tools/vcpkg-triplets/x64-windows.cmake) also uses `VSINSTALLDIR` to keep dependency compilation aligned with the game's compiler. Mixing different MSVC STL versions can cause missing `__std_*` symbols at link time.

### Direct CMake commands

From an x64 Visual Studio developer prompt with `VCPKG_ROOT` set:

```text
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

For the multi-configuration debug preset:

```text
cmake --preset windows-debug
cmake --build --preset windows-debug --config Debug
ctest --preset windows-debug -C Debug
```

Build just one target when appropriate:

```text
cmake --build --preset windows-release --target immune
cmake --build --preset windows-release --target immune_tests
```

[CMakeLists.txt](../CMakeLists.txt) defines `immune`, `immune_tests`, and libraries for `core`, `platform`, `config`, `sim`, `render`, `vfx`, `game`, `gui`, `ui`, `audio`, and `app`. Test sources matching `tests/test_*.cpp` are collected with `CONFIGURE_DEPENDS`; rerun configuration if a new source is not being picked up.

The release executables are in `bin\immune.exe` and `bin\immune_tests.exe` under the release build directory. A Visual Studio build normally adds a configuration directory, such as `bin\Debug\immune.exe`. Required runtime DLLs are copied beside each executable after linking.

### An isolated build while the game is open

A running executable can prevent Windows from replacing it during linking. Leave the user's running game alone and configure a separate build directory from a developer prompt:

```text
cmake --preset windows-release -B "%LOCALAPPDATA%\horde-verify-build"
cmake --build "%LOCALAPPDATA%\horde-verify-build" --target immune immune_tests
```

Use that directory's `bin` executables for verification. The root-level `build_with_vcvars.bat`, `build_verify.bat`, `ps_build.bat`, and `test_vcvars.bat` are local convenience/diagnostic scripts; despite its name, `build_verify.bat` still builds the shared release preset. The supported configure/build/test entry point is `tools/build.bat`.

## Assets, config, and saves

```powershell
$immuneExe = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune.exe"
& $immuneExe
& $immuneExe --level assets/levels/campaign_01_first_bend.json --sandbox
```

With no arguments, the app opens the front end. A `--level` path enters a level directly. For experiments, use `--sandbox` or a separate `--save` file; ordinary interactive runs can update the user's persistent progression.

[FileIO.cpp](../src/platform/FileIO.cpp) searches upward from the executable and then from the current directory for an `assets` directory. The most reliable working directory is the repository root. `IMMUNE_ASSET_ROOT`, when needed, must name the **repository/package root containing `assets`**, because `asset_path()` appends `assets` itself:

```powershell
$env:IMMUNE_ASSET_ROOT = (Get-Location).Path
```

Level and explicit config paths are ordinary filesystem paths. Give the full level filename, not a bare name, to CLI modes. The interactive gym command `level gym` has name resolution; `--screenshot gym` does not.

## Command-line reference

[Cli.cpp](../src/app/Cli.cpp) and [Cli.h](../src/app/Cli.h) are the authoritative parser and defaults. `--help` is useful but currently omits several supported options listed here. Choose one mode per invocation; the parser accepts multiple mode flags and generally uses the last one, rather than rejecting ambiguous combinations.

| Mode | Invocation | Result |
|---|---|---|
| Play | `immune` or `immune --level <file>` | Windowed front end/game |
| Benchmark | `immune --bench <scenario>` | Timing JSON on stdout |
| Simulation script | `immune --sim-test <script.json>` | Assertion JSON on stdout, exit 0/1 |
| Screenshot | `immune --screenshot <level.json>` | PNG plus state JSON on stdout |
| Autoplay | `immune --autoplay --level <file>` | Balance JSON on stdout or `--report` path |
| Scenario list | `immune --list-scenarios` | JSON array of benchmark scenarios |
| Default config export | `immune --dump-config <dir>` | Compiled default tuning files |
| Editor | `immune --editor [file.json]` | Windowed editor; template when no file is supplied |
| Level validation | `immune --level-check <file\|dir>` | Validation JSON, exit 0/1 |
| Level formatting | `immune --level-fmt <file\|dir> [--check]` | Canonical rewrite or check, JSON summary |
| Help | `immune --help` or `-h` | Usage text |

### Common options

| Option | Current behavior/default |
|---|---|
| `--seed N` | Unsigned integer; fixed default `0x123456789abcdef0`; decimal or `0x` syntax |
| `--threads N` | `1` means fully serial (zero workers); values above 1 create `N-1` workers; `0`/omitted uses automatic worker selection |
| `--level PATH` | Level file for play/bench; script `level` overrides it; autoplay requires it |
| `--config DIR` | Alternate tuning directory; disables interactive hot reload |
| `--width N`, `--height N` | Positive framebuffer dimensions; defaults 1600 by 900 |
| `--ui-scale F` | UI multiplier from 0.75 through 1.5; default 1 |
| `--exec "commands"` | Startup gym script in interactive/editor/screenshot modes; command context varies by mode |
| `--save PATH` | Alternate progression save path for interactive play |
| `--sandbox` | Interactive run with all unlocks, no tree bonuses, no progression payout |
| `--no-vsync` | Disable interactive vsync |
| `--verbose` | Debug logging |
| `--quiet` | Disable logging; stdout reports are still emitted |

### Mode-specific options

| Option | Applies to | Meaning |
|---|---|---|
| `--ticks N` / `--tick N` | Bench / screenshot | Aliases for the same tick count; bench defaults to 600, screenshot to 0 |
| `--out PATH` / `-o PATH` | Screenshot | PNG path; default `shot.png`; create its parent directory first |
| `--scenario NAME` | Screenshot | Populate a registered benchmark scenario before ticking |
| `--towers` | Screenshot | Deploy one direct cell of each type at available sites |
| `--tower NAME` | Screenshot | Enables placement and restricts it to one type |
| `--ui` | Screenshot | Render the HUD and use the full level-session tick loop |
| `--focus x,y` | Screenshot | Override camera center |
| `--view-height H` | Screenshot | Override view height in world units; zero uses level/world framing |
| `--profile NAME` | Autoplay | `greedy-cheapest`, `spread-coverage`, or `single-type:<type>`; greedy is the default |
| `--report PATH` | Autoplay | Write report to a file instead of stdout |
| `--max-ticks N` | Autoplay | Stop at this tick limit; zero uses 108,000 ticks (30 simulated minutes) |
| `--check` | Level formatting | Do not write; exit 1 if any file would change |

Arguments rejected by the parser return exit 2. Operational mode failures generally return 1. A successful autoplay run returns 0 even if the bot loses or reaches its tick limit: inspect its report's `result`.

## Troubleshooting

| Symptom | Check |
|---|---|
| Missing C++ standard headers or `cl.exe` | Use the developer environment/wrapper; CMake on `PATH` alone is insufficient for Ninja + MSVC |
| Missing vcpkg toolchain | Verify `VCPKG_ROOT` and `scripts/buildsystems/vcpkg.cmake` |
| `LNK1104` / `LNK1168` replacing an executable | Check whether that executable is running; use an isolated build |
| Missing `SDL2.dll` or another runtime DLL | Keep post-build copied DLLs beside the executable |
| Shader/font/icon errors | Run from the repository root or set `IMMUNE_ASSET_ROOT` correctly; inspect stderr |
| Headless GL creation fails | Confirm a usable OpenGL 4.5 context/driver; hidden windows still require GL |
| A CLI capture shows an unexpected lane | Check the supplied file path and stderr for fallback warnings |
| Config reports unknown fields after a source/config update | Rebuild from the same checkout; an older executable may not understand the current config schema |
