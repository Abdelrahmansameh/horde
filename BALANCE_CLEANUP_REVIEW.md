# Balance cleanup deletion review

Pending your approval. No existing files have been edited or deleted. This checklist is the only new file created for this review.

The proposal separates three scopes:

- **A — Balance documentation:** remove the existing balancing approach, bot strategies, tuning experiments, telemetry instructions, difficulty targets, and old balance conclusions.
- **B — General test documentation:** also remove test guides, test inventories, and verification recipes. This is broader than balancing, so it has its own approval scope.
- **C — Actual tools and implementation:** optional deletion of source files, scripts, and generated reports. Approval of A or B does not authorize C.

All boxes are pending. Line numbers refer to the working copy inspected on October 17, 2026; section names identify the intended content if lines move. Several candidates already contain local changes, and some are untracked.

## A Balance documentation

### Whole file

- [ ] **A1 DELETE [docs/BALANCE.md](docs/BALANCE.md).** Removes automatic runs, bot profiles and placement strategy, sweeps, telemetry interpretation, harness limitations, and the tuning workflow. The file also contains an engine performance benchmark section at lines 108–131; whole-file deletion removes that documentation too.

### Sections and references

Keep each file below and remove only the listed content.

| Pending | File | Content marked for removal |
| --- | --- | --- |
| A2 | [docs/CAMPAIGN_DESIGN.md](docs/CAMPAIGN_DESIGN.md) | **Review of the previous levels** (16–33); the **Investment rewarded** column (39–50); the numeric difficulty target curve and interpretation (52–67); **Verification for future edits** (69–73); **Playtest observations after rebuilding** (75–83); final test counts and autoplay conclusions (97–99). Retain current geometry descriptions and spatial lessons. |
| A3 | [docs/TOOLS.md](docs/TOOLS.md) | `config_editor.py` and `balance_sweep.py` catalog rows (8–9); sweep help example (21); balance portions of report references/output instructions (25–27); **Browser tuning editor** (29–42). Adjust the opening description. Keep build and asset-generation helpers. |
| A4 | [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Balance navigation link (3); browser tuning editor instructions (82–89); copied-config experiments (98–99); console tuning instructions (101–120); **Dumps and reproducibility** workflow (122–135); **Adding a tunable** procedure (137–149). Retain runtime file ownership, parsing, value application order, and factual reload/hash behavior where needed. |
| A5 | [docs/GYM.md](docs/GYM.md) | Tuning-experiment wording (3, 63); `autoplay` / `bot` command (78); config manipulation instructions (88–99); config rows and autoplay callback wording in the context table (109–112). Keep ordinary console, editor, movement, and VFX reference material unless B is also approved. |
| A6 | [docs/PROGRESSION.md](docs/PROGRESSION.md) | Price/effect editing guidance and finite Antibody budget prescription (274–280). Keep the current progression rules, costs, effects, and save format. |
| A7 | [docs/BUILDING.md](docs/BUILDING.md) | Autoplay mode row (119); autoplay wording in `--level` (133); `--profile`, `--report`, and `--max-ticks` rows (156–158); bot-result/exit-status explanation (161). |
| A8 | [docs/LEVELS.md](docs/LEVELS.md) | Autoplay comparison in the fallback paragraph (41); final headless-parity/Balance sentence (142); comparative autoplay recommendation (174). Retain authored level rules and schemas. |
| A9 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/SIMULATION.md](docs/SIMULATION.md) | Bot/telemetry workflow wording in the architecture inventory and runner guidance (ARCHITECTURE 28, 104–106); bot placement wording (SIMULATION 292); optional per-owner balance telemetry explanation (335–336). Preserve shared session order, ATP accounting, owner identities, and combat events used by gameplay or rendering. |
| A10 | [README.md](README.md), [docs/README.md](docs/README.md), [plan.md](plan.md), [docs/AGENT_BRIEF.md](docs/AGENT_BRIEF.md) | Balance/tuning-helper descriptions; Balance navigation and source-map entries; autoplay/sweep/telemetry inventory and old harness conclusions. Main anchors: README 60; docs index 31, 65, 72; plan 23, 41–43, 48–50; agent brief 39–41. Split mixed sentences so useful build/gameplay information survives. |
| A11 | [.agents/skills/run-immune/SKILL.md](.agents/skills/run-immune/SKILL.md), [.claude/skills/run-immune/SKILL.md](.claude/skills/run-immune/SKILL.md) | Both copies: Balance reference (20), autoplay example (98), autoplay wording (78, 104–105), and balance experiment/report guidance (109–110). Keep launch/build/capture instructions. |

- [ ] **A12 CLEAN LINKS.** Remove remaining links to `BALANCE.md` and instructions sending readers to the retired workflow. Check root documents, `docs/`, and both local skill copies. Update mixed descriptions without claiming the implementation was removed.

## B General test documentation

This scope removes documentation for general engineering tests as well as balance-related tests. The actual tests and runners stay in place unless separately approved under C.

