# Balance cleanup record

**Documentation and balancing-code cleanup completed on October 21, 2026. Existing generated reports are retained.**

The approved A/B changes removed the marked balancing guidance from documentation. Your follow-up authorized retirement of the related code and tools. Existing working-copy edits, gameplay data, configuration APIs, and general engineering tools were preserved.

## Completed documentation work

- [x] **A1:** Removed `docs/BALANCE.md` after moving its engine timing reference to [Performance benchmarks](docs/TESTING.md#performance-benchmarks).
- [x] **A2 / B2:** Trimmed [Campaign design](docs/CAMPAIGN_DESIGN.md); retained geometry, spatial lessons, factual rewards, captures, and ordinary verification.
- [x] **A3:** Trimmed [Tools](docs/TOOLS.md); retained build helpers, engine performance reporting, debug images, and asset generation.
- [x] **A4:** Trimmed [Configuration](docs/CONFIGURATION.md); retained loading, schemas, live application, console API semantics, reload/dump/hash behavior, and correctness guidance.
- [x] **A5:** Trimmed [Gym](docs/GYM.md); retained progression cheats, manual diagnostics, UI fixtures, and screenshot recipes.
- [x] **A6:** Trimmed [Progression](docs/PROGRESSION.md); retained current costs, effects, rewards, persistence, and implementation/testing guidance.
- [x] **A7:** Trimmed [Building](docs/BUILDING.md); retained normal launch, build, CTest, script, capture, and performance commands.
- [x] **A8:** Trimmed [Levels](docs/LEVELS.md); retained schemas, compatibility fields, authoring, validation, and screenshot limitations.
- [x] **A9:** Trimmed [Architecture](docs/ARCHITECTURE.md) and [Simulation](docs/SIMULATION.md); retained shared session order, accounting, ownership, combat behavior, and general tests.
- [x] **A10:** Updated [README](README.md), the [documentation index](docs/README.md), and [project status](plan.md).
- [x] **A11:** Updated both local run-immune references: [.agents](.agents/skills/run-immune/SKILL.md) and [.claude](.claude/skills/run-immune/SKILL.md).
- [x] **A12:** Removed retired-guide links and redirected engine performance references to Testing.
- [x] **A13:** Trimmed the local, Git-ignored [campaign review note](scratch/level-review/early/review.md); retained geometry, screenshots, and validation evidence.
- [x] **B1:** Updated [Testing](docs/TESTING.md), including the retained performance section. The later C2 work below preserves the useful gameplay coverage.
- [x] **B3:** Updated the [Agent brief](docs/AGENT_BRIEF.md); retained general evidence and test-selection guidance.

## Completed implementation cleanup

- [x] **C1:** Removed the balance sweep script, automatic player, headless entry point, report collector, build registrations, CLI options, and interactive bot commands.
- [x] **C2:** Removed the two bot/report test files after migrating gameplay checks into [test_level_session.cpp](tests/test_level_session.cpp). Coverage includes authored starting ATP and income, progression bonuses, allowed/forbidden paid deployment, per-family accounting, and serial session replay. [CLI tests](tests/test_cli.cpp) reject the retired options.
- [x] **C3:** Removed the old numeric difficulty progression requirements from [campaign tests](tests/test_campaign_design.cpp). Geometry, positive releases, used ports/routes, release spacing, framing, and undefended objective damage remain checked.
- [x] **C4:** Removed the damage-attribution collector, recording hooks, and unused fluid ownership storage. Gameplay owner IDs, combat events, damage/income calculations, and generic family counters remain.
- [x] **C6:** Removed the browser tuning-editor script. Gameplay JSON loading, validation, reload/dump/hash behavior, and manual console configuration remain available.

Removed files:

- `tools/balance_sweep.py`
- `tools/config_editor.py`
- `src/app/AutoplayMode.cpp` and `.h`
- `src/game/autoplay/AutoPlayer.cpp` and `.h`
- `src/game/telemetry/RunTelemetry.cpp` and `.h`
- `src/sim/Attribution.h`
- `tests/test_autoplay.cpp`
- `tests/test_autoplay_level_rules.cpp`

The shared [level-rule helper](src/game/session/LevelSession.cpp) now serves App, HUD captures, and the replacement tests. It preserves authored opening ATP plus the player's progression bonus, income multipliers, and cell restrictions.

## Retained local reports

**C5 was outside the code-removal request.** The existing 208 generated report files remain in these Git-ignored directories, with no retired code left to generate them:

- [tools/balance/](tools/balance/)
- [scratch/level-review/final-balance/](scratch/level-review/final-balance/)
- [scratch/level-review/final-balance-final-routes/](scratch/level-review/final-balance-final-routes/)
- [scratch/level-review/implemented-balance/](scratch/level-review/implemented-balance/)
- [scratch/level-review/final-balance-repaired/](scratch/level-review/final-balance-repaired/)

Screenshots, benchmark history, traversal/validation logs, gameplay values, config/level JSON, progression/save formats, and unrelated working-copy changes remain intact.

## Verification

- Isolated `RelWithDebInfo` build of `immune` and `immune_tests`: passed.
- Focused `[session],[cli],[campaign],[deployment]` run: **27 cases / 45,081 assertions passed**.
- Full regression run: **811 of 813 cases passed**; 1,290,803 of 1,290,805 assertions passed. The two failures are **the shipped deployment economy matches fallback defaults** and **the shipped swarmer death bursts match the compiled defaults**. Both failures also reproduce in the existing executable from before the removal, with the same mismatched values.
- Retained config-application simulation script: **4 assertions passed**.
- Existing `tests/scripts/smoke.json`: its two population-count assertions fail identically before and after removal (527 and 559 rather than 750); both executables produce the same final state hash. No gameplay data or expected values were changed to hide these failures.
- CTest discovers 813 cases, including all four replacement session cases and no retired bot cases. The executable rejects `--autoplay` with exit 2, and help omits the four retired options.
- Retired module/API reference search, documentation link checks, and `git diff --check`: passed.

Detailed build and test logs are in `%LOCALAPPDATA%\horde-verify-build\balance-cleanup-*.log`; simulation reports are in the same directory.
