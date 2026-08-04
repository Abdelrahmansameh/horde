# IMMUNE — Agent Brief

**Read this first, before touching any file.** It is the standing contract for
every sub-agent working on this project.

Read next, in order: `DESIGN.md` (what the game is), `docs/ARCHITECTURE.md` (the
frozen contracts and why they are shaped that way), `docs/CONVENTIONS.md` (how to
write code here).

---

## The seven rules

### 1. Own your directories. Touch nothing else.
Your assignment names the directories you own. Other agents are editing this same
working tree at the same time. Editing outside your assignment causes conflicts
you will not see until someone else's build breaks.

If you need a change in a file you do not own, **report it up**. Do not make it.

### 2. Interface headers are frozen.
Every header listed in `docs/ARCHITECTURE.md` is a contract that parallel agents
are implementing against right now. You may:

- implement declared functions in your own `.cpp` files,
- add private members to classes you own,
- add new files inside directories you own.

You may **not** change a declared signature, remove a member, reorder an enum, or
alter a documented struct layout — in *any* header, including your own — without
the orchestrator broadcasting the change to the whole wave.

`PathogenFamily`'s order is the renderer's batch order. `ChaffInstance`'s layout
is mirrored in a shader. The `--bench` JSON keys are what perf regressions are
measured against. These look like details and are not.

### 3. Leave the build green.
Configure, build, and `ctest` must all pass before you report done. A red build
blocks every sibling agent in your wave. Run the full sequence in §"Commands"
below — not just the file you were editing.

### 4. Prove it. Don't claim it.
Finish by **running** `--bench`, `--sim-test`, and/or `--screenshot` and pasting
the actual output into your report. "The tests should pass" is not a report.

Visual work must include a screenshot you generated, **read back yourself**, and
described. You can read PNG files; use that.

### 5. Perf budgets are acceptance criteria.
From DESIGN.md §8.6, measured by `--bench` on this machine (RTX 3070, MSVC
release):

| Subsystem | Bench key | Budget |
|---|---|---|
| Chaff @ 10k: flow + separation + instanced render | `chaff_update` + `render_submit` | **< 4 ms** |
| ≤200 named agents | `ecs_tick` | **< 2 ms** |
| Spatial hash rebuild | `spatial_hash` | **< 1 ms** |
| Whole frame | `frame_total` | **< 16.6 ms** |

If your change pushes a number over budget, that is a **failure to report**, not
a footnote. Report the actual JSON.

### 6. Data-oriented in the hot path.
No per-agent virtual calls. No per-enemy heap allocation. No OOP-per-chaff-unit.
No allocation, exceptions, or logging inside a sim tick. DESIGN.md §8.2 is a hard
constraint and `docs/CONVENTIONS.md` §2–3 spell out what it means concretely.

### 7. Determinism is not optional.
Same seed ⇒ same `state_hash()` at every tick, on any machine, at any thread
count. No `rand()`, no `std::random_device`, no wall-clock reads in sim logic, no
shared-RNG or shared-float accumulation across threads. Take an `Rng&`; fork per
range. Breaking determinism breaks every other agent's ability to verify work.

---

## Commands

Everything runs from the repo root:
`C:\Users\Abdel\OneDrive\Documents\horde`

The MSVC environment and `VCPKG_ROOT` are not on the default PATH, so use the
helper script (or run the raw commands from a Developer Command Prompt).

### Build and test

```bat
tools\build.bat configure            :: cmake --preset windows-release
tools\build.bat build                :: cmake --build --preset windows-release
tools\build.bat test                 :: ctest --preset windows-release
tools\build.bat all                  :: all three
tools\build.bat all windows-debug    :: same, debug preset
```

Raw equivalents, inside a Developer Command Prompt with `VCPKG_ROOT` set:

```bat
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

Build output: `%LOCALAPPDATA%\horde-build\windows-release\`
Binaries:     `%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe`

For convenience:

```bat
set IMMUNE=%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe
```

### Verify

```bat
:: Perf. --quiet keeps stdout pure JSON.
%IMMUNE% --bench chaff10k --ticks 600 --quiet
%IMMUNE% --bench mixed    --ticks 600 --quiet
%IMMUNE% --list-scenarios

:: Behaviour. Exit code 0 = pass, 1 = fail.
%IMMUNE% --sim-test tests\scripts\smoke.json --quiet
echo exit=%ERRORLEVEL%

:: Visual. Then READ the PNG back and describe it.
%IMMUNE% --screenshot assets\levels\capillary.json --tick 300 --out shot.png

:: Determinism spot-check: identical seeds must give identical hashes.
%IMMUNE% --sim-test tests\scripts\smoke.json --quiet
```

Useful global flags: `--seed N`, `--level PATH`, `--width/--height N`,
`--threads 1` (fully serial — use it to prove a result is scheduling-independent),
`--verbose`, `--quiet`, `--help`.

### Run the game

```bat
%IMMUNE%
```
F1 debug overlay · TAB threat overlay · SPACE pause · `,`/`.` speed · F12 screenshot.

---

## Adding verification for your own work

**A bench scenario** — add an entry to `bench_scenarios()` in `src/app/Modes.cpp`
(orchestrator-approved; it is a shared file).

**A sim-test** — add `tests/scripts/<name>.json`. Schema v1:

```json
{
  "schema": 1,
  "name": "macrophage_thins_a_horde",
  "seed": 42,
  "ticks": 600,
  "max_chaff": 20000,
  "actions": [
    { "tick": 0,  "type": "spawn_chaff", "family": "virus", "count": 2000,
      "pos": [16, 72], "radius": 4 },
    { "tick": 30, "type": "place_tower", "tower": "macrophage", "pos": [100, 72] }
  ],
  "assertions": [
    { "tick": 600, "metric": "chaff_count",   "op": "<",  "value": 500 },
    { "tick": 600, "metric": "objective_integrity", "op": ">", "value": 50 }
  ]
}
```

Metrics: `chaff_count`, `named_count`, `total_density`, `objective_integrity`,
`chaff_killed_total`, `chaff_leaked_total`, `tick`, `state_hash`.
Operators: `==` `!=` `<` `<=` `>` `>=`.
Families: `virus` `bacteria` `fungal_spore` `parasite` `cancer_cell` `allergen`.

**A unit test** — add `tests/test_<subject>.cpp` and list it in
`tests/CMakeLists.txt`. Test names must not start with `-` (CTest passes the name
to the binary and Catch2 would read it as a flag).

---

## Reporting

When you finish, report:

1. **What you implemented**, by file.
2. **Build status** — the actual `ctest` summary line.
3. **Perf** — the actual `--bench` JSON, with the relevant budget lines called out.
4. **Behaviour** — `--sim-test` output and exit codes.
5. **Visuals** — the screenshot you read back, described in your own words.
6. **Anything you could not do**, plainly. A known gap reported honestly is worth
   far more to the orchestrator than a success that turns out not to be one.

If something in a frozen header genuinely blocks you, say so explicitly and
propose the exact signature change. Do not work around it silently.
