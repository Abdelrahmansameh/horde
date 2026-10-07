# Simulation internals

This guide describes the current engine tick and its data. Read
[ARCHITECTURE.md](ARCHITECTURE.md) for module boundaries and the outer
`game::step_level()` sequence. Game systems and configuration own gameplay rules
above these engine stores.

## Setup, bounds and capacities

[SimWorld](../src/sim/SimWorld.h) owns chaff, spatial queries, tissue/clearance/
flow, squads, ECS, damage fields, rounds, swarmers, fluid, hostile attacks,
scars, combat events, RNG, and lifetime counters. `SimDesc` supplies seed,
capacities, grid sizes, tuning and rebake budget. `init()` resets stores and ECS
systems/context as well as entities; callers register level systems afterwards.

`world_bounds` is the play/camera rectangle. `sim_bounds` covers actual solver
grids and out-of-bounds retirement; empty means use `world_bounds`. Level loading
can expand it for offscreen spawns. Do not cull incoming enemies against the
camera rectangle or use the visible view as a solver extent.

Bulk stores refuse/drop spawns at capacity instead of growing per agent. Inspect
return values and overflow counters for missing units. `DamageSystem::reserve()`
reserves vector storage but does not itself enforce a hard maximum. Runtime ECS
creation has different allocation behavior from a pre-sized SoA slot.

## Canonical world tick order

[SimWorld.cpp](../src/sim/SimWorld.cpp) is authoritative. One call advances one
fixed 1/60-second step:

1. Rebuild the chaff spatial hash from beginning-of-tick positions.
2. Update squad centroids and route anchors from those same positions.
3. Move chaff; record objective leaks/out-of-bounds retirements and leak damage.
4. Advance burrowing/slither after motion and before targeting/damage.
5. Run ordered ECS systems, including named behavior and legacy tower release.
6. Build inflammation zones and apply their current effects.
7. Apply aggregate damage fields.
8. Update existing rounds; turn incendiary impacts into timed fields.
9. Build named-target snapshots; update swarmers and apply their damage,
   fields, splashes, round spawns, scar builds and heals.
10. Expire chaff slow timers before fluid refresh.
11. Update hostile attacks, land their ECS damage, and remove expired/destroyed
    scars.
12. Solve fluid, then refresh named-agent slows from wet coverage.
13. Apply scar-contact damage and combine all removed-density totals.
14. Spread slow contagion from qualifying deaths; emit capped chaff-death events
    while the retiring agents still have positions.
15. Compact chaff once; separate damage kills from leaks/out-of-bounds
    retirements; age/clear damage-field submissions.
16. Close optional attribution, pump pending flow rebakes, increment world tick.

The hash retains beginning-of-tick membership during movement/combat. Queries
test current position/density where required. Compacting between damage sources
would make indices and hash membership disagree. New rounds requested by
swarmers first integrate next tick because the projectile pass already ran.
New swarmer splashes exist before this tick's fluid pass. Fields submitted after
the field pass first apply next tick. Preserve these timings when adding combat.

`CombatEventSink` accumulates until consumed. Interactive rendering drains it
once per frame, potentially after several ticks. Engine runners that never
drain it can reach event capacity; that is cosmetic and must not affect gameplay.

## Chaff storage and identity

[ChaffBuffers](../src/sim/chaff/ChaffBuffers.h) has parallel fixed-sized arrays,
with only `[0, count())` live:

| Streams | Purpose |
| --- | --- |
| `pos_*`, `prev_pos_*`, `vel_*`, `wander_*` | Motion, render interpolation, correlated wander |
| `family`, `density`, `flags` | Tuning/visual family key, HP-like mass, alive/retirement/debuff/contact bits |
| `generation`, `squad_id` | Identity and squad membership |
| `slow_remaining`, `slow_factor` | Timed slow, distinct from its flag |
| `host_index`, `host_generation`, `host_kind` | A latched pathogen's host |
| `burrow_state`, `burrow_timer`, `burrow_target_*` | Underground phase/timer/emergence point |
| `toxin_rounds`, `toxin_cooldown`, `toxin_reload` | Bacterial toxin magazine |
| `size_scale` | Lifetime spawn-size multiplier applied to drawn and collision bodies |
| `hit_flash`, `replication_*`, `latch_heading`, `burrow_anim`, `body_heading`, `slither_phase`, `toxin_spit_pulse` | Cosmetic feedback/animation; these do not decide motion or damage |

