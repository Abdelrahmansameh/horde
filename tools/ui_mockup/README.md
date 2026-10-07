# UI mockup generators

[Documentation index](../../docs/README.md) · [UI framework](../../docs/UI_FRAMEWORK.md) · [Assets](../../docs/ASSETS.md)

These scripts produce HTML artboards for the supplementary
[design canvas](https://claude.ai/artifact/BmwSmwUEERL8X7rbVyfvWp).
They generate visual prototypes, not the compiled player UI.

| Script | Output artboards |
|---|---|
| [hud_screens.py](hud_screens.py) | Main, Placing, Inspect, Prep, Critical, Kit; shared palette, shapes, illustrations, and sample world |
| [menu_screens.py](menu_screens.py) | Menu, Tree, Levels, Victory, Defeat; imports HUD helpers |

## Generate locally

From the repository root:

```powershell
python tools/ui_mockup/hud_screens.py
python tools/ui_mockup/menu_screens.py
```

They use the Python standard library and write `tools/ui_mockup/project/*.dc.html`.
`project/canvas.json` is a tracked canvas index. The generated HTML references
`support.js`, supplied by the external canvas rather than this repository.
Opening the files as ordinary standalone pages does not provide that runtime.

Generated `.dc.html` files in this output folder are **not** excluded by the
current root `.gitignore`. Review repository status after generation and stage
only intentional source/snapshot files. Do not hand-edit generator output when
the change should survive regeneration; edit the scripts.

`menu_screens.py` reads tree node data from
`src/game/meta/ImmunityTree.cpp` and campaign maps from
`assets/levels/campaign_*.json`. Prototype prices use hardcoded constants rather
than reading `meta.json`. Demo levels/wallet/purchases and other labels are
sample data. Reading some live data does not make the whole prototype
authoritative for gameplay.

## Snapshot and icon extraction

The committed reference snapshot is
[docs/ui-concepts/canvas/project/](../../docs/ui-concepts/canvas/project/).
It is separate from generator output. `tools/extract_icons.py` reads that
snapshot, not these newly generated artboards. Deliberately review/update the
snapshot before extracting icons when a source illustration changes:

```powershell
python tools/extract_icons.py --check
```

The check reports stale/missing output; regeneration writes SVGs into
`assets/ui/icons/`. The live progression map uses
`tools/gen_tree_layout.py` and `assets/ui/tree_layout.json`, not this prototype's
Tree layout.

## Design status

The prototypes establish plum outlines, biological panels, lavender player
controls, flesh host controls, and Fredoka/Nunito fonts. They retain historical
stationary-tower inspection/Sell and tree interactions. The live game deploys
five types of mobile cells directly and uses a radial tree with progressive
reveal and hover cards. Current interaction and layout contracts live in
[UI framework](../../docs/UI_FRAMEWORK.md); current costs and effects live in
gameplay config and the maintained subject references.

External canvas publishing is a separate workflow requiring access and a user
request. No repository script publishes automatically. If publishing artboards,
reconcile the remote index before replacing `canvas.json`; local source
generation alone does not imply publication or refresh the committed snapshot.
