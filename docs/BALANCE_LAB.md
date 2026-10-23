# Balance Lab

The later focused Firing Cycle merge replaces Granule Replenishment and Trigger
Rate with one five-rank upgrade. The retained campaign and power results below
use the earlier V15 tree and runtime; they have not been rerun or relabeled for
the merge. Its estimated effects and prices are in [Progression](PROGRESSION.md).

Balance Lab runs controlled automatic players through the actual level session,
records what happened, and compares matched seeds. Its purpose is to make
questions about combat, upgrades, waves and economy answerable with reproducible
evidence. Enjoyment still needs human playtests; a controller's clear rate is not
a human difficulty rating.

The runner advances the shared fixed-step `LevelSession` and uses real paid cell
deployment. The Python tool plans experiments, executes independent processes,
checks result validity, computes comparisons and writes a standalone HTML report.
It does not implement a second combat simulation.

## Establish controller adequacy first

A correct simulation can record a poor strategy accurately. Losing a wave with
an automatic controller therefore cannot establish that the wave, economy or
progression needs changing. Controller adequacy is a separate report section,
shown before outcomes and growth charts. Without declared known-achievable
references it reads `uncalibrated`; failed references identify a controller/input
calibration failure, and gameplay balance remains inconclusive. Passing a few
references supports comparisons within those references' scope, not a general
human skill rating. The analyzer generates no automatic gameplay tuning advice.

The player confirmed that the first fresh campaign wave is easily beatable.
The initial automatic policy lost that reference, so its earlier stalled
journey is evidence to improve the controller. It is not evidence that the
opening wave should be weakened or the player should receive stronger stats.
Calibrate formation, positioning and reinforcement behavior, then compare the
reproduced play with a human recording before interpreting campaign difficulty.

A paid-action reference now reproduces that achievable wave with twenty baseline
Neutrophils and the same 300 ATP, deployment cadence, enemy release and geometry.
Changing placement to a tight early cohort cleared all nine tested seeds with
70–100 integrity, without upgrades, abilities or reinforcements. The reproducible
spec is [`fresh_first_wave_paid_actions.json`](../assets/balance/references/fresh_first_wave_paid_actions.json).
The automatic-controller calibration suite uses separate held-out seeds and
intentionally contains no golden action script. A successful scripted reference
establishes achievable play; it does not count as the automatic controller
passing. Additional cell types, fronts and upgrade profiles need their own
relevant references before extending that controller's evidence scope.

The curated first-wave level and the paid-action reference embed an exact slice
of the shipped first level. Python tests compare all fields except description
and the campaign's later waves; planning from the curated level also rejects a
drifted slice. When authored geometry, economy or wave data changes, refresh both
documents from the current first level, retain only its first wave, and rerun
the paid reference and held-out automatic calibration. Existing reports retain
their historical embedded inputs and do not silently adopt new authored data.

After the controller's cohesion correction, a verification run cleared all
16 held-out first-wave cases: eight Competent and eight Tactical, each with
100 integrity. Those automatic runs used the real earned-income reinforcement
loop (420–435 ATP spent from an initial 300), whereas the scripted feasibility
reference spent only the opening 300. This qualifies the declared fresh
first-wave criterion; later waves and broader campaign tactics remain outside
that qualification. It does not establish a need to adjust gameplay stats.

Route planning follows the public navigation field. Authored squad waypoints
guide lateral formation; the movement kernel gives navigation forward authority,
so those waypoints alone cannot establish where enemies will travel. Read-only
traces include wall projection and tissue-mask containment, and must reach an
objective before the controller relies on their shared coverage. Symmetric side
traces carry equal demand; a centre sample does not invent a 2:1 fork split.
Authored fork alternatives remain conservative pre-fork coverage constraints:
lateral squad guidance can send a cohort onto an alternative that a simple
entrance flow trace does not sample. Discarding those alternatives stranded
defenders on the wrong side of the level-4 island; an exact rendered replay
confirmed the bypass.

Recruitment uses current-wave counts and living cells' physical coverage. The
controller prefers a shared early interception site before an actual fork, or
separate cohorts when none exists. Distinct entrances can pool defenses at a
shared corridor with room to fight before the objective. Coverage is fractional
and never credits one cell more than once. Cached guidance refreshes during play;
normal deployment validation remains authoritative for every purchase.