`spawn()` initializes a slot, `kill()` marks retirement, and `compact()`
swap-removes agents while carrying every stream. `ChaffHandle` has an index hint
and generation. Generations use a monotonically increasing per-world counter,
not a per-slot reuse count; `resolve()` can recover agents moved by compaction.
Raw indices are stable only until compaction and must not survive ticks. World
initialization resets identities, so handles do not survive a level reload.

All damage paths use `apply_density_loss()`. It owns density bookkeeping and hit
feedback, applies the slowed-target multiplier, and ignores underground
parasites. Do not directly subtract a stream in a new weapon. A captive can be
hidden without being burrowed: `kHidden` controls targeting/motion, while
`burrow_state` controls underground immunity.

New streams need reserve, clear, spawn, swap-compaction, debug invariant and
identity coverage. Decide whether they are gameplay state requiring a hash
decision or visual output that must remain irrelevant to gameplay. See
[test_chaff_soa.cpp](../tests/test_chaff_soa.cpp),
[test_chaff_hit_flash.cpp](../tests/test_chaff_hit_flash.cpp), and
[test_chaff_death_vfx.cpp](../tests/test_chaff_death_vfx.cpp).

[SizeJitter.h](../src/sim/SizeJitter.h) hashes the generation into a lifetime
size multiplier around 1 using configured jitter, clamped below 1 to prevent
nonpositive bodies. It does not consume the world RNG. Chaff can pin a positive
spawn `size_scale` for fixtures; swarmers derive their scale from the profile.
Collision, reach, body rendering and relevant event sizes use that same scale,
and both stores hash it as gameplay state. New collision/batcher paths must use
the effective body size rather than silently return to the family/profile base.

## Crowd movement

[ChaffSystem](../src/sim/chaff/ChaffSystem.h) streams SoA instead of per-agent
objects. It combines shared flow, local separation/alignment, density pressure
and crowd relief, correlated wander, speed/acceleration limits, and wall response.
Squads add anchor guidance and stronger separation between groups. Positional
contact relaxation resolves overlap; steering forces alone cannot maintain
spacing in a packed lane.

Neighbors read saved positions/velocities so parallel ranges do not observe
partially moved agents. Per-range RNG derives from a fresh world tick draw.
This avoids shared RNG races but changing the partition can assign different
draw sequences to individual agents.

Zero flow uses the clearance-field gradient for recovery. SDF response adjusts
normal/tangent motion; mask containment prevents a walker that started on tissue
from ending inside a wall/runtime block. Hidden, latched, pending-dead and
underground agents receive their explicit state handling. Family tuning supplies
size/speed/contact/pressure/replication/steering; visual silhouette also derives
collision radius.

Objective arrival uses the configured footprint and counts as a leak. Replication
appends initialized agents subject to capacity and adds to spawned tallies.
Tests: [test_chaff_system.cpp](../tests/test_chaff_system.cpp),
[test_bacteria_flow.cpp](../tests/test_bacteria_flow.cpp), and
[test_squads.cpp](../tests/test_squads.cpp).

## Spatial broadphase

[SpatialHash](../src/sim/spatial/SpatialHash.h) is a uniform grid over
`sim_bounds`. Counting/scatter passes build CSR spans: flat agent indices plus
per-cell starts/occupancy. There is no vector per cell.

Circle, rectangle, cone and neighborhood queries return conservative cell
candidates. Callers perform exact geometry/flag checks. Reserve reusable output
vectors; bulk separation uses raw spans rather than construct a vector per
agent. Cell size/scan extent must cover interaction radii and mixed-family sizes.
Occupancy describes the last rebuild, not automatically synchronized
post-compaction state. See [test_spatial_hash.cpp](../tests/test_spatial_hash.cpp).

