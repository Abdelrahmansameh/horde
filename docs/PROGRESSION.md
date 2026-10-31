# Campaign, rewards, and Strengthen Immunity

This is the current progression implementation reference. The design overview
is [../PROGRESSION.md](../PROGRESSION.md), while [gameplay](GAMEPLAY.md),
[immune cells](IMMUNE_CELLS.md), and [abilities/economy](ABILITIES_ECONOMY.md)
cover the run itself. Numerical defaults below are checked-in tuning, not
final balance commitments. Source and config are authoritative.

## Responsibilities and sources

| Source | Owns |
| --- | --- |
| [MetaProgression.cpp](../src/game/meta/MetaProgression.cpp) / [header](../src/game/meta/MetaProgression.h) | Currency balances, node levels, purchase/refund rules, run rewards, campaign clears, JSON save/load |
| [ImmunityTree.cpp](../src/game/meta/ImmunityTree.cpp) / [header](../src/game/meta/ImmunityTree.h) | Compiled node catalog, stable keys, parents, maximum levels, cost weights, effects |
| [meta.json](../assets/config/meta.json) | Reward amounts, generic stat price curve, unlock/capstone prices, respec fee |
| [App.cpp](../src/app/App.cpp) | Load/apply the player's progression, collect terminal run results, save changes |
| [UiBridge.cpp](../src/app/UiBridge.cpp), [TreeScreen.cpp](../src/ui/front/TreeScreen.cpp) | Build UI models, campaign ordering, display and purchase interaction |
| [tree_layout.json](../assets/ui/tree_layout.json) | Positions and appearance of tree nodes; does not define purchase rules |

## New campaign and campaign gating

`reset_to_new_game()` starts with every tree level at zero, zero balances, and
no campaign clears. Neutrophil is available innately without an unlock node.
Degranulation Cycle is the unbought root and the only initial upgrade. Other cell
types and all three abilities require tree unlocks. There is no equipped
loadout: every owned capability and stat effect applies to each campaign run
automatically.

The selector filters level filenames beginning with `campaign_` and sorts
them alphabetically. The first level is open; each later level requires the
immediately previous level to be cleared. The current implementation is
sequential, not a branching region unlock graph. Level `name` is the stable
ID used for clear records and first-clear reward lookup; `display_name` is
presentation. Renaming the stable ID creates a new clear/reward identity.

Sandbox runs (`--sandbox`), the level named `gym`, and editor playtests use
baseline tuning and all unlocks, and do not award persistent currencies.
Ordinary direct `--level` play is not automatically a sandbox. Terminal
victory/defeat calls `finish_run()`; abandoning or restarting an unfinished
run does not pay a terminal reward.

## Two persistent currencies

**Memory Cells** fund leveled upgrades. Every
terminal campaign run receives the configured floor, including a first-wave
loss. The current reward formula is:

```text
Memory Cells = 30
             + 20 * waves_cleared
             + floor(chaff_killed_total / 125)
             + 8 * elites_killed
             + 50 * bosses_killed
             + (won ? 60 : 0)
```

The 30-point floor funds the first 30-point Degranulation Cycle rank even
after a zero-wave defeat. More cleared waves and kills fund further purchases.
The intended loop is to struggle against later waves, buy useful permanent
improvements after a loss, clear the level, choose a new tool, and bring it into
the next larger horde. A median of two to four attempts is a tuning target,
not an enforced gate or an established human difficulty measurement.

The per-run result is capped to a 32-bit reward before crediting the 64-bit
bank. App counts completed wave advances and sums player-killed chaff by family,
excluding leaks and out-of-bounds retirement. A clearing timeout can advance
with survivors, so the wave count does not prove that every enemy was killed.
This is agent count, unlike
the ATP economy's density-removal income. App currently leaves elite/boss
kill inputs zero: the shipped roster has no named elites/bosses and no
game-layer named-kill tally. The formula supports those inputs for future
callers/tests but they are not current campaign earnings.

**Antibodies** fund cell unlocks, ability unlocks, and capstones. The first
clear of a stable level ID pays one; replaying that ID pays zero additional
Antibodies. There is no grade multiplier in the current reward formula and
no ongoing post-campaign Antibody trickle.

