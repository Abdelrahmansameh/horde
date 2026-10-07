# Balance runs and performance reports

IMMUNE provides a headless automatic player for comparative balance experiments and a separate benchmark harness for subsystem timing. The automatic player deploys individual mobile cells. Report keys named `towers` remain compatibility aliases; they no longer imply a stationary emitter was bought. See [Gameplay](GAMEPLAY.md) for the player model and [Testing](TESTING.md) for verification limits.

## One automatic run

From the repository root:

```powershell
$immuneExe = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune.exe"
& $immuneExe --autoplay --level assets/levels/campaign_01_first_bend.json --profile greedy-cheapest --seed 1 --threads 1 --report "$env:TEMP\immune-balance-run.json"
```

`--autoplay` requires a real level file and fails on a missing/unloadable level instead of falling back to the test lane. Without `--report`, JSON goes to stdout. `--config DIR` selects alternate tuning. `--max-ticks N` bounds the run; zero uses 108,000 ticks (30 simulated minutes at 60 ticks/second).

A completed report has `result: cleared`, `lost`, or `tick_limit`. Each returns process exit 0; a loss is data. Setup/profile/report-writing failure returns 1. Read `result` rather than interpreting the exit status as a win.

To watch the bot in the interactive app:

```powershell
& $immuneExe --level assets/levels/campaign_01_first_bend.json --sandbox --exec "autoplay on; time 8"
```

Use a separate save if progression is part of the experiment. `--sandbox` removes progression bonuses/unlocks/payouts from a comparison. The interactive bot and headless bot share `AutoPlayer`, but their surrounding setup currently differs as described below.

## Strategies and placement planning

| Profile | Behavior |
|---|---|
| `greedy-cheapest` (default; alias `greedy`) | Cheapest affordable planned direct cell |
| `spread-coverage` (alias `spread`) | Fill planned sites in coverage order |
| `single-type:<type>` | Restrict every planned purchase to the named cell type |

The parser accepts current type names `neutrophil`, `macrophage`, `cytotoxic_t`, `goblet_cell`, and `fibroblast`. The bot never casts active abilities or sells. Default decision cadence is 30 ticks (half a second) and the plan has at most 32 sites. It spends through the economy and validates/deploys through the same direct-cell API used by the player.

[AutoPlayer.cpp](../src/game/autoplay/AutoPlayer.cpp) derives its plan from level geometry rather than per-level strategy files:

1. Sample vessel splines and ignore candidates with nonfinite flow cost.
2. Score narrowness (weight 1), lateness along the flow field (0.6), extra lane coverage (0.5 each), and placement-zone priority (0.8; concentrated zones double the hint).
3. Sort stably, accept spaced candidates, assign types from their family-mask coverage of the authored waves with diminishing returns per type, and validate direct deployment.
4. Re-check placement and affordability when buying.

The reference coverage/spacing radius is currently the Macrophage swarmer search radius. This remains a general-purpose planner: a poor result can describe a weak plan, sparse legal sites, or a cell's behavior as well as pricing. A zero-instance type is not proof by itself that no human strategy would use it.

## Sweeping levels, strategies, and seeds

The [sweep helper](../tools/balance_sweep.py) requires Python 3.10+ and only the standard library:

```powershell
python tools/balance_sweep.py --seeds 3 --profiles all
python tools/balance_sweep.py --levels assets/levels/campaign_01_first_bend.json assets/levels/campaign_02_island_climb.json --profiles core --jobs 2
python tools/balance_sweep.py --levels assets/levels/campaign_01_first_bend.json --profiles single-type:fibroblast --seeds 1
```

Executable resolution is `--exe`, then the `IMMUNE` environment variable, then `%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe`. Default output is `tools/balance/`, gitignored. It writes `runs/<level>__<profile>__seed<N>.json`, `summary.csv`, and `summary.md`. Colons in profile names become underscores in filenames.