### Whole file

- [ ] **B1 DELETE [docs/TESTING.md](docs/TESTING.md).** This includes Catch2/CTest, simulation-script schema/actions/assertions, GL tests, screenshot recipes, content/performance checks, and reproducibility procedures. These are not all balancing tools.

### Sections and references

| Pending | File | Content marked for removal |
| --- | --- | --- |
| B2 | [README.md](README.md), [docs/README.md](docs/README.md) | Test/simulation guide links, the documented tests inventory, and test source-map entries. README 46–48, 59; docs index 26, 50, 81, 87, 96. Retain normal launch and editor links. |
| B3 | [docs/AGENT_BRIEF.md](docs/AGENT_BRIEF.md), [docs/CONVENTIONS.md](docs/CONVENTIONS.md) | Test/benchmark command recipes and runner-selection procedures (AGENT_BRIEF 22, 28–29, 39–49); **Tests and evidence** (CONVENTIONS 88–104); related test-upkeep links and prescriptions. Keep task scope, build setup, coding conventions, and runtime contracts. |
| B4 | [docs/BUILDING.md](docs/BUILDING.md) | Dedicated CTest/test-executable recipes and test discovery instructions (29, 39–47, 58, 66, 73, 76–78, 86, 89); bench/sim-test/scenario mode rows (116–117, 120), benchmark-only option descriptions (148, 150), and test navigation. Keep accurate compilation, launch, and wrapper descriptions. |
| B5 | [docs/GAMEPLAY.md](docs/GAMEPLAY.md) | **Where to verify a change** (125–134). |
| B6 | [docs/IMMUNE_CELLS.md](docs/IMMUNE_CELLS.md), [docs/PATHOGENS.md](docs/PATHOGENS.md), [docs/ABILITIES_ECONOMY.md](docs/ABILITIES_ECONOMY.md) | **Verification anchors** sections (IMMUNE_CELLS 187–196; PATHOGENS 135–146; ABILITIES_ECONOMY 124–131), plus surrounding test-fixture guidance. Keep the actual cell, pathogen, ability, and economy behavior. |
| B7 | [docs/PROGRESSION.md](docs/PROGRESSION.md), [PROGRESSION.md](PROGRESSION.md) | Test inventory in **Verification and safe extensions** (266–272), and the root overview's “testing anchors” description (8–10). A6 covers the remaining balancing guidance in that section. |
| B8 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Test links in core-service rows (132–139); **Performance evidence** (146–172); runner/test comparison guidance and Testing links (104–106, 125–128, 186). Retain core API contracts, runtime dependencies, and factual hash limitations. |
| B9 | [docs/SIMULATION.md](docs/SIMULATION.md) | Distributed test/source links (96–98, 131–133, 146, 179–183, 203–205, 218–219, 242–245, 278–281, 311–312, 329, 346) and the Testing/runner reference (367). Keep simulation mechanics and runtime determinism limitations. |
| B10 | [docs/RENDERING.md](docs/RENDERING.md) | Test inventories and links (49, 80–81, 112–115, 140–142, 165–167, 179–180, 208–210, 224); verification recipes/advice in **Captures and verification** (227–251). Retain screenshot runtime behavior where useful, and combat-event/VFX implementation details. |
| B11 | [docs/AUDIO_PLATFORM.md](docs/AUDIO_PLATFORM.md) | Sim-test/benchmark setup and test references (24–26, 34–36, 62–64, 100–102, 179–180); test steps/inventory in **Adding or verifying a sound** (194–208). Keep platform/audio implementation and ordinary sound-authoring information. |
| B12 | [docs/UI_FRAMEWORK.md](docs/UI_FRAMEWORK.md), [docs/LEVEL_EDITOR.md](docs/LEVEL_EDITOR.md) | Scripted UI/gym fixture recipes (UI_FRAMEWORK 75–90), test/fixture inventories (177–190), editor test-fixture guidance and test inventory (LEVEL_EDITOR 123, 141). Keep player UI, normal GUI editor controls, and playtest/save behavior. |
| B13 | [docs/GYM.md](docs/GYM.md) | Headless-test context matrix (102–115), scripted UI fixtures (117–140), **Useful recipes** (142–158), and test prescriptions in **Extending the console** (162). Keep basic manual console command reference; deleting the entire gym guide is a separate choice. |
| B14 | [docs/LEVELS.md](docs/LEVELS.md), [docs/TOOLS.md](docs/TOOLS.md), [docs/ASSETS.md](docs/ASSETS.md) | Test/performance fixture catalog entries and runner-fallback instructions (LEVELS 26–33, 41), test/autoplay authoring step (174); test-produced flow-debug recipe (TOOLS 44–68), test/performance portions of **Routine verification** (127–140); GL-test instruction and Testing link (ASSETS 88, 94–95). Retain level schemas/validation and asset generation. |
| B15 | [DESIGN.md](DESIGN.md), [plan.md](plan.md), both local run-immune skill copies | General test authority/inventory references, benchmark/test recipes, and old verification conclusions. Retain game design and ordinary build/launch guidance. |