Pooling also has to cover the public authored alternatives of both approaches.
Complete flow traces alone missed squad-directed bypasses on levels 5 and 8.
Zero-current-weight alternatives add no invented demand, but still constrain a
hold that could otherwise override safe pre-fork placement. Paired real-wave
checks retain the failed and corrected action streams, rather than forcing a
specific coordinate or interpreting idle defenders as inadequate player stats.

Authored near-parallel branches also retain their earliest shared interception
site, even when their later separation is smaller than a geometric fork test.
Suppressing that site sent level-6 defenses into a late chamber while the actual
horde passed along a septum. The V15 correction restored the paid, earned-profile
first-wave clear and its earlier successful action stream. This rule applies to
authored alternatives; lateral navigation samples do not invent extra forks.

An earlier controller revision, on two independent gameplay and placement seeds
using an actually earned pre-level-4
snapshot cleared the unchanged first two authored waves with 100 integrity and
zero leaks, including the dorsal-to-ventral route change. These wave-slice probes
verify that fork correction; their completion rewards are not carried forward
or treated as a whole-level campaign clear.

The final map exposed another controller error: incomplete traces stopped at a
wall that real enemies slide along, leaving large idle reserves. Its bounded
first-wave reference is preserved in
[`final_first_wave_qualification.json`](../assets/balance/references/final_first_wave_qualification.json).
Both Competent and Tactical clear that same earned-profile encounter with
100 integrity and no leaks under the V15 controller. A different, actually
earned control profile also cleared it in earlier qualification. These are paid, first-wave-only
qualification cases, not fresh-profile or full-campaign victories. The local
[`repeatable automatic experiment`](../assets/balance/experiments/controller_final_wave_calibration.json)
and [`verification`](../assets/balance/references/final_first_wave_automatic_verification.json)
retain the historical earned profile, 0.262144 income multiplier, exact inputs,
results and runtime hashes. They do not silently track later campaign tuning.

Two prior level-8 zero-wave defeats became real two-wave clears at 18.4 and
60.8 integrity with the V14 coverage gate, with no remaining enemies or timeout
advances. The known earned level-5 clear profile also completed its four-wave
slice, but one enemy crossed an intermediate wave's clearing grace period before
being killed; that warning remains visible. A weaker entry profile still lost
the fourth wave while suffering actual casualties. A routing correction does
not guarantee a weak build wins, and an intermediate grace advance is not a
clean per-wave elimination.

`assets/balance/calibration/fresh_campaign.json` defines that explicit reference.
It checks actual fresh progression and standard rules, with no synthetic grants,
modified wallets or config overrides. Apply it to an existing report without
rewriting the recorded experiment:

```powershell
python tools/balance_lab.py report scratch/balance/growth --calibration assets/balance/calibration/fresh_campaign.json
```

Reference definitions are preserved in `analysis-calibration.json` and the
machine-readable summary. Additional references can specify `level`, `policies`,
`variants`, `profile`, `minimum_waves_cleared`, `expected_status`,
`minimum_samples`, `minimum_success_rate`, `require_standard_rules`, and an
explicit `source`. Training runs and diagnostic pressure probes do not satisfy
these gameplay references. Missing or unresolved reference observations leave
adequacy inconclusive.

## Run an experiment

Choose an executable built from the current sources. The verification runs for
this checkout used `%LOCALAPPDATA%\horde-verify-build\bin\immune.exe`; a shared
build made before the lab was added needs rebuilding. All Python run commands
take an explicit `--exe` so results retain the chosen build's fingerprint.

```powershell
$immuneExe = Join-Path $env:LOCALAPPDATA 'horde-build/windows-release/bin/immune.exe'
python tools/balance_lab.py run assets/balance/experiments/smoke.json --exe $immuneExe --out scratch/balance/smoke
```

Open `scratch/balance/smoke/report.html`. It links to `summary.json`, the original
experiment, every exact RunSpec, runner reports with actions and provenance, and
stderr logs. Choose a new directory for each execution; mixing old results with
new settings would invalidate a comparison. Results are local artifacts.