## Purchase rules and prices

Each node has exactly one parent except the Degranulation Cycle root. Degranulation Cycle is
the central hub with nine children; every other node has at most five children.
Owning any level of a parent opens its ordinary children. A continuation upgrade
requires the preceding stage to be fully ranked. Every stage has at most five
ranks. A purchase must be below the node's maximum level and satisfy the parent
and both currency requirements. Refused
purchases leave balances and levels unchanged.

| Node kind | Current price / additional requirement |
| --- | --- |
| Cell root | One Antibody, no Memory Cells; starter owned |
| Ability root | One Antibody, no Memory Cells; related cell root owned |
| Leveled cell, hub, or ability line | Memory Cells: `(20 + 15 * current_level) * cost_weight / 100`, rounded to nearest integer by the positive integer calculation `(base * weight + 50) / 100` |
| Cell capstone | One Antibody, no Memory Cells; related cell root owned, or the starter for Neutrophil; no branch-point quota |

`current_level` counts earlier stages of the same line as well as ranks in the
current stage, so the price curve continues across upgrades. It is zero for the
first purchase of a line. At weight 100 the consecutive
prices are 20, 35, 50, 65, 80. Branch points remain available as descriptive
counts of purchased cell-stat levels; they no longer gate capstones.
Every purchasable cell unlock grows directly from Degranulation Cycle, a Memory Cell
upgrade, so players choose the cell-unlock order independently. Each ability
instead grows directly from its related cell root and therefore requires that
cell's Antibody unlock before its own ability unlock. Capstones follow the
same related-cell rule. `discovery_offers()` reports affordable cell, ability
and capstone choices after a first clear; if the starter is still unbought, an
offer records its required preparation purchase and cost explicitly.

The tree screen reveals owned nodes and their next available children,
including ancestors needed to connect imported ownership. Hidden future
nodes still exist in the catalog; appearance does not change prerequisites.
The radial layout centers on Degranulation Cycle. Its nine first-layer spokes
have sectors weighted by the square root of their leaf counts, with gaps
between sectors. Each occupies its own region around the
full circle and grows outward through concentric rings, with ability branches
distributed among the cell upgrades.
There is no starter node or reserved inlet gap. The view can pan/zoom
independently of the purchase graph.

## Upgrade catalog

Every numeric upgrade has one stated effect and the same fixed bonus per rank.
Direct bonuses are measured against the unupgraded base, not multiplied per rank.
Conditional aura and vulnerability bonuses apply to the current damage or rate;
their own per-rank bonuses remain additive.
Rate upgrades add to the base rate; the corresponding duration is the base
duration divided by the resulting rate multiplier. Related internal fields may
move together when they represent one player-facing effect, such as both dimensions
of a scar. Unlocks and capstones retain their individual one-time effects.

Long lines are split into successive nodes with at most five ranks each.
The table lists the total ranks across stages. Continuations require the previous
stage to be fully purchased and their connected parent to be owned; they continue
the original price curve. Degranulation Cycle II is connected after Granule Capacity.

### Hub and abilities

| Upgrade | Key | Total ranks | Effect per rank |
| --- | --- | ---: | --- |
| Bone Marrow Reserve | `hub.bone_marrow_reserve` | 8 | +40 starting ATP per rank |
| Efficient Clearance | `hub.efficient_clearance` | 8 | +10% base ATP from kills per rank |
| Hematopoiesis | `hub.field_requisition` | 3 | -7% cell placement cost |
| Cytokine Signaling | `hub.systemic_potency` | 3 | +6% work rate per rank for Neutrophil, Cytotoxic T, Macrophage and Fibroblast |
| Emergency Reserves | `hub.elite_response` | 4 | +15% health for every cell |
| Homeostasis | `hub.homeostasis` | 4 | -10% integrity lost per leak |
| Membrane Resilience | `hub.membrane_resilience` | 4 | -8% damage taken from pathogen attacks |
| Complement Cascade Burst | `ability.complement.unlock` | 1 | Unlock: complement proteins rupture a target, then cascade to the next one and the next |
| Cooldown Reduction | `ability.complement.cooldown` | 6 | -8% base cooldown per rank |
| Extended Cascade | `ability.complement.chain` | 4 | +2 targets the cascade spreads to, up to 16 |
| Fever Response | `ability.fever.unlock` | 1 | Unlock: a fever surge that heals all deployed cells |
| Cooldown Reduction | `ability.fever.cooldown` | 6 | -8% base cooldown per rank |
| Healing Strength | `ability.fever.magnitude` | 6 | +25% base healing per rank |
| Fibrin Clot | `ability.clot.unlock` | 1 | Unlock: a temporary bar across the lane |
| Cooldown Reduction | `ability.clot.cooldown` | 6 | -8% base cooldown per rank |
| Barrier Duration | `ability.clot.duration` | 6 | +20% base barrier duration per rank |
| Adaptive Memory | `hub.adaptive_memory` | 20 | +6% base cell health per rank |
| Adaptive Work | `hub.adaptive_work` | 20 | +4% work rate per rank for Neutrophil, Cytotoxic T, Macrophage and Fibroblast |

