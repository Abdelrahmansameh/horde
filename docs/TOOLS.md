# Developer tools

The helpers documented here cover builds, performance reports, debug pictures, and generated UI assets. Run the examples from the repository root. Use Python 3.10 or newer for the scripts; most need only the standard library. The exceptions are font generation (`fontTools`) and the optional tree-layout PNG preview (`Pillow`). None of these Python helpers is automatically run by CTest.

| Tool | Input | Output / purpose | Extra requirement |
|---|---|---|---|
| [build.bat](../tools/build.bat) | CMake preset and operation | Configure/build/test Windows binaries | MSVC, CMake, Ninja for release, vcpkg |
| [bench_report.py](../tools/bench_report.py) | Executable and benchmark scenarios | Budget table and optional local history | Built `immune.exe`; GL for render timings |
| [balance_lab.py](../tools/balance_lab.py) | Experiment manifests and pinned run reports | Paired combat tests, campaigns, growth calibration and HTML reports | Built `immune.exe` |
| [balance_catalog.py](../tools/balance_catalog.py) | Executable upgrade/config catalog | Controlled rank probes and coverage reports | Built `immune.exe` |
| [preview_level.py](../tools/preview_level.py) | Binary `.imff` flow-field dump | Debug PNG | None |
| [extract_icons.py](../tools/extract_icons.py) | Committed HTML design snapshot | SVG icons | None |
| [gen_tree_layout.py](../tools/gen_tree_layout.py) | C++ progression catalog/edges | Tree layout JSON, optional preview PNG | Pillow only for `--preview` |
| [build_fonts.py](../tools/build_fonts.py) | Variable fonts and OFL licenses | Subset static TTF files and combined license | fontTools; network if sources are missing |
| [ui_mockup/hud_screens.py](../tools/ui_mockup/hud_screens.py), [menu_screens.py](../tools/ui_mockup/menu_screens.py) | Prototype code and selected game data | HTML design artboards | None |

## Build wrapper and report helpers

```powershell
.\tools\build.bat all windows-release
python tools/bench_report.py --help
```