```powershell
# Inspect the full run matrix without executing the game.
python tools/balance_lab.py plan assets/balance/experiments/unit_ablations.json --out scratch/balance/ablation-plan
# Regenerate analysis from already recorded results.
python tools/balance_lab.py report scratch/balance/smoke
# Replace a censored power grid while preserving the real recorded journey.
python tools/balance_lab.py calibrate scratch/balance/growth --exe $immuneExe --pressure-scales 0.0625 0.125 0.25 0.5 --out scratch/balance/growth-refined
# Reproduce an individual encounter with matching build/assets.
& $immuneExe --balance-run scratch/balance/smoke/runs/<run>.spec.json --out scratch/balance/replay.json
# Run the orchestration and statistics tests without the executable.
python -m unittest discover -s tests/python -p test_balance_lab.py
```

`--jobs` controls independent subprocesses; the default is one. This is distinct
from simulation worker threads. `--timeout` limits wall-clock seconds per runner
process, while `max_ticks` limits simulated time. A wall-clock timeout can reflect
performance or infrastructure and cannot establish that a defense loses.

## Record a human play session

```powershell
& $immuneExe --record-balance scratch/balance/human --level assets/levels/campaign_01_first_bend.json
& $immuneExe --balance-run scratch/balance/human/<recording>.json --out scratch/balance/human/replayed-report.json
& $immuneExe --balance-replay scratch/balance/human/replayed-report.json --tick 1800 --out scratch/balance/human/frame.png
```

Recording observes the real play session without re-simulating it while the
player plays. It pins level, tuning, progression and accepted paid actions/casts,
records wave transitions, and uses serial deterministic simulation settings.
Importing the recording reproduces those inputs through the shared session and
checks the observed state, economy, wave clocks and combat/TTK counters. A replay
mismatch is invalid evidence. An abandoned run imports as a timeout at its last
observed tick, without terminal rewards. A recording with no simulated tick or
an accepted action after its last simulated tick is retained with a diagnostic
and `replayable: false`.

Human recordings can reveal actions, losses and recovery patterns missing from
the automatic controllers. Use them to improve controller behavior and design
hypotheses; a few recorded players do not establish population-level difficulty
or enjoyment. Inspect the rendered replay as well as the recorded metrics.

## Ask a specific balance question

The retry progression design aims for a small initial defense that can handle
the opening but is pressured by later waves. Losses pay permanent Memory Cells
for useful upgrades; a first clear pays one Antibody for a new cell, ability or
capstone. Discoveries cost no Memory Cells, and capstones no longer require a
branch-point quota. Related-cell prerequisites still keep each discovery in its
own branch. This gives a clear a new tactical option instead of only another
stat increment. Skilled builds may clear a level on their first attempt.

The minimum completed-defeat reward is now 30 Memory Cells, enough for the first
20-point starter rank even when no wave was cleared. Wave and kill rewards add to
that floor, so substantial attempts can fund more progress. This does not make
a timeout or invalid experiment eligible for a reward. Earlier v1 experiments
used 10- or 20-point floors; retain their pinned inputs as separate revisions.

The frozen retry experiments are
[`retry_progression_training.json`](../assets/balance/experiments/retry_progression_training.json)
and [`retry_progression_heldout.json`](../assets/balance/experiments/retry_progression_heldout.json).
They compare control, economy and offense discovery orders with real purchases
through all ten authored levels. Training seeds are 31013, 31019 and 31033;
held-out base seeds are 72011, 82019, 92031, 102043 and 112059. Their maximum
50-attempt ranges do not overlap. The seed allocation was amended before any
held-out execution or outcome inspection; the presets retain that amendment.
Strategies deliberately share a base seed for paired comparisons. Freeze priorities and
controller settings before looking at held-out outcomes. Keep the two splits in
separate output directories; a policy's code name is not a player skill rating.

Declared journey targets are a median of two to four attempts per cleared level,
a maximum of five attempts per level, and purchases following at least 80% of
observed losses with a following attempt. Typical substantial losses should fund
one to three useful ranks; purchase counts alone do not prove those ranks help.
Paired unit/rank experiments establish that combat value. The report joins each
attempt's waves, rewards, next purchases, discovery offers and observed TTK.
These targets are design hypotheses. Later-route controller failures, incomplete
journeys and unqualified human difficulty remain explicit limitations.

