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
| Memory Cells | Terminal campaign wins and losses pay a base 30, plus 15 per cleared wave, one per 200 killed chaff, and a 40 win bonus. Even a zero-wave defeat funds the first 30-point starter rank. Elite/boss reward inputs exist but App currently does not populate them. |
| Antibodies | One on a level's first clear; replays grant none. |
| Cell or ability unlock | One Antibody and no Memory Cells. Other cells require the starter; active abilities require their related cell. |
| Leveled upgrade | Memory Cell price rises with purchased level and the node's cost weight. |
| Cell capstone | One Antibody and no Memory Cells, after its related cell is owned; Neutrophil's capstone requires the starter. No branch-point quota. |
| Respec | Refunds tracked purchases in both currencies, subtracts 25 Memory Cells, and resets every tree level to zero. Neutrophil remains available innately, and campaign clears remain. |

The tree is a compiled catalog in
[ImmunityTree.cpp](src/game/meta/ImmunityTree.cpp), with one parent per node
except Firing Cycle and at most five children per ordinary node. Firing Cycle
is the unbought root, the only initial upgrade, and the visual center of the tree.
Buying its first level opens seven spokes: the economy branch at Bone Marrow
Reserve, Neutrophil's remaining upgrades directly at Firing Cycle, the Neutrophil capstone,
and the four other cell unlocks. All seven hub upgrades stay within the economy
branch. Active abilities and capstones grow directly from their related cell,
so a first-clear Antibody can buy a new tactical option without first purchasing
an unrelated stat chain.
Neutrophil is available innately without an unlock node. The spokes fill the
circle around Firing Cycle in sectors weighted by branch size, with
descendants growing outward through concentric rings in their own sectors.

## Intended retry loop

The first encounter starts with a small army. Later waves should expose its
limits while losses earn permanent upgrades that improve the next attempt.
A first clear grants an Antibody and offers a choice of a new cell, active
ability or capstone. The next level brings a larger horde and more release
pressure, giving that new tool a purpose. A skilled composition can clear on
its first try; the game does not force a loss.

The current tuning target is a median of two to four attempts per level, with
typical substantial losses funding one to three useful ranks. These targets
remain unmet across the measured strategies. V15 training on one seed completed
all ten levels with the control and offense paths; fresh economy training stopped
at level 7. The frozen held-out check completed two of six journeys: control and
economy on seed 82019. The other four reached levels 3 or 8 and exhausted the
five-attempt limit. First-try clears remain visible, and human playtests are
still needed. The campaign's authored counts
rise from 2,884 to 35,822 across ten levels; peak scheduled density/sec now rises
roughly 1.32 times per level, with explicit cohorts and readable per-port gaps.
See [campaign design](docs/CAMPAIGN_DESIGN.md) and [Balance Lab](docs/BALANCE_LAB.md)
for the exact curve and evidence limits.

Opening ATP rises from 300 to 750 across the campaign, with up to another 200
from permanent reserve ranks. Baseline damage-income is 0.85 ATP per density
removed. Its level multiplier stays at 1 through level 4, then multiplies by
0.75 each level, reaching 0.17797852 on level 10. Efficient Clearance multiplies
the resulting rate by up to 1.5. Wave bounties retain their authored values. Growing hordes and
wave bounties still fund larger armies, while permanent upgrades and tool choices
matter more than the former large opening-budget increase. Macrophage deployment
costs 35 ATP. Field Requisition has three ranks at 7% each, up to a 21%
placement discount. The Neutrophil capstone, Granule Priming, increases round
damage by 25%.

Balance values live in [meta.json](assets/config/meta.json); per-node effects
and level limits live in the compiled catalog. These are implemented numbers,
not undecided placeholders. Config and source are authoritative if values
change.

## Terminology after direct deployment

Code/save names can retain older wording. `neutrophil.squad_size` now buys
**Granule Capacity**, adding magazine rounds to each Neutrophil, rather than
cells per spawner volley. Vitality and Stamina each add 9 base HP and 10% speed per rank;
persistent deployments do not gain a timed lifetime. Nodes with keys ending
in `tower_health` now improve the cell's own resilience. Macrophage sustain
heals the directly deployed cell that swallowed the prey. Fever's upgrade
improves healing strength.

Several stable save keys now express more useful effects: `neutrophil.round_damage`
buys Firing Cycle (-14% reload time and -6% firing/gathering time per rank), `macrophage.arms` buys
Cluster Intake (+2 captives and +0.6 capture radius), and `macrophage.wall` buys
Defensive Membrane (+20% HP and +3 percentage points of body blocking).
`hub.elite_response` is Emergency Reserves (+15% health for every cell), while
`hub.systemic_potency` is Coordinated Response (shorter role-specific work cycles
and faster mucus delivery). Different lines, a larger army and reinvested ATP
can multiply together; individual rank bonuses remain finite.

## Future progression design

The current UI unlocks a sequential campaign, not a branching region graph.
Grade-based payouts, endless-mode Antibody income, and cross-branch bridge
nodes remain design possibilities. No such feature is implied by a field or
legacy code path reserved for future use. A malformed or newer unsupported
save is preserved rather than overwritten with a fresh campaign.

The focused Firing Cycle merge removes Trigger Rate. The five merged ranks cost
30, 53, 75, 98 and 120 MC at current settings. Old purchased ranks from both
keys add together up to rank five; actual paid refund balances are preserved.
The previous campaign results retain their original runtime and tree. This
merge uses an estimate of the old combined strength; no new balance pass was run.
