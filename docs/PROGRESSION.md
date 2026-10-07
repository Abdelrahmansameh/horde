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

`reset_to_new_game()` owns only the Neutrophil root, with zero balances and
no campaign clears. Other cell types and all four abilities require tree
unlocks. There is no equipped loadout: every owned capability and stat effect
applies to each campaign run automatically.

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

**Memory Cells** fund leveled upgrades and part of each capstone. Every
terminal campaign run receives the configured floor, including a first-wave
loss. The current reward formula is:

```text
Memory Cells = 10
             + 15 * waves_cleared
             + floor(chaff_killed_total / 200)
             + 8 * elites_killed
             + 50 * bosses_killed
             + (won ? 40 : 0)
```

The per-run result is capped to a 32-bit reward before crediting the 64-bit
bank. App counts fully cleared waves and sums player-killed chaff by family,
excluding leaks and out-of-bounds retirement. This is agent count, unlike
the ATP economy's density-removal income. App currently leaves elite/boss
kill inputs zero: the shipped roster has no named elites/bosses and no
game-layer named-kill tally. The formula supports those inputs for future
callers/tests but they are not current campaign earnings.

**Antibodies** fund cell unlocks, ability unlocks, and capstones. The first
clear of a stable level ID pays one; replaying that ID pays zero additional
Antibodies. There is no grade multiplier in the current reward formula and
no ongoing post-campaign Antibody trickle.

## Purchase rules and prices

Each node has exactly one parent except the Neutrophil root; no node has more
than three children. Owning any level of a parent opens its children. A
purchase must be below the node's maximum level and satisfy the parent,
capstone threshold if applicable, and both currency requirements. Refused
purchases leave balances and levels unchanged.

| Node kind | Current price / additional requirement |
| --- | --- |
| Cell root | One Antibody |
| Ability root | One Antibody |
| Leveled cell, hub, or ability line | Memory Cells: `(20 + 15 * current_level) * cost_weight / 100`, rounded to nearest integer by the positive integer calculation `(base * weight + 50) / 100` |
| Cell capstone | 150 Memory Cells + one Antibody; at least six stat points in that cell's branch and its immediate parent owned |

`current_level` is zero for the first purchase. At weight 100 the consecutive
prices are 20, 35, 50, 65, 80. Branch points count purchased levels of that
cell's `Stat` nodes only, excluding unlock roots, capstones, and hub nodes.
The three core hub paths and the two ability gates require Memory Cells, so
a cell/ability unlock never requires a different Antibody unlock first.

The tree screen reveals owned nodes and their next available children,
including ancestors needed to connect imported ownership. Hidden future
nodes still exist in the catalog; appearance does not change prerequisites.
The radial layout can pan/zoom independently of the purchase graph.

## Hub and ability nodes

Neutrophil leads directly to Bone Marrow Reserve, Efficient Clearance, and
Rapid Metabolism. The following table lists the next parent, maximum level,
cost weight, and effect **per purchased level**.

| Hub line | Parent | Max | Weight | Effect per level |
| --- | --- | ---: | ---: | --- |
| Bone Marrow Reserve | Neutrophil | 5 | 100 | +40 starting ATP |
| Efficient Clearance | Neutrophil | 5 | 100 | +10% baseline ATP per density removed |
| Rapid Metabolism | Neutrophil | 5 | 100 | +10% baseline passive ATP/sec |
| Field Requisition | Rapid Metabolism | 4 | 125 | -5% baseline cell price; rounded, minimum 1 ATP |
| Systemic Potency | Efficient Clearance | 3 | 150 | +4% baseline shooter/latch/burst damage; also scales incendiary and scar-contact damage |
| Homeostasis | Efficient Clearance | 4 | 125 | -10% baseline integrity lost per leaked agent |
| Elite Response | Field Requisition | 4 | 100 | +10% damage against named targets; no shipped named roster today |
| Membrane Resilience | Field Requisition | 4 | 125 | -8% hostile damage taken by cells, scars, and legacy towers |

Macrophage and Cytotoxic T roots grow from Bone Marrow Reserve. Goblet Cell
and Fibroblast roots grow from Rapid Metabolism. Neutrophil's own lines begin
at Round Damage beneath Bone Marrow Reserve.

| Ability unlock | Parent | Children (all max 3, weight 120) |
| --- | --- | --- |
| Complement Cascade Burst | Systemic Potency | Cooldown Reduction: -10% baseline cooldown/level; Chain Links: +2 hops/level from default eight |
| Histamine Flare | Systemic Potency | Cooldown Reduction: -10%/level; Radius: +12% baseline radius/level |
| Fever Response | Homeostasis | Cooldown Reduction: -10%/level; Healing Strength: +25% baseline healing magnitude/level |
| Fibrin Clot | Homeostasis | Cooldown Reduction: -10%/level; Barrier Duration: +20% baseline lifetime/level |

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
| Round Damage | Bone Marrow Reserve | 5 | +15% per-round damage |
| Trigger Rate | Round Damage | 4 | -8% volley interval, gather time, and reload time |
| Accuracy | Round Damage | 3 | -20% round spread and volley cone |
| Aggro Range | Round Damage | 3 | +10% search radius |
| Granule Capacity | Trigger Rate | 3 | +2 magazine rounds; weight 130 |
| Cell Vitality | Aggro Range | 4 | +20% cell HP and +10% speed |

