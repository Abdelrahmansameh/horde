# Active abilities and ATP economy

ATP is a run-local resource for deploying cells. Active abilities are separate,
cooldown-gated tools that spend no ATP. Their availability comes from permanent
Strengthen Immunity unlocks in campaign play. See [gameplay](GAMEPLAY.md),
[immune cells](IMMUNE_CELLS.md), and [progression](PROGRESSION.md).

Defaults below are taken from the checked-in
[economy.json](../assets/config/economy.json),
[abilities.json](../assets/config/abilities.json), and
[meta catalog](../src/game/meta/ImmunityTree.cpp). Source and config remain the
authority as balance changes.

## ATP ledger

| Baseline input | Value | Timing |
| --- | ---: | --- |
| Starting ATP | 300 | New level/run |
| Passive income | 4 ATP/second | Simulation ticks outside Prep |
| Damage income | 0.5 ATP per density removed | Credited after the simulation tick |
| Wave reward | Authored `atp_reward` | When that wave finishes Clearing |
| Cell cost | 8 / 12 / 20 / 24 / 30 | Per successful Neutrophil / Cytotoxic T / Macrophage / Goblet Cell / Fibroblast deployment |

[Economy.cpp](../src/game/economy/Economy.cpp) accumulates fractional passive
and density income in a shared fractional remainder, crediting whole ATP as
it becomes available. Damage income is for player density removal, not
merely a drop in population due to leaks or leaving bounds. This includes
projectile and swarmer damage through
[SimWorld.cpp](../src/sim/SimWorld.cpp)'s combined damage accounting.

`deploy_cells()` validates and spawns each cell before charging it. A failed
placement costs nothing; Shift batches can partially succeed and then stop
when the next cell is unaffordable, invalid, or over capacity. Cells are not
refundable through the normal HUD. The retained `refund_fraction: 0.7` applies
to legacy spawner selling, not to normal direct deployment.

Permanent hub nodes raise starting ATP, passive income, and damage income,
or reduce global placement costs. Level `economy.starting_atp` can override
the base, while retaining the player's Bone Marrow Reserve bonus. A level's
`income_multiplier` scales both passive income and ATP per density. The level
rules are applied after the tree in `App::apply_level_rules()`.

Prep blocks passive income; it does not prevent collecting damage income from
surviving enemies or paying a wave reward. Pausing stops simulation ticks, so
simulation-based income and ability cooldowns stop. Speed changes accelerate
their simulation-time advancement. Reinitializing the economy resets the
ledger; it is not a persistent wallet.

## Casting and cooldowns

[ActiveAbilities.cpp](../src/game/abilities/ActiveAbilities.cpp) refuses a
locked or cooling ability. All abilities start ready after configuration and
have independent cooldowns. A successful cast starts its cooldown; a refused
cast does not. `ActiveAbilitySystem::tick()` reduces timers by fixed simulation
dt, including Prep. New runs reset them.

The HUD uses Q/W/E/R in the following order. Complement, Histamine, and Clot
arm a world cursor and cast on the next world click; right-click/Escape
cancels. Fever ignores a target and casts immediately when its button or E is
pressed. A new campaign starts with none unlocked. Sandbox tools expose all
four baseline abilities.

| Ability | Cooldown | Current baseline effect |
| --- | ---: | --- |
| Complement Cascade Burst (`Q`) | 90 seconds | Short chain-shaped damage field: radius 6 per hop, kill rate 120, up to eight default links. |
| Histamine Flare (`W`) | 45 seconds | Soft-edged circle at the clicked point: radius 14, kill rate 30, duration 1.5 seconds, falloff 1. |
| Fever Response (`E`) | 60 seconds | Heal every surviving deployed cell by 30% of that cell's maximum HP, capped at max. |
| Fibrin Clot (`R`) | 75 seconds | Solid bar oriented across local flow: full length 14, full width 3, lifetime 8 seconds. |

## Damage abilities

Complement submits a `FieldShape::Chain` with a short 0.1-second lifetime.
The damage system starts at the selected point, repeatedly finds the nearest
unvisited eligible chaff within the per-link radius, applies damage, and jumps
to its position. A chain stops when no further candidate is in reach. The
tree's Chain Links line adds two links per level. This is a chain field over
ordinary chaff, not an unrestricted screen-wide deletion or a direct named
enemy attack.

Histamine submits a timed circular field with soft edge falloff.
`kill_rate` is simulation damage tuning, not a promised number of agents
killed: the current density-thinning path removes `rate * dt * multiplier`
from eligible agents, subject to shape/falloff and debuffs. The damage system
also supports a probabilistic thinning mode. See
[DamageField.cpp](../src/sim/damage/DamageField.cpp) before translating a rate
into a player-facing description.

Both casts can be aimed without cell-placement-zone checks. They still only
damage eligible agents within their queried geometry; clicking an empty point
can waste a ready cast. Hidden burrowed parasites are not eligible targets.

## Fever healing

Fever's `fever_cooldown_relief: 3` is interpreted for direct cells as
`3 * 0.1 * max_health`, hence the 30% heal. Pending-kill cells are skipped:
Fever does not resurrect them. Its baseline effect is global over surviving
deployed cells, with no radius restriction or damage field.

Healing Strength multiplies this magnitude by `1 + 0.25 * level`. Optional
`fever_linger_seconds` and `fever_linger_rate` fields support extra healing
over time, but both are zero by default and the current tree does not enable
a linger duration. The radius/kill-rate fields present in the common Fever
config record do not create a Fever damage nova.

For legacy spawners the same cast reduces each tower's current release
cooldown, and optional linger continues that relief. This retained behavior
does not turn Fever into a normal-play attack-rate buff.

## Fibrin geometry

A clot uses local flow to orient its bar across the lane. It requires usable
tissue and positive dimensions and refuses a bar that would seal the lane
completely. Rejection leaves its cooldown ready. A valid clot carves cells
from the tissue mask, marks flow dirty for rebake, and blocks deployment into
its footprint. It has a timer rather than scar-style Health.

When its timer expires, the registered upkeep system restores the cells it
originally carved, without turning pre-existing blocked cells into walkable
ground. The shared [RuntimeBlock](../src/sim/flowfield/RuntimeBlock.h) rules
also serve Fibroblast scars. Clot does no baseline contact damage; its benefit
is redirection and added time for defenses. Barrier Duration increases the
8-second lifetime by 20% of baseline per level.

## Verification anchors

- [Ability tests](../tests/test_active_abilities.cpp): independent cooldowns,
  damage fields, direct-cell healing, clot validation and dissolve/restore.
- [Immunity tests](../tests/test_immunity_tree.cpp): cooldown/radius/link/heal
  modifiers and shared combat effects.
- [Deployment tests](../tests/test_towers.cpp): per-cell ATP charging and partial
  batches.
- [Session](../src/game/session/LevelSession.cpp): ordering of simulation,
  passive/damage/wave income, cooldown ticking, and outcome checks.
