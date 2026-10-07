# Pathogens and hostile pressure

The shipped game roster has three ordinary pathogen families: `virus`,
`bacteria`, and `parasite`. Their definitions are data feeding shared simulation
systems, rather than one entity class per family. Current defaults come from
[enemies.json](../assets/config/enemies.json) and
[sim.json](../assets/config/sim.json); the config and implementation are
authoritative when tuning changes.

Related references: [gameplay](GAMEPLAY.md), [immune cells](IMMUNE_CELLS.md),
[abilities](ABILITIES_ECONOMY.md), and [progression](PROGRESSION.md).

## Baseline roster

| Family | Visual identity | Initial density | Speed tier / maximum speed | Special behavior |
| --- | --- | ---: | --- | --- |
| Virus | Bright green, smaller rounded body | 0.6 | Fast / 16.875 | Replication, lunge and latch attacks |
| Bacteria | Yellow-green, larger body | 0.9 | Fast / 16.875 | Toxin-pellet magazines |
| Parasite | Brown, long slithering worm | 1.6 | Normal / 10.125 | Forward burrowing; no crowd collisions or baseline hostile attack |

Speeds use world units/second. Movement acceleration, crowding, fluid drag,
slows, and squad guidance affect actual motion, so maximum speed is not a
guaranteed travel rate. Every current family has `size_jitter: 0.1`, giving each
new agent a stable, deterministic 0.9–1.1 multiplier on drawn size and collision
body through [SizeJitter.h](../src/sim/SizeJitter.h). It does not multiply that
agent's density or leak cost. Chaff density is its damageable quantity, not its
objective leak cost. Removing some density earns ATP even before the agent
dies; every surviving agent that leaks costs the same baseline one integrity.
Permanent run rewards count killed agents, rather than density removed.

## Shared movement and damage

[EnemyRoster.cpp](../src/game/enemies/EnemyRoster.cpp) combines family data
and speed profiles into chaff tuning. Ordinary agents live in the shared
[ChaffBuffers](../src/sim/chaff/ChaffBuffers.h) store and move through
[ChaffSystem.cpp](../src/sim/chaff/ChaffSystem.cpp). Flow fields steer toward
the goal, authored squad paths guide cohorts, and local steering/contact
response provides spacing, pressure, wall containment, and splash around
obstructions. Family flags opt into behaviors such as replication or hiding.

Player projectiles, drain, swallowing, damage fields, and scar contact effects
remove density through shared damage paths. Wet coverage applies a timed
slow. Hidden agents are excluded from ordinary target and damage queries.
Deaths, leaks, and out-of-bounds retirement have separate accounting so a leak
is not paid as a player kill.

## Virus: replication and passengers

The baseline replication rate is 0.05 per second. The movement pass rolls a
probability of `rate * dt` per eligible agent, then resolves births serially.
One successful replication adds a daughter of baseline family density and
separates the two visible bodies around their previous shared center. This
is not a fixed five-second reproduction timer. The simulation caps replication
at 128 additions per tick and stops additions when the chaff store is full.
Daughters join the parent's squad if there is room; otherwise they are
ungrouped. The default maximum inherited squad size is 90.

Viruses near friendlies lunge and latch, ride the host's movement, and drain
health until they or the host die. Baseline latch DPS is 2 per virus, reach is
5 beyond body contact, and lunge speed is 30. A direct cell can carry three
passengers; a collagen scar can carry 40. The retained legacy tower limit is
16. Viruses already riding a friendly stop advancing along the lane. Additional
viruses beyond that host's cap continue moving rather than piling unlimited
damage onto one host.

Virus aura and toxin damage are zero in the current config. Their visible
replication split and latch animation are separate visual tuning, not extra
damage mechanics.

## Bacteria: visible toxin bursts

Bacteria do not replicate or latch in the baseline roster. They aim toxin
pellets at nearby immune cells and scars. Pellets travel toward an aimed
position and apply one hit on contact, so the attack has flight and collision
rather than being an invisible ambient damage aura.

Baseline tuning is 9 damage per pellet, range 16, speed 32, hit radius 1.35,
eight shots per magazine, a 0.14-second shot interval, and a 0.55-second
reload. Shots are staggered across the horde and finite projectile budgets
bound the work. The old description of bacteria hurting everything through
a toxin aura is stale: `aura_dps` and `aura_radius` are zero.

The implementation is
[HostileAttacks.cpp](../src/sim/hostile/HostileAttacks.cpp), with target and
pellet data defined in [HostileAttacks.h](../src/sim/hostile/HostileAttacks.h).
Hostile damage is enabled in the checked-in game config; a bare test world
without that tuning can intentionally have hostile attacks disabled.

## Parasite: burrow and emerge

Parasites use ordinary chaff storage but enable
[Burrow.cpp](../src/sim/burrow/Burrow.cpp) and the slithering visual stream.
They ignore crowd collision response (`collides: false`), rather than
jostling with virus/bacteria bodies. Tissue containment and navigation still
matter. Their hostile latch, aura, and toxin damage values are all zero.

A parasite cycles through surface cooldown, dive, underground travel,
telegraphed exit, and emergence. Current defaults are a 5-second surface
cooldown with ±1.5-second jitter, 1.2-second dive, 1.4 seconds underground,
a visible exit telegraph during the last 0.6 seconds, and 1-second emergence.
The hidden flag starts with the dive and remains through underground travel
and emergence, clearing only on return to the surface. Throughout those phases
the parasite cannot be targeted or damaged by the current roster. There is no
implemented anti-burrow cell or ability.

Exit candidates come from a forward cone, 14–42 world units away, within
55 degrees of local flow. A candidate must be walkable and reachable, reduce
cost-to-goal by at least 8, keep 30 cost-to-goal away from the objective,
respect wall clearance 2 and play-edge margin 8, and pass other topology
checks. Up to 16 candidates are scored for forward progress, crowding, and
defensive coverage. No suitable candidate means stay surfaced and retry after
0.5 seconds. Diving normally leaves the old squad, so it does not wait for a
cohort it has jumped ahead of.

The current coverage penalty is built from live ECS spawner towers in
`SimWorld::build_burrow_threats()`. Directly deployed cells are not included in
that list. Do not promise that parasites actively score and avoid normal
cell coverage; forward progress and crowd avoidance still apply in normal
play. Slither heading and body waves are cosmetic, driven by movement, and
do not introduce additional attack or reach.

## Elites, bosses, and removed content

The current `elites` array is empty, `EnemyRoster::load_defaults()` registers
no game elite archetypes, and `spawn_elite()` returns an invalid handle for
unknown/unimplemented IDs. The named-agent ECS infrastructure and test
archetypes remain; they support future development and tests, not a hidden
campaign roster. Adding an object to `enemies.json` alone does not implement a
new named enemy: config application matches already-known roster IDs.

Run reward fields for elite/boss kills likewise exist but App currently leaves
them zero. Do not document former named elites, fungi, or other retired
families as playable threats based on historical design or test fixtures.

## Verification anchors

- [Enemy roster tests](../tests/test_enemies_roster.cpp): family mapping and
  empty shipped elite roster.
- [Chaff tests](../tests/test_chaff_system.cpp) and
  [squad tests](../tests/test_squads.cpp): movement, replication, grouping.
- [Hostile tests](../tests/test_hostile.cpp): passenger and toxin behavior.
- [Burrow tests](../tests/test_burrow.cpp): candidate rules, hidden state,
  emergence, and determinism.
- [Bacteria flow tests](../tests/test_bacteria_flow.cpp) and
  [toxin scenario](../tests/scripts/bacteria_toxin_burst.json): movement and
  the real configured attack path.