### Neutrophil

| Upgrade | Key | Total ranks | Effect per rank |
| --- | --- | ---: | --- |
| Degranulation Cycle | `neutrophil.round_damage` | 20 | +10% granule regrowth rate per rank |
| Chemotaxis | `neutrophil.aggro_range` | 6 | +10% base sensing range per rank |
| Granule Capacity | `neutrophil.squad_size` | 6 | +1 granule per rank |
| Directed Release | `neutrophil.accuracy` | 6 | -10% base spread per rank |
| Cell Vitality | `neutrophil.vitality` | 10 | +9 base health per rank |
| Granule Priming | `neutrophil.capstone` | 1 | +25% base granule damage |
| Rapid Release | `neutrophil.release` | 20 | +5% release rate per rank |
| Granule Damage | `neutrophil.granule_damage` | 8 | +5% granule damage per rank |
| Cell Speed | `neutrophil.speed` | 10 | +10% movement speed per rank |

### Cytotoxic T

| Upgrade | Key | Total ranks | Effect per rank |
| --- | --- | ---: | --- |
| Cytotoxic T | `cytotoxic.unlock` | 1 | Unlock: killer T cells that bind to a pathogen and break it down from within |
| Perforin Potency | `cytotoxic.drain` | 20 | +15% base bound damage per rank |
| Receptor Affinity | `cytotoxic.attach_speed` | 3 | +25% binding rate per rank |
| Search Radius | `cytotoxic.search` | 6 | +10% base hunt radius per rank |
| Cell Stamina | `cytotoxic.stamina` | 8 | +10% base movement speed per rank |
| Cell Resilience | `cytotoxic.tower_health` | 10 | +10 base health per rank |
| Rapid Pursuit | `cytotoxic.capstone` | 1 | +100% base cell movement speed |
| Cell Vitality | `cytotoxic.stamina_health` | 8 | +9 base health per rank |
| Cytokine Potency | `cytotoxic.cytokine_drain` | 3 | +6% bound damage per rank |
| Adaptive Potency | `cytotoxic.adaptive_drain` | 20 | +4% bound damage per rank |

### Macrophage

| Upgrade | Key | Total ranks | Effect per rank |
| --- | --- | ---: | --- |
| Macrophage | `macrophage.unlock` | 1 | Unlock: engulfs the horde with pseudopods while its body blocks the lane |
| Cluster Intake | `macrophage.arms` | 1 | +2 enemies per pull |
| Pseudopod Speed | `macrophage.grab_speed` | 16 | +10% pseudopod work rate per rank |
| Phagosome Capacity | `macrophage.captives` | 3 | +1 enemy per pull |
| Search Radius | `macrophage.search` | 6 | +10% base target reach per rank |
| Body Health | `macrophage.health` | 10 | +20% base health per rank |
| Defensive Membrane | `macrophage.wall` | 6 | +1 percentage point blocking per rank |
| Phagocytic Nourishment | `macrophage.capstone` | 1 | Each enemy engulfed heals the cell |
| Cluster Radius | `macrophage.intake_radius` | 1 | +0.6 capture radius per rank |
| Capture Radius | `macrophage.captives_radius` | 3 | +0.4 capture radius per rank |
| Pseudopod Reach | `macrophage.grab_reach` | 12 | +3% reach per rank |
| Cell Mobility | `macrophage.mobility` | 12 | +3% movement speed per rank |
| Reinforced Body | `macrophage.wall_health` | 6 | +20% health per rank |

