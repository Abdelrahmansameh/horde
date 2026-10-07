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
| Memory Cells | Terminal campaign wins and losses pay a base 10, plus 15 per cleared wave, one per 200 killed chaff, and a 40 win bonus. Elite/boss reward inputs exist but App currently does not populate them. |
| Antibodies | One on a level's first clear; replays grant none. |
| Cell or ability unlock | One Antibody, after its parent is owned. |
| Leveled upgrade | Memory Cell price rises with purchased level and the node's cost weight. |
| Cell capstone | Owned parent, six stat points in that cell's branch, 150 Memory Cells, and one Antibody. |
| Respec | Refunds tracked purchases in both currencies, subtracts 25 Memory Cells, and retains Neutrophil and campaign clears. |

The tree is a compiled catalog in
[ImmunityTree.cpp](src/game/meta/ImmunityTree.cpp), with one parent per node and
at most three children. The Neutrophil root leads to Bone Marrow Reserve,
Efficient Clearance, and Rapid Metabolism; cell and ability unlock paths pass
through Memory Cell nodes, so buying one capability does not require buying a
different Antibody unlock first.

Balance values live in [meta.json](assets/config/meta.json); per-node effects
and level limits live in the compiled catalog. These are implemented numbers,
not undecided placeholders. Config and source are authoritative if values
change.

## Terminology after direct deployment

Code/save names can retain older wording. `neutrophil.squad_size` now buys
**Granule Capacity**, adding magazine rounds to each Neutrophil, rather than
cells per spawner volley. Vitality and Stamina improve cell health and speed;
persistent deployments do not gain a timed lifetime. Nodes with keys ending
in `tower_health` now improve the cell's own resilience. Macrophage sustain
heals the directly deployed cell that swallowed the prey. Fever's upgrade
improves healing strength.

## Future progression design

The current UI unlocks a sequential campaign, not a branching region graph.
Grade-based payouts, endless-mode Antibody income, and cross-branch bridge
nodes remain design possibilities. No such feature is implied by a field or
legacy code path reserved for future use. A malformed or newer unsupported
save is preserved rather than overwritten with a fresh campaign.