The selected V15 training curve has completed fresh control and offense journeys
on base seed 31013. Control cleared levels 1–10 in
`5, 1, 4, 1, 2, 1, 1, 3, 1, 4` attempts (23 total); offense used
`3, 1, 1, 1, 1, 1, 5, 2, 1, 4` (20 total). Their medians, 1.5 and 1,
are below the declared two-to-four target: several new tools enable immediate
clears, while the harder encounters require retries. These are controller
observations, not evidence that every first encounter feels helpless to a person.
The final clears retained 31.6 and 8.2 integrity respectively.
Control had four intermediate clearing-grace advances during its level-8 clear;
offense had one each on levels 5 and 6. Those attempts ended with zero enemies,
but they are not clean elimination at every individual wave boundary.

The original economy purchase order stalled at level 7 after five attempts.
Keeping its exact earned bank and changing only the next ordinary purchase
priorities bought three macrophage ranks for 60 Memory Cells and cleared all
nine waves on the first additional attempt. That continuation is reported
separately from the frozen five-attempt failure. It motivates testing a fresh
economy path that invests in the cells it deploys; it does not justify silently
turning a failed journey into a success or granting extra currency.

The current V15
[`retry-loop proof`](../scratch/progression-rework/training-v15-selected-prefix-proof/proof.json)
records level-3 losses paying 97, 93 and 114 Memory Cells, actual paid work-cycle
and captive-capacity purchases, then a seven-wave clear at 43 integrity paying
244 Memory Cells and one Antibody. The next paid discovery is Fever. Friendly
damage-death TTK rose from 11.58 seconds on the first loss to 13.90 on the clear;
survivors and consumed cells remain separate. Retry seeds and combat exposure
differ, so that aggregate change is not an isolated health-upgrade effect.

Authored pressure now has a separate, reproducible scheduling contract. Enemy
counts, geometry and wave rewards were retained. Opening ATP now rises from
300 to 750, while release durations and dependent starts were compressed to remove level and later-wave pressure
troughs. Every port retains distinct homogeneous cohorts separated by at least
1.25 seconds. The first campaign wave is unchanged. Per-level peak authored
density/sec is approximately 81, 107, 141, 186, 245, 324, 428, 564, 745 and 984.
These scheduling rates do not measure how fast enemies contact the defense.
The roughly 1.32x authored release curve is supported by actual fresh retry
journeys; canonical combat-capacity probes and held-out journeys provide separate
checks. Authored density alone is not a measure of experienced difficulty.

The selected control journey's actual paid snapshots were also tested in the
same canonical encounter, with income and active abilities disabled. All 72
real-engine probes terminated validly, with no pool denials or grace advances.
Each setting used two diagnostic seeds and the same 60-second preparation per
wave. The table gives the highest pressure cleared on both seeds and the next
tested setting that failed that criterion, rather than an exact capacity:

| Earned snapshot | Fixed 300 ATP | Actual next-attempt opening ATP |
| --- | --- | --- |
| Fresh | Below 0.25x | Below 0.25x, 300 ATP |
| After level 4 and subsequent paid purchases | 0.5x to below 1x | 1x to below 2x, 655 ATP |
| After level 8 and subsequent paid purchases | 1x to below 2x | At least 8x, 885 ATP |

The endpoint results are censored by the tested grid. This demonstrates improved
cell, unlock and cost capacity under the named controller; it does not establish
an exact exponential power curve or measure every active ability. Death-only
TTK can fall as the army improves because most stronger cells survive: compare
the death sample with living and hit-alive cells before interpreting its mean.
The [`power verification`](../scratch/progression-rework/player-power-v15/results/verification.json)
retains spending, all grid points, survivor counts and the unchanged source
journey hashes.

The final held-out batch ran the three frozen purchase paths on base seeds
72011 and 82019, without further tuning. All 105 attempts were valid. Control
and economy completed all ten levels on 82019 in 19 and 24 attempts; control
stopped at level 3 on 72011, and the other three paths stopped at level 8 under
the five-attempt limit. All 58 observed loss-to-next-attempt transitions bought
paid ranks. Twenty-two intermediate grace advances remain in the detailed
report. These results leave the intended pacing inconsistent across the tested
paths; human playtesting and further iteration remain necessary.

