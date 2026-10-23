# Reproducible balance experiments

These experiments measure configured automatic controllers in the real gameplay
simulation. They provide evidence about units, economy, upgrades and campaign
progression. They do not measure enjoyment or predict a human win rate.

Controller adequacy appears before outcome tables. Supply known-achievable
references through experiment-level `controller_calibration.references` or
`report --calibration assets/balance/calibration/fresh_campaign.json`.
Uncalibrated losses leave gameplay balance inconclusive. A failed achievable
reference calls for investigating the controller and matching inputs; the tool
never converts a loss into an automatic recommendation to weaken the game.
Pressure capacity calibration is a different measurement from adequate play.

The retry progression presets freeze three discovery priorities across separate
training and held-out seed sets. Their `progression_targets` declare a journey
median of 2–4 attempts, at most 5 attempts per level, and purchases after at least
80% of losses with an observed follow-up. Actual rank value remains a separate
combat experiment. The report's `progression_journeys` joins retries, payouts,
following purchases, first-clear discovery offers and observed TTK. A failed
target under an unqualified controller does not diagnose game balance.

`campaign_pressure.json` records the authored scheduling, opening-wallet and
selected late damage-income revisions, with the earlier timing-only curve kept
in `history/`. Inspect current scheduling with
`python tools/campaign_pressure.py --check --out scratch/balance/pressure.json`.
The curve increases density released per second across all levels and waves;
distinct homogeneous cohorts retain at least 1.25 seconds between releases at
the same port. This is authored release pressure, not measured contact DPS.

```powershell
python tools/balance_lab.py plan assets/balance/experiments/smoke.json --out scratch/balance/plan
python tools/balance_lab.py run assets/balance/experiments/smoke.json --exe "$env:LOCALAPPDATA/horde-build/windows-release/bin/immune.exe" --out scratch/balance/smoke
python tools/balance_lab.py report scratch/balance/smoke
python -m unittest discover -s tests/python -p test_balance_lab.py
```

Use a fresh output directory for each execution. `report.html` is standalone and
links to the machine-readable summary, original experiment, exact run specs,
recorded action reports and logs. Run specs can be replayed with
`immune --balance-run spec.json --out result.json`. Replay needs the matching
build and assets recorded in the report provenance. This is a simulation replay;
visual inspection remains a separate game capture.

Refine a censored pressure grid without rerunning the original campaign:

```powershell
python tools/balance_lab.py calibrate scratch/balance/growth --exe path/to/immune.exe --pressure-scales 0.0625 0.125 0.25 0.5 --out scratch/balance/growth-refined --jobs 2
```

The new directory copies the source journey and executes a fresh calibration
grid. It pins the canonical probe document and the original attempt's six
gameplay config documents; it does not silently substitute current asset tuning
for historical tuning. Keep the game build compatible with that recorded source.
Calibration outcomes are not pooled with the campaign's win rate.

## Experiment format, schema 1

`base` is the RunSpec. `policies` is a list of named controller objects.
`variants` is a list of named patches; objects merge recursively, arrays replace.
Every policy/variant uses the same distinct `seeds`. `baseline` names the control
variant (default `baseline`). `metric` names a summary field, or a dotted report
field such as `economy.total_spent`. Missing/non-finite metrics remain missing.
No comparison substitutes zero for missing data.

Supported kinds:

- `paired`: compare each variant with the baseline on matching seeds and policy.
- `sweep`: baseline plus one-axis variations in `sweeps`. Each sweep has `name`,
  `values` and one of `path` (a RunSpec path), `override` (an exact flat config
  registry path), or `tree` (an exact stable upgrade key, synthetic rank grant).
- `search`: train all candidates on `train_seeds`, then evaluate only the top
  `top_k` per policy on disjoint `heldout_seeds`. Complete terminal training
  samples rank by clear rate, then the selected metric (`maximize` defaults true).
  An incomplete candidate is ineligible. Held-out results never select a winner.
  The prespecified baseline also runs on held-out seeds for paired comparison.
