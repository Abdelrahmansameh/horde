# Pathogens and hostile pressure

The shipped game roster has four ordinary pathogen families: `virus`,
`bacteria`, `parasite`, and `spore`. Their definitions are data feeding shared simulation
systems, rather than one entity class per family. Current defaults come from
[enemies.json](../assets/config/enemies.json) and
[sim.json](../assets/config/sim.json); the config and implementation are
authoritative when tuning changes.

Related references: [gameplay](GAMEPLAY.md), [immune cells](IMMUNE_CELLS.md),
[abilities](ABILITIES_ECONOMY.md), and [progression](PROGRESSION.md).

## Baseline roster

| Family | Visual identity | Initial density | Speed tier / maximum speed | Special behavior |
| --- | --- | ---: | --- | --- |
| Virus | Bright green, smaller rounded body | 0.9 | Fast / 16.875 | Replication, lunge and latch attacks |
| Bacteria | Yellow-green, larger body | 0.9 | Fast / 16.875 | Toxin-pellet magazines |
| Parasite | Bright lavender, long slithering worm | 1.6 | Normal / 10.125 | Forward burrowing; no crowd collisions or baseline hostile attack |
| Spore | Marigold, the largest round body, coil inside | 3.0 | Slow / 15.0 (just under bacteria) | Long-range harpoon: plants and pumps toxin into the biggest cell in range |

Parasite UI icons use the matching lavender palette; the spore's
(`pathogen_spore`) its marigold.

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
Their sprites draw behind viruses and bacteria, including lingering corpses.
They pass through other pathogen families (`collides: false`). They still
space themselves from other parasites, but positional overlap corrections are
limited to one quarter of walking speed rather than an entire collision radius
per tick. Tissue containment and navigation still matter. Their hostile latch,
aura, and toxin damage values are all zero.

A parasite cycles through surface cooldown, dive, underground travel,
telegraphed exit, and emergence. Current defaults are a 5-second surface
cooldown with ±1.5-second jitter, 1.2-second dive, 1.4 seconds underground,
a visible exit telegraph during the last 0.6 seconds, and 1-second emergence.
Once ready, it saves its burrow until immune coverage reaches it or its
immediate path toward the objective. Defenders left behind do not trigger a
dive unless their reach still covers the parasite.
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

Defensive coverage includes live spawner towers and mobile immune cells. Exits
inside overlapping coverage are penalized, so parasites prefer safer ground
while still making progress. Slither heading and body waves are cosmetic, driven by movement, and
do not introduce additional attack or reach.

The slither phase follows signed **actual displacement**, including wall and
crowd correction, rather than requested velocity. The idle wiggle fades out
while moving. All centreline bends travel on the same ground track; the long
curl repeats after two primary wavelengths, so the shared phase wraps at 4π.
`head_amplitude` is retained as the config key for primary wave strength;
`curl` and `probe` add long and fine travelling bends rather than head-only
deformations. The shader converts wavelength with each instance's actual
size, and the renderer interpolates position, heading, wave and backbone bend
together. Cosmetics run after combat movement, so body blocking and cell pulls
cannot escape the distance measurement; gameplay burrows still start before
damage is applied.

A circular backbone estimates curvature from the turn per distance travelled.
Steering is filtered over 0.1 seconds and curvature over 0.2 seconds. A further
spatial bound limits the endpoint motion caused by changing the bend to one
quarter of travelled distance, capped at one quarter wavelength per second.
This prevents steering noise or tiny steps from snapping the entire body.
Curvature is also bounded to prevent folded sprites, and held during a burrow
or stop. Abrupt changes of direction or external sideways shoves can still cause
brief slip; this is a local curvature approximation rather than a stored trail.
The `[slither]` tests measure production GLSL ground-track drift, phase wraps,
size variation and curved motion. `[slither-live]` traces exercise actual gym
and campaign movement, crowds and authored waves; see [Testing](TESTING.md).

## Spore: the harpoon