### Goblet Cell

| Upgrade | Key | Total ranks | Effect per rank |
| --- | --- | ---: | --- |
| Goblet Cell | `goblet.unlock` | 1 | Unlock: secretes globs of mucus that slow whatever they coat |
| Slow Strength | `goblet.slow_strength` | 6 | -10% base remaining speed per rank |
| Splash Radius | `goblet.splash_radius` | 6 | +15% base splash radius per rank |
| Slow Duration | `goblet.slow_duration` | 8 | +20% base slow duration per rank |
| Weakening Mucus | `goblet.weakness` | 8 | +10% damage to slowed enemies per rank |
| Cell Resilience | `goblet.tower_health` | 10 | +10 base health per rank |
| Clinging Mucus | `goblet.capstone` | 1 | When a slowed target dies, its mucus coats nearby chaff and slows them too |
| Cytokine Mobility | `goblet.cytokine_speed` | 3 | +6% movement speed per rank |
| Adaptive Mobility | `goblet.adaptive_speed` | 20 | +4% movement speed per rank |

### Fibroblast

| Upgrade | Key | Total ranks | Effect per rank |
| --- | --- | ---: | --- |
| Fibroblast | `fibroblast.unlock` | 1 | Unlock: a builder that keeps walling the lane near its post until the horde kills it |
| Scar Health | `fibroblast.scar_health` | 10 | +20% base scar integrity per rank |
| Reinforce Rate | `fibroblast.reinforce` | 6 | +30% base repair per rank |
| Scar Size | `fibroblast.scar_size` | 6 | +10% base scar dimensions per rank |
| Territory | `fibroblast.build_radius` | 6 | +12% base build reach per rank |
| Inflammation | `fibroblast.inflammation` | 6 | +10% ally damage near scars per rank |
| Cell Resilience | `fibroblast.tower_health` | 10 | +20% base health per rank |
| Rapid Fibrosis | `fibroblast.build_rate` | 8 | +12% construction rate per rank |
| Scar Network | `fibroblast.scar_count` | 4 | +1 wall each Fibroblast keeps standing |
| Fibrotic Bulwark | `fibroblast.capstone` | 1 | +100% base scar length and width |
| Inflammatory Regrowth | `fibroblast.regrowth` | 6 | +10% ally granule regrowth near scars per rank |

## Applying purchases to a run

App copies the loaded `GameConfig` to `run_config_`, folds purchases into it
with `apply_immunity_tree()`, configures cell/economy/ability systems, and
supplies the remaining unlock masks, hostile damage multiplier, and world-wide
`ImmunityTuning`. Level-specific rules have the final word, including allowed
cell types and income overrides. Fresh copies prevent bonuses from compounding
across reloads or level transitions.

The Bone Marrow Reserve adds 40 ATP per rank to the loaded baseline. Run
setup adds that bonus to each map's authored opening wallet. Efficient Clearance
adds 10% of base kill income per rank. Separate enemy-scaling rules affect the
campaign independently; see [Campaign design](CAMPAIGN_DESIGN.md).

The simplified upgrades change balance relative to historical campaign runs.
Prices and tracked spending remain intact for existing purchases. Earlier saves
also receive the split-out nodes corresponding to combined effects they owned;
those migrated ranks do not add to the refund ledger. The new fixed bonuses do
not reproduce the exact old compounded effects, so current progression still
needs an earned campaign balance pass.

Historical V15 and earlier fixed-controller campaign results use their own
frozen inputs and remain evidence for those revisions. They are not win-rate
or power-growth measurements of this overhaul. New earned-profile, matched
pressure and full-campaign checks must distinguish no-grace valid clears from
capacity-censored, interrupted and failed cases. See [Balance Lab](BALANCE_LAB.md)
for the evidence contract. Human playtests are still needed for feel and pacing.