- `budget`: test the explicit `budgets` grid at `budget_path` (default
  `opening_atp`). Report the lowest tested budget meeting `target_win_rate`,
  failing lower brackets, unresolved budgets and per-seed monotonicity violations.
  This is an empirical threshold under one controller, not a minimum achievable
  by any strategy. A grid tolerates non-monotonic controller behavior that would
  invalidate an unguarded binary search.
- `campaign`: run real sequential campaign attempts, reward accounting and
  purchases. Its RunSpec has `campaign.levels`, `max_attempts_per_level`,
  `max_total_attempts`, and `purchase_policy: {priority: [stable keys], repeat: true}`.
  Each journey starts with its own progression state; it does not edit a save.

An optional experiment-level `growth_targets` names design hypotheses:
`enemy_factor_per_level` and `player_factor_per_level`. No target is hardcoded
into the analyzer. Authored population/release trajectories report adjacent
ratios, geometric growth per transition, a log-linear fit and its R-squared.

An optional experiment-level `power_probe` executes after campaign journeys.
Its `base` is a canonical micro RunSpec requiring explicit `opening_atp`,
`spend_limit`, and `income_enabled: false`. `pressure_scales` increases enemy
counts at unchanged release timing; `seeds` and `minimum_clear_rate` define the
empirical pressure grid. `every_n_clears` samples progression, always including
the initial state and last clear. `snapshot_basis` defaults to `next_attempt`,
using the real selected purchases made before the next attempt. `after_clear`
instead measures the immediate post-clear state with unspent reward currencies.
The final clear has no next attempt and is labeled accordingly. These settings
are Python experiment fields; they are never forwarded as C++ campaign options.
An optional `snapshot_ids`, such as `["initial", "after-level-0"]`, limits an
expensive verification grid to explicitly chosen stages without changing their
recorded progression or purchases.

Probes apply the exact `progression_snapshot` to the same map, composition policy
and reaction cadence, with the wallet controlled by the selected mode. They report the highest tested passing
pressure, the next tested failing pressure, invalid/unresolved points, Wilson
intervals and per-seed monotonicity violations. A finite complete bracket gets a
descriptive geometric midpoint. All-passing/all-failing grids are censored and
do not become an exact power estimate. Relative capacity gets conservative
bracket-ratio bounds. Player growth fits use only uncensored bracket estimates;
target comparisons remain inconclusive when any stage is unresolved/censored.
`wallet_mode: "fixed"` (the default) holds the configured opening wallet/spending
cap constant to isolate upgrade and cost efficiency. `wallet_mode:
"journey_opening"` instead uses the actual corresponding attempt's recorded
`economy.opening_atp` as both starting wallet and spending cap, including authored
level money and progression bonuses. `wallet_modes: ["fixed", "journey_opening"]`
runs both, with separate brackets, relative growth curves and fits. Fixed-wallet
efficiency alone is not total player power. Both modes disable in-run income;
real journey reinvestment captures the remaining economic feedback. Actual
opening ATP and surviving army count/HP are shown as descriptive indicators.

## RunSpec fields

All specs use `schema: 1`, `seed`, `max_ticks`, and `sample_ticks` (60 ticks is
one simulated second). A single encounter names `level`; a journey names the
level list in `campaign`. Relative file paths resolve against the repository.

`controller` has `policy` (`novice`, `competent`, or `tactical`), optional
`reaction_ticks`, `prep_ticks`, `reserve_atp`, and `weights`, an object of cell
names to nonnegative weights. Controller names describe implemented behavior,
not calibrated player percentiles.

`progression` can contain `memory_cells`, `antibodies`, and legal ordered
`purchases: [{key: "neutrophil.round_damage", levels: 1}]`, or a source `save`.
`profile: "all_unlocked"` grants capabilities without upgrades for diagnostic
roster comparisons. `grant_tree: {"neutrophil.round_damage": 3}` deliberately
creates a synthetic upgrade profile; it is not evidence of campaign affordability.
These two modes cannot be combined. Use explicit synthetic unlock roots alongside
synthetic ranks when comparing upgraded mixed teams. `controller.use_abilities:
false` isolates cell combat from ability casts.

