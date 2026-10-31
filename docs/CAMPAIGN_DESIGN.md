# Campaign level design

The campaign should feel like defending living tissue against an infection that spreads into a systemic war. Every passage must carry an authored enemy route, every obstacle must change an engagement, and each new level should ask the player to manage more pressure with the immunity they have developed. The ten-level sequence introduces routing, convergence, and simultaneous fronts before combining them.

## Design rules

1. **Enter and leave beyond the frame.** Entire spawn discs and organ footprints stay outside the resting camera at 16:9 and 21:9. Vessels continue through those ports. Pathogens walk into the battlefield and damage organ integrity after walking out.
2. **Give every passage a job.** Author complete entrance-to-organ routes and use every entrance and variant in the waves. Remove terminal shelves and loops that normal enemies bypass. Broad deployment space can support reserves and reinforcement, but its surrounding routes must produce a real reason to use it.
3. **Teach convergence before separation.** Early forks rejoin around an island, then separate entrances meet at a defendable junction. Later levels retain independent collectors so one crowded choke cannot solve the entire map.
   Concentrated pockets should support several mobile cells at one bend or fork. Reinforcement and replacement are part of the defense; spreading a few cells evenly across an entire route is not the opening lesson.
4. **Put obstacles in the fight.** Rounded follicles, septa, plaques, and villus ridges divide a cohort, bend its advance, or create an interception pocket. Keep passages sufficient for squads and give each bypass a real route.
5. **Vary the battlefield, not just its bends.** Compact confluences, overlapping pools, separate lobes, and broad continuous fields ask different defense questions. Keep connections short where pools meet. In large fields, fan cohorts across several approaches and create separate fallback lines. Avoid long featureless exit runs, uniform grids, and ornamental ring roads.
6. **Release recognizable cohorts.** Pin every entry to its entrance. Keep each release homogeneous and leave at least 1.25 seconds between the end of one release and the next release at that port. Different entrances can attack together while retaining distinct silhouettes.
7. **Express body regions through geometry and color.** Vessel types control the renderer palette; region names alone do not. Gold lymph, blood-filled marrow pools, violet renal lobes, and coral intestinal fields accompany biological silhouettes. Preserve contrast for cells and pathogens.
8. **Raise pressure as well as population.** Increase the sum of concurrently released density per second through successive waves and levels. More enemies released over a proportionately longer duration only extend a fight. Preserve readable port gaps when compressing durations and rebuild dependent start times; do not overlap different families at one entrance to manufacture a pressure peak.

## Historical review of the previous levels

All ten levels were reviewed through real OpenGL captures. Capture evidence is in `scratch/level-review/early`, `middle`, and `late`.

| Level | Observed problem | Direction |
| --- | --- | --- |
| 1 First Bend | Readable bends; later families overlap at the entrance. | Preserve the route and teach separate cohorts. |
| 2 Island Climb | Useful split and rejoin; a terminal shelf has no traffic role. | Keep both island routes active and remove the shelf. |
| 3 Two Chambers | One entrance feeds huge chambers; outer shelves stay empty and the organ is visible. | Rebuild a compact lymph-node confluence. |
| 4 Twin Channels | Legible fronts, overlapping release windows, visible endpoint. | Preserve channels; clarify timing and hide the exit. |
| 5 Ring Road | Hordes use upper-left arcs and stem, leaving most lower/right roads unused. | Broad overlapping marrow pools, purposeful bone islands, and separate drains. |
| 6 Circuit Board | Eleven entrances ultimately feed one corner through a mechanical grid. | Two asymmetric renal lobes with distinct travel times and separate drains. |
| 7 Chevron Sieve | Broad field and scattered chevrons give weak placement direction. | A continuous intestinal battlefield with ordered streams and obstacle shoulders. |
| 8 Blob Field | One entrance reverses front progression; travel distances are excessive. | Add another purposeful route and reduce physical scale. |
| 9 The Funnel | Exterior strips add unused space. | Trim exterior lumen. |
| 10 Core Breach | Two fronts converge on a visible central organ. | Four breaches feed local fights and hidden lower drains. |