## Tissue, clearance and flow

Level baking builds linked representations:

1. Splines add walkable lumen; authored obstacles subtract solids.
2. Geometry-backed render SDF supplies smooth swept shapes/fillets; its sign
   feeds back into the mask during the shared geometry bake.
3. `DistanceField` computes signed mask clearance for steering/placement.
4. `FlowField` solves cost-to-goal and derives smoothed descent directions.

[LevelLoader](../src/game/level/Level.cpp) and
[RenderSdf](../src/game/level/RenderSdf.h) own geometry construction.
[FlowField](../src/sim/flowfield/FlowField.h) owns solving/sampling. The cost
solve uses a four-neighbor upwind eikonal update with heap propagation.
`allow_diagonals` allows combining both upwind axes into a round metric; it does
not select an eight-neighbor graph. Multi-goal regions match oriented objective
footprints. Smoothing is checked against descent to avoid introducing traps.

`sample()` interpolates guidance; zero means no guidance. `sample_cost()` returns
infinity when unreachable. `reachable()` is the validation query;
`sample_with_support()` also reports how much of the stencil is valid for clean
overlay edges.

`mark_dirty(rect)` queues runtime edits. Incremental solving invalidates
dependent costs and propagates beyond the edited rectangle as necessary; a
margin is not a guarantee that a reroute stays inside it. `rebake_pending()`
drains edits for correctness comparisons. `pump_rebake()` processes complete
regions and checks elapsed budget between them. At least one runs, so an
expensive region can exceed budget. Queue progress can differ by machine/tick.
SimWorld uses the pump even in runners ticking the world directly.

Tests: [test_flowfield_mask.cpp](../tests/test_flowfield_mask.cpp),
[test_flowfield_topology.cpp](../tests/test_flowfield_topology.cpp),
[test_flowfield_rebake.cpp](../tests/test_flowfield_rebake.cpp),
[test_tissue_raster.cpp](../tests/test_tissue_raster.cpp), and
[test_render_sdf.cpp](../tests/test_render_sdf.cpp).

### Runtime blocks and scars

Towers do not carve the mask; their footprints control placement spacing and
sprite size. Fibrin Clot/scars use
[RuntimeBlock.h](../src/sim/flowfield/RuntimeBlock.h) to carve oriented bars,
record displaced cells, restore them on removal, and mark flow dirty. Static
clearance stays unchanged.

`TissueMask` has separate walkability/runtime-block planes. `walkable()` answers
where pathogens may walk; `authored_wall()` answers which solids also stop
friendly rounds. Friendly swarmers read the static SDF and cross runtime blocks.
Chaff and named walkers both use `contain_to_walkable()`, so an elite cannot
bypass a clot that blocks chaff. Placement clearance remains authored-tissue
clearance by design.

[ScarSystem](../src/sim/scar/Scars.h) makes health-bearing ECS bars across local
flow. Builds reinforce nearby/overlapping scars instead of stacking, respect
owner caps, and refuse lane-severing blocks. Hostile damage or optional lifetime
expiry triggers footprint restoration and destruction. Tests:
[test_scars.cpp](../tests/test_scars.cpp),
[test_active_abilities.cpp](../tests/test_active_abilities.cpp).

## Squads

[SquadRegistry](../src/sim/squad/Squads.h) stores authored routes and live squad
anchors/descriptors; chaff carries membership. It is a steering/readability
layer, not a second agent owner. Centroids update before movement. Anchors follow
routes subject to group progress; registry retirement/reuse is distinct from
chaff compaction.

Within groups, the normal crowd kernel remains active. Between groups, guidance
and separation preserve readable formations. Disabled tuning or no routes falls
back to ungrouped movement. Use the squad debug overlay for routes/anchors.
Tests: [test_squads.cpp](../tests/test_squads.cpp),
[test_level_lanes.cpp](../tests/test_level_lanes.cpp).

