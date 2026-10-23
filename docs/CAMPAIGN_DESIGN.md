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

## Review of the previous levels

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

Wins and losses grant at least 30 Memory Cells, with wave and kill rewards on top. First clears grant an Antibody for a selectable new cell, ability or capstone. The intended rhythm is an initially small defense, useful permanent improvements after a loss, then a new tactical option after a clear. Unlocks are elective. A median of two to four attempts per level remains a tuning target: measured journeys include first-try clears and five-loss stalls. The game does not force a loss. See [Progression](PROGRESSION.md) for the purchase graph and rewards.

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

The retry pacing revision preserves all existing wave populations, family
populations, routes, geometry and wave rewards. It changes release durations,
dependent start times, opening wallets and later damage-income multipliers.
The first campaign wave stays unchanged.
Peak authored density/sec rises by roughly 1.32 times per level and also rises
within each level, removing the former mid-campaign troughs and later-wave
release-rate resets.

| Level | Waves | Total authored agents | Opening ATP | Peak authored density/sec | Income multiplier |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 6 | 2,884 | 300 | 80.83 | 1 |
| 2 | 6 | 3,705 | 335 | 106.69 | 1 |
| 3 | 7 | 5,640 | 370 | 140.84 | 1 |
| 4 | 7 | 7,335 | 410 | 185.91 | 1 |
| 5 | 8 | 10,776 | 455 | 245.40 | 0.75 |
| 6 | 8 | 13,604 | 505 | 323.94 | 0.5625 |
| 7 | 9 | 18,737 | 560 | 427.63 | 0.421875 |
| 8 | 9 | 22,702 | 620 | 564.48 | 0.31640625 |
| 9 | 10 | 30,200 | 685 | 745.15 | 0.23730469 |
| 10 | 10 | 35,822 | 750 | 983.60 | 0.17797852 |

Density uses the loaded family base densities; this is a scheduling measure,
not measured contact DPS or a human difficulty rating. For example, level 6's
final release now takes about 15.25 seconds instead of the original 31.60, with the same
3,100 enemies and preserved per-port gaps. The exact before/after cohorts and
metrics are in [campaign_pressure.json](../assets/balance/campaign_pressure.json).
The income multiplier affects damage-derived ATP, not starting wallets or wave
bounties. It slows reinforcement accumulation while the permanent tree and new
tools increase efficiency. Authored counts exclude enemies created by hostile
replication; actual campaign reports retain those additional agents.

The earlier timing-only v2 curve is retained in
[campaign_pressure_v2.json](../assets/balance/history/campaign_pressure_v2.json).
Its training prefixes showed useful early retries followed by excessive growth
in the offense path. The current revision couples the stronger pressure curve with
smaller opening-wallet increases, 0.85 baseline ATP per density removed, a
35-ATP Macrophage price. The selected later income decay starts after level 4.
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
smaller 30-point change is now selected. These early results used historical
controllers and do not establish the current whole-campaign difficulty.

With the V15 controller and selected 0.75 decay, fresh control and offense
journeys complete all ten levels in 23 and 20 attempts on training seed 31013.
Control attempts by level are 5, 1, 4, 1, 2, 1, 1, 3, 1, 4; offense attempts
are 3, 1, 1, 1, 1, 1, 5, 2, 1, 4. These include first-try clears and difficult
spikes, not a guarantee of two to four tries on every map. The original economy
priority stalls on level 7. A paid continuation buys three 20-MC Macro role
ranks and clears that level next attempt; this motivates a training-only economy
priority revision. Its fresh journey still stops at level 7; the continuation
does not replace that failure.

The final frozen held-out check uses both seeds 72011 and 82019 on all three
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

The approved shapes are implemented with ordinary variable-width vessel sweeps and polygon obstacles. Outer tissue polygons trim the broad sweeps to the biological silhouettes; they do not create extra gameplay barriers. That geometry revision retained the existing level identities, opening ATP, wave populations, family populations, rewards, and release times. The later retry pacing revision above compresses release timing and changes opening budgets while retaining the geometry, populations and wave rewards. Each port releases homogeneous cohorts with at least a 1.25-second gap before its next release, and every authored route variant is used.

Each map has two offscreen exits sharing one offscreen primary organ footprint. A hidden collector keeps the organ center on tissue where needed. This preserves one integrity pool and avoids the runtime's unsupported independent objective leakage.

The revised content passes 121 level cases (43,699 assertions), the all-ten entrance traversal case (115 assertions), and a final campaign check (41,295 assertions across four cases). All 25 shipped levels validate with zero errors; all three revised files are canonical. Level 3's file hash is unchanged. Actual clean, gameplay, and framing captures are in `scratch/level-review/early`, `middle`, and `approved7`.