The [`held-out verification`](../scratch/progression-rework/heldout-v15-final-first-two/progression-verification.json)
keeps those failures, exact rewards, purchases, TTK samples and censor counts.
The three strategies share each base seed for paired comparison, so the six
journeys are not six independent seed samples. Reserved base seeds 92031,
102043 and 112059 remain unrun. The packaged playtest launcher uses a separate
persistent save and records human attempts for subsequent comparison.

[`campaign_pressure.json`](../assets/balance/campaign_pressure.json) preserves
exact before/after cohort timing and density metrics. Inspect the current curve
without launching the game:

```powershell
python tools/campaign_pressure.py --check --out scratch/balance/pressure.json
python -m unittest discover -s tests/python -p test_campaign_pressure.py
```

For a combat comparison, start with a fixed initial ATP wallet, a deployment
spending cap and income disabled. Keep the map, seeds, controller and other
settings fixed. Compare survival, leaks, cell losses and actual money spent, not
just raw damage or kills. Equal spending caps may leave different unspent
remainders because cell prices differ.

For support units, compare an equal-budget team with and without that unit.
Goblet slowing and Fibroblast walls can prevent leaks or preserve allies without
receiving kill credit. The team outcome and marginal change provide stronger
evidence than a support unit's own damage number. Repeat across open lanes,
chokes, multiple fronts and real campaign geometry.

For an upgrade, first compare a synthetic one-rank change with defense and
resources controlled. Then run the same comparison with real income enabled.
Damage can buy more reinforcements through kill income, while a cost or income
upgrade can compound into much more late-wave combat strength. A numerical
modifier and its full-run value are different measurements. The fixed-wallet
upgrade suite includes an income upgrade as a negative control; it should do
nothing while income is disabled.

For campaign progression, run a complete journey with real rewards and legal
purchases. Each journey owns a fresh progression state. Compare multiple unlock
orders and controller abilities, including retries, currency accumulation,
time per clear and where progress stalls. Maxed or freely granted profiles
cannot establish that a campaign purchase is affordable or reachable.

For multiplicative growth, use the `journey_growth` pilot. It tracks authored
enemy populations and release pressure across levels, then calibrates the
player's actual purchased progression against a canonical encounter at the
same wallet, map and controller settings. It increases count without stretching
release duration. The largest tested passing pressure and next failing pressure
form an empirical capacity bracket. This tests whether upgrades and unlocks
produce more effective defense, rather than calling summed damage stats "power."

The report plots relative population and calibrated capacity on a logarithmic
axis, lists adjacent growth ratios and geometric factors, and fits log growth.
`growth_targets` are explicit configurable design hypotheses. An all-passing
grid is a lower bound needing harder probes; an all-failing grid is an upper
bound needing easier ones. Neither supplies an exact player power value. Target
comparison stays inconclusive for censored, incomplete or non-monotonic stages.
The pilot's one seed exercises the workflow and does not support a population
claim. Expand seeds, pressure scales and campaign coverage for actual tuning.

Power snapshots distinguish a post-clear state with unspent rewards from the
actual purchases made before the next attempt. The latter is the default.
Economic upgrades' extra money is held fixed in the combat calibration and
remains visible separately in real campaign earnings, opening wallets and
reinforcement spending.

The pilot evaluates two wallet scopes. `fixed` measures upgrade/cost efficiency
at a constant budget. `journey_opening` uses the recorded actual starting ATP
for that progression stage, capturing the stronger army funded by increasing
level resources and reserve upgrades. Each has its own capacity brackets and
growth fit. Both keep in-run income disabled for controlled comparison; actual
campaign runs retain reinvestment. Opening ATP, surviving unit composition,
counts and remaining HP are descriptive context, while empirical pressure
capacity measures effective combat strength under the probe controller.

