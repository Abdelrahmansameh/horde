# Immune cells and construction

Normal play deploys individual, persistent cells. Historical `TowerType`,
`TowerSystem`, `SwarmerProfile`, and `towers.json` names remain in code; they do
not imply factories in the current HUD. See [gameplay](GAMEPLAY.md) for input,
placement validation, and lifetime, and [progression](PROGRESSION.md) for
permanent upgrades.

The tuning source is [towers.json](../assets/config/towers.json), parsed by
[TowerConfig.cpp](../src/game/config/TowerConfig.cpp), mapped to profiles in
[TowerSystem.cpp](../src/game/towers/TowerSystem.cpp), and executed by
[Swarmers.cpp](../src/sim/swarm/Swarmers.cpp). The following values are current
checked-in baselines before tree effects; source/config remain authoritative.

## Baseline roster

| Cell / config key | ATP | Cell HP | Speed | Search radius | Cell size | Kind |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Neutrophil / `neutrophil` | 8 | 10 | 22 | 28 | 2.0 | `shooter` |
| Cytotoxic T / `cytotoxic_t` | 12 | 10 | 30 | 12 | 0.705 | `latch` |
| Macrophage / `macrophage` | 20 | 120 | 15 | 26 | 2.6 | `arbor_grabber` |
| Goblet Cell / `goblet_cell` | 24 | 10 | 25 | 14 | 1.8 | `mucus_bomber` |
| Fibroblast / `fibroblast` | 30 | 60 | 12 | 14 | 1.6 | `builder` |

Speed and radius use world units; HP comes from `swarm.max_health`. Each current
profile has `size_jitter: 0.1`: at spawn, a deterministic generation-based hash
gives the cell a stable size multiplier between 0.9 and 1.1, scaling both its
drawn and collision body. The table and placement-clearance check use nominal
`swarm.size`, before this per-cell variation. See
[SizeJitter.h](../src/sim/SizeJitter.h).

In particular, `stats.max_health` is retained spawner health, not the health of a
directly deployed cell. Similarly, `stats.fire_interval`,
`swarm.release_per_shot`, `stats.footprint_radius`, and `swarm.lifetime` serve
legacy spawner/nonpersistent paths. Do not use them to describe how many cells
a normal click buys, the current cell's placement clearance, or timed expiry.
All five current `family_mask` values are 255, allowing all shipped families.

## Neutrophil: magazines and mobile fire

Neutrophils seek targets, form a loose firing line, hold a standoff, and kite
rather than simply following the horde into contact. Each cell gathers and
fires a magazine of projectile granules, then reloads. Projectiles travel and
perform collision tests; this is not a continuously applied damage circle.

Baseline mechanics are nine rounds per magazine, 2.8 damage per hit, a
0.03-second interval inside the volley, 0.22-second gather, and 1.3-second
reload. Rounds travel at 20 world units/second with a 0.45 hit radius; shot
spread is 0.04 and the volley cone is 0.3 radians. Formation spacing is 4.8.
Shooter aiming leads moving targets, while projectile lifetime and finite
reach prevent unlimited-range shots.

Tree damage and accuracy improve output; Trigger Rate scales volley spacing,
gather, and reload together. **Granule Capacity** adds two magazine rounds per
level, despite its retained `neutrophil.squad_size` save key. Incendiary Rounds
adds burning patches on impacts.

## Cytotoxic T: attach, ride, drain, retarget

A Cytotoxic T searches for a target, enters attachment range, spends its
0.1-second attachment animation locking on, and then drains 4.2 density/HP per
second. It follows the target while attached and can find another target when
the previous one dies. Its baseline attach radius is 0.55. Directly deployed
free latchers have bodies; once riding a host they stop pushing other cells.

Drain damage does not require a projectile or damage field. Apoptosis Trigger
adds a nearby chaff pulse when the cell finishes a host and increases drain
against named threats. A persistent cell is not consumed with its first kill.

## Macrophage: pseudopods and swallowing

Macrophages hold a front with physical bodies and independent pseudopod arms.
An arm selects prey, extends, grips, pulls the captured chunk toward the body,
swallows it, and recovers. Captured chaff is hidden from competing targeting
while held; if the Macrophage dies its unfinished captives are released.