**Incendiary Rounds**, parent Accuracy: impacts create radius-1.6 burning
patches, with base damage 3 over 1.2 seconds. Systemic Potency scales their
damage. The world caps ignition patches per tick through `ImmunityTuning`.

### Cytotoxic T

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Attach Speed | Cytotoxic T root | 3 | -25% attachment time |
| Drain DPS | Cytotoxic T root | 5 | +15% drain |
| Search Radius | Cytotoxic T root | 3 | +10% search radius |
| Cell Stamina | Attach Speed | 3 | +10% speed and +15% cell HP |
| Cell Resilience | Search Radius | 4 | +20% cell HP |

**Apoptosis Trigger**, parent Drain DPS: finishing a chaff host creates a
radius-2.5 pulse with damage 3; the latcher also receives a 1.5 named-damage
multiplier. There is no named campaign target today. Stamina and Resilience
both affect the same cell's HP through separate multipliers.

### Macrophage

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Extend / Latch / Pull / Recover | Macrophage root | 4 | -10% duration of every grab phase |
| Captive Capacity | Macrophage root | 3 | +1 captive per pull and +0.4 cluster radius |
| Body Health | Macrophage root | 4 | +20% cell HP |
| Arm Count | Grab-cycle line | 1 | +1 arm; weight 200; profile still clamps to supported maximum |
| Search Radius | Grab-cycle line | 3 | +10% reach |
| Wall Spacing / Body Block | Body Health | 3 | -10% formation gap; currently does not change body-block coefficient |

**Phagocytic Sustain**, parent Captive Capacity: each swallowed enemy heals
the directly deployed Macrophage by 4 HP, capped at its maximum. Legacy
nonpersistent units instead heal their releasing tower.

### Goblet Cell

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Splash Radius | Goblet Cell root | 3 | +15% splash radius |
| Slow Strength | Goblet Cell root | 4 | -15% remaining-speed factor, making the slow stronger |
| Cell Resilience | Goblet Cell root | 4 | +20% cell HP |
| Weakening Mucus | Splash Radius | 3 | Slowed targets take +10% damage; weight 150 |
| Slow Duration | Slow Strength | 4 | +20% timed slow duration |

**Anaphylactic Shock**, parent Slow Duration: a slowed chaff death spreads
its slow factor to nearby chaff within radius 3.5 for 2.5 seconds. The world
caps spreading deaths per tick. Weakening Mucus is tied to the timed slow;
it does not restore the old permanent marked-target mechanic.

### Fibroblast

| Line | Parent | Max | Effect per level |
| --- | --- | ---: | --- |
| Build Radius | Fibroblast root | 3 | +12% site search radius and display/search reach |
| Scar Health | Fibroblast root | 5 | +20% scar HP |
| Cell Resilience | Fibroblast root | 4 | +20% builder-cell HP |
| Scar Size | Build Radius | 3 | +10% scar half-length and half-width |
| Reinforce Rate | Scar Health | 3 | +30% HP restored per completed reinforcement |
| Inflammation | Scar Health | 3 | Scars create inflamed zones with radius `scar.half_extents.x + 8` (14 with the default half length); allied cell damage and shooter reload speed there gain +10%; weight 150 |

**Inflammatory Scarring**, parent Inflammation: standing scars deal base
contact damage at rate 8, reaching 0.8 beyond their faces. Systemic Potency
scales the rate. The current effect deals damage; it does not add the slow
promised by some older design prose.

## Applying purchases to a run

App copies the loaded `GameConfig` to `run_config_`, folds purchases into it
with `apply_immunity_tree()`, configures cell/economy/ability systems, and
supplies the remaining unlock masks, hostile damage multiplier, and world-wide
`ImmunityTuning`. Level-specific rules have the final word, including allowed
cell types and income overrides. Fresh copies prevent bonuses from compounding
across reloads or level transitions.

Most upgrades are numeric config edits. Per-cell capstones travel in the
`SwarmerProfile`; global rules such as slow weakness, incendiary patches,
contagion, scar contact damage, leak reduction, and inflammation live in
[Immunity.h](../src/sim/Immunity.h). No simulation system needs to read the
save or understand tree purchase UI.

Persistent direct cells never expire merely because time passes. Save keys
such as `neutrophil.squad_size`, `cytotoxic.tower_health`,
`goblet.tower_health`, and `fibroblast.tower_health` retain old naming but now
govern magazine capacity or the direct cell's health. Preserve stable keys
when relabeling a node unless an explicit migration is supplied.

## Respec and persistence

Respec is implemented. It requires at least one tracked purchase and enough
Memory Cells after refund to pay the configured 25-Memory-Cell fee. It refunds
the tracked Memory Cells and Antibodies, subtracts the fee, resets purchased
nodes, and restores the starting Neutrophil root. It preserves campaign
clears and does not repay first-clear Antibodies from replayed levels.

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
unlocked tower indices to owned roots. Version 1 remaps indices around a
removed roster slot. Old memories/loadouts have no successor and are dropped.
Unknown node keys in a supported save are skipped, saved levels are clamped
to each current maximum, and Neutrophil is always restored as owned. Imported
old purchases have no reconstructed spend history to refund.

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
change prerequisites or gameplay. Adding new first-clear milestones requires
considering the finite campaign's Antibody budget, rather than assuming
repeat clears will supply more.
