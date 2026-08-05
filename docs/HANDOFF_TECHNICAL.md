# IMMUNE — Technical Handoff

**Read this if you're picking up engineering work with no prior context.** It gets you from a cold clone to a running, verified build, tells you exactly what's real vs. stubbed module by module, and lists the gotchas that cost real time to discover. Pair it with [`HANDOFF_GAME_DESIGN.md`](HANDOFF_GAME_DESIGN.md) (what to build), [`ARCHITECTURE.md`](ARCHITECTURE.md) (why the contracts are shaped this way), [`CONVENTIONS.md`](CONVENTIONS.md) (code style/patterns), and [`AGENT_BRIEF.md`](AGENT_BRIEF.md) (the standing rules if you're running parallel sub-agents — still accurate, still worth using).

---

## 1. Environment (verified working on this machine)

- MSVC 14.44 (VS 2022 Community), Windows SDK 10.0.26100, CMake, Ninja (bundled with VS)
- vcpkg at `C:\dev\vcpkg`, `VCPKG_ROOT` env var set — bootstrap with `bootstrap-vcpkg.bat` if starting fresh, manifest mode (`vcpkg.json`), no `vcpkg integrate install` needed
- RTX 3070 (or any GL 4.5-capable GPU)

## 2. Build, in one shot

```bat
tools\build.bat all windows-release
```

This configures, builds, and runs `ctest`. First run pulls ~10 dependencies via vcpkg and takes a while; cached after that. Binaries land at `%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe` (and `immune_tests.exe`).

**Two presets exist** (`CMakePresets.json`): `windows-release` (Ninja, single-config, binaries in `bin/` directly — this is the one everything in this doc assumes) and `windows-debug` (Visual Studio generator, multi-config, binaries in `bin/Debug/`). They use different generators on purpose; don't "fix" this without checking `HANDOFF_TECHNICAL.md`'s note in §4 first, it was a deliberate, verified split.

### Gotchas that will cost you time if you don't know them

1. **Every `cmake`/`ninja` invocation needs the VS2022 x64 dev environment** (`LIB`/`INCLUDE` set) or linking fails with `LNK1104: cannot open file 'kernel32.lib'`. `tools\build.bat` already handles this (`call vcvars64.bat` internally) — use it, don't call `cmake` raw from an arbitrary shell.
2. **`C1041: cannot open program database ... .pdb`** — a stale `mspdbsrv.exe` from an earlier, unrelated build session is almost always the cause (it's the helper `/FS` needs to serialize concurrent PDB writes across parallel compiles). Fix: `taskkill /IM mspdbsrv.exe /F` and retry.
3. **`imgui.ini` gets written to the working directory at runtime** (ImGui's own window-layout cache). It's gitignored; if you see it in `git status`, something's misconfigured, not you.
4. Source tree is inside OneDrive; **build output deliberately lives outside it** (`%LOCALAPPDATA%\horde-build\`) to avoid sync-lock build failures. Don't redirect `binaryDir` back into the repo.

## 3. Verify a build is actually good — the four tools that matter

The executable ships headless verification modes specifically so you (or an agent) never have to eyeball a window to know if something works:

```bat
set IMMUNE=%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe

:: Full regression suite
%LOCALAPPDATA%\horde-build\windows-release\bin\immune_tests.exe

:: Perf gate (the §8.6 numbers) — combined chaff_update+render_submit must stay under 4ms
%IMMUNE% --bench chaff10k --ticks 600 --quiet

:: Scripted behavior, exit 0/1
%IMMUNE% --sim-test tests\scripts\tower_thins_horde.json --quiet

:: Deterministic single-frame capture — then actually READ the PNG, don't just check exit code
%IMMUNE% --screenshot assets\levels\capillary_test.json --scenario mixed --tick 120 --out shot.png
```

As of this handoff: **189/189 tests pass**, perf gate holds with ~3-4x margin (chaff_update+render_submit ≈1.16-1.18ms avg vs. 4ms budget), both sim-test and screenshot paths work.

**VS Code**: `.vscode/tasks.json` and `.vscode/launch.json` wrap all of the above as one-click tasks (`Ctrl+Shift+P` → "Run Task" → "Verify Everything (Release)" is the single best one). Note **`.vscode/` is gitignored** — these exist locally on this machine but aren't in the repo; recreate them (or `git add -f` if you want them tracked) if working from a fresh clone.

## 4. What's real vs. stubbed — module by module

This is the ground truth as of commit `bba6429`. "Real" means implemented and test-covered, not just compiling.

| Module | Status | Notes |
|---|---|---|
| `core/*` | ✅ Real | Fixed clock, seeded PCG32 RNG (`Rng::fork()` for deterministic parallel streams), arena allocator, job system, profiler |
| `platform/*` | ✅ Real | SDL2 window/GL4.5 context, input (now with an ImGui raw-event hook, see below) |
| `sim/flowfield/*` | ✅ Real | Spline rasterization (`TissueRaster.h`), 8SSEDT distance field, multi-goal Dijkstra flow field, **exact** incremental rebake (proven equal to full rebake, not approximated) |
| `sim/chaff/*` | ✅ Real | SoA buffers, uniform-grid spatial hash, movement kernel, MSVC auto-vectorization confirmed via disassembly |
| `sim/damage/*` | ✅ Real | Both `DensityThinning`/`ProbabilisticRemoval` modes, all 4 field shapes (circle/rect/cone/chain) |
| `sim/ecs/*` | ✅ Real | Deterministic system scheduler, AI state machine, telegraph primitive, named-agent cap enforcement |
| `render/*` | ✅ Real for chaff/tissue/entities | Instanced draw per family, density-LOD blob crossfade (mass-conservation proven), tissue substrate. **`submit_fields()` is a no-op** — see `HANDOFF_GAME_DESIGN.md` §5 |
| `game/level/*` | ✅ Real | JSON parsing, vessel rasterization → tissue/flow-field baking, ECS `Objective` entity spawning, portal storage |
| `game/towers/*` | ✅ Real | Placement/validation/upgrade/sell/targeting, all 8 towers with distinct mechanics (see design handoff §3 for simplifications) |
| `game/enemies/*` | ✅ Real | 6-family roster, 3 elites. Fungal hazard and allergen modifier are parsed/flagged but not wired — see design handoff §4 |
| `game/wave/*` | ✅ Real | `Prep→Spawning→Clearing→Complete` state machine, spawns from real level portals. `WaveDef::modifier` and `atp_reward` are parsed but unused — no code path consumes them yet |
| `game/economy/*` | ✅ Real | Full ATP ledger (passive income, kill credit, spend/refund) — this was real from Wave 0, not a later addition |
| `game/meta/*` | ✅ Real (ledger only) | Unlock/loadout bookkeeping works; no content drives it yet (everything unlocked by default except tower gating) |
| `ui/Hud.cpp` | ⚠️ Partial | Real ImGui build menu, click-to-place, status readout. **No tower selection/upgrade/sell UI, no threat overlay** (needs a lane data model that doesn't exist — see design handoff) |
| `audio/*` | ❌ Stub | Null mixer — `post()`/`update()` accept and discard everything. Call sites already exist in `App.cpp` waiting for real sound |
| Win/lose | ❌ **Missing entirely** | `GameStateId::LevelComplete`/`LevelFailed` exist, nothing ever requests them. See §6 below for exact hook points |
| Attack VFX | ❌ Missing | See `HANDOFF_GAME_DESIGN.md` §5 |

## 5. The parallel sub-agent workflow (if you want to keep using it)

This project was built almost entirely via parallel Claude Code sub-agents, coordinated by an orchestrator session (frozen-contract pattern). It worked well — worth continuing. The pattern, if unfamiliar:

1. **Wave 0** wrote every module's public header as a **frozen contract** (interface only, documented rationale, compiling stub `.cpp`) plus `docs/ARCHITECTURE.md`/`CONVENTIONS.md`/`AGENT_BRIEF.md`.
2. Each subsequent wave launches **N sub-agents in parallel**, each owning a disjoint set of `.cpp` files (never headers outside their own module) in the *same* working tree. No git worktrees needed — ownership discipline substitutes for it.
3. **Orchestrator-owned files** (`SimWorld.{h,cpp}`, `app/**`, root `CMakeLists.txt`) are off-limits to sub-agents; they report what wiring they need and the orchestrator (you, this time) makes the small connecting edits personally. This is *deliberate* — it's the seam where cross-module integration bugs would otherwise hide.
4. Every agent proves its own work headlessly (`--bench`/`--sim-test`/`--screenshot`, read the PNG back) before reporting done — "prove it, don't claim it" is rule 4 of `AGENT_BRIEF.md`.
5. The orchestrator **independently re-verifies** every completion (rebuild from scratch, re-run tests, don't trust the self-report) before considering a wave done.

If you spawn a new wave, brief each agent with: the exact frozen header(s) it's implementing, the file paths it owns, explicit "do not touch" boundaries, and pointers to already-built utilities it should reuse rather than reinvent (this cut agent exploration time substantially every time it was done well). `docs/AGENT_BRIEF.md` is the standing brief every agent should read first.

## 6. Concrete next steps (matches `HANDOFF_GAME_DESIGN.md` §7's priority order)

### 6.1 Win/lose state transitions (small, do this first)

Nothing currently calls `state_.request(GameStateId::LevelComplete)` or `LevelFailed`. The check belongs in `App::tick_sim()` (`src/app/App.cpp`), after `sim_.tick(...)`:
- **Fail**: `sim_.snapshot().objective_integrity <= 0.0f`
- **Complete**: `waves_.status().all_waves_complete && sim_.snapshot().chaff_count == 0` (don't declare victory while the last wave's stragglers are still alive)

`App.cpp` is orchestrator-owned per the pattern above — either do this yourself directly (it's genuinely small) or have a sub-agent propose the change and you apply it. `GameState.h`/`.cpp` need no changes; the enum and transition machinery already exist correctly.

### 6.2 Damage field + telegraph rendering

- `Renderer::submit_fields()` (`src/render/Renderer.cpp`) needs an actual draw pass — even a flat-alpha circle per field shape would be a massive experiential improvement over nothing. `assets/shaders/` already has the procedural-SDF pattern established (see `chaff.frag`/`entity.frag`) to follow.
- Telegraph rendering: `sim::ecs::NamedFrame::telegraphs` (populated correctly every tick in `NamedAgents.cpp`) needs to reach `Renderer::submit_entities()` or a new pass, and elites need their `comp::Sprite::atlas_index` actually set to `2` (diamond, already defined in `entity.frag`) when `AiState::Telegraphing`.

### 6.3 Everything else

See `HANDOFF_GAME_DESIGN.md` §7 for the full prioritized list (fungal hazard, allergen wiring, feel pass, first real level) — that document owns the "what and why," this one only owns the "where in the code."

## 7. Known technical debt (real, documented by the agents that found them — not guesses)

- **Determinism holds per fixed thread count, not across different thread counts.** Same seed + same `--threads N` → bit-identical, always (verified). Same seed with a *different* thread count can diverge slightly, because `ChaffSystem`'s RNG forking is keyed to parallel-range index, which shifts with thread count. `CONVENTIONS.md` currently overclaims "deterministic at any thread count" — worth correcting the doc rather than the architecture, since nothing in gameplay actually depends on cross-thread-count reproducibility.
- **`DamageStats::cells_touched` is a bounding-box estimate**, not an exact count — `SpatialHash`'s internal cell-gathering is private and wasn't exposed for this.
- **`TowerSystem::validate()`'s `WouldBlockAllPaths` check is a local ~14-cell-radius reachability search**, not a full-level graph search (see design handoff §3) — theoretically incomplete for pathological geometries, never observed to actually fail.
- **`populate_scenario()` (used by `--bench`/`--screenshot --scenario`) spawns chaff uniformly across world bounds**, not from portals along the flow field — fine for perf/stress testing, means screenshots taken this way show a scattered cloud rather than a horde following the vessel. Use a real level + Play mode (once win/lose exists) to see the intended flow.