See [Building](BUILDING.md) for compiler setup, presets, isolated builds, output paths, and game CLI options. See [Performance benchmarks](TESTING.md#performance-benchmarks) for benchmark commands, reports, and budgets.

The benchmark report script resolves the executable from `--exe`, then `IMMUNE`, then `%LOCALAPPDATA%\horde-build\windows-release\bin\immune.exe`. Supply `--exe` when comparing a separate build. Benchmark history defaults to the Git-ignored `tools/bench_history.csv`. Results depend on local hardware and configuration; keep the revision and inputs with performance comparisons.

For automated balance measurements, paid player actions, TTK, progression journeys,
human recordings and replay captures, use the [Balance lab](BALANCE_LAB.md).
Controller calibration and gameplay conclusions are separate: a losing bot does
not establish that a human-beatable encounter is too difficult.

## Flow-field debug pictures

Despite its name, `preview_level.py` does **not** load a level JSON file. It visualizes `.imff` dumps written by [FlowFieldDebug.h](../src/sim/flowfield/FlowFieldDebug.h). [test_flowfield_topology.cpp](../tests/test_flowfield_topology.cpp) writes selected topology dumps under the system temporary directory's `immune_flowfield_debug` folder and prints their paths. There is no game CLI flag that exports this format currently.

```powershell
$flowDump = Join-Path $env:TEMP 'immune_flowfield_debug/bifurcation.imff'
$flowPng = Join-Path $env:TEMP 'immune-flow-cost.png'
python tools/preview_level.py $flowDump --out $flowPng --mode cost
```

Use the filename actually reported by the topology test. Options are:

| Option | Meaning |
|---|---|
| Positional input | `.imff` file |
| `-o`, `--out` | Required output PNG path |
| `--mode cost` | Cost-to-goal colors; default |
| `--mode mask` | Walkable tissue and mask costs |
| `--mode sdf` | Signed-distance visualization |
| `--px N` | Pixels per grid cell; default 6 |
| `--no-arrows` | Hide flow-direction arrows |
| `--arrow-stride N` | Sample one arrow every N cells; default 2 |
| `--goal X,Y` | Mark a goal in **cell coordinates**, repeatable |

The format is little-endian version 1: `IMFF` magic, version, grid width/height, cell size, origin x/y, then row-major walkability bytes, mask cost floats, signed-distance floats, goal-cost floats, and interleaved direction x/y floats. Missing SDF data is zero; nonfinite goal costs are exported as -1. The helper checks magic, version, and exact file length. Its PNG encoder uses `struct` and `zlib`, so no plotting package is required. This is useful for reachability/direction debugging; inspect the live editor or a game screenshot for the rendered level itself.

## Generated icons

```powershell
python tools/extract_icons.py --check
python tools/extract_icons.py
```

The icon extractor reads the committed [docs/ui-concepts/canvas/project](ui-concepts/canvas/project) HTML snapshot and writes normalized SVGs in [assets/ui/icons](../assets/ui/icons). It handles both line icons and larger illustrations, adjusting view boxes and padding for the game's 64-unit icon convention. Hand-authored utility glyphs such as arrows, plus/minus, and recenter are maintained separately.

`--check` compares expected output with files on disk and returns 1 for stale/missing generated icons without writing. Running without it replaces generated output. Change the intended snapshot/source illustration, regenerate, review the SVG diff and live appearance, then retain both input and output. See [Assets](ASSETS.md) for runtime SVG loading and tint behavior.

The extractor does not read fresh output from `tools/ui_mockup/project`. Updating a prototype alone does not update the committed snapshot or runtime icons.

## Progression tree layout

```powershell
python tools/gen_tree_layout.py --check
python tools/gen_tree_layout.py
```

The generator reads the node enum/catalog/edges in [ImmunityTree.h](../src/game/meta/ImmunityTree.h) and [ImmunityTree.cpp](../src/game/meta/ImmunityTree.cpp). It writes [assets/ui/tree_layout.json](../assets/ui/tree_layout.json), arranging descendants in radial wedges. It verifies complete catalog coverage, the Neutrophil root, at most three children per node, and non-overlapping node positions. This is the live progression map layout.

`--check` returns 1 if the generated JSON differs and writes nothing. With no check flag, the normal output file is rewritten. To inspect a PNG without rewriting the JSON:

```powershell
python -m pip install pillow
$treePng = Join-Path $env:TEMP 'immune-tree-layout.png'
python tools/gen_tree_layout.py --check --preview $treePng
```

Pillow is imported only for `--preview`; checking/regenerating JSON needs only the standard library. A preview should accompany review of catalog/layout changes, and the live Tree screen remains the final check of icon sizes, labels, hit areas, and camera framing.

## Font generation

```powershell
python -m pip install fonttools
python tools/build_fonts.py
python tools/build_fonts.py --src path/to/local-font-sources
```

The script instantiates the variable Fredoka and Nunito fonts into the static files used by the UI. It emits Fredoka Medium (500), SemiBold (600), Bold (700), and Nunito SemiBold (600), Bold (700), ExtraBold (800), Black (900), under [assets/fonts](../assets/fonts). Subsets cover Basic Latin, Latin-1, and the punctuation/arrows/math symbols listed in the script. Static instances avoid relying on variable-font support in the runtime font rasterizer.

The source directory must contain `Fredoka-VF.ttf`, `Nunito-VF.ttf`, `OFL-Fredoka.txt`, and `OFL-Nunito.txt`. Missing files are downloaded from the Google Fonts repository even when `--src` is provided. Without `--src`, the helper uses a temporary `assets/fonts/.src` directory and removes it after generation. It combines licenses into `assets/fonts/OFL.txt`; keep that license with the fonts.

Generation overwrites the seven TTF outputs. Review intended weight/glyph changes and check the actual UI after rebuilding. It is unnecessary for ordinary code or documentation edits; checked-in fonts are the runtime inputs.

## HTML UI prototypes

```powershell
python tools/ui_mockup/hud_screens.py
python tools/ui_mockup/menu_screens.py
```

The scripts resolve their output directory relative to their own location, so they can run from the repository root. HUD generation writes Main, Placing, Inspect, Prep, Critical, and Kit artboards; menu generation writes Menu, Tree, Levels, Victory, and Defeat. The menu generator imports shared HUD shapes and reads selected live catalog/config/level data, alongside sample wallet, level, and purchase state.

Output lives in `tools/ui_mockup/project/*.dc.html`, separately from the committed reference snapshot. These artboards rely on the external canvas's `support.js` runtime. They are design prototypes rather than the compiled game UI and retain some historical interactions. Edit generator source for changes that must survive regeneration; review Git status because this output directory's HTML is not excluded by the root ignore rules. See the [mockup README](../tools/ui_mockup/README.md), [snapshot README](ui-concepts/README.md), and [UI framework](UI_FRAMEWORK.md).

## Routine verification

For documentation/content work, check paths and command help, then use read-only validation appropriate to the changed inputs:

```powershell
$immuneExe = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune.exe"
& $immuneExe --help
& $immuneExe --level-check assets/levels
& $immuneExe --level-fmt assets/levels --check
python tools/extract_icons.py --check
python tools/gen_tree_layout.py --check
```

A check failure is evidence to inspect, not permission to rewrite unrelated authored data. Formatting drift, stale generated assets, semantic level errors, and performance-budget failures have different remedies. [Testing](TESTING.md) describes the executable tests and capture workflow.