## Progression and immunity development

Wins and losses grant at least 30 Memory Cells, with wave and kill rewards on top. First clears grant an Antibody for a selectable new cell, ability or capstone. The intended rhythm is an initially small defense, useful permanent improvements after a loss, then a new tactical option after a clear. Unlocks are elective. A median of two to four attempts per level remains a tuning target; historical journeys include first-try clears and five-loss stalls. The game does not force a loss. See [Progression](PROGRESSION.md) for the current upgrades, purchase graph and rewards.

| Level | Spatial lesson |
| --- | --- |
| 1 | One broad capillary sweeping from the upper left to a hidden bottom exit, framed close to the bend |
| 2 | Split and rejoin around an island |
| 3 | Two lymph entrances meet |
| 4 | Two approaching channels |
| 5 | Three entrances across overlapping marrow pools |
| 6 | Four feeders into two unequal renal lobes |
| 7 | Four ordered breaches across one broad intestinal field |
| 8 | Two broad routes around islands |
| 9 | Three fronts entering a funnel |
| 10 | Four breaches into two defended drains |

## Current exponential population and release curve

The current revision targets `round(3000 * 1.4^(level - 1))` authored agents
and approximately 1.4 times peak authored density/sec at each transition.
Pressure also rises between waves within a level. It changes populations,
family mix and release schedules while retaining geometry, wave counts, prep
times, authored opening ATP and wave bounties. Homogeneous cohorts at the same
port retain at least 1.25 seconds between releases.

| Level | Waves | Total authored agents | Spore agents | Opening ATP | Base peak authored density/sec | Income multiplier |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 6 | 3,000 | 0 | 300 | 155.22 | 1.0 |
| 2 | 6 | 4,200 | 0 | 335 | 217.31 | 1.0 |
| 3 | 7 | 5,880 | 0 | 370 | 304.23 | 1.0 |
| 4 | 7 | 8,232 | 12 | 410 | 425.92 | 1.0 |
| 5 | 8 | 11,525 | 20 | 455 | 596.29 | 1.0 |
| 6 | 8 | 16,135 | 32 | 505 | 834.81 | 1.0 |
| 7 | 9 | 22,589 | 48 | 560 | 1,168.74 | 1.0 |
| 8 | 9 | 31,624 | 72 | 620 | 1,636.23 | 1.0 |
| 9 | 10 | 44,274 | 108 | 685 | 2,290.72 | 1.0 |
| 10 | 10 | 61,983 | 162 | 750 | 3,207.01 | 1.0 |

The table uses current campaign counts and baseline family densities before
the late-map durability multipliers below:
virus and bacteria 0.9, parasite 1.6, spore 3.0. Level 1 contains only viruses;
bacteria begin at level 2 wave 2. Parasites retain their authored introduction
waves. Harpoon spores first appear at level 4 wave 3, then level 5 wave 2,
and from wave 1 on levels 6–10. Their small cohorts introduce a ranged siege
interaction; equal released density does not imply equal hostile damage.

The design record is
[level-design.json](../scratch/balance/exponential-growth-2026-10-28/level-design.json);
current [level assets](../assets/levels) remain authoritative. The peak curve
is anchored at 2,290.72 density/sec on level 9 and extends to 3,207.01 on
level 10. Authored counts exclude hostile replication. These are scheduling
targets, not measured contact DPS, controller capacity or human difficulty.

The map income multiplier is now 1.0 throughout. Damage-derived income starts
at 0.85 ATP per baseline density before Efficient Clearance and the durability
compensation below, while wave bounties
and opening wallets remain separate. Continued paid upgrades and Adaptive
Memory are intended to grow player capacity alongside the larger horde; this
requires matched earned-profile combat evidence rather than multiplying every
cell statistic by the population factor.

### Late-map durability and hostile damage

