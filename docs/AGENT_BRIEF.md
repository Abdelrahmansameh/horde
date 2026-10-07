# Agent and contributor brief

Start with the [documentation index](README.md), [current design](../DESIGN.md),
[architecture](ARCHITECTURE.md), and [conventions](CONVENTIONS.md). Read the
subject reference before changing a system. This file is an entry guide, not
a duplicate command or schema manual.

## Establish the task and baseline

Inspect repository status and any active file ownership before editing. Keep
unrelated user changes intact. For parallel work, agree on disjoint paths and
coordinate shared interfaces; historical wave tables do not define current
ownership. Follow the user's task scope and existing authorization.

Check implementation and call sites when docs/headers disagree. Names such as
`TowerSystem` remain from an earlier model: **normal play deploys cells directly**.
Five cell types and virus/bacteria/parasite are active; shipped elite/boss content
is empty. Campaign and sandbox apply different unlock/reward rules.

## Build and verify

The maintained commands are in [Building](BUILDING.md) and [Testing](TESTING.md).
On the configured Windows development machine, from the repository root:

```powershell
.\tools\build.bat all
$immuneExe = Join-Path $env:LOCALAPPDATA 'horde-build/windows-release/bin/immune.exe'
& $immuneExe --sim-test tests/scripts/smoke.json --quiet
& $immuneExe --bench chaff10k --ticks 600 --threads 1 --quiet
```

The helper contains machine-specific setup paths. Use Developer Prompt/raw
commands or adjust the helper according to Building when those paths differ.
Keep build output outside OneDrive. If the user has the game open, do not
terminate their process to relink: use a separate build directory or coordinate
the build. See the local [run-immune skill](../.agents/skills/run-immune/SKILL.md)
for execution guidance.

Select checks for the changed contract. Unit tests, sim scripts, autoplay, and
screenshots validate different layers. Sim scripts do not run the complete
campaign session; legacy gym towers do not exercise player deployment.
Documentation changes need source/link/example checks. Do not claim checks
you did not run, and do not require unrelated benchmarks for a small prose edit.

Visual changes require an actual screenshot read back by the author. Capture
into `scratch/` or another output folder. Check shader errors as well as process
exit status. For timing changes, measure the relevant benchmark on a recorded
build/machine; old budget tables were targets and are not evidence of current
performance.

## Important contracts

- Simulation uses fixed 60 Hz time and seeded RNG; visual/audio randomness
  stays separate. See [Simulation](SIMULATION.md) for thread/rebake caveats and
  the limited coverage of `state_hash`.
- Preserve GPU struct/shader layouts, enum order, save keys, and machine-readable
  report meanings. Update all consumers when changing one.
- Keep crowd-scale storage data-oriented and capacity-bounded. Profile hot-path
  changes; avoid per-pathogen allocations or polymorphic dispatch.
- Full gameplay tick order belongs to `game/session/LevelSession`. Avoid creating
  a second gameplay sequence in a tool or test.
- Tuning is strict JSON with schema-derived access. Live, construction-time,
  level override, and tree-only values have different application paths.
- Screens emit intents/menu results; App applies gameplay mutations. GUI capture
  must prevent world actions from leaking through controls.

## Report the result

State what changed and why, relevant checks with actual outcomes, and any
material limitations or pre-existing failures. Link useful source/doc/output
files. Include screenshot evidence for visual work and measured reports for
performance work. Update affected references in the same change and add new
topics to [the index](README.md).
