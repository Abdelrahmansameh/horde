# Horde capacity and measurement

The shipped enemy pool is 65,536 live agents. Simulation, compiled game config,
renderer and CPU batch defaults share `core/CapacityDefaults.h`; `sim.json` carries
the same value. This is a live-storage ceiling, not a total level population.
Authored releases, replication births and agents simultaneously alive are
different quantities. A level can release more than the pool over time if
agents retire fast enough.

The previous 16,384 pool denied authored enemies or replications in measured L9
runs. Raising the pool preserves those requests; it does not preserve the old
outcome after a previously denied organism can now interact. This change alone
does not establish performance or controller adequacy for larger encounters.

Squad storage is 4,096 entries, so a 65,536-agent population in authored 48-agent
cohorts does not exhaust the old 1,024-entry registry and silently switch later
cohorts to ungrouped flow following. Agent handles and spatial indices are
32-bit; lifetime spawn and combat totals are 64-bit. Squad IDs remain 16-bit,
with 65,535 reserved as the no-squad sentinel; the new registry fits that range.

`sim.hostile.max_toxin_shots` (16,384) and `sim.hostile.max_harpoons` (4,096)
reserve damage-producing hostile storage. They do not change attack strength,
range, ammunition or cadence. A blocked ready shooter keeps its old retry
behavior. Balance combat metrics expose `toxin_shots_denied_capacity` and
`harpoons_denied_capacity`; either makes the native run invalid rather than
silently measuring a weakened enemy. Visual latch/harpoon/toxin event limits and
the 512-per-tick cosmetic death cap remain separate from damage storage.

Spore siege is restricted to the playable arena supplied by `SimWorld`, while
ordinary approach movement still runs in the larger simulation/spawn apron.
An off-board spore cannot acquire a hose or plant; a hose whose shooter leaves
the arena releases. This matches defenders' existing off-board target exclusion
without exposing invisible enemies or changing configured range/damage. Reload
clocks continue while the spore approaches the arena.

Authored waves also guard the actual expanded birth sites. A legal site keeps
its original packed position; only a site without walkability, a route to the
organ or clearance for the family's maximum body size is reprojected toward a
legal local approach near the same marker. Counts, clocks and the one formation
phase RNG draw are unchanged. This prevents a large parasite formation from
being born inside a capped off-board wall and recovered onto an unreachable
outer apron. Unconstrained packing/scenario APIs retain their old semantics.
Wave rows and summaries expose `spawn_sites_reprojected` and
`spawn_sites_denied_geometry`. If the local geometry has no legal site, the
shortfall is explicit and invalidates a native balance run; it earns no kills.

Levels can author public `enemy_scaling` rules with `durability_multiplier` and
`damage_multiplier`, each finite in `[0.01,64]` and defaulting to `1`. Durability
scales every family's spawn density/HP; damage multiplies the common hostile
damage factor after earned resistance. Counts, size, speed, reach and geometry
remain separate authored inputs. Macrophages retain their full-density swallow.
Kill ATP per density divides by durability, including balance runs with an
explicit opening wallet, so one tougher organism pays the same ATP as its
unscaled counterpart. Fresh base tuning is used on every application; repeated
level loads or tuning refreshes do not stack these factors or alter global enemy
definitions. Default rules preserve the previous simulation and canonical JSON.

Interactive initialization loads capacities before creating draw buffers.
Screenshots and scripts without an explicit `max_chaff` use their loaded config;
explicit script and benchmark capacities remain reproducibility inputs. Replay,
benchmark and screenshot hose draw buffers follow the world's hostile tuning.
Capacity settings still need a fresh application/world for complete adoption;
mid-run tuning changes must not be treated as a capacity-qualified capture.

## Memory and performance scope

At 65,536 entries the chaff SoA reserves approximately 9.81 MiB (157 bytes per
agent), plus 1.06 MiB for optional per-agent combat lifecycle stamps. This
excludes vector bookkeeping, spatial/collision scratch, hostile state and all
friendly/projectile/fluid pools. The triple-buffered four-family chaff draw
buffer reserves 27 MiB instead of 6.75 MiB (36-byte instances). Only live
instances are packed/submitted; a larger reservation does not mean 65k agents
are always processed.

The existing `chaff10k`, `chaff10k_towers` and `mixed` scenarios retain their old
16k reservations for comparison. New `chaff16k`, `chaff32k` and `chaff64k` scenarios measure
16,384, 32,768 and 65,536 initial ordinary crowd agents with the same 64k reservation. The saturated 64k probe can
defer replication by design and is a workload stress test, not evidence of a
capacity-valid campaign. Like the legacy crowd benchmarks, these omit spores;
the focused capacity tests separately exercise a full all-spore batch and more
than 1,024 simultaneous hoses.

After building, record serial timing and actual GL availability separately:

```powershell
& $immuneExe --bench chaff10k --ticks 60 --threads 1
& $immuneExe --bench chaff16k --ticks 60 --threads 1
& $immuneExe --bench chaff32k --ticks 60 --threads 1
& $immuneExe --bench chaff64k --ticks 60 --threads 1
& $immuneTests "[capacity]"
```

Benchmark average/p99 simulation, spatial hash and render submission. A missing
GL context gives no rendering-performance evidence. Dense lane jams, long
spore hoses, reinforced friendly armies, fluid and projectile emission need
actual campaign/replay checks as well; increasing the enemy pool does not
raise their separate capacities or promise a frame rate.

The neighbour gather now charges every examined CSR candidate, including self,
underground/ghost bodies and coincident positions. Its strict candidate budget
is `8 * max_neighbors_sampled + 1`; the extra entry accommodates self while
retaining the previous eligible-neighbour policy in ordinary surface crowds.
The old walk counted only eligible candidates, so a pile of skipped bodies
could evade the work budget. Target/collision eligibility is unchanged.

This is still a truncation: in a pathological pile the skipped entries can
consume the scan before a later valid contact is reached. Own-cell-first and
per-agent rotation are retained, but bounded work cannot guarantee complete
contact coverage in an arbitrarily overcrowded cell. Benchmark telemetry
records total candidate entries and the per-agent maximum; focused regressions
cover coincident piles, underground piles and ordinary symmetric pair contact.
