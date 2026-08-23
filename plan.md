# IMMUNE — Full Implementation Plan

## Context

`DESIGN.md` is a complete game design document for **IMMUNE**, a lane-based tower defense where the spectacle is a 5,000–10,000-agent pathogen horde flowing through a body's vessel network at 60 FPS. It specifies gameplay, art direction, and — critically — a technical strategy (§8) built on a custom C++ engine: two-tier agents (SoA chaff vs. ECS named agents), flow-field movement instead of pathfinding, aggregate damage instead of per-unit hit registration, and instanced rendering with density-based LOD.

The working directory contains **only that document**. No code, no build system, no repo. This plan takes the project from zero to a complete, playable game: every system in the doc working end-to-end, all 8 towers, all 6 pathogen families, wave director, economy, meta-progression, and ~12 handcrafted levels across all 5 regions. Numeric balance is left to a tuning pass, per the doc's own closing note.

Implementation will be carried out by **sub-agents working in parallel**. That constraint shapes this plan as much as the design does: the architecture is split into modules with frozen interface contracts and strict file ownership so that four agents can work simultaneously without colliding, and the game is built to **verify itself headlessly** so each agent can prove its work without a human watching the screen.

### Verified environment

| Tool | Status |
|---|---|
| MSVC 14.44 (VS 2022 Community) | ✅ installed |
| Windows SDK | ✅ 10.0.26100 |
| CMake | ✅ 4.1.2 |
| Ninja | ✅ bundled with VS |
| Git, Python 3.13 | ✅ installed |
| GPU | ✅ RTX 3070 (GL 4.6 capable) |
| vcpkg | ❌ **must be bootstrapped — step 0** |

### Locked decisions

- **Renderer:** SDL2 (window/input/audio) + OpenGL 4.5 core with DSA, SSBOs, and compute shaders. Loader via `glad`.
- **Dependencies:** vcpkg manifest mode (`vcpkg.json`), toolchain wired through `CMakePresets.json`.
- **Assets:** fully procedural. Zero binary art or audio files — SDF/noise shaders for all visuals, runtime synthesis for all sound.
- **Scope:** all systems + full rosters + ~12 levels across all 5 regions.

---

## Foundational decisions this plan adds

These are not in `DESIGN.md` but are required to make parallel agent work tractable. They should be treated as binding.

**1. Deterministic fixed-timestep simulation.** Sim runs at a fixed 60 Hz tick, decoupled from render, driven by an explicit seeded PRNG passed through the sim context — no global `rand()`, no wall-clock reads in sim code. This makes runs reproducible, which is what makes every headless test below possible.

**2. The game must be able to verify itself.** Sub-agents cannot look at a screen. The executable therefore ships three non-interactive modes from Wave 0 onward:

- `immune --bench <scenario> --ticks N` — runs the sim headless, prints per-subsystem timings (chaff update, spatial hash, ECS, render submit) as JSON. This is how the §8.6 perf budgets get enforced continuously rather than discovered at the end.
- `immune --sim-test <script.json>` — scripted scenario (spawn X, place tower Y at t=N), asserts end-state invariants, exit code 0/1.
- `immune --screenshot <level> --tick N --out <file.png>` — renders one deterministic frame to PNG. **Agents can read PNG files directly**, so this is real visual verification, not a proxy for it.

**3. Levels are authored as splines, not painted masks.** A level is a JSON file defining vessel centerline splines with per-point width, plus spawn points, objective points, and placement zones. At load, splines rasterize into a tissue mask → signed distance field → flow field. This gives the organic, branching, variable-width vessels §4 calls for while remaining fully authorable in text by an agent with no visual editor. A `tools/preview_level.py` script renders a level JSON to a PNG for quick eyeballing.

**4. Build outputs live outside OneDrive.** The source tree is inside a synced OneDrive folder; a C++ build directory there would generate constant sync churn and can cause locked-file build failures. `CMakePresets.json` sets `binaryDir` to `$env{LOCALAPPDATA}/horde-build/${presetName}`.

