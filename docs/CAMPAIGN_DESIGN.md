# Campaign level design

The campaign should feel like defending living tissue against an infection that spreads into a systemic war. Every passage must carry an authored enemy route, every obstacle must change an engagement, and each new level should ask the player to manage more pressure with the immunity they have developed. The ten-level sequence introduces routing, convergence, and simultaneous fronts before combining them.

## Design rules

1. **Enter and leave beyond the frame.** Entire spawn discs and organ footprints stay outside the resting camera at 16:9 and 21:9. Vessels continue through those ports. Pathogens walk into the battlefield and damage organ integrity after walking out.
2. **Give every passage a job.** Author complete entrance-to-organ routes and use every entrance and variant in the waves. Remove terminal shelves and loops that normal enemies bypass. Broad deployment space can support reserves and reinforcement, but its surrounding routes must produce a real reason to use it.
3. **Teach convergence before separation.** Early forks rejoin around an island, then separate entrances meet at a defendable junction. Later levels retain independent collectors so one crowded choke cannot solve the entire map.
   Concentrated pockets should support several mobile cells at one bend or fork. Reinforcement and replacement are part of the defense; spreading a few cells evenly across an entire route is not the opening lesson.
4. **Put obstacles in the fight.** Rounded follicles, septa, plaques, and villus ridges divide a cohort, bend its advance, or create an interception pocket. Keep passages sufficient for squads and give each bypass a real route.
5. **Vary the battlefield, not just its bends.** Compact confluences, overlapping pools, separate lobes, and broad continuous fields ask different defense questions. Keep connections short where pools meet. In large fields, fan cohorts across several approaches and create separate fallback lines. Avoid long featureless exit runs, uniform grids, and ornamental ring roads.
6. **Release recognizable cohorts.** Pin every entry to its entrance. Release one family for several seconds, leave at least one second between releases, and then introduce the next family. Different entrances attack together to create the war feeling.
7. **Express body regions through geometry and color.** Vessel types control the renderer palette; region names alone do not. Gold lymph, blood-filled marrow pools, violet renal lobes, and coral intestinal fields accompany biological silhouettes. Preserve contrast for cells and pathogens.

## Review of the previous levels

Three subagents reviewed all ten levels through real OpenGL captures and automatic play. Early levels used greedy-cheapest; middle and late levels used spread-coverage, seed 1, one thread, with current base tuning. Raw evidence is in `scratch/level-review/early`, `middle`, and `late`.

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
| 9 The Funnel | Opening 5,000 viruses sharply exceeds level 10's 390; exterior strips add unused space. | Trim exterior lumen and restore ascending threats. |
| 10 Core Breach | Two fronts converge on a visible central organ. | Four breaches feed local fights and hidden lower drains. |

Original automatic runs supplied 300 ATP on every map, ignoring authored bankrolls. Their early losses support observed routing and coverage concerns, not an exact campaign difficulty ranking. Autoplay and HUD captures now honor level starting ATP, income multipliers, and cell restrictions. Autoplay also loads configured squad tuning. Both still use base tuning without purchased tree effects.

## Progression and immunity development

First clears grant Antibodies; cleared waves and kills grant Memory Cells. Unlocks are elective. Levels encourage useful investments while leaving room for different branches; see [Progression](PROGRESSION.md) for the purchase graph and rewards.

| Level | Spatial lesson | Investment rewarded |
| --- | --- | --- |
| 1 | One capillary with two bends | Neutrophil placement, reload coverage, initial stats. |
| 2 | Split and rejoin around an island | Range, damage, economy, or a first specialist. |
| 3 | Two lymph entrances meet | Merge coverage and specialized family response. |
| 4 | Two approaching channels | Shared reserves and cohort response. |
| 5 | Three entrances across overlapping marrow pools | Local interception and moving reserves around bone islands. |
| 6 | Four feeders into two unequal renal lobes | Split deployment, different response times, support cells, and economy. |
| 7 | Four ordered breaches across one broad intestinal field | Wide defense lines, simultaneous sectors, reinforcement, control, and sustain. |
| 8 | Two broad routes around islands | Upgraded damage and sustained coverage. |
| 9 | Three fronts entering a funnel | Area effects, reserves, repeated rotations. |
| 10 | Four breaches into two defended drains | Established roster, invested stats, chosen abilities. |

The authored opening and final populations and total level population increase through the campaign; wave counts never decrease. The initial target curve is:

