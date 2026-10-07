# UI design references

[Documentation index](../README.md) · [UI framework](../UI_FRAMEWORK.md) · [Assets](../ASSETS.md)

## Committed Living Membrane snapshot

[canvas/project/](canvas/project/) holds an HTML/index snapshot of the design
canvas taken on 2026-09-28. The supplementary
[external canvas](https://claude.ai/artifact/BmwSmwUEERL8X7rbVyfvWp) is private to
its owner's account. The HTML uses the canvas's `support.js` runtime, which is
not included here; this folder is a source reference rather than a standalone
website or the game UI implementation.

| Artboard | Reference purpose |
|---|---|
| Menu | Title composition and macrophage mascot |
| Tree | Earlier skill tree appearance and reusable icons |
| Levels | Vessel campaign route and cell thumbnails |
| Main | HUD during a wave |
| Placing | Placement ghost, range, dock, and cost feedback |
| Inspect | Legacy ECS tower inspection and Sell popup |
| Prep | Preparation banner and incoming composition |
| Critical | Low-integrity warning and vignette |
| Victory / Defeat | Result layouts |
| Kit | Reusable component appearance |

The **runtime** is in `src/gui` and `src/ui`. Current placement deploys mobile
cells directly; it has no normal-play Sell popup. The current tree uses generated
radial coordinates, parent edges, progressive reveal, and hover cards instead
of the original Tree artboard's layout/top cost panel. Do not copy old prices,
tower terminology, or tree rules from the snapshot into gameplay documentation.

## Source workflow

[tools/ui_mockup/README.md](../../tools/ui_mockup/README.md) explains the HTML
generators. They write a separate `tools/ui_mockup/project/` folder. Icon
extraction reads this committed snapshot, so a reviewed snapshot update is a
separate step from generating prototypes. See [Assets](../ASSETS.md).

## Earlier explorations

Earlier image explorations had names such as Cell Arcade, Living Membrane,
Immune Field Guide, Soft Tissue, Cell Pockets, and Bud Cluster. The associated
PNG previews are not present in this checkout; this reference no longer embeds
broken image links. Their labels and numbers were illustrative, not balance
data. The chosen visual language uses biological membranes, plum outlines,
lavender player cells, flesh-toned host panels, and Fredoka/Nunito text.

Use current HUD and front-end screenshots to assess changes. Verify unclipped
layout at supported window/UI scales, legible integrity and ATP, ready/cooldown
feedback, true world-space placement range, clear valid/invalid placement, and
pointer capture. Runtime screenshots and actual controls take precedence over
exploration descriptions.