**5. Contract-first parallelism.** Wave 0 writes `docs/ARCHITECTURE.md` plus **compiling, stubbed interface headers** for every module boundary. Later agents implement against frozen headers. An agent that needs a contract change requests it from the orchestrator rather than editing another agent's header.

---

## Repository layout

```
horde/
  CMakeLists.txt  vcpkg.json  CMakePresets.json  .gitignore  DESIGN.md
  docs/           ARCHITECTURE.md  CONVENTIONS.md  AGENT_BRIEF.md
  src/
    core/         types, math, fixed-step clock, log, arena alloc, job system, seeded RNG
    platform/     SDL window, GL context, input mapping, file I/O
    render/       GL wrappers, shader manager, camera, instanced sprite pass,
                  density-LOD blob pass, field-VFX pass, PNG capture
    sim/
      flowfield/  spline→mask→SDF→flow bake, incremental rebake, sampling
      spatial/    uniform grid hash
      chaff/      SoA agent buffers, movement, separation, spawn/despawn, replication
      damage/     aggregate damage fields, erosion accounting
      ecs/        EnTT world, components, system scheduler, named-agent AI
    game/
      towers/     placement, targeting, upgrades, abilities, the 8-cell roster
      enemies/    2 pathogen families; the elite tier is an empty framework
      wave/       wave director, spawn tables, difficulty curve
      economy/    ATP income, costs, refunds
      level/      level JSON schema, loader, objective/integrity meter
      meta/       unlocks, antibody-memory modifiers, save/load
    ui/           HUD, build menu, threat overlays, menus
    audio/        synth voices, mixer, event bus bindings
    app/          main, game loop, state machine, CLI modes
  assets/shaders/ *.glsl (the only "assets" that exist)
  assets/levels/  *.json
  tests/          Catch2 unit tests + bench scenarios + sim-test scripts
  tools/          preview_level.py, bench_report.py
```

### Dependencies (vcpkg.json)

`sdl2` · `glad[gl-api-45]` · `glm` · `entt` · `nlohmann-json` · `imgui[sdl2-binding,opengl3-binding]` (debug tooling + HUD backend) · `stb` (PNG capture) · `catch2` (tests). Audio uses SDL2's audio callback — no extra dependency.

---

## Execution: five waves

Each wave ends at an **integration gate** that the orchestrator (not a sub-agent) verifies. Agents within a wave run in parallel in the same working tree, each owning disjoint directories. Ordering follows §8.7's milestone advice: **prove the performance assumption before building content on top of it.**

### Wave 0 — Foundation (1 agent, serial, blocks everything)

Nothing can parallelize until the contracts exist.

- Bootstrap vcpkg (`git clone microsoft/vcpkg` → `bootstrap-vcpkg.bat`), set `VCPKG_ROOT`, wire the toolchain into `CMakePresets.json`.
- `git init` the project; `.gitignore` covering build outputs and vcpkg artifacts.
- Full directory skeleton, root `CMakeLists.txt` with one target per module, Catch2 test target.
- `src/core/`: fixed-step clock, logging, seeded PRNG, arena allocator, small job system (worker pool sized to hardware concurrency).
- `src/platform/`: SDL2 window + GL 4.5 core context + glad load + input mapping.
- `src/app/`: game loop with fixed sim tick / variable render, plus the three CLI modes (`--bench`, `--sim-test`, `--screenshot`) wired as real entry points even though they have almost nothing to drive yet.
- **`docs/ARCHITECTURE.md` + stubbed interface headers for every module below.** This is the wave's most important output.
- `docs/CONVENTIONS.md`: naming, SoA layout rules, no-exceptions-in-hot-path, error handling, test conventions.
- `docs/AGENT_BRIEF.md`: the standing rules every sub-agent reads first (file ownership, frozen headers, leave-build-green, how to run bench/tests).

**Gate:** clean configure + build from a cold clone; window opens showing a cleared frame; `--screenshot` writes a readable PNG; `ctest` passes; `--bench` prints a JSON timing block.

---

### Wave 1 — Core tech (4 agents in parallel)

This wave is where the project succeeds or fails. Everything after it is comparatively ordinary game code.