Levels 1–4 retain ordinary per-agent tuning. From level 5 onward, each map adds
another 35% durability and 12% hostile attack damage, compounded. The public
level fields are `enemy_scaling.durability_multiplier` and
`enemy_scaling.damage_multiplier`, both defaulting to 1. This raises individual
pathogen endurance and contact threat after early upgrades would otherwise
turn most hits into immediate kills. It preserves all authored counts,
cohorts, release timing, geometry, wave rewards and opening wallets.

| Level | Durability multiplier | Attack damage multiplier | Durability-weighted peak density/sec |
| --- | ---: | ---: | ---: |
| 1–4 | 1.000000 | 1.000000 | Base table above |
| 5 | 1.350000 | 1.120000 | 805.00 |
| 6 | 1.822500 | 1.254400 | 1,521.45 |
| 7 | 2.460375 | 1.404928 | 2,875.53 |
| 8 | 3.321506 | 1.573519 | 5,434.75 |
| 9 | 4.484033 | 1.762342 | 10,271.68 |
| 10 | 6.053445 | 1.973823 | 19,413.48 |

Shared run setup divides ATP per removed density by the same durability factor.
Thus an otherwise identical pathogen costs more damage to kill without paying
more total ATP merely because its HP increased. Efficient Clearance, authored
map income, wave bounties and kill-count Memory Cell rewards remain separate.
The weighted peak is damageable mass released per second, not hostile attack
DPS or measured difficulty. The roughly 1.4-times base release increase and
1.35-times durability increase combine to about 1.89 times this late-map load;
fresh matched-runtime earned-profile checks must establish the resulting play.

## Historical timing and income experiments

The earlier retry-pacing revision retained populations and family mix while
compressing release timing and changing opening wallets. Its peak authored
density rose about 1.32 times per level, and its late damage-income multiplier
decayed by 0.75 per level after level 4. Those are historical settings, not the
current table. For example, its level-6 final release took about 15.25 seconds
instead of 31.60 with the same 3,100 enemies. The before/after cohorts and old
income curve remain in
[campaign_pressure.json](../assets/balance/campaign_pressure.json).

The earlier timing-only v2 curve is retained in
[campaign_pressure_v2.json](../assets/balance/history/campaign_pressure_v2.json).
Its training prefixes showed useful early retries followed by excessive growth
in the offense path. That retry revision coupled the stronger pressure curve with
smaller opening-wallet increases, 0.85 baseline ATP per density removed, and a
35-ATP Macrophage price. Its selected later income decay started after level 4.
Permanent reserve,
income and work-rate upgrades still grow player power. These are explicit tuning
interventions, not evidence that a whole human difficulty curve has been proven.
The preserved V3 trial at 0.65 income stalled all three training paths on level 1.
V4 at 0.85 restored first clears in four or five attempts on training seed 31013;
only the economy path cleared all three pilot levels. Those historical results
did not meet the intended retry rhythm across all builds.
A same-runtime first-level reward experiment kept waves and ATP income fixed:
a 30-Memory-Cell floor cleared the three training paths in four, three and three
attempts on seed 31013. A 40-point floor took four attempts on each path. The
smaller 30-point floor was selected and remains current. These early results used historical
controllers and do not establish the current whole-campaign difficulty.

With the historical V15 controller and selected 0.75 decay, fresh control and offense
journeys completed all ten levels in 23 and 20 attempts on training seed 31013.
Control attempts by level are 5, 1, 4, 1, 2, 1, 1, 3, 1, 4; offense attempts
are 3, 1, 1, 1, 1, 1, 5, 2, 1, 4. These include first-try clears and difficult
spikes, not a guarantee of two to four tries on every map. The original economy
priority stalls on level 7. A paid continuation buys three 20-MC Macro role
ranks and clears that level next attempt; this motivates a training-only economy
priority revision. Its fresh journey still stops at level 7; the continuation
does not replace that failure.

