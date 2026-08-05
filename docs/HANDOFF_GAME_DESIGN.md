# IMMUNE — Game Design Handoff

**Read this if you're picking up design/content work with no prior context.** It tells you what the game is *supposed* to be, what it *actually* is right now, where they diverge, and what's worth deciding next. Pair it with [`HANDOFF_TECHNICAL.md`](HANDOFF_TECHNICAL.md) (the "how do I build/run/verify this" side) and [`../DESIGN.md`](../DESIGN.md) (the original vision doc — still the source of truth for anything this file doesn't call out as changed).

---

## 1. The one-paragraph pitch

IMMUNE is a lane-based tower defense where you play the immune system of a host body. Waves of pathogens (chaff — thousands of cheap, mass-simulated agents) flow along vessel-shaped paths toward an objective; you place immune-cell towers that erode the horde with aggregate damage fields rather than fighting individual units. The spectacle *is* the horde — thousands of agents reading as a coherent, carve-able mass at once. Full details: `DESIGN.md`.

## 2. What's actually true right now (verified, not aspirational)

As of commit `bba6429`, the game **launches, loads a level, spawns real waves, lets you place all 8 towers, and towers measurably kill chaff** — proven via `tests/test_wave_director.cpp` and `tests/scripts/tower_thins_horde.json` (400 agents → 15 agents from one placed Macrophage over 5 simulated seconds). 189/189 automated tests pass. See `HANDOFF_TECHNICAL.md` §5 for the module-by-module implementation ledger.

**What this is not yet:** a game you'd hand someone to *play* end to end. The two sharpest gaps:

- **There is no win or lose.** Objective integrity can hit 0%, every wave can clear — nothing transitions the game state. It just keeps running. `GameStateId::LevelComplete`/`LevelFailed` exist as enum values and are never requested.
- **Combat is nearly invisible.** The simulation is honest and correct (towers really do submit damage fields, elites really do have wind-up timers), but almost none of it renders. See §5.

## 3. Design decisions made during implementation (not in DESIGN.md — read before contradicting them)

DESIGN.md is the vision; these are calls made while building it, by sub-agents and the orchestrating session, where the design doc was silent or ambiguous. Treat them as the current default, not as immutable — but know they're *intentional*, not accidental, and changing them has ripple effects noted below.

- **Levels are vessel splines, not lanes.** DESIGN.md talks about "lanes"; the implementation has centerline splines with per-point width (`assets/levels/*.json`), rasterized into a tissue mask → flow field. A single winding vessel functions as an implicit lane. There is **no explicit multi-lane data model** — no per-lane identity, no per-lane threat readout. If "readable per-lane threat indicators" (DESIGN.md §2) matters, this needs a real design pass: what *is* a lane when the geometry is a free-form spline network with bifurcations, not a fixed set of parallel corridors? This is a design question, not just an implementation gap.

- **Chaff death has two damage modes, both implemented, no verdict yet.** `DensityThinning` (deterministic, smooth erosion) and `ProbabilisticRemoval` (grainier, whole-agent pops) both exist behind one switch exactly as DESIGN.md §10 asked for. **Nobody has done the "feel pass" to pick a default or per-tower assignment.** `DensityThinning` is currently the code default everywhere.

- **Objective integrity: 1 point lost per leaked chaff agent, flat.** Not in DESIGN.md at all (it doesn't specify a formula). This is a placeholder pending Wave 3A's real economy/difficulty tuning — a 100-agent leak currently empties a full-health objective in ~1 second, which is almost certainly too punishing for real play. Needs balancing once win/lose states exist to actually test against.

- **Wave generation is flat, not per-region.** `WaveDirector::generate()` produces an escalating virus→bacteria→fungal-spore table regardless of which region/level it's for. DESIGN.md §2's region table (skin → capillary → lymphatic → mucosal → organ chamber, each with a distinct lane character and teaching role) isn't reflected in wave composition at all yet — every level currently plays the same regardless of its intended teaching moment.

- **Tower mechanic simplifications** (documented in `TowerSystem.cpp`'s comments, made by the sub-agent that implemented it, each a genuine judgment call where DESIGN.md §5 was underspecified):
  - B-Cell "fires a homing antibody" → implemented as instant application of the Marked debuff, no actual projectile flight/travel time.
  - Mast Cell's density trigger threshold → reuses `range * kill_rate` since no dedicated field exists in `TowerStats`.
  - Complement Cascade's "chain-jump through dense clusters" → implemented as a bounded 8-link nearest-neighbor walk (2A's judgment call, in `DamageField.cpp`), not a specified algorithm.
  - `WouldBlockAllPaths` placement validation uses a local ~14-cell-radius reachability check around the footprint rather than a full-level graph search, since `SimWorld` doesn't retain full level topology at that granularity. In practice this means a tower placed in a very long, narrow dead-end could theoretically pass validation while still cutting off something far away — untested edge case, not confirmed broken, just not proven safe either.

- **Enemy roster's `EnemyRoster::apply_to_tuning()` derives everything from speed tier + behavior flags**, not hand-authored per-family numbers. This is a deliberate "roster and sim kernel can't disagree by construction" choice (see `HANDOFF_TECHNICAL.md`), but it also means per-family *feel* (how fast is "fast," how punishing is replication) has never had a tuning pass — it's whatever fell out of the derivation formula.

## 4. Content status

| Asset | Status |
|---|---|
| `default_test_level()` (hardcoded, not a JSON file) | Simple single winding vessel, portal→objective. Test fixture. |
| `assets/levels/capillary_test.json` | Single winding vessel, moderate width variation. Test fixture. |
| `assets/levels/chokepoint_pinch.json` | Deliberate narrow pinch (width 11→2.5→11). Verified via screenshot to actually look like a bottleneck. Reasonable template for a real chokepoint level. |
| `assets/levels/floodplain_mucosal.json` | Three wide vessels (18-22 wide) converging into one 24-wide trunk. Verified via screenshot. Reasonable template for a real floodplain level. |

**None of these are designed content** — they're verification fixtures built to prove the loader/flow-field pipeline works, not to be fun or teach anything. DESIGN.md §4's full region progression (skin epidermis tutorial → capillary chokepoints → lymphatic wide-flow → mucosal floodplain spectacle → organ-chamber boss hubs) has zero real levels. Wave 4A (~12 levels) hasn't started.

**Towers:** all 8 from DESIGN.md §5 exist with real, distinct mechanics (see §3 above for the simplifications). None have had a numeric balance pass. Upgrade tiers (2-3 per tower, per the design doc's "numeric scaling only, no new mechanics mid-tree" rule) exist as data but haven't been played against real waves to see if they feel right.

**Enemies:** all 6 families from DESIGN.md §6 exist. 3 elites (parasite burrower, Bacteria biofilm colony, Cancer Cell tumor). Two real gaps:
- Fungal spore's "leaves lingering hazard zone on death" (§6) — the `leaves_hazard` flag is set but nothing spawns a hazard when one dies. Small, well-scoped implementation task.
- Allergen's "curveball wave type... friendly overreaction risk/reward mechanic" (§6) — `WaveDef::modifier` (including `AllergenOverreaction`) is parsed but `WaveDirector::tick()` never reads it. A standalone `allergen_overreaction()` function exists in `EnemyRoster.cpp` but nothing calls it during play.
- Cancer tumor grows but has no actual consequence — "expands from within a defended zone if ignored" (§6) is missing the "if ignored, X happens" part entirely.

## 5. Visual feedback for combat — the single biggest experiential gap

This deserves its own section because it undercuts the game's core promise (§7: *"cause and effect must be visible from a zoomed-out view — this is the core spectacle promise"*).

- **Tower damage fields render nothing.** `Renderer::submit_fields()` is a literal no-op beyond incrementing a counter for the HUD. No AoE circle, no beam, no toxin cloud, no histamine bloom — a tower's actual kill zone is invisible. You'd see chaff density near a tower quietly decrease with zero on-screen cause.
- **Elite telegraphs are computed correctly and never drawn.** The sim tracks `ActiveTelegraph{target_point, radius, progress}` accurately every tick — real wind-up data, exactly per DESIGN.md's readability pillar intent. Nothing reads it for rendering. There's even a pre-built "diamond = telegraphed/alert" shader shape sitting unused in `entity.frag` — the plumbing was clearly anticipated and just never finished.
- **No impact/pop VFX** when an elite attack lands or an elite/boss dies (DESIGN.md §7's death-VFX tier 2/3).
- Chaff correctly has **no** per-agent death animation (that's intentional — §7 tier 1 is aggregate erosion only) but the erosion effect is supposed to read against a *visible* field boundary, which per the above doesn't exist. Right now "erosion" is just agents quietly vanishing.

This is essentially all of Wave 3D's original scope, still fully open, plus a smaller telegraph-rendering piece that could go to whoever ends up owning attack feedback (arguably 3B/3D boundary — your call).

## 6. Open design questions

From DESIGN.md §10, still genuinely open:
- Single-player campaign only, or endless/horde-survival as a secondary mode?
- Co-op?
- Precise input/targeting scheme for continuous placement — a build-menu-and-click MVP exists now (see technical handoff), never play-tested for feel.

New ones surfaced by implementation:
- **What is a "lane" when levels are free-form vessel splines with bifurcations, not fixed corridors?** Blocks the per-lane threat overlay (§2's readability pillar) until answered.
- **DensityThinning vs. ProbabilisticRemoval — pick one, or assign per-tower?** Both work; nobody has played them side by side.
- **What should the objective-integrity formula actually be?** Current 1-point-flat-per-leak is a placeholder, untested against real difficulty curves.
- **How aggressive should the allergen risk/reward be**, once it's wired up at all?

## 7. Recommended next priorities, roughly in order of "unlocks the most"

1. **Win/lose state transitions.** Small, sharp, and nothing else can really be *played* (as opposed to poked at) until the game can end. See `HANDOFF_TECHNICAL.md` for the exact hook points.
2. **Damage field + telegraph rendering.** Even a minimal circle-and-fade would transform how the game reads, and the data to draw from already exists correctly on the sim side.
3. **A feel pass**: play a level start-to-finish once win/lose and basic visual feedback exist, then tune objective-integrity loss, wave pacing, and pick a thinning-mode default.
4. Fungal hazard + allergen wiring (both small, well-scoped).
5. First real designed level (pick one region, make it actually teach something per §4's table) once the above makes "playing a level" a meaningful test.