Most upgrades are numeric config edits. Per-cell capstones travel in the
`SwarmerProfile`; global rules such as slow weakness,
contagion, scar contact damage, leak reduction, and inflammation live in
[Immunity.h](../src/sim/Immunity.h). No simulation system needs to read the
save or understand tree purchase UI.

Persistent direct cells never expire merely because time passes. Save keys
such as `neutrophil.squad_size`, `cytotoxic.tower_health`,
`goblet.tower_health`, and `fibroblast.tower_health` retain old naming but now
govern magazine capacity or the direct cell's health; `fibroblast.build_radius`
is shown as Territory. Preserve stable keys
when relabeling a node unless an explicit migration is supplied.

The same compatibility rule preserves `neutrophil.round_damage` for Degranulation
Cycle, `macrophage.arms` for Cluster Intake, `macrophage.wall` for
Defensive Membrane, `hub.elite_response` for Emergency Reserves and
`hub.systemic_potency` for Cytokine Signaling. These keys keep their purchased
ranks while applying the current effects.

## Respec and persistence

Respec is implemented. It requires at least one tracked purchase and enough
Memory Cells after refund to pay the configured 25-Memory-Cell fee. It refunds
the tracked Memory Cells and Antibodies, subtracts the fee, and resets every
tree level to zero, including Degranulation Cycle. Neutrophil remains available
innately. Respec preserves campaign clears and does not repay first-clear
Antibodies from replayed levels.

The default save is `save.json` inside the platform user-data directory from
`SDL_GetPrefPath("IMMUNE", "IMMUNE")`; use `--save <path>` for an explicit
file. App saves after purchases/respec and terminal campaign results. The
current JSON save version is 5, with:

- `memory_cells`, `antibodies`, and tracked `spent_memory_cells` /
  `spent_antibodies` balances.
- `tree`, an object of owned stable node keys to purchased levels.
- `progress`, containing `levels_completed`, `completed_level_ids`, and
  `unlocked_regions`. The region field is retained data; current selector
  gating uses the sequential clear records.

Version 1/2 saves migrate their old `antibody_points` to Memory Cells and
unlocked tower indices to owned roots where an unlock node exists. Version 1
remaps indices around a removed roster slot. Old memories/loadouts have no
successor and are dropped. Unknown node keys in a supported save, including
the removed `neutrophil.unlock`, are skipped. Older saves with long lines distribute purchased ranks across stages. Saves
from before the single-effect redesign also receive corresponding split-off ranks;
the original spending ledger remains unchanged, while effect magnitudes use the
new linear bonuses. Saved levels are otherwise
clamped to each current maximum. Loading never grants a free Degranulation Cycle level; Neutrophil
availability is innate. Skill reset and developer edits likewise do not force
ownership of Degranulation Cycle. Imported old purchases have no reconstructed spend
history to refund.

Malformed JSON, a missing version, or a newer unsupported version fails
loading without partially changing progression. At App startup a failed load
starts a fresh in-memory campaign but disables writing that save, preserving
the user's unreadable or newer file. This is separate from a genuinely absent
file, which starts a new writable campaign.

## Verification and safe extensions

[test_meta_progression.cpp](../tests/test_meta_progression.cpp) covers rewards,
first clears, purchases, respec, save round trips, and migration/error cases.
[test_immunity_tree.cpp](../tests/test_immunity_tree.cpp) covers unique keys,
tree topology, costs, effects, unlock enforcement, and capstone combat paths.
[test_menu.cpp](../tests/test_menu.cpp) covers campaign/UI rules.

When adding a node, update the enum, compiled catalog, parent edges, effect
application, layout asset/generator, and focused tests together. When changing
prices, change `meta.json`; when changing an effect or max level, change the
catalog/application code and this reference. A tree-layout edit alone cannot
change prerequisites or gameplay.

The earlier Degranulation Cycle merge removed Trigger Rate. Its first five ranks still
cost 30, 53, 75, 98 and 120 MC. Old purchased ranks from both keys add together
up to the line maximum of twenty across its stages; actual paid refund balances are preserved.
An already-migrated five-rank save remains rank five, with new paid ranks
available; loading does not invent veteran ranks or Adaptive Memory ownership.
New keys are appended to the enum so existing numeric node indices stay stable.