A local macro verification imported two real ten-level journeys, including
their legal purchases, retry rewards, ten authored level documents and original
executable/config fingerprints. Forty distinct probes measured the initial
state and the actual purchases before the next level after the first clear.
On the same first-wave geometry and one independent probe seed, the fresh state
cleared 1x pressure and failed 2x; both later states cleared 2x and failed 4x at
the same 300 ATP. Their actual next-level wallets of 500 and 460 ATP cleared
4x and failed 8x. All forty probes were valid terminal results with finite
brackets and no observed reversals. Sixteen numeric-format repeat runs are
retained separately and excluded from analysis counts.

The local artifacts are
[`macro-verification/report.html`](../scratch/balance-lab/macro-verification/report.html)
and [`verification.json`](../scratch/balance-lab/macro-verification/verification.json).
This demonstrates that the measurement follows actual progression beyond the
initial state; a coarse one-seed grid cannot establish a human difficulty curve
or sustained exponential growth through the whole campaign. Retry earnings
contributed to these snapshots, and the original controller's later route
allocation was inadequate. No shipped gameplay stats were changed.

Release rate deserves a separate test from total population. The
[`release_pressure` suite](../assets/balance/experiments/release_pressure.json)
compares the same number released twice as fast, twice as many released at the
same rate, choke geometry, and explicit reinvestment variants. Alexandra
Sidorina's [Balance in td-games](https://www.gamedeveloper.com/design/balance-in-td-games)
connects enemy spacing, exposure time and defensive throughput to wave design
and economy. That is a useful hypothesis for IMMUNE; its mobile cells, hostile
enemies and crowding need empirical tests rather than the article's fixed-tower
formulas or numerical targets.

For strategy discovery, search compositions and reserves on training seeds.
Select candidates there, then evaluate those selected candidates on separate
held-out seeds. Never pool training and held-out rows or choose a winner using
the held-out results. A stronger controller can expose effective strategies. A
weaker controller can expose its own tactical gaps; compare its behavior with
recorded human play before attributing the difficulty to game design.

## Report metrics

The single-encounter report preserves summary, wave, timeline, action, unit,
enemy, economy, diagnostic and provenance data supplied by the runner. The
experiment's `metric` selects the primary numeric comparison. A plain field
name means `summary.<name>`; dotted paths refer to the report root. `metrics`
can add more fields, each with its own per-policy summaries and paired changes.
Missing fields stay missing rather than becoming zero.

Campaign analysis adds attempts, retries, levels cleared, terminal rewards,
Memory Cells per simulated minute, purchases and final progression. Timeouts do
not receive an inferred reward. Reward speed helps identify grind incentives,
but should be read beside how far the player progressed and what they can buy.

`progression_journeys` preserves each level's entry wave reach, ordered losses,
actual rewards, subsequent purchases, first-clear discovery offers, and enemy
and friendly TTK. Optional `progression_targets` checks the median attempts to
clear, maximum attempts per level, and observed post-loss purchase rate. It does
not label a purchased rank useful without a corresponding mechanic probe.
Non-search experiments may set `evaluation_split` to `training` or `heldout`;
their declared seeds and outcomes stay separate, without strategy selection.

A campaign with an explicit output path also appends completed attempts to
`<report path>.progress.jsonl` as it runs. Each line records the level, attempt,
outcome, reward, purchases, discovery choices, progression and state hash. The
callback only observes the completed attempt; observer-on/off tests preserve
the resulting hashes, economy and progression. Inspect this journal during long
journeys instead of waiting for the complete report. A partial journal does not
turn a process timeout into a completed campaign or award an unfinished attempt.

Inspect absolute leaks and objective integrity as well as kill percentage.
Baseline objective integrity is 100 and each leaked agent costs one integrity.
With tens of thousands of enemies, a high percentage killed can still mean a
decisive loss. Look for when fronts overwhelm the defense, how long recovery
takes, and how much money is consumed replacing dead cells.

Enemy family and friendly cell lifecycle tables report first-effective-hit to
damage-death TTK separately from spawn-to-damage-death lifetime. TTK of zero is
a valid first-hit kill at the 60 Hz observation resolution. Histogram p50/p90
values are lower/upper bounds, not invented precise percentiles. Always compare
damage-death sample counts with alive and hit-alive survivors: a surviving or
escaping enemy has no observed damage-death TTK. Consumption, expiration, leaks
and out-of-bounds exits remain separate outcomes. The `ttk_probe` suite increases
virus density deliberately to reveal multi-hit thresholds and compares synthetic
damage ranks under one fixed resource budget.