Baseline configuration supplies four arms and up to three nearby captives per
pull, within a 2.4 cluster radius. The phase durations are 0.14 seconds extend,
0.05 latch, 0.1 pull, and 0.017 recover. Body block is 0.9 and wall spacing 4.5;
profile values are clamped to supported ranges. A completed pull removes the
captives' remaining density. This behavior also supports named targets in
development scenes, although the game roster currently ships none.

Phagocytic Sustain restores four HP per swallowed enemy to the direct cell,
capped at its maximum. Its retained nonpersistent path instead credits the
releasing legacy tower.

## Goblet Cell: a consumed mucus delivery

A Goblet Cell pursues an eligible target and consumes itself in a mucus splash
on engagement or after its one-second chase limit. It does not deal baseline
burst damage. The splash emits 500 fluid droplets at speed 20, with radius 5
and 5.6-second droplet lifetime. Wet coverage slows targets; the baseline slow
factor 0.12 means retaining 12% of movement speed, and the timed slow lasts
three seconds after coverage stops refreshing it.

Mucus particles move, collide, spread, and expire in
[Fluid.cpp](../src/sim/fluid/Fluid.cpp); wet coverage is applied by
[SimWorld.cpp](../src/sim/SimWorld.cpp). Particle counts are subject to the
world's finite fluid capacity. Weakening Mucus makes slowed targets take more
damage; Anaphylactic Shock spreads a dying slowed target's slow to nearby
chaff. Pair baseline Goblet Cells with actual damage sources.

## Fibroblast: a consumed builder and persistent scar

Placement chooses a nearby build goal immediately. The cell walks there, then
consumes itself while submitting a scar build or reinforcement request. It
does not repeatedly construct walls from its original position. The site
picker searches 12 candidates in an annulus from 4 to 18 world units, requires
walkable tissue and usable flow, avoids existing/planned wall overlaps, and
refuses a wall that would seal the local lane. If no new site fits, it can
choose a nearby damaged scar to reinforce; with nothing useful to do the
deployment fails as `NoBuildSite`.

The baseline scar has half-length 6, half-width 1, 220 HP, spacing 6, and
reinforcement of 50 HP per consumed builder. Rotation follows local flow with
configured tilt, and the scar is trimmed to fit the tissue. Build-time
validation runs again: a site taken while the builder travels can become
reinforcement instead. `max_scars: 3` is a per-owner limit; direct deployment
gives each individual cell its own attribution owner, so this is not a global
three-scar cap for the player's board.

[Scars.cpp](../src/sim/scar/Scars.cpp) carves a solid bar from the tissue mask,
marks flow for rebake, and gives the wall a Health component. Baseline lifetime
zero means no timed expiration. Hostile damage can destroy it, after which
its carved cells are restored. Inflammation buffs nearby allied cells and
shooter reloads; Inflammatory Scarring adds contact damage around the wall.

## Ownership, capacity, and legacy APIs

Each direct cell receives a stable high-range attribution handle without an
owning ECS tower. Damage reports and a builder's scar can use that handle.
The checked-in swarmer capacity is 32,768, controlled by
[sim.json](../assets/config/sim.json). Failed placements and capacity failures
do not charge ATP; a partial batch charges only its successful cells.

`TowerSystem::place()` still creates spawner entities; `sell()` still refunds
their invested ATP using the refund fraction. Those APIs and their volley
settings support tests and legacy scenes. Their comments may mention old
terrain stamping, but their implementations and tests establish that spawner
placement/selling does not modify the tissue mask or flow field. No normal HUD
action sells a direct cell or buys an in-run upgrade.

## Verification anchors

- [Tower tests](../tests/test_towers.cpp): direct deployment and roster profile
  mapping, plus explicitly legacy spawner behavior.
- [Swarmer tests](../tests/test_swarmers.cpp): magazines, attachment, captives,
  direct-cell bodies, retirement, and mucus delivery.
- [Scar tests](../tests/test_scars.cpp): build/refuse/reinforce and restore.
- [Immunity tree tests](../tests/test_immunity_tree.cpp): upgrade effects and
  capstones, including direct shooter reload near inflamed scars.