That revision's frozen held-out check used both seeds 72011 and 82019 on all three
paths with five attempts per level. Control and economy complete all ten levels
on 82019 in 19 and 24 attempts. On 72011, control stops at level 3 and economy
and offense stop at level 8; offense also stops at level 8 on 82019. All 105
attempts are valid, and 58 of 58 losses with a following attempt buy paid ranks.
The reports preserve 22 grace advances with enemies still alive at the wave
transition. These checks establish real progression and reachable full clears,
but do not establish consistent pacing, every build's viability, or human
enjoyment. No tuning followed the held-out outcomes. Per-level ledgers and
first-hit TTK sample/censor counts are linked from [Balance Lab](BALANCE_LAB.md).

## Verification for future edits

Run level validation, canonical formatting checks, campaign tests, and actual gameplay captures. Campaign tests protect off-screen footprints, explicit port targeting, 1.25-second release gaps, rising loaded-density release pressure, positive route use, and complete walkable paths on all ten levels. Verify every entrance reaches the hidden primary organ and causes damage when undefended.

Run `python tools/campaign_pressure.py --check --out scratch/balance/pressure.json`
to inspect the current schedule without launching the game, and
`python -m unittest discover -s tests/python -p test_campaign_pressure.py`
for scheduling and frozen experiment contracts. Training and held-out retry
experiments use separate seeds and the same discovery priorities; invalid,
timed-out or tactically unqualified controller runs do not establish a gameplay
balance defect. See [Balance Lab](BALANCE_LAB.md) for those evidence rules.

Inspect simultaneous attacks on screen.

## Playtest observations after rebuilding

All four initial remakes were inspected in actual preparation and active-wave captures. Their entrance-specific traversal probes reached the hidden organ. The retained maps also received entrance-specific probes; these caught and resolved traps at level 9's final sieve and level 10's lower drain.

The initial review's full-suite verification passed 783 of 786 cases. The remaining three cases also failed in the previous build: a tower-health ordering assertion and two HUD assertions. Those logs are in `scratch/level-review/full-tests-final.txt`, with older-build comparisons in the same directory.

## Approved revisions to levels 5–7

Level 3 remains the compact confluence reference. The next three maps use distinct spatial patterns rather than repeating its fork and final merge:

- **Marrow Pools:** three arrivals cross overlapping broad pools around two bone islands. Parallel routes use both sides of the islands; short crossings support local interception and moving reserves. Gold upper tissue blends into blood-colored lower pools.
- **Renal Lobes:** four arrivals feed two unequal chambers separated by tissue. A long upper septum and a curved lower septum create different holding positions. The lower feeders have deliberately different approach lengths. Wine-violet and lilac palettes distinguish the lobes.
- **Intestinal Battlefield:** four ordered left breaches enter one continuous field. Upper, middle, and lower approaches fan around attached folds and a central island toward two broad drains. Waves activate two, then three, then four sectors before sustained simultaneous pressure. The center also provides space for reserves.

The approved shapes are implemented with ordinary variable-width vessel sweeps and polygon obstacles. Outer tissue polygons trim the broad sweeps to the biological silhouettes; they do not create extra gameplay barriers. That geometry revision retained the existing level identities, opening ATP, wave populations, family populations, rewards, and release times. The subsequent historical retry revision compressed timing and changed opening budgets. The current exponential revision changes populations, family mix and timing while retaining those shapes, current opening budgets and wave rewards. Each port releases homogeneous cohorts with at least a 1.25-second gap before its next release, and every authored route variant is used.

Each map has two offscreen exits sharing one offscreen primary organ footprint. A hidden collector keeps the organ center on tissue where needed. This preserves one integrity pool and avoids the runtime's unsupported independent objective leakage.

The revised content passes 121 level cases (43,699 assertions), the all-ten entrance traversal case (115 assertions), and a final campaign check (41,295 assertions across four cases). All 25 shipped levels validate with zero errors; all three revised files are canonical. Level 3's file hash is unchanged. Actual clean, gameplay, and framing captures are in `scratch/level-review/early`, `middle`, and `approved7`.