## Named agents and ECS

[EcsWorld](../src/sim/ecs/EcsWorld.h) schedules `PreUpdate`, `AI`, `Movement`,
`Combat`, then `PostUpdate`. Within phases, integer sort key wins, then
registration index. [Components.h](../src/sim/ecs/Components.h) supplies shared
transform/velocity/health/sprite/tower/scar/telegraph data.

[AiStateMachine.h](../src/sim/ecs/AiStateMachine.h) defines bounded archetype
tables, ordered transition conditions, state speed scales, attack windup/active/
recovery, steering and optional function-pointer hooks. First matching transition
wins; there is no enemy subclass hierarchy.
[NamedAgents.cpp](../src/sim/ecs/NamedAgents.cpp) installs timer/AI/movement/
combat/cleanup systems. `SpawnOrder` gives canonical iteration because EnTT
storage order changes on destruction; `NamedSeed` supplies per-entity randomness.
The game roster registers behaviors and specialized hooks.

Check entity validity when resolving `EntityId`. SimWorld builds flat sorted
named-target and friendly tower/scar snapshots for kernels that should not depend
on registry storage order; it lands returned damage on ECS health.

For a behavior extension, register an archetype, use existing phases, preserve
the readable telegraph, and test spawning/destruction/reinitialization.
Tests: [test_ecs_scheduler.cpp](../tests/test_ecs_scheduler.cpp),
[test_named_agents.cpp](../tests/test_named_agents.cpp),
[test_enemies_roster.cpp](../tests/test_enemies_roster.cpp).

## Damage fields, rounds and swarmers

[DamageSystem](../src/sim/damage/DamageField.h) evaluates circle/rect/cone/
bounded-chain fields via spatial candidates. Default thinning subtracts
`kill_rate * dt` density with falloff/debuff factors. Optional probabilistic
removal rolls whole-agent deaths using per-field RNG. Family masks restrict
targets. `friendly_fire` fields are skipped by the chaff pass; the flag alone
is not a universal implemented friendly-damage dispatcher.

Persistent fields (`lifetime <= 0`) must be re-submitted each tick; timed fields
age themselves. `rendered_fields()` snapshots the last apply pass. Rendering
the live submission buffer after clear would omit persistent fields. Damage
fields never compact chaff.

[Projectiles](../src/sim/projectile/Projectiles.h) are real bounded SoA rounds,
with lifetimes, owners, family masks, piercing/incendiary flags, approximate
crowd broadphase hits and swept authored-wall grid checks. This is not a global
nearest-target or per-bullet rigid-body solver. Cosmetic tracers are separate
VFX objects; removing them cannot remove the round.

[Swarmers](../src/sim/swarm/Swarmers.h) are the live immune cells used by normal
direct deployment, and also the units released by legacy spawner towers.
Profiles supply movement/life/contact/attack/build parameters. Current mappings
are Shooter (Neutrophil), ArborGrabber (Macrophage), Latch (Cytotoxic T),
MucusBomber (Goblet Cell), Builder (Fibroblast). Units target eligible chaff or
named snapshots; hidden targets are excluded. Result/effect buffers request
changes to other stores, then SimWorld applies them.

Arbor grabbers have multiple arm phases/captive handles and group wall behavior.
Latch units drain hosts, shooters make rounds, bombs make fluid, builders request
scars. A new kind must resolve identities across compaction rather than retain
raw chaff indices. Tests: [test_damage_field.cpp](../tests/test_damage_field.cpp),
[test_projectiles.cpp](../tests/test_projectiles.cpp),
[test_swarmers.cpp](../tests/test_swarmers.cpp),
[test_towers.cpp](../tests/test_towers.cpp).

Direct `deploy()` creates exactly one unit with `kPersistent`, which suppresses
ordinary age-limit decay. It can still die from hostile damage or complete its
contact/build behavior. It has no ECS tower owner: the high-range attribution
ID groups its combat/scars only. Legacy `place()` creates an ECS spawner, whose
volleys normally have finite lifetime. Use `validate_deploy()` and
`deploy_cells()` for new player/bot placement paths rather than reviving the
legacy spawner assumptions.