| Option | Current default/meaning |
|---|---|
| `--levels [paths...]` | Default: every immediate `assets/levels/*.json` except `gym`, `lane_schema_test`, `capillary_test`, `floodplain_max_horde` |
| `--profiles core` | Two general strategies |
| `--profiles single` | Four single-type profiles: Neutrophil, Macrophage, Cytotoxic T, Goblet Cell |
| `--profiles all` | `core` plus those four single-type profiles |
| `--profiles a,b` | Explicit comma-separated strategy list; use this to include Fibroblast |
| `--seeds N` | N seeds per level/profile, starting at 1; default 3 |
| `--jobs N` | Concurrent processes; default half available CPU count, at least 1 |
| `--max-ticks N` | Forwarded if nonzero; default harness limit |
| `--out DIR` | Output directory |

An explicitly supplied directory includes every immediate JSON file and does not apply the default skip set. The default sweep includes legacy content as well as campaign files; pass the intended levels when judging campaign difficulty. Each subprocess uses `--threads 1 --quiet`, runs from the repo root, and has a 900-second wall-clock timeout. The helper has no `--config` option; use single `--autoplay --config` invocations for a separate config directory, or compare deliberate repository tuning revisions.

The digest compares level/strategy results, aggregates compatibility `towers_by_type`, and flags mean wave integrity cost more than one population standard deviation above that level's mean. The flag is a lead for review, not a design rule. A loss or tick limit does not fail a sweep; failed subprocesses/reports do, and return exit 1.

## Reading telemetry

[RunTelemetry.cpp](../src/game/telemetry/RunTelemetry.cpp) emits schema 1. The header includes `level`, `level_name`, `profile`, `seed`, `config_hash` (string), `config_dir`, `result`, `ticks`, `seconds`, and `waves_reached`. Keep the raw reports with the executable/source revision when comparing tuning.

| Section | Useful interpretation |
|---|---|
| `cells` (`towers` alias) | Each direct deployment's owner id, type, position, lane, build/remove tick, invested/refunded ATP, family damage/kills, named damage, alive/active ticks |
| `cells_by_type` (`towers_by_type` alias) | Aggregate instances, investment, density removed, kills, throughput, uptime |
| `waves[]` | Zero-based index/name/modifier, start/end ticks, time spent in prep/spawning/clearing, integrity cost, ATP earned/spent, unit counts, family outcomes, peak horde/density |
| `families[]` | Spawned, killed by damage, leaked, out-of-bounds retired, still alive, leak rate, attributed density removed |
| `totals` | Population outcome totals, final integrity/ATP, earned/spent ATP |
| `economy` | Peak ATP, mean banked ATP, and ATP integrated over time (`idle_atp_seconds`) |
| `timeline[]` | Samples every 30 ticks: ATP/income, horde/density, integrity, direct-cell count and retained tower alias |

`atp_per_density` is investment divided by removed density; lower can indicate better value for **damage-producing cells**. It is not a complete value measure for protective barriers or mucus control. `uptime` is active ticks divided by alive ticks; low uptime can reflect placement, travel, lack of targets, or support roles. Ratios with a zero denominator are `null`, not zero.

Family accounting reconciles spawned with damage kills, leaks, out-of-bounds retirements, and still-alive population. Viral replication contributes new spawns, so original wave counts are not the final denominator. The historical total `chaff_killed_total` includes multiple removal reasons; use family outcome fields to distinguish them.

Damage attribution uses owner handles carried by simulation damage events through [Attribution.h](../src/sim/Attribution.h), including the reserved high-range ids of direct cells. The collector receives events; the sim does not query it for decisions. [test_autoplay.cpp](../tests/test_autoplay.cpp) includes attribution/session/report coverage. A report's aggregate alone cannot diagnose an unexpected cell path: inspect instance position, lane, lifetime, and the observed run.

## Current harness limits

The headless loop calls the shared `step_level()`, but [AutoplayMode.cpp](../src/app/AutoplayMode.cpp) assembles its own setup. At present:

- It uses base tuning without player progression effects.
- It does not call interactive `App::apply_level_rules()`, so per-level `economy.starting_atp`, `income_multiplier`, and `allowed_towers` are not installed there. Interactive bot runs do apply them.
- It passes `win.survive_seconds`, authored waves, placement zones, enemy/tower/ability tuning, and geometry to the session.
- Horde capacity is fixed at 32,768, rather than the interactive configured capacity.
- It does not assign the loaded `sim.squads` block to its `SimDesc`, so that layer uses its constructor defaults.
- Abilities are configured and their cooldowns tick, but the bot does not cast them; this setup does not install the extra ability ECS cleanup system.

Screenshot `--ui` similarly has its own setup and is not a full campaign/progression replay. Keep these scope differences with any balance conclusion about schema-2 maps. Do not use a headless win rate alone to claim the player's exact campaign experience.

Inputs should include revision, level, config, seed, profile, and thread count. The bot itself uses no wall clock or RNG, but the surrounding sim's flow rebake has a wall-clock budget. Repeated runs with geometry-changing behavior can vary; prefer serial runs and repeated baselines rather than promising exact replay from the seed alone.

## Performance benchmarks

```powershell
& $immuneExe --list-scenarios
& $immuneExe --bench chaff10k --ticks 600 --threads 1
python tools/bench_report.py --ticks 1200 --scenario chaff10k --scenario mixed --no-history
```

Registered scenarios are `empty`, `chaff1k`, `chaff10k`, `chaff10k_towers`, `named200`, and `mixed`. They seed 0/1,000/10,000 ordinary agents, 0/200 named agents, and optionally 16 long-lived damage fields. Despite its name, `chaff10k_towers` adds damage fields rather than purchasing a defense plan. Benchmarks tick the sim and submit rendering; they do not play authored waves or the automatic player.

Benchmark JSON contains `scenario`, `ticks`, `seed`, final `agents.chaff/named`, and `timings_ms`. Timing entries contain `avg`, `p50`, `p99`, `min`, `max`, and `samples`. Standard keys are `chaff_update`, `spatial_hash`, `ecs_tick`, `render_submit`, and `frame_total`; additional recorded subsystem keys are retained.

The [report helper](../tools/bench_report.py) grades averages against strict thresholds:

| Timing | Budget |
|---|---:|
| `chaff_update + render_submit` | < 4 ms |
| `ecs_tick` | < 2 ms |
| `spatial_hash` | < 1 ms |
| `frame_total` | < 16.6 ms |

It accepts `--exe`, `--ticks` (default 600), `--seed`, repeatable `--scenario`, `--history`, `--no-history`, and `--json`. Default is every registered scenario. It prints a table, can print graded rows, and appends per-scenario results plus time/commit to the gitignored `tools/bench_history.csv`. Exit 0 means every average budget passed; 1 means a budget or execution failed. The summed p99 shown for chaff/render is a sum of their individual percentiles, not the percentile of combined frame samples.

The benchmark attempts a hidden GL context for real render submission. If context/renderer setup fails, it warns and records render submission as zero while still running sim timings. Run without `--quiet` to confirm rendering is active before interpreting the combined budget. Timings are hardware/workload dependent and reports do not include a config hash; record tuning separately and compare on the same machine with consistent thread settings.

## Making a tuning change

Choose a concrete hypothesis, gather a baseline across relevant seeds/profiles, change one controlled input, and repeat. Inspect the report differences alongside an interactive observation. Use `config list/get/set` for live experiments and a copied config directory for isolated CLI trials; see [Configuration](CONFIGURATION.md). `config dump` can persist an interactive experiment, while CLI `--dump-config` writes compiled defaults and would replace that experiment with defaults if aimed at the same directory.

Update documented values when keeping a tuning change, and retain the evidence needed to identify which level/config/revision generated the reports.
