# IMMUNE — Coding Conventions

Binding for every wave. Where a convention exists to protect a performance or
determinism property, the reason is stated — follow the reason, not just the
letter.

---

## 1. Naming

| Thing | Style | Example |
|---|---|---|
| Namespace | `lower_snake` | `immune::sim`, `immune::render` |
| Type / struct / enum class | `PascalCase` | `ChaffBuffers`, `PathogenFamily` |
| Enumerator | `PascalCase` | `PathogenFamily::FungalSpore` |
| Function / method | `lower_snake` | `apply_density_loss()`, `sample_cost()` |
| Variable / parameter | `lower_snake` | `world_pos`, `kill_rate` |
| Private data member | trailing underscore | `count_`, `cell_size_` |
| Public SoA stream | bare `lower_snake` | `pos_x`, `density`, `flags` |
| Compile-time constant | `kPascalCase` | `kTicksPerSecond`, `kFamilyCount` |
| Macro | `IMMUNE_SCREAMING` | `IMMUNE_LOG_INFO`, `IMMUNE_PROFILE_SCOPE` |
| File | matches its primary type | `SpatialHash.h` / `.cpp` |

Everything lives under `namespace immune`, then a module namespace
(`sim`, `render`, `game`, `ui`, `audio`, `platform`, `app`). Free functions that
are implementation detail go in an anonymous namespace in the `.cpp`.

Includes use paths rooted at `src/`: `#include "sim/chaff/ChaffBuffers.h"`. Never
relative (`../`). Order: own header, then project headers, then third-party, then
standard library, each group separated by a blank line.

---

## 2. SoA layout rules (hot path)

These are not style preferences. DESIGN.md §8.2 makes them a hard constraint.

1. **Anything that exists in thousands is SoA.** Parallel `std::vector<T>`, one
   per field. No `struct Agent`, no `std::vector<Agent>`.
2. **Split vectors per axis** in the hottest streams (`pos_x`/`pos_y`, not
   `std::vector<Vec2>`). Early-outs test one axis, and SIMD wants scalar streams.
3. **Reserve once at level load.** A hot-path container must never grow. Hitting
   capacity is a reported failure, not a reallocation.
4. **Keep flags packed.** Per-agent state is a `u8` bitset, not a set of bools.
   Adding a seventh bool to a chaff agent is a design smell — reach for a flag or
   ask whether it belongs on chaff at all.
5. **Removal is swap-and-compact, once per tick.** Indices are stable *within* a
   tick and invalid across one. Nothing may cache a raw index across ticks; use a
   generational handle if you truly need identity.
6. **Iterate in index order.** Random access across a 10k array defeats the
   prefetcher and gives up the entire reason for SoA.

---

## 3. The hot path

The "hot path" is anything inside `SimWorld::tick()` and
`Renderer::submit_*()`. Inside it:

- **No allocation.** No `new`, `malloc`, `std::vector::push_back` that can grow,
  `std::string`, `std::function` construction, or `shared_ptr`. Use pre-reserved
  buffers or `core::Arena`.