Wave advancement can happen after the clearing timeout even with survivors.
Treat a timeout advance or an enemy backlog as distinct from a clean clear.
Do not infer successful elimination merely from a higher current wave index.

## Statistical and validity rules

Reports are conditional on the named controllers and settings. Different seeds
measure variation within those conditions; they do not measure human skill
variation. The policy names `novice`, `competent` and `tactical` refer to code
behavior and have no calibrated player percentile.

Clear rates use terminal outcomes and show invalid and timeout counts alongside
them. A separate conservative bound uses all attempted runs. Binary outcomes use
Wilson 95% intervals. Numeric means and paired deltas use deterministic
percentile-bootstrap intervals; one observation has no interval. These are
exploratory estimates, without correction for testing many alternatives.

Paired comparisons require matching seed, controller and phase, with terminal
outcomes on both sides. Invalid or unresolved pairs are explicitly omitted.
Unknown statuses, a nonzero subprocess exit, malformed JSON or a missing explicit
validity marker cannot become evidence of success. The command exits 2 if an
experiment contains unresolved or invalid runs; valid defeats and stalled
campaigns are usable evidence and do not by themselves fail the command.

Budget threshold experiments evaluate an explicit money grid. They report the
lowest tested passing budget, a lower failing bracket, unresolved points and
per-seed reversals where more ATP performed worse. A finite tested grid establishes
an empirical controller threshold, not the theoretical minimum spending needed
by any player.

## Specifications and curated suites

The complete Python experiment fields and RunSpec conventions are in
[`assets/balance/README.md`](../assets/balance/README.md). Ready-to-run suites are
in [`assets/balance/experiments`](../assets/balance/experiments), with controlled
geometries and release/reference fixtures in [`assets/balance/levels`](../assets/balance/levels).
The full campaign suite references the actual authored ten-level sequence.

New balance experiments should name the hypothesis, preserve their exact inputs,
separate intentional diagnostic grants from legal progression, and record the
build/assets that produced the results. Tune shipped data only after reproducing
the observed failure and checking that the proposed change helps across several
strategies and relevant maps.

## Discover and exercise every upgrade

The executable exports its actual tree, prerequisite edges, per-rank prices,
config effects, non-config immunity rules and tuning registry:

```powershell
& $immuneExe --balance-catalog --out scratch/balance/catalog.json
python tools/balance_catalog.py generate --exe $immuneExe --out scratch/balance/upgrade-probes --seeds 101 202
python tools/balance_catalog.py run scratch/balance/upgrade-probes --exe $immuneExe --node 'ability.complement.cooldown' --jobs 2
python tools/balance_catalog.py report scratch/balance/upgrade-probes
python tools/balance_catalog.py self-test --catalog scratch/balance/upgrade-probes/catalog.json
```

Each generated comparison changes one node, keeps other prerequisite/support
grants identical, and records real acquisition prices separately. These synthetic
grants isolate direct effects; campaign journeys test legal affordability.
Role-specific contexts pair support units with allies. Income upgrades receive
real reinvestment tests; direct damage abilities receive explicit zero-defense
diagnostic pulses, while healing and barriers retain allied/route contexts.

`coverage.json` distinguishes unrun, invalid/unresolved, unexercised and measured
comparisons. Opportunity gates require the mechanic to occur: repeated casts for
cooldowns, actual friendly damage for durability, and scar construction/damage
for scar health. Unlock-only opportunities apply to the upgraded arm because
the baseline correctly lacks that cell or ability. A measured experiment can still show
zero or non-monotone benefit under its controller; inspect actions and TTK.

The former extra-arm node now grants Cluster Intake: more captives and a larger
collection radius, without buying an unsupported arm channel. The catalog
reports its actual captive ceiling separately. Emergency Reserves replaces the
named-enemy-only line with actual cell health; Granule Replenishment improves
reload throughput instead of ordinary one-hit overkill. Coordinated Response
improves each role's actual work cycle, and Defensive Membrane changes health
and real crowd displacement. Stable save keys remain unchanged, so old key names
in experiment files are not the current player-facing descriptions.