| Agent | Owns | Delivers |
|---|---|---|
| **1A — Flow field** | `sim/flowfield/`, `tools/preview_level.py` | Spline→tissue-mask rasterization, SDF, flow-field bake (cost field + Dijkstra/eikonal sweep + gradient), bilinear sampling, **incremental region rebake** on tower placement. Unit tests on hand-built topologies; PNG dumps of field vectors. |
| **1B — Chaff sim** | `sim/chaff/`, `sim/spatial/` | SoA buffers (pos, vel, family, density-HP, flags), uniform-grid spatial hash with per-frame rebuild, flow sampling + separation impulse, spawn/despawn/compaction, job-system parallelism, SIMD where it pays. |
| **1C — Renderer** | `render/`, `assets/shaders/` | GL wrappers (DSA), shader manager with hot-reload, tilted-topdown camera, **instanced sprite pass** (one draw call per family, per-instance buffer from SoA), **density-LOD blob pass** with seamless crossfade, PNG capture. |
| **1D — ECS layer** | `sim/ecs/` | EnTT world, component set (transform, health, AI state, telegraph), system scheduler with explicit ordering, named-agent update loop, one placeholder elite that walks the flow field. |

**Gate (the critical one):** `--bench chaff10k` sustains 10,000 agents with flow sample + separation + instanced render under **4 ms/frame**, spatial hash under **1 ms**, 200 named agents under **2 ms**, on this RTX 3070 / MSVC release build. A screenshot shows a coherent river of agents flowing along vessels and through a bifurcation. **If this gate fails, stop and revisit §8.2 before proceeding** — every later wave assumes it holds.

---

### Wave 2 — Gameplay systems (4 agents in parallel)

| Agent | Owns | Delivers |
|---|---|---|
| **2A — Damage & erosion** | `sim/damage/` | Aggregate damage fields (region + kill-rate), density-threshold thinning of chaff inside fields, kill accounting feeding economy, the edge-erosion visual language of §7. Resolves the open question in §10 by implementing both probabilistic removal and deterministic density thinning behind one switch, then picking by feel. |
| **2B — Tower framework + roster** | `game/towers/` | Grid-free continuous placement with light snapping to valid tissue zones, placement validation + flow-field rebake trigger, spatial-hash-driven targeting, 2–3 upgrade tiers each, one active ability cap. All 8 cells: Macrophage, Neutrophil (+NETs, micro-unit spawns), Dendritic, Cytotoxic T, B-Cell/antibody homing, NK, Mast, Complement Cascade (chain-jump through clusters). |
| **2C — Enemy roster** | `game/enemies/` | 2 families (virus, bacteria) with §6's color/silhouette/tempo rules; virus replication driving exponential pressure. The elite/boss tier is a registered-but-empty framework pending a roster redesign (DESIGN.md §14). |
| **2D — Level pipeline** | `game/level/`, `assets/levels/` | Level JSON schema + loader + validation, spawn points, objective structure and integrity meter, placement-zone definition, and 2 test levels (one switchback, one floodplain) that later content agents use as templates. |

**Gate:** `--sim-test` scripts prove each tower measurably thins a horde; a screenshot shows a damage field visibly carving the pathogen river; a tower placed in a lane causes a visible reroute; bench still inside budget with towers active.

---

### Wave 3 — Shell, feel, and spectacle (4 agents in parallel)

| Agent | Owns | Delivers |
|---|---|---|
| **3A — Wave director & economy** | `game/wave/`, `game/economy/` | Wave composition tables per region, difficulty curve, inter-wave timing, ATP passive + per-kill income, costs/refunds, win/lose conditions on organ integrity. |
| **3B — UI/HUD** | `ui/` | Build menu, tower info + upgrade panel, resource and wave-preview readouts, pause/speed controls, and the §2 readability layer: per-lane threat indicators that stay legible at 10k agents. |
| **3C — Audio** | `audio/` | Procedural synth voices (noise/FM/subtractive), SDL2 audio callback mixer, event-driven SFX bound to the game event bus, adaptive intensity music layer keyed to horde density. |
| **3D — VFX pass** | `render/` (VFX passes only), `assets/shaders/` | Field VFX as render-target shader effects per §8.5: toxin clouds, histamine blooms, antibody tides, complement lightning cascade; tissue substrate with heartbeat pulse; parallax cytokine particulate; the three-tier death VFX budget of §7. |

