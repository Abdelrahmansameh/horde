# Strengthen Immunity: progression overview

Strengthen Immunity is the implemented permanent upgrade system. A new
campaign begins with Neutrophil unlocked, no abilities, and zero Memory Cells
or Antibodies. All owned improvements apply automatically to campaign runs;
there is no pre-run loadout or in-run tier upgrade system.

See [docs/PROGRESSION.md](docs/PROGRESSION.md) for the complete reference:
reward calculation, purchase rules, every upgrade line, capstones, campaign
gating, respec, save format, and implementation/testing anchors.

## Current rules

| Resource or action | Current checked-in default |
| --- | --- |
| ATP | Run-local currency for direct cell deployment; see [abilities and economy](docs/ABILITIES_ECONOMY.md). |
| Memory Cells | Terminal campaign wins and losses pay a base 30, plus 20 per cleared wave, one per 125 killed chaff, and a 60 win bonus. Even a zero-wave defeat funds the first 30-point starter rank. Elite/boss reward inputs exist but App currently does not populate them. |
| Antibodies | One on a level's first clear; replays grant none. |
| Cell or ability unlock | One Antibody and no Memory Cells. Other cells require the starter; active abilities require their related cell. |
| Leveled upgrade | Memory Cell price rises with purchased level and the node's cost weight. |
| Cell capstone | One Antibody and no Memory Cells, after its related cell is owned; Neutrophil's capstone requires the starter. No branch-point quota. |
| Respec | Refunds tracked purchases in both currencies, subtracts 25 Memory Cells, and resets every tree level to zero. Neutrophil remains available innately, and campaign clears remain. |

The tree is a compiled catalog in
[ImmunityTree.cpp](src/game/meta/ImmunityTree.cpp), with one parent per node
except Degranulation Cycle and at most five children per ordinary node. Degranulation Cycle
is the unbought root, the only initial upgrade, and the visual center of the tree.
Buying its first level opens the original nine spokes: the economy branch at Bone Marrow
Reserve, Neutrophil's remaining upgrades directly at Degranulation Cycle, the Neutrophil capstone,
and the four other cell unlocks. Economy upgrades stay within the economy branch. Active abilities and capstones grow directly from their related cell,
so a first-clear Antibody can buy a new tactical option without first purchasing
an unrelated stat chain.
Neutrophil is available innately without an unlock node. The spokes fill the
circle around Degranulation Cycle in sectors weighted by branch size, with
descendants growing outward through concentric rings in their own sectors.

## Intended retry loop

The first encounter starts with a small army. Later waves should expose its
limits while losses earn permanent upgrades that improve the next attempt.
A first clear grants an Antibody and offers a choice of a new cell, active
ability or capstone. The next level brings a larger horde and more release
pressure, giving that new tool a purpose. A skilled composition can clear on
its first try; the game does not force a loss.

The current tuning target is a median of two to four attempts per level, with
typical substantial losses funding one to three useful ranks. This is a design
target, not established human pacing. Historical V15 results retain their older
tree, population, income and controller inputs; they do not qualify the current
overhaul. The current campaign's authored counts rise from 3,000 to 61,983,
with about 1.4 times population and peak scheduled density/sec per level,
explicit cohorts and readable per-port gaps. Level 1 is virus-only; bacteria
begin on level 2 and harpoon spores on level 4.
See [campaign design](docs/CAMPAIGN_DESIGN.md) and [Balance Lab](docs/BALANCE_LAB.md)
for the exact curve and evidence limits.

Authored opening ATP rises from 300 to 750 across the campaign. Bone Marrow
Reserve adds 40 ATP per rank, up to 320 across its eight ranks. Efficient
Clearance adds 10% of base kill income per rank. Enemy durability and damage
also increase across later campaign maps; see [campaign design](docs/CAMPAIGN_DESIGN.md).
Hematopoiesis provides a 7% base placement-cost reduction per rank, and Granule
Priming adds 25% base granule damage.

Balance values live in [meta.json](assets/config/meta.json); per-node effects
and level limits live in the compiled catalog. These are implemented numbers,
not undecided placeholders. Config and source are authoritative if values
change. Each upgrade has one effect with a fixed base bonus per rank. Long
lines use successive nodes of at most five ranks, with each continuation
requiring the preceding node to be fully ranked. Combined effects have been
split into separate upgrades on their relevant branches. Existing saves migrate
their purchased ranks to the corresponding nodes and retain their spend ledger;
the simplified numbers change their exact power. The new balance needs campaign
playtesting.

## Terminology after direct deployment

Some stable save keys retain older names. `neutrophil.round_damage` is
Degranulation Cycle, which increases granule regrowth rate; Rapid Release and
Granule Damage are separate upgrades. `neutrophil.squad_size` increases magazine
capacity by one granule per rank. `macrophage.arms` increases capture capacity;
Cluster Radius is separate. `macrophage.wall` increases blocking; Reinforced Body
increases health. `hub.adaptive_memory` increases cell health; Adaptive Work
increases work rate. Persistent deployments do not expire merely with time.
See the [upgrade catalog](docs/PROGRESSION.md#upgrade-catalog) for all lines.

## Future progression design

The current UI unlocks a sequential campaign, not a branching region graph.
Grade-based payouts, endless-mode Antibody income, and cross-branch bridge
nodes remain design possibilities. No such feature is implied by a field or
legacy code path reserved for future use. A malformed or newer unsupported
save is preserved rather than overwritten with a fresh campaign.

The earlier firing-cycle merge removed Trigger Rate. Its foundation ranks still
cost 30, 53, 75, 98 and 120 MC; fifteen further paid ranks continue the line.
When an older two-key save is migrated, the purchased ranks are combined up to
the current twenty-rank limit; an already-migrated rank-five save stays at five.
Actual paid refund balances are preserved. Historical campaign results retain
their original runtime and tree; they are not measurements of the new ranks.