| Level | Starting ATP | Waves | Opening enemies | Final enemies |
| --- | ---: | ---: | ---: | ---: |
| 1 | 300 | 6 | 138 | 850 |
| 2 | 420 | 6 | 170 | 1100 |
| 3 | 560 | 7 | 210 | 1450 |
| 4 | 720 | 7 | 260 | 1900 |
| 5 | 920 | 8 | 330 | 2450 |
| 6 | 1150 | 8 | 410 | 3100 |
| 7 | 1400 | 9 | 500 | 3800 |
| 8 | 1700 | 9 | 610 | 4600 |
| 9 | 2050 | 10 | 740 | 5500 |
| 10 | 2450 | 10 | 900 | 6500 |

Population is one pressure measure. Route length, independent defenses, cohort density, recovery time, and ATP also determine practical difficulty. Compare actual play before treating the numeric curve as a guarantee that every tree build experiences strictly increasing difficulty.

## Verification for future edits

Run level validation, canonical formatting checks, campaign tests, and actual gameplay captures. Campaign tests protect off-screen footprints, explicit port targeting, release gaps, positive route use, the population curve, and complete walkable paths on all ten levels. Verify every entrance reaches the hidden primary organ and causes damage when undefended.

Use both general automatic profiles, recording configuration, executable revision, seed, and threads. Inspect simultaneous attacks on screen. Automatic players do not cast abilities or reproduce purchased progression; human campaign play remains necessary for tuning particular tree builds.

## Playtest observations after rebuilding

Both general automatic profiles were run across all ten levels with corrected level economy and current tuning. These base-tree runs lost in waves 1 or 2 and often stopped buying with substantial ATP available. The planner uses a finite, widely spaced placement plan and does not reinforce lost positions. Treat these results as coverage and routing diagnostics, not a completed human difficulty calibration.

A starter-cell concentration probe on level 1 clarifies this limit. Twelve Neutrophils costing 96 ATP, clustered at the first bend, cleared a 138-virus release plus replication with full integrity; nine widely spaced bot positions lost. This probe uses bare simulation and an unsquadded release, rather than a full campaign session. It supports the opening placement lesson and affordability, without establishing a complete campaign win.

All four initial remakes were inspected in actual preparation and active-wave captures. Their entrance-specific traversal probes reached the hidden organ. The retained maps also received entrance-specific probes; these caught and resolved traps at level 9's final sieve and level 10's lower drain.

The initial review's full-suite verification passed 783 of 786 cases. The remaining three cases also failed in the previous build: a tower-health ordering assertion and two HUD assertions. Those logs are in `scratch/level-review/full-tests-final.txt`, with older-build comparisons in the same directory. The autoplay-rule integration check launches a separate process so loaded tuning does not affect subsequent unit tests.

## Approved revisions to levels 5–7

Level 3 remains the compact confluence reference. The next three maps use distinct spatial patterns rather than repeating its fork and final merge:

- **Marrow Pools:** three arrivals cross overlapping broad pools around two bone islands. Parallel routes use both sides of the islands; short crossings support local interception and moving reserves. Gold upper tissue blends into blood-colored lower pools.
- **Renal Lobes:** four arrivals feed two unequal chambers separated by tissue. A long upper septum and a curved lower septum create different holding positions. The lower feeders have deliberately different approach lengths. Wine-violet and lilac palettes distinguish the lobes.
- **Intestinal Battlefield:** four ordered left breaches enter one continuous field. Upper, middle, and lower approaches fan around attached folds and a central island toward two broad drains. Waves activate two, then three, then four sectors before sustained simultaneous pressure. The center also provides space for reserves.

The approved shapes are implemented with ordinary variable-width vessel sweeps and polygon obstacles. Outer tissue polygons trim the broad sweeps to the biological silhouettes; they do not create extra gameplay barriers. The existing level identities, opening ATP, wave populations, family populations, rewards, and release times are preserved. Each port releases homogeneous cohorts with at least a 1.25-second gap before its next release, and every authored route variant is used.

Each map has two offscreen exits sharing one offscreen primary organ footprint. A hidden collector keeps the organ center on tissue where needed. This preserves one integrity pool and avoids the runtime's unsupported independent objective leakage.

The revised content passes 121 level cases (43,699 assertions), the all-ten entrance traversal case (115 assertions), and a final campaign check (41,295 assertions across four cases). All 25 shipped levels validate with zero errors; all three revised files are canonical. Level 3's file hash is unchanged. Actual clean, gameplay, and framing captures are in `scratch/level-review/early`, `middle`, and `approved7`.

The six new base-tree autoplay runs still lose during wave 1, with substantial ATP unspent; see `scratch/level-review/implemented-balance`. They establish neither a successful defense nor final human balance. Keep the authored difficulty curve as the progression target, and tune practical difficulty through campaign play with purchased upgrades and active reinforcement.
