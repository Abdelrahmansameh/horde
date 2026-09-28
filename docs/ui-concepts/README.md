# In-match HUD overhaul concepts

These are visual previews generated from the current match screenshot on 2026-09-28. They explore layout and art direction; labels, enemy counts, cooldowns, and some in-image descriptions are illustrative.

## Biological revision — current direction

The first set below was rejected because its shiny science-fiction surface did not match the flat tissue art. The revised prompts use the current screenshot as the direct visual reference: matte cell membranes, existing rose/burgundy palette, painted 2D shapes, very little cool color, no gloss or luminous framing.

### 04 — Soft Tissue

![Soft Tissue preview](04-soft-tissue.png)

Pale tissue folds carry compact status readouts and a shallow build row. This is the gentlest visual transition from the battlefield, though the top folds need stronger separation from the vessel at small sizes.

### 05 — Cell Pockets

![Cell Pockets preview](05-cell-pockets-v2.png)

Status information sits inside darker tissue cells. The current early-match state is respected: Neutrophil is the only unlocked tower and no ability controls are shown. The one open build pocket expands while placement is armed. This is the clearest revised concept. `05-cell-pockets.png` is the earlier draft with an incorrect tower description; the v2 image corrects it.

### 06 — Bud Cluster

![Bud Cluster preview](06-bud-cluster.png)

Five towers appear as a budding tissue cluster in a later-game state. This checks how the system scales with the full actual roster: Neutrophil, Macrophage, Cytotoxic T, Goblet Cell and Fibroblast. Names, costs and number order are pulled from the current game files. The preview suggests a promising minimal footprint, but the labels need to be larger before implementation.

**Working direction:** build from the dark tissue pockets of 05, using the compact full-roster cluster of 06 as the unlocked collection grows. Keep the biological forms matte and the placement ring restrained.

## First explorations — rejected style

## 01 — Cell Arcade

![Cell Arcade preview](01-cell-arcade.png)

A playful, high-contrast command bar and tactile tower dock. The objective, wave and ATP form a quick top scan. The selected build card, cost and world-space range ring are immediately visible. This is the strongest foundation for quick, repeated tower placement.

## 02 — Living Membrane

![Living Membrane preview](02-living-membrane.png)

An organic frame that grows from the screen edges. The left rail keeps the tower roster visible while the bottom placement strip explains the current action. This preserves the most world space but needs careful restraint so decorative edges do not compete with units.

## 03 — Immune Field Guide

![Immune Field Guide preview](03-immune-field-guide.png)

A tactical specimen-card treatment. It has the clearest expanded tower identity and selected-tower context. The large lower panels need to shrink or collapse during heavy combat so the vessel remains visible.

## Shared interaction requirements

- Keep integrity, current wave/next composition, ATP and income visible at all times.
- Show only unlocked towers, with affordable/available state, actual cost, role and number keys 1–5.
- Tower placement shows a snapped ghost, true world-space range, and unmistakable valid/invalid feedback. Left click places repeatedly; right click or Escape cancels.
- The selected-tower panel shows integrity, attack information and refund before Sell. In-run tower upgrades do not exist.
- Unlocked abilities show ready/cooldown state without obscuring the battlefield.
- Never clip the build dock at 1600×900 or let UI obscure key lane bends.
- Use the real game's tower and ability names, costs, wave data and key bindings. Generated labels and icons in these images are exploratory.

## Image generation briefs

- **Cell Arcade:** Replace the current ImGui panels on the real pink tissue battlefield with playful plum capsules, cream type, bright functional color, a top objective/wave/ATP command band, a five-card tower dock, a small ability rail and a clear valid-placement ghost.
- **Living Membrane:** Replace the panels with a translucent organic screen-edge frame, a left tower rail, compact top status pods, a bottom placement instruction strip and precise in-world placement feedback.
- **Immune Field Guide:** Replace the panels with inked tactical specimen cards, a slim top status strip, selected tower information, a lower tower roster, four ability tokens and restrained world-space callouts.
- **Soft Tissue:** Match the source art exactly, making low-profile HUD pockets and build slots from pale matte epithelial folds, with no futuristic effects.
- **Cell Pockets:** Nest the early game's integrity, wave and ATP data in burgundy tissue cells, with a single expandable Neutrophil build bud and no locked slots or abilities.
- **Bud Cluster:** Use a later-game full-roster example with five accurate tower names and costs, presenting them as a compact cluster of cell buds that grows from the lower tissue edge.

All three used the built-in image generation tool with the current screenshot as an edit reference.