## Hostile attacks and burrowing

[HostileSystem](../src/sim/hostile/HostileAttacks.h) supports per-family latch,
toxin shots and aura damage against swarmers/towers/scar bars. Shipped viruses
latch/feed; bacteria launch visible toxin pellets. Queries run from friendlies
into the chaff hash, then passengers advance serially. Generation/validity checks
release passengers when hosts die or slots recycle.

World applies returned tower damage to ECS health; the game tower system removes
dead towers from its placed list. ScarSystem owns scar destruction. Swarmers
killed here are flagged and retired by their store. Renderer disappearance is
never the destruction trigger.

[BurrowSystem](../src/sim/burrow/Burrow.h) runs between movement and attacks.
Tuned parasites choose exits from threat/flow/tissue inputs and pass through
timed dive/underground/emergence phases. Burrow state governs immunity/neighbor
exclusion; animation/slither streams only control presentation. Tests:
[test_hostile.cpp](../tests/test_hostile.cpp), [test_burrow.cpp](../tests/test_burrow.cpp).

## Fluid

[FluidSystem](../src/sim/fluid/Fluid.h) uses serial double-density relaxation,
viscosity and SDF projection. Substeps are tunable (default three per tick).
Corrected positions recover velocity; signed far-pressure supplies surface
attraction and near-pressure limits clumping. Its neighbor grid and seeded
emission are separate from per-particle shared world RNG draws.

Particles deposit wet coverage; chaff sweeps that coarse grid once for damage/
slow refresh instead of particle-agent pair tests. Named slows sample the same
coverage. Film/lifetime parameters determine persistence. Slow expiry runs
before refresh, so remaining in mucus continuously renews it.

`draw_radius()` follows solver spacing so render can reconstruct a connected
surface. Highlights/spray belong in render/VFX. See
[test_fluid.cpp](../tests/test_fluid.cpp).

## Accounting and immunity rules

`last_damage_stats().density_removed` combines active damage sources and feeds
kill income. Its `density_removed_by_family` remains field-only, not all-source
family damage. Optional [DamageAttribution](../src/sim/Attribution.h) records
per-owner/per-family combat for telemetry without changing choices.

Legacy `chaff_killed_total` means all compacted retirements, including leaks and
out-of-bounds despawns. Per-family kill tallies subtract those retirements and
mean damage kills. Spawn totals include replication. Do not infer income or
outcomes from count differences.

[ImmunityTuning](../src/sim/Immunity.h) carries rules such as leak damage,
incendiary fields, slow contagion, scar damage and inflammation. Game progression
folds purchases into tuning/profile copies; sim does not load saves or traverse
the tree. See [test_immunity_tree.cpp](../tests/test_immunity_tree.cpp).

## Reproducibility and hashes

Exact comparisons pin build, config, level, seed, workers, commands, and rebake
completion policy. Different worker counts change bulk RNG partitions, and
budgeted rebake progress is time-sensitive. Fixed 60 Hz removes variable frame
`dt` from integration; it does not prove cross-compiler/CPU/thread bit equality
or identical interactive input timing.

`SimWorld::state_hash()` mixes tick; selected chaff motion/density/flags/slow/
burrow/toxin streams; selected swarmer position/target/life/group/health/arbor
state and lifetime body-size scales; fluid positions; toxin shots; squad hash;
and some counters. It omits
cosmetics and does not serialize full ECS, friendly rounds, objective integrity,
RNG, or pending flow/damage state. Equal hashes are regression evidence, not
complete world equality. `named::state_hash()` separately covers named agents.

For new gameplay state, make an explicit hash decision and add direct invariant
or system-specific comparisons where the hash lacks coverage. Never hash struct
padding. Dropping events/particles must leave gameplay fingerprints and counters
unchanged. See [TESTING.md](TESTING.md) for runners and script assertions.
