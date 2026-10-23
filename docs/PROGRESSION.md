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
Firing Cycle is the unbought root and the only initial upgrade. Other cell
types and all four abilities require tree unlocks. There is no equipped
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
             + 15 * waves_cleared
             + floor(chaff_killed_total / 200)
             + 8 * elites_killed
             + 50 * bosses_killed
             + (won ? 40 : 0)
```

The 30-point floor funds the first 30-point Firing Cycle rank even
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

Each node has exactly one parent except the Firing Cycle root. Firing Cycle is
the central hub with seven children; every other node has at most five children.
Owning any level of a parent opens its children. A purchase must be below the
node's maximum level and satisfy the parent and both currency requirements. Refused
purchases leave balances and levels unchanged.

| Node kind | Current price / additional requirement |
| --- | --- |
| Cell root | One Antibody, no Memory Cells; starter owned |
| Ability root | One Antibody, no Memory Cells; related cell root owned |
| Leveled cell, hub, or ability line | Memory Cells: `(20 + 15 * current_level) * cost_weight / 100`, rounded to nearest integer by the positive integer calculation `(base * weight + 50) / 100` |
| Cell capstone | One Antibody, no Memory Cells; related cell root owned, or the starter for Neutrophil; no branch-point quota |

`current_level` is zero for the first purchase. At weight 100 the consecutive
prices are 20, 35, 50, 65, 80. Branch points remain available as descriptive
counts of purchased cell-stat levels; they no longer gate capstones.
Every purchasable cell unlock grows directly from Firing Cycle, a Memory Cell
upgrade, so players choose the cell-unlock order independently. Each ability
instead grows directly from its related cell root and therefore requires that
cell's Antibody unlock before its own ability unlock. Capstones follow the
same related-cell rule. `discovery_offers()` reports affordable cell, ability
and capstone choices after a first clear; if the starter is still unbought, an
offer records its required preparation purchase and cost explicitly.

The tree screen reveals owned nodes and their next available children,
including ancestors needed to connect imported ownership. Hidden future
nodes still exist in the catalog; appearance does not change prerequisites.
The radial layout centers on Firing Cycle. Its nine first-layer spokes
have sectors weighted by the square root of their leaf counts, with gaps
between sectors. Each occupies its own region around the
full circle and grows outward through concentric rings, with ability branches
distributed among the cell upgrades.
There is no starter node or reserved inlet gap. The view can pan/zoom
independently of the purchase graph.

## Hub and ability nodes

Firing Cycle has no parent and is the only upgrade initially available.
Buying its first level opens Bone Marrow Reserve as the single economy branch,
Granule Capacity, Accuracy and Aggro Range as Neutrophil's stat choices, its capstone, and all four other cell
unlocks. Abilities and other capstones connect directly to their related cell
roots. The following
table lists the next parent, maximum level, cost weight, and effect **per
purchased level**.

| Hub line | Parent | Max | Weight | Effect per level |
| --- | --- | ---: | ---: | --- |
| Bone Marrow Reserve | Firing Cycle | 5 | 100 | +40 starting ATP |
| Efficient Clearance | Bone Marrow Reserve | 5 | 100 | +10% baseline ATP per density removed |
| Field Requisition | Bone Marrow Reserve | 3 | 125 | -7% baseline cell price; maximum 21%, rounded, minimum 1 ATP |
| Coordinated Response | Efficient Clearance | 3 | 150 | -6% role-specific attack/work time; faster mucus delivery |
| Homeostasis | Efficient Clearance | 4 | 125 | -10% baseline integrity lost per leaked agent |
| Emergency Reserves | Field Requisition | 4 | 100 | +15% baseline health for every direct cell |
| Membrane Resilience | Field Requisition | 4 | 125 | -8% hostile damage taken by cells, scars, and legacy towers |

Coordinated Response multiplies shooter volley/gather/reload times, attachment
time, all Macrophage grab phases, and builder cooldown/secretion times by
`max(0.1, 1 - 0.06 * rank)`. Cytotoxic drain and Goblet movement speed divide by
that factor. The Goblet chase fuse stays unchanged. It does not increase nominal
per-shot damage or capstone damage.

All seven hub lines belong to the economy subtree beneath Bone Marrow Reserve.
No cell or ability unlock is inside that subtree. Macrophage, Cytotoxic T,
Goblet Cell, and Fibroblast roots each grow directly from Firing Cycle.
Neutrophil's magazine, accuracy and range choices grow directly beneath
Firing Cycle.

| Ability unlock | Parent | Children (all max 3, weight 120) |
| --- | --- | --- |
| Complement Cascade Burst | Cytotoxic T root | Cooldown Reduction: -10% baseline cooldown/level; Chain Links: +2 hops/level from default eight |
| Histamine Flare | Goblet Cell root | Cooldown Reduction: -10%/level; Radius: +12% baseline radius/level |
| Fever Response | Macrophage root | Cooldown Reduction: -10%/level; Healing Strength: +25% baseline healing magnitude/level |
| Fibrin Clot | Fibroblast root | Cooldown Reduction: -10%/level; Barrier Duration: +20% baseline lifetime/level |

Ability stat lines grow directly from their unlock; neither sibling stat
requires the other. Unlocks themselves have maximum level one.

## Cell branches

Effects scale baseline values by `1 + magnitude * level` for growth and
`max(0.1, 1 - magnitude * level)` for reductions, unless the table gives a
flat addition. These are not compounded once per purchased level; different
applicable lines can multiply one another. All cell stat lines use weight
100 unless noted.

### Neutrophil

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Firing Cycle | None (tree root) | 5 | -14% reload time and -6% firing/gathering time; weight 150 |
| Accuracy | Firing Cycle | 3 | -20% round spread and volley cone |
| Aggro Range | Firing Cycle | 3 | +10% search radius |
| Granule Capacity | Firing Cycle | 3 | +2 magazine rounds; weight 130 |
| Cell Vitality | Aggro Range | 4 | +9 base cell HP and +10% speed |

**Granule Priming**, parent Firing Cycle: neutrophil rounds deal 25% more damage.

### Cytotoxic T

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Attach Speed | Cytotoxic T root | 3 | -25% attachment time |
| Drain DPS | Cytotoxic T root | 5 | +15% drain |
| Search Radius | Cytotoxic T root | 3 | +10% search radius |
| Cell Stamina | Attach Speed | 3 | +9 base cell HP and +10% speed |
| Cell Resilience | Search Radius | 4 | +10 cell HP |

**Apoptosis Trigger**, parent Cytotoxic T root: finishing a chaff host creates a
radius-10 pulse with total center-density damage 1.2; the latcher also receives a 1.5 named-damage
multiplier. There is no named campaign target today. Resilience adds 10 base HP
per rank, then Stamina adds 9 HP per rank. Emergency Reserves multiplies
the resulting health along with every other cell's health.

### Macrophage

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Extend / Latch / Pull / Recover | Macrophage root | 4 | -10% duration of every grab phase |
| Captive Capacity | Macrophage root | 3 | +1 captive per pull and +0.4 cluster radius |
| Body Health | Macrophage root | 4 | +20% cell HP |
| Cluster Intake | Grab-cycle line | 1 | +2 captives per pull and +0.6 capture radius; weight 200 |
| Search Radius | Grab-cycle line | 3 | +10% reach |
| Defensive Membrane | Body Health | 3 | +20% cell HP and +3 percentage points of body blocking, capped at 100% |

**Phagocytic Sustain**, parent Macrophage root: each swallowed enemy heals
the directly deployed Macrophage by 4 HP, capped at its maximum. Legacy
nonpersistent units instead heal their releasing tower.

### Goblet Cell

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Splash Radius | Goblet Cell root | 3 | +15% splash radius |
| Slow Strength | Goblet Cell root | 4 | -15% remaining-speed factor, making the slow stronger |
| Cell Resilience | Goblet Cell root | 4 | +10 cell HP |
| Weakening Mucus | Splash Radius | 3 | Slowed targets take +10% damage; weight 150 |
| Slow Duration | Slow Strength | 4 | +20% timed slow duration |

**Anaphylactic Shock**, parent Goblet Cell root: a slowed chaff death spreads
its slow factor to nearby chaff within radius 3.5 for 2.5 seconds. The world
caps spreading deaths per tick. Weakening Mucus is tied to the timed slow;
it does not restore the old permanent marked-target mechanic.

### Fibroblast

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Territory | Fibroblast root | 3 | +12% site search radius, roaming leash radius, and display/search reach |
| Scar Health | Fibroblast root | 5 | +20% scar HP |
| Cell Resilience | Fibroblast root | 4 | +20% builder-cell HP |
| Scar Size | Territory | 3 | +10% scar half-length and half-width |
| Scar Network | Scar Size | 2 | +1 wall each Fibroblast keeps standing (`max_scars`); weight 150 |
| Rapid Fibrosis | Cell Resilience | 4 | -12% rest between walls (`build_cooldown`) |
| Reinforce Rate | Scar Health | 3 | +30% HP restored per completed reinforcement |
| Inflammation | Scar Health | 3 | Scars create inflamed zones with radius `scar.half_extents.x + 8` (14 with the default half length); allied cell damage and shooter reload speed there gain +10%; weight 150 |

**Inflammatory Scarring**, parent Fibroblast root: standing scars deal base
contact damage at rate 8, reaching 0.8 beyond their faces. The current effect
deals damage; it does not add the slow
promised by some older design prose.

## Applying purchases to a run

App copies the loaded `GameConfig` to `run_config_`, folds purchases into it
with `apply_immunity_tree()`, configures cell/economy/ability systems, and
supplies the remaining unlock masks, hostile damage multiplier, and world-wide
`ImmunityTuning`. Level-specific rules have the final word, including allowed
cell types and income overrides. Fresh copies prevent bonuses from compounding
across reloads or level transitions.

Current authored opening wallets are 300, 335, 370, 410, 455, 505, 560, 620,
685 and 750 ATP; Bone Marrow Reserve adds up to 200. Damage-income starts at
0.85 ATP per density removed. Level multipliers are 1 for levels 1-4, then
0.75, 0.5625, 0.421875, 0.31640625, 0.23730469 and 0.17797852. Efficient
Clearance multiplies the resulting rate by up to 1.5; level 10 therefore pays
about 0.1513 ATP per density before that upgrade, or 0.2269 at rank five.
Wave bounties are separate and unchanged. Macrophages cost 35 ATP before
placement discounts. These values reduce the former entry-budget and income
snowball while retaining growth from waves, purchased efficiency and new tools.
V15 training on seed 31013 completes all ten levels in 23 control attempts and
20 offense attempts with real rewards and purchases. The original economy
priority stalls; a continuation spends 60 earned MC on three Macro role ranks
and clears level 7 on its next attempt. This is training evidence for spending
adaptation, beyond the original five-attempt limit. Its revised fresh economy
priority still stops at level 7 after five losses. The frozen held-out check
contains six journeys on bases 72011 and 82019: control and economy complete all
ten levels on 82019, while the other four stop at level 3 or 8 after five losses.
All 105 attempts are valid; all 58 losses with an observed next attempt lead to
paid ranks, but a purchase alone does not prove a useful improvement. There are
22 flagged grace advances with surviving enemies, retained in the reports.
First-try clears and stalls mean the two-to-four-attempt target is not met
consistently. These results are conditional on the tested controllers and seeds;
human playtests remain necessary. See [Balance Lab](BALANCE_LAB.md) for the
frozen inputs, per-level ledgers, TTK sample counts and evidence limits.

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

The same compatibility rule preserves `neutrophil.round_damage` for Granule
Replenishment, `macrophage.arms` for Cluster Intake, `macrophage.wall` for
Defensive Membrane, `hub.elite_response` for Emergency Reserves and
`hub.systemic_potency` for Coordinated Response. These keys keep their purchased
ranks while applying the current effects.

## Respec and persistence

Respec is implemented. It requires at least one tracked purchase and enough
Memory Cells after refund to pay the configured 25-Memory-Cell fee. It refunds
the tracked Memory Cells and Antibodies, subtracts the fee, and resets every
tree level to zero, including Firing Cycle. Neutrophil remains available
innately. Respec preserves campaign clears and does not repay first-clear
Antibodies from replayed levels.

The default save is `save.json` inside the platform user-data directory from
`SDL_GetPrefPath("IMMUNE", "IMMUNE")`; use `--save <path>` for an explicit
file. App saves after purchases/respec and terminal campaign results. The
current JSON save version is 3, with:

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
the removed `neutrophil.unlock`, are skipped. Saved levels are clamped to each
current maximum. Loading never grants a free Firing Cycle level; Neutrophil
availability is innate. Skill reset and developer edits likewise do not force
ownership of Firing Cycle. Imported old purchases have no reconstructed spend
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

The focused Firing Cycle merge removes Trigger Rate. The five merged ranks cost
30, 53, 75, 98 and 120 MC at current settings. Old purchased ranks from both
keys add together up to rank five; actual paid refund balances are preserved.
The previous campaign results retain their original runtime and tree. This
merge uses an estimate of the old combined strength; no new balance pass was run.
