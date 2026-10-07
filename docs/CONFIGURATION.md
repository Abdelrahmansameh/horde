# Gameplay configuration

[Documentation index](README.md) · [Balance](BALANCE.md) · [Gym](GYM.md)

Gameplay numbers are loaded from complete, strict JSON files. The compiled
defaults support bootstrap and tests; they are not a substitute for missing
runtime files. The generic machinery is in [src/config](../src/config/);
the game-specific structures and schemas are in
[GameConfig.h](../src/game/config/GameConfig.h),
[GameConfig.cpp](../src/game/config/GameConfig.cpp),
[TowerConfig.cpp](../src/game/config/TowerConfig.cpp), and
[EnemyConfig.cpp](../src/game/config/EnemyConfig.cpp).

## Files and ownership

All six gameplay files use `"schema": 1` and are loaded in this order:

| File | What it controls |
|---|---|
| [towers.json](../assets/config/towers.json) | Five cell stat blocks, kind-specific mechanics, deployment interval, death effects |
| [enemies.json](../assets/config/enemies.json) | Speed tiers, family appearance/physics, replication, hit/death effects, hostile attacks, parasite burrowing, elite catalog |
| [sim.json](../assets/config/sim.json) | Pool capacities, spatial cells, flow baking, crowd limits, squads, cell contact, hostile limits, mucus solver |
| [economy.json](../assets/config/economy.json) | Starting ATP, passive income, density bounty, retained refund setting |
| [abilities.json](../assets/config/abilities.json) | Four ability cooldowns, radii, damage, duration, Fever heal parameter, Clot dimensions |
| [meta.json](../assets/config/meta.json) | Run rewards, tree prices, capstone thresholds, respec fee |

[ui_theme.json](../assets/config/ui_theme.json) is loaded separately by the
GUI. It is neither bound into the gameplay registry nor included in the six-file
gameplay hash. See [UI framework](UI_FRAMEWORK.md) for its token syntax.

The family/cell identities and enum ordering are code contracts. For example,
each tower kind determines which mechanics block is legal; changing `kind`
does not turn one cell into another. The loader derives nominal collision radius
from the family's visual silhouette. The current `size_jitter` fields give chaff
and cells a stable per-spawn size factor used by body rendering and collision.
See [Simulation](SIMULATION.md) for `size_scale` and its identity-based derivation.
The economy's `refund_fraction` wins over the duplicated tower global; it is
retained machinery and does not provide a normal-play selling feature.

## Parsing and validation

[Json.h](../src/config/Json.h) and [Field.h](../src/config/Field.h) enforce
required fields, accepted keys, field types, vector sizes, and enum spellings.
Schema-backed objects reject unknown keys. The current tower/enemy loaders do
not reject unknown top-level keys, so do not rely on those extras being applied
or surviving a dump. For `u32`, `u8`, and `i32`, numeric values must be whole and
within range: `8.0` is accepted, while `8.5` is rejected. `u64` uses a JSON integer
reader. Prefer integer spelling for integer settings. Errors carry the file and
field path.

Schemas describe standard-layout structs with field offsets. That one field
list drives parsing, dumping, registry access, and field descriptions. Not
every accepted numeric value is automatically a sensible gameplay value:
respect ranges described by the consuming system and check the result in play.

`parse_game_config` stages a complete `GameConfig` and assigns it only after
all six documents pass. `ConfigStore::load_dir` similarly stages raw JSON
documents. On startup a missing file, malformed document, or invalid schema
fails tuning initialization.

## How values reach a run

In interactive play, [App::apply_tuning_config](../src/app/App.cpp) performs:

1. Copy loaded base `config_` to `run_config_`.
2. Fold permanent tree effects into the copy for a campaign run; sandbox skips
   these bonuses and unlocks all cells and abilities.
3. Apply cell, enemy, economy, ability, hostile, burrow, and immunity settings
   through the systems' configuration seams.
4. Reapply the loaded level's rules last, including its economy and placement
   restrictions. A hot reload must not erase a level override.

Tree-only capstone fields are intentionally excluded from the authored JSON
and dump. They come from the node catalog and the player's purchases.
Rebuilding from a fresh base copy prevents bonuses from compounding after
reload. Capacity and geometry construction values feed a new `SimDesc` and
level bake; restart the level to reliably apply those changes. Do not assume
editing a stat retroactively reconstructs existing cells or live effects.

## Editing and live reload

Edit a file directly, or start the standard-library tuning UI:

```powershell
python tools/config_editor.py
```

It serves the selected config folder locally and scrapes descriptions/enums
from the C++ schemas. See [Tools](TOOLS.md) for its options. The game checks
file contents every 0.5 seconds in ordinary interactive play. It does not rely
on modification times. A syntactic or schema error keeps the previously applied
game settings; the log explains the failure. Correct the file to trigger a
fresh poll. A schema-invalid but syntactically valid file may already reside
in the store, so distinguish the store's raw documents from applied settings.

Launching with `--config <directory>` pins that directory and disables automatic
polling, including theme polling in `App::poll_config_reload`. Headless modes
load a config once. Use a full copied directory for an experiment and record
its contents; all six required files must be present.

The interactive console supports:

```text
config list macrophage
config get towers.macrophage.stats.max_health
config set towers.macrophage.stats.max_health 800
config reload
config dump
```

`set` updates bound base structs and reapplies supported live settings; it does
not save a file. `reload` explicitly reloads the selected folder. `dump` writes
all six current base gameplay files into that folder, so it persists console
experiments and can overwrite other unsaved file edits. Registry paths can
differ from nested JSON paths; discover them with `config list` rather than
guessing. `config list` without a filter truncates long output.

Simulation-script contexts have narrower callbacks than interactive play;
screenshots do not expose the gameplay config callbacks. Check [Gym](GYM.md)
before using the same command in another mode.

## Dumps and reproducibility

`immune --dump-config <directory>` writes **compiled bootstrap defaults** via
`default_game_config()`. It does not export the live game, tree bonuses, or
arbitrary values from `--config`. Use the console's `config dump` for loaded
base settings. The dump excludes `ui_theme.json`.

`ConfigStore::hash` is FNV-1a over filenames and raw text in the six-file load
order, stripping carriage returns. CRLF/LF alone does not change it; spaces
and formatting do. A console `config set` does not update the stored text or
hash. This is a file-input fingerprint, not a full hash of effective run
settings, tree purchases, level overrides, or theme. Record those other inputs
along with seed, commands, thread count, executable version, and report.
See [Simulation](SIMULATION.md) for the limits of `state_hash`.

## Adding a tunable

1. Add the member to its game or simulation tuning struct and document units,
   valid range, and whether it is construction-time or live.
2. Add its `IMMUNE_CONFIG_FIELD` or enum field to the relevant schema; keep
   the struct standard-layout. Add allowed root keys when needed.
3. Wire parse/dump/bind and the apply or `SimDesc` construction path. Updating
   a schema alone does not make a value affect a running system.
4. Add it to every required shipped file and any full config test fixtures.
   Check compiled defaults if they are intended to match authored defaults.
5. Verify failure messages, round trips, registry access, and an actual effect
   using [test_config.cpp](../tests/test_config.cpp) and a relevant behavior test.
6. Update the owning gameplay/engine reference and tuning tool metadata as needed.
