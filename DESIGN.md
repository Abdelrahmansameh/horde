# IMMUNE: game design and current scope

IMMUNE is a lane defense game built around a dense, moving horde of pathogens
inside stylized body tissue. The player spends ATP to deploy immune cells,
uses emergency abilities, and protects an objective through authored waves.
Between completed or failed campaign attempts, Memory Cells and Antibodies
buy permanent improvements in Strengthen Immunity.

This document describes the design intent and identifies what currently exists.
The detailed implementation reference is in [docs/README.md](docs/README.md).
When a design statement and executable behavior disagree, the implementation,
checked-in configuration, and focused tests are authoritative. Values in the
documentation are current defaults, not a promise that balance is final.

## Current game

| Subject | Implemented behavior | Reference |
| --- | --- | --- |
| Deployment | One click places one persistent cell; holding repeats; Shift requests up to ten cells per placement event. Cells act autonomously. | [Gameplay](docs/GAMEPLAY.md), [immune cells](docs/IMMUNE_CELLS.md) |
| Roster | Neutrophil, Cytotoxic T, Macrophage, Goblet Cell, Fibroblast. A new campaign owns only Neutrophil. | [Immune cells](docs/IMMUNE_CELLS.md) |
| Pathogens | Virus replicates and latches; bacteria fire toxin pellets; parasites slither and burrow. The game roster currently contains no elites or bosses. | [Pathogens](docs/PATHOGENS.md) |
| Economy | ATP pays for cells. Passive income runs outside Prep; removing pathogen density and finishing waves also pay ATP. | [Abilities and economy](docs/ABILITIES_ECONOMY.md) |
| Abilities | Complement Cascade Burst, Histamine Flare, Fever Response, Fibrin Clot, permanently unlocked through the tree. | [Abilities and economy](docs/ABILITIES_ECONOMY.md) |
| Waves | A level supplies its spawn schedule. Normal play manually starts waves unless Auto is enabled. | [Gameplay](docs/GAMEPLAY.md) |
| Campaign | The level selector uses the ordered `campaign_*.json` files; clearing the previous level opens the next. | [Progression](docs/PROGRESSION.md) |
| Meta progression | One radial tree, two persistent currencies, implemented costs and effects, save migration, and respec. | [Progression](docs/PROGRESSION.md), [root progression overview](PROGRESSION.md) |

Normal play has no placed spawner towers, tower selling, in-run tier upgrades,
or pre-run loadout. Some code and config retain the historical names
`TowerSystem`, `TowerType`, `towers.json`, and `PlaceTower`. Legacy spawner
APIs remain usable in tests and development scenes, so finding one in source
does not establish a player-facing mechanic.

## Design pillars

1. **The horde is the spectacle.** Agents should read as a crowded moving mass:
   spreading at bends, piling up against obstructions, splitting, and rejoining.
   Flow fields, local steering, contact response, and squad guidance produce
   the pathogen movement; mucus also uses a particle fluid simulation.
2. **Readable decisions under pressure.** Family colors and distinct silhouettes
   identify threats. Incoming wave previews, objective integrity, ATP, and
   ability cooldowns should explain the next useful action.
3. **Position and combinations matter.** Bends, repeated passes, convergences,
   solid scars, and temporary clots create useful defensive locations. Damage
   cells benefit from the time bought by control cells.
4. **Biological character with practical rules.** Cytotoxic T cells ride targets,
   Macrophages pull in prey, Goblet Cells consume themselves into mucus, and
   Fibroblasts spend themselves on collagen. These are stylized mechanics,
   rather than a biological simulation.
5. **Failed terminal attempts still advance the campaign build.** Persistent
   Memory Cell rewards allow another purchase and another attempt. First-clear
   Antibodies make unlocking new capabilities depend on campaign progress.

These are evaluation criteria for changes, not additional runtime rules.
For example, preferring generous bends to a single dominant bottleneck is a
level-design preference; the loader does not enforce a universal ban on narrow
lanes. The checked-in campaign includes varied obstacle and lane layouts.

## Decisions already implemented

- Direct deployments have no timed expiry. They can die to hostile attacks,
  leave the simulation bounds, or be consumed by their own action. Buying a
  Neutrophil does not buy a factory that produces more Neutrophils.
- Friendly cells do not reserve placement space, though bodies can push during
  simulation. Scars and clots are solid geometry; both refuse a placement that
  would seal the local lane completely.
- Goblet mucus slows rather than dealing baseline damage. Weakening Mucus and
  its capstone add permanent synergy through the tree.
- Fever heals directly deployed cells. Its retained tower-cooldown behavior
  supports legacy scenes; it is not the normal-play description of the ability.
- Tree effects modify a fresh copy of loaded tuning, with unlock masks and
  world modifiers supplied alongside it. There is no equipped subset of owned
  upgrades or abilities.
- Wave definitions live in the level files. There is no current region-based
  wave generator or `assets/config/waves.json`.

## Aspirations and extension points

The original design explored a branching anatomical region campaign, threat
grades, named elite/boss encounters, and endless or overtime play. Those are
future design subjects, not implemented campaign features. The current campaign
is sequential, the shipped elite table is empty, and reward calculation does
not apply grade multipliers. A level can author a survival-time objective, but
that does not by itself create an endless mode.

The renderer and simulation are built for large hordes. Historic targets such
as 5,000–10,000 agents at 60 FPS are performance goals; they are not a hardware
guarantee. Use the repository's benchmark and profiling workflows to measure
an actual change.

Future design work should make its status explicit: describe the proposed
player behavior, identify the current behavior it changes, and update both the
relevant subject document and tests when implementation lands. Avoid restoring
old factory prices, timed swarmer populations, in-run upgrades, or unimplemented
enemy rosters by copying earlier design prose into a current reference.

## Implementation anchors

- [App](src/app/App.cpp): campaign flow, input, level loading, tree application,
  outcome rewards, and saves.
- [Level session](src/game/session/LevelSession.cpp) and
  [wave director](src/game/wave/WaveDirector.cpp): run progression and outcome.
- [Tower system](src/game/towers/TowerSystem.cpp) and
  [swarmers](src/sim/swarm/Swarmers.cpp): direct deployment and cell behavior.
- [Enemy roster](src/game/enemies/EnemyRoster.cpp),
  [hostile attacks](src/sim/hostile/HostileAttacks.cpp), and
  [burrowing](src/sim/burrow/Burrow.cpp): pathogen behavior.
- [Immunity tree](src/game/meta/ImmunityTree.cpp),
  [meta progression](src/game/meta/MetaProgression.cpp), and
  [configuration](assets/config): permanent growth and current tuning.