- [ ] **B16 CLEAN LINKS.** Remove all remaining `TESTING.md` links, including the navigation link in [docs/PUBLISHING.md](docs/PUBLISHING.md). Preserve publishing/standalone packaging checks, licenses, and release instructions; those serve distribution rather than game balancing.

## C Optional implementation and artifact removal

These are inspection candidates only. Each item requires explicit approval beyond documentation cleanup.

### Dedicated balance files

- [ ] **C1 DELETE these nine files as one connected removal:**
  - [tools/balance_sweep.py](tools/balance_sweep.py)
  - [src/app/AutoplayMode.cpp](src/app/AutoplayMode.cpp)
  - [src/app/AutoplayMode.h](src/app/AutoplayMode.h)
  - [src/game/autoplay/AutoPlayer.cpp](src/game/autoplay/AutoPlayer.cpp)
  - [src/game/autoplay/AutoPlayer.h](src/game/autoplay/AutoPlayer.h)
  - [src/game/telemetry/RunTelemetry.cpp](src/game/telemetry/RunTelemetry.cpp)
  - [src/game/telemetry/RunTelemetry.h](src/game/telemetry/RunTelemetry.h)
  - [tests/test_autoplay.cpp](tests/test_autoplay.cpp)
  - [tests/test_autoplay_level_rules.cpp](tests/test_autoplay_level_rules.cpp) — currently untracked.

  This also requires partial edits to `CMakeLists.txt`, `.gitignore`, `src/app/App.{cpp,h}`, `src/app/Cli.{cpp,h}`, `src/app/main.cpp`, and `src/game/gym/GymCommands.{cpp,h}`. Remove bot registration, CLI dispatch/options, interactive callbacks, and obsolete comments in `src/app/Modes.h` and `src/game/session/LevelSession.h`. Keep the shared gameplay session and configuration code.

- [ ] **C2 DELETE balance attribution instrumentation.** Candidate file: [src/sim/Attribution.h](src/sim/Attribution.h). Requires partial edits to `src/sim/damage/DamageField.{cpp,h}`, `src/sim/projectile/Projectiles.{cpp,h}`, `src/sim/swarm/Swarmers.{cpp,h}`, and `src/sim/fluid/Fluid.{cpp,h}`. Preserve gameplay ownership, damage/economy behavior, combat events, and family counters used by the result screen.

- [ ] **C3 DELETE generated balance outputs.** [tools/balance/](tools/balance/) currently contains 164 ignored files: 162 run JSON reports, `summary.csv`, and `summary.md`. This is local output and is outside the tracked documentation cleanup. Historical campaign evidence under `scratch/level-review/` is also related, but its mixed screenshots/logs/data need a separate exact-file inventory before deletion.

### Other tools and tests requiring a scope choice

- [ ] **C4 DELETE the manual tuning editor:** [tools/config_editor.py](tools/config_editor.py). Also update its launch entries in `.vscode/launch.json` and any affected task wiring.
- [ ] **C5 REMOVE selected old balance assertions:** population/difficulty curve assertions in [tests/test_campaign_design.cpp](tests/test_campaign_design.cpp), and [tests/scripts/config_tuning_changes_the_sim.json](tests/scripts/config_tuning_changes_the_sim.json). Keep the campaign test's routing, spawn, camera, and geometry checks. `test_campaign_design.cpp` is currently untracked.
- [ ] **C6 DELETE the entire gym documentation:** [docs/GYM.md](docs/GYM.md), instead of the A5/B13 trims. Also remove Gym links from the docs index, root README, level editor/UI docs, and local skill copies. This would remove manual editor/rendering/console reference material too; actual gym code remains unless separately inventoried and approved.

The following are **kept by default**: `tools/bench_report.py`, `tools/bench_history.csv`, engine benchmark and sim-test runners, screenshots, gym implementation, normal build tooling, and `.vscode` performance/sim-test tasks. Removing those systems would require a further exact deletion list. There are **69 C++ test files and 18 simulation scripts** overall; most cover ordinary engine/game/UI/editor correctness rather than balancing.

## Content retained

- Current gameplay rules, cell/pathogen behavior, abilities, economy, progression, and save formats.
- Existing numerical defaults, config JSON, campaign level JSON, and runtime schema/loader facts. Discarding the balancing workflow does not reset the game's current values.
- Normal building and launch, GUI level editing, structural level validation, publishing, assets, shaders, and licenses.
- Shared simulation/session logic, rendering events, gameplay owner IDs, and generic result counters.

## Approval choices

- **A only:** clear the balancing approach and its documentation while keeping general test documentation.
- **A and B:** also clear the general test/verification documentation.
- **Add specific C items:** remove only the named implementation/tools/artifacts as well.

Individual rows can be excluded or revised before approval. No deletion is authorized by this checklist itself.