Vitality and Stamina add nine base HP per rank alongside their speed bonus;
Cyto and Goblet Resilience add ten. These flat additions cross actual shipped toxin
hit thresholds, where the former small percentage health changes often did not.
Global Emergency Reserves multiplies the resulting health.

Healing and barriers need different opportunity contexts from damage abilities.
The generated Fever probes pay for ten Neutrophils and use explicitly labelled,
shared diagnostic overrides to sustain actual toxin injury. Identical scripted
cast attempts use the normal unlock/cooldown API; locked or cooling attempts are
allowed to fail. Report accepted casts, damage-death TTK, deaths, surviving cells
and remaining HP together. The large supporting army in the earlier fixture
rarely crossed the controller's aggregate 25% missing-health trigger, so its zero
casts did not measure healing value. Scripted mechanic evidence does not qualify
the automatic controller's healing decisions.

Clot unlock/cooldown probes pay for three defenders against compact bacterial
pulses. Duration probes instead use sustained 20-second releases through the
diagnostic choke, with the same two legal casts in every rank arm. Explicitly
reduced objective leak damage keeps this diagnostic alive long enough to measure
enemy retirement lifetime, including leaks. Longer passage time measures delay;
unchanged leaks or friendly deaths do not establish a survival benefit. Likewise,
health upgrades need actual damage thresholds, weakening mucus needs allied
damage that is not already a one-hit kill, and scar-capacity upgrades need enough
construction to reach the old limit.

The local ability verification covers all six Fever/Clot nodes on two seeds:
[`coverage.json`](../scratch/progression-rework/catalog-abilities-v2/coverage.json).
Forty records represent 34 distinct controlled cases; the
[`case index`](../scratch/progression-rework/catalog-abilities-v2/unique-case-index.json)
identifies repeated shared baselines. Every selected node has its declared
opportunities, but these synthetic grants and deliberately altered encounters
establish neither campaign affordability nor universal usefulness. Small samples,
censored survivors and non-monotone outcomes remain visible in the reports.

## Fluid capacity and performance

Particle denials invalidate balance evidence. Twenty simultaneous paid Goblet
splashes previously requested 10,000 particles against an 8,192 pool; sixty
requested 30,000. Raising capacity alone removes that loss but retains the
physical particle cost. The shipped configuration now uses 200 droplets per
splash and a 32,768 pool. `coverage_full` remains 5: it controls fluid wetness and
damage, while Goblet slowing requires positive deposited mass, so lowering that
threshold would not preserve slow coverage. Crowd occupancy controls particle
braking and is also unchanged.

The serial pinned-runtime stress probes retained approximately 95–97% of the
uncapped 500-droplet slow exposure with 56–62% less process CPU time. Two ordinary
two-front seeds retained about 85% of slow exposure; this is a measured change
in representation and combat behavior, not exact equivalence or an FPS promise.
[`verification.json`](../scratch/progression-rework/fluid-diagnostics/verification.json)
preserves paid inputs, pool highwater/denials, slow exposure, outcomes and wall/CPU
timing. Its capacity-failing baselines remain invalid balance outcomes.

An exact reconstruction of the invalid level-3 campaign attempt matched its
original state hash, actions, economy, wave status and summary. Changing only
these two fluid fields produced a valid loss with zero denials, so the campaign
experiment can record the normal loss payout instead of stopping as invalid. The
[`paired replay`](../scratch/progression-rework/fluid-diagnostics/control-l3-replay/verification.json)
retains the original runtime, level, seed and legitimately earned progression.
Later campaign verification must use the new settings and retain the same
capacity guards.

The level-9 journey also exposed the former 8,192-round projectile ceiling.
An exact reconstruction reproduced 73,696 suppressed damaging rounds. Raising
only that ceiling to 65,536 produced a valid clear with no denials and a measured
peak of 16,009 live rounds. The simulation and default GPU instance budgets now
both reserve 65,536. Renderer counters report actual submitted instances, including
when a deliberately smaller GPU budget clips a diagnostic. The
[`projectile pair`](../scratch/progression-rework/projectile-capacity-v7/verification.json)
retains both outcomes. Its roughly 1.3 ms CPU per simulated tick is a whole-run
headless average, not a rendered frame-rate measurement.