`overrides` maps exact config registry paths to values. `opening_atp` explicitly
replaces a level's authored starting ATP for wallet experiments. `spend_limit`
caps total deployment spending, `income_enabled: false` disables both kill and
wave income, and `allowed_cells` restricts the available roster. These are
diagnostic interventions and should appear in provenance. Equal ATP caps do not
guarantee equal actual spending: inspect actual ATP spent and any remainder.

## Included suites

- `smoke`: compare open lane, choke, and simultaneous fronts.
- `controller_calibration`: the exact first authored campaign wave as an
  explicitly annotated slice, with automatic controllers tested on held-out
  seeds against a known-achievable fresh-profile reference. The separate
  paid-action reference proves feasibility and is not used as the bot's script.
- `unit_ablations`: equal 300 ATP cap, no income, single-type rosters and removal
  of support types. Compare team outcomes; support value need not appear as kills.
- `upgrade_ranks`: fixed-wallet Neutrophil ranks, plus an income-upgrade negative
  control with income disabled. Grants are synthetic; campaign legality is separate.
- `team_upgrade_ranks`: marginal team value of support stats, capacities and each
  capstone, using explicit synthetic cell unlocks and no ability casts.
- `ability_ablations`: remove each ability or all abilities, then compare cooldown
  ranks under the same roster and budget. Inspect actual casts; an unused ability
  does not establish its inherent strength.
- `sensitivity`: exact +/-20% density, damage and income parameter probes.
  Changing density affects both enemy resistance and income; use a fixed-wallet
  variant when isolating combat resistance.
- `strategy_search`: compare composition and reserve policies, then evaluate
  the selected strategy on seeds unseen during selection.
- `budget_threshold`: find empirical starting-money brackets with income disabled.
- `ttk_probe`: fixed-wallet first-hit TTK and lifetime observations, including
  deliberate multi-hit enemy density and synthetic damage ranks. Damage deaths,
  censored live survivors and non-damage exit reasons remain separate.
- `release_pressure`: separate compressed release rate from longer duration at
  unchanged rate, then compare choke geometry and explicit income reinvestment.
- `campaign_journey`: actual ten-level progression from zero currencies with
  alternative elective unlock priorities, retries, real rewards and purchases.
- `journey_growth`: small three-level pilot with actual progression snapshots,
  empirical pressure probes and configurable multiplicative growth hypotheses.
  Its single-seed defaults exercise the pipeline; expand seeds and pressure
  grids before making tuning decisions.

The micro maps are diagnostic content with positive authored routes. Their small
cohorts are for rapid controlled probes; campaign-sized horde pressure comes
from the actual campaign level specs. Running these experiments reads shipped
tuning and writes results to the selected output directory.

## Evidence and failure semantics

Runner reports must have `schema: 1`, a `summary` with explicit `valid: true`,
and a known status. Encounters end `cleared`, `defeated`, `timeout`, or `invalid`;
journeys end `completed`, `stalled`, `timeout`, or `invalid`. Nonzero runner exits,
invalid reports and wall-clock process timeouts cannot become ordinary victories
or defeats. A timeout is unresolved. Missing reports never reuse older output.

Clear rates use terminal outcomes only and display invalid/timeout counts beside
them. A separate lower bound uses all attempts. Rates use Wilson 95% intervals;
continuous means and paired deltas use a deterministic percentile bootstrap.
One observation has no bootstrap interval. These are exploratory conditional
uncertainty estimates, not population-level human statistics or tests corrected
for many comparisons. Search training and held-out phases stay separate.

The command exits 2 for invalid or unresolved runs and 0 for completed
experiments, including valid defeats and stalled journeys. A controller losing
is usable evidence, not a broken test. A controller timing out needs investigation.