**Gate:** a full level is playable start to finish with sound and HUD; screenshots at several ticks show the promised spectacle reading clearly from a zoomed-out view.

---

### Wave 4 — Content, meta, and tuning (3 agents, then serial polish)

| Agent | Owns | Delivers |
|---|---|---|
| **4A — Campaign content** | `assets/levels/` | ~12 levels across all 5 regions with escalating lane character per §2's table, region-appropriate wave tables, campaign flow and unlock ordering. |
| **4B — Meta-progression** | `game/meta/` | Cell-type unlocks, passive antibody-memory modifiers as a pre-run loadout, per-region cosmetic skins, versioned save/load. |
| **4C — Test & profiling hardening** | `tests/`, `tools/` | Fill unit-test gaps, add regression sim-tests per tower/enemy, `tools/bench_report.py` to track perf across commits and catch regressions. |

**Final serial pass (orchestrator + 1 agent):** balance tuning against real playthroughs, the §10 feel decisions, bug fix, and a `README.md` covering build and run.

---

## Sub-agent coordination rules

To be restated in every agent prompt and codified in `docs/AGENT_BRIEF.md`:

1. **Own your directories, touch nothing else.** The tables above are authoritative. Same working tree, disjoint paths — no git worktrees needed unless collisions actually appear.
2. **Interface headers are frozen at wave start.** Need a contract change? Report it up; the orchestrator amends and re-broadcasts. Never silently edit another agent's header.
3. **Leave the build green.** Configure, build, and `ctest` must pass before you report done. A red build blocks every sibling agent in your wave.
4. **Prove it, don't claim it.** Every agent finishes by running `--bench`, `--sim-test`, or `--screenshot` and reporting actual output. Visual work must include a screenshot the agent has read back and described.
5. **Perf budgets are acceptance criteria, not aspirations.** If your change pushes `--bench` over §8.6's budget, that's a failure to report, not a footnote.
6. **Data-oriented in the hot path.** No per-agent virtual calls, no per-enemy heap allocation, no OOP-per-chaff-unit. §8.2 is a hard constraint.

---

## Verification

**Per-agent, continuous:**
```
cmake --preset windows-release && cmake --build --preset windows-release
ctest --preset windows-release
immune --bench chaff10k --ticks 600        # JSON timings vs §8.6 budgets
immune --sim-test tests/scripts/<case>.json
immune --screenshot <level> --tick 300 --out shot.png   # then Read the PNG
```

**Per-wave gate (orchestrator):** run the wave's gate criteria above, plus launch the game interactively and confirm it plays. The `--screenshot` path is the primary tool for reviewing sub-agent visual work without trusting a text description of it.

**Whole-game acceptance:**
- Campaign playable start to finish, all 5 regions, no crashes.
- All 8 towers and 6 pathogen families functional and visibly distinct.
- A mucosal floodplain level sustains ≥10,000 concurrent chaff agents at 60 FPS.
- Perf budgets from §8.6 met on this machine, tracked by `tools/bench_report.py`.
- Cold-clone build works from `README.md` instructions alone.

---

## Risks

| Risk | Mitigation |
|---|---|
| 10k-agent budget missed at Wave 1 gate | Gate is deliberately first. Fallback ladder: move chaff update to compute shader → widen density-LOD threshold → reduce separation query radius. Reassess scope before Wave 2 rather than after. |
| Incremental flow-field rebake too slow on tower placement | Budget rebake across frames with a stale-field grace period; agents keep flowing on the old field for a tick or two, which is also visually fine. |
| Contract churn stalls parallel agents | Wave 0 spends real effort on `ARCHITECTURE.md`; any change goes through the orchestrator and is broadcast to the whole wave at once. |
| Fully procedural visuals read as muddy at density | §6's color/silhouette/tempo rule is an explicit acceptance criterion at every screenshot review, not a late art judgment. |
| OneDrive sync interferes with builds | `binaryDir` points outside OneDrive from Wave 0. |