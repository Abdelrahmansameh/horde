---
name: run-immune
description: Build, launch, drive, benchmark, or capture the IMMUNE game in the horde repository. Use for real-executable verification, screenshots, console reproduction, and interactive play or editor checks.
---

# Running IMMUNE

Run from the repository root so assets resolve. If launching elsewhere, set
`IMMUNE_ASSET_ROOT` to the **repository root containing assets**, not to
`<repo>/assets`: `asset_path` appends `assets/` itself.

Read the maintained references for the work at hand:

- [Building](../../../docs/BUILDING.md): machine-specific helper setup, presets,
  shared/isolated builds, complete CLI flags.
- [Testing](../../../docs/TESTING.md): Catch2, sim-script schema/metrics, GL tests,
  screenshots, benchmarks, and verification scope.
- [Gym](../../../docs/GYM.md): console commands and context availability.
- [Level editor](../../../docs/LEVEL_EDITOR.md): editor operations and playtesting.
- [Balance](../../../docs/BALANCE.md): autoplay, telemetry, and sweep comparisons.
- [Simulation](../../../docs/SIMULATION.md): state hash and reproducibility limits.

## Choose a build

Shared release executable:
`%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe`.

An existing isolated development build may be at
`%LOCALAPPDATA%\horde-verify-build\bin\immune.exe`; verify that it exists and
matches the source before using it. Never terminate the user's running game
to unlock a build. If relinking is blocked by a live executable, use a separate
build directory as described in Building. Local `claude_build.bat` and
`claude_verify_build.bat` wrappers, when present outside the repo, are machine
conveniences rather than portable prerequisites.

For the configured Windows machine:

```powershell
.\tools\build.bat all
$immuneExe = Join-Path $env:LOCALAPPDATA 'horde-build/windows-release/bin/immune.exe'
& $immuneExe --help
```

The helper initializes MSVC/vcpkg using machine-specific paths. Follow Building
when those paths do not exist; do not assume a plain shell has MSVC headers.

## Capture the actual game

```powershell
& $immuneExe --screenshot assets/levels/gym.json --tick 240 --towers --out scratch/cells.png
& $immuneExe --screenshot assets/levels/campaign_01_first_bend.json --ui --tick 60 --width 1920 --height 1080 --out scratch/prep.png
```

Use a full level file path. A missing/invalid level can fall back to a test lane;
inspect stderr and the report so a fallback is not mistaken for the requested
level. `--tick` defaults to 0, so no simulation has advanced unless requested.
Pass an explicit output path into scratch or external output.

- `--focus x,y` / `--view-height h` frame close views in world units.
- `--towers` / `--tower <type>` deploy direct cells for a capture.
- `--exec "<gym commands>"` runs commands before ticking. A command failure
  stops the chain and is logged; the mode may still capture and succeed.
- `--ui` adds player screens and runs the common gameplay session with waves
  and ATP. Without it the capture advances bare simulation.
- `--config <directory>` selects a complete tuning folder. Screenshot gym
  context lacks config/camera callbacks: use this flag and framing flags.
- `--ui-scale 0.75..1.5` changes UI sizing. Screenshot `ui pointer x y`
  uses logical UI coordinates, not world or framebuffer coordinates.

Read the resulting PNG yourself and describe what is actually visible.
Inspect stderr for shader compilation, fallback levels, failed exec commands,
and deployment diagnostics. Build success or an image file alone is not proof
that the intended pass or combat occurred. Simulation tick time and cosmetic
animation time are separate; equal hashes are not pixel equality.

## Direct cells versus legacy emitters

Normal HUD and autoplay use `validate_deploy/deploy` and create persistent
mobile cells. Screenshot `--towers` and sim-script `place_tower/place_swarm`
also use direct deployment. The gym's `tower` command still calls legacy
`validate/place` and creates a stationary ECS emitter. Its `sell` and `fire`
commands address those legacy emitters. Do not use a legacy tower scenario as
evidence for player placement or assume `sell` affects screenshot direct cells.

For UI fixtures, use `ui dump` before relying on widget paths.
`ui screen menu|tree|levels|pause|victory|defeat` and
`ui tree new|sample|full` are screenshot fixture commands, not live campaign
state changes. Set tree progress before opening the tree to frame that fixture.
Legacy inspect/Sell widgets can still be tested but are not a normal selling
feature.

## Other verification

```powershell
& $immuneExe --sim-test tests/scripts/smoke.json --quiet
& $immuneExe --bench chaff10k --ticks 600 --threads 1 --quiet
& $immuneExe --list-scenarios --quiet
& $immuneExe --autoplay --level assets/levels/campaign_01_first_bend.json --profile greedy-cheapest --report scratch/run.json
& $immuneExe --level-check assets/levels
& $immuneExe --level-fmt assets/levels --check
```

Use checks appropriate to the changed behavior. Sim-test runs bare simulation;
autoplay shares session tick order but differs from interactive level-rule
application. Compare baseline results before calling a script failure a
regression. Preserve executable/config/level/seed/commands/thread settings:
range-based RNG and elapsed-time flow rebakes can change outcomes.
`state_hash` is a partial state fingerprint, not full game equivalence.
`--dump-config` writes compiled defaults; interactive `config dump` saves
loaded base tuning. See the references above before interpreting a report.