The spore is the horde's siege piece, built to beat the Macrophage and to lose
to a swarm of Cytotoxic T cells. Macrophages cannot target or capture spores;
other immune cells can still attack them. It is ordinary chaff with one behavior,
[Harpoon.h](../src/sim/hostile/Harpoon.h), run by
[HostileAttacks.cpp](../src/sim/hostile/HostileAttacks.cpp) after the friendly
walks and before deaths. Unlike latch and toxin it is driven from the
shooter's side: a capped friendly hash walk cannot see 44 units, and spores
are few.

Every `retry` (0.25) seconds a ready spore searches every swarmer and disc
tower for the **biggest body** within `range` (44) of its centre, well outside a Macrophage's 26-unit search,, nearest among
equals. A Macrophage is chosen before a Neutrophil, a Neutrophil before a
Goblet Cell or Fibroblast, and a Cytotoxic T last. There is no minimum range or
dead zone: a T cell riding the spore is a legal target when it is all there
is. Collagen scars are never targeted.

A shot then runs DRAW (0.45 s windup, turned onto the target), FIRE (the hose
tip flies at 150 units/s and homes on the target's membrane), PUMP (a slug
every 0.26 s, each travelling the hose at 70 units/s, for as long as the
target lives: `pulses` 0 means no fixed volley) and RECOVER (6 s reload,
which is also how long before it can attach to anything else). **A slug deals its 84 damage when it reaches the target, not when it
leaves**, so what the player sees arrive is what hurts. Two slugs (168) kill an
unupgraded Macrophage (120); a tougher cell just takes more of them. The hose
lets go only when the target dies or gets past `break_range` (56). A spore that dies
mid-shot severs its hose, which does no more damage.

A spore stands its ground (`hold` 1) while it fights and while it reloads with
a target still in range, so it stops at the edge of its range and sieges a
cell rather than walking into its reach; with nothing in range it walks the
lane. The chaff kernel scales both its speed and its share of the contact
push by `1 - hold`: without the second, the crowd behind a planted spore
shoved it forward into the cells, and its neighbours now part round it
instead. Both factors are exactly 1 for every other family.

The look is in [chaff.frag](../assets/shaders/chaff.frag) (`sdf_spore`,
`spore_pose`) and [harpoon.frag](../assets/shaders/harpoon.frag). The body is
the bacterium's wet sac on an egg, with its hose wound inside as three slanted
rings. It squashes back during the draw, snaps into a stretch and recoils on
the launch, and squeezes a bolus toward its spout for every slug, timed so
the bolus reaches the mouth as that slug leaves it. The coil winds back in
over the reload. The hose is the body's own membrane drawn out: the same
colour ramp, rim and soft edge, rooted inside the spout's mouth (the renderer
mirrors `spore_pose` to follow it through every squash and recoil) and
necking down from it in a fillet that does not swing. Beyond the neck it is
a lash in flight and a damped standing wave once anchored, and slugs swell it
and glow as they travel. `HarpoonPulse` combat events splash toxin out of the
target's membrane on the bite and on every hit.

Every number lives in `enemies.json`'s `harpoon` block (off for every other
family). State lives on the chaff streams (`harpoon_state`, `harpoon_timer`,
`harpoon_scan`, `harpoon_sighted`, `hold`, and the cosmetic `harpoon_anim`)
and in the hostile system's hose list. Both are folded into `state_hash()` only
when a spore or hose exists, so spore-free worlds hash as before. The
`chaff10k` bench spawns only the three horde families.

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
- [Harpoon tests](../tests/test_harpoon.cpp): target order, damage on slug
  arrival, planting, severing, switch-off, and determinism.
- [Burrow tests](../tests/test_burrow.cpp): candidate rules, hidden state,
  emergence, and determinism.
- [Bacteria flow tests](../tests/test_bacteria_flow.cpp) and
  [toxin scenario](../tests/scripts/bacteria_toxin_burst.json): movement and
  the real configured attack path.