- **No exceptions.** No `throw`, and no calls that can throw. Return values or
  status enums instead. (`/EHsc` stays on for third-party code; our code just
  doesn't use exceptions here.)
- **No virtual calls per agent.** Family behaviour is a flag bit and a table
  lookup, never a subclass.
- **No logging.** `IMMUNE_LOG_*` formats and locks. Log outside the tick.
- **No wall-clock reads** other than through `Profiler`/`ScopedTimer`, and never
  as an input to sim logic.
- **No `std::unordered_map` iteration** where behaviour depends on order.

Outside the hot path (loading, UI, tools) normal, readable C++ is expected. Do
not micro-optimize a level loader.

---

## 4. Determinism

The rules that make `--sim-test` and `--screenshot` meaningful:

1. Every random draw comes from an `Rng&` threaded through explicitly. No
   `rand()`, no `std::random_device`, no static generator, no thread_local RNG.
2. Parallel work forks its RNG per range index (`Rng::fork`), never shares one.
3. No floating-point accumulation into a shared variable from multiple threads —
   FP addition is not associative, so the result would depend on scheduling.
4. Sim logic never reads wall-clock time or frame rate. It sees `Tick` and
   `kFixedDt`.
5. Sim logic never reads input state. Player actions arrive as explicit commands.
6. Container iteration that affects sim state must be ordered. Sort before
   iterating an associative container.
7. Same seed + same script ⇒ same `SimWorld::state_hash()` at every tick, on any
   machine, at any thread count. If you break this, you have broken the project's
   verification substrate.

---

## 5. Error handling

- **No exceptions across module boundaries.** Constructors do not throw; two-phase
  init (`create()` / `init()` returning `bool` plus an `error()` accessor) is the
  house pattern for anything that can fail.
- **Return `std::optional<T>`** for "might not exist" reads (`read_text_file`).
- **Return a result struct** (`LevelLoadResult`, `PlacementQuery`) when the caller
  needs to know *why* it failed, especially when the UI must explain it.
- **`assert` for invariants you believe cannot be violated**; a runtime check plus
  a log for anything that depends on data or user input.
- Third-party code that throws (nlohmann::json parsing) is wrapped in a
  `try`/`catch` at the boundary and converted to a status.
- Never swallow an error silently. A stub that returns a default must say so in a
  comment naming the wave that owns it.

---

## 6. Headers and contracts

- Every interface header opens with a comment stating **what it is, who owns it,
  and why it is shaped that way**. The rationale is the point: it's what stops a
  later agent from "simplifying" a deliberate constraint.
- Headers listed in `docs/ARCHITECTURE.md` are **frozen**. Need a change? Ask the
  orchestrator. Never silently edit another agent's header.
- Prefer forward declarations in headers; include in the `.cpp`. `sim` headers in
  particular must not pull in `render` or SDL.
- Public constants that cross modules go in `core/Types.h`.
- `#pragma once`, never include guards.

---

## 7. Tests

- Catch2 v3, one `tests/test_<subject>.cpp` per subject, registered in
  `tests/CMakeLists.txt`, discovered by CTest.
- **Test names must not begin with `-`.** CTest passes the test name to the
  binary and Catch2 will parse a leading `--` as a flag. Write
  `"cli screenshot defaults to tick 0"`, not `"--screenshot defaults to tick 0"`.
- Tag tests: `[core]`, `[sim]`, `[render]`, `[app]`, plus `[determinism]` and
  `[bench]` where they apply.
- Test the **contract**, not the implementation: invariants, boundary conditions,
  and round-trips (`screen_to_world` inverts `world_to_screen`; same seed gives
  the same stream; parallel and serial `parallel_for` agree).
- Every stubbed subsystem gets its invariant tests written *now*, so the wave that
  implements it has an immediate signal.
- Behavioural verification that needs a running sim belongs in a
  `tests/scripts/*.json` sim-test, not in a unit test.
- The build must stay green. A red build blocks every sibling agent in your wave.

---

## 8. Comments

Explain **why**, not what. `// increment i` is noise; `// Do not advance i: the
swapped-in agent must be tested too` is the comment that prevents the next bug.

Mark unimplemented work with the owning wave, e.g.
`// Wave 1B: parallel_for over [0, count) doing flow sample + separation.`
Bare `TODO` without an owner is not acceptable in this codebase.

---

## 9. Build

- C++20, MSVC `/W4 /permissive-`. Warnings are signal; do not suppress them
  wholesale.
- One CMake library target per module; dependencies point one way only.
- All dependencies come from the vcpkg manifest. Do not vendor a library or add a
  submodule.
- Build output lives outside the source tree
  (`$LOCALAPPDATA/horde-build/<preset>`) because the repo sits inside OneDrive
  and a build directory there causes sync churn and locked-file failures.
- **No binary assets, ever.** No PNGs, WAVs, fonts, or meshes in the repo. All
  visuals are shader-generated; all audio is synthesized at runtime. The only
  files in `assets/` are `.glsl` and `.json`.
