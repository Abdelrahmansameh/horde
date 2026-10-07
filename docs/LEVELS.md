# Levels and authored content

Levels are JSON documents in [assets/levels](../assets/levels). They author geometry, lanes, spawn markers, objectives, buildable zones, and explicit wave tables. [Level.cpp](../src/game/level/Level.cpp) loads them; [LevelWriter.cpp](../src/game/level/LevelWriter.cpp) writes canonical JSON; [LevelValidate.cpp](../src/game/editor/LevelValidate.cpp) supplies the editor and CLI validation rules. Use [Level Editor](LEVEL_EDITOR.md) for interactive authoring.

## Discovery and the current catalog

The app scans the immediate `assets/levels` directory for `.json` files and skips unreadable files. Backup files ending in `.json.bak` are not levels. The player campaign consists of filenames beginning with `campaign_`, sorted lexicographically; keep the zero-padded numeric prefix when adding a campaign level. The level's `name` identifies progression records, while `display_name` supplies its human title. Renaming `name` can affect existing save records.

The following is the catalog in the current checkout. Wave counts are authored array lengths, not a promise that every map remains at that length after tuning.

| Campaign file | Display name | Region | Waves |
|---|---|---|---:|
| [campaign_01_first_bend.json](../assets/levels/campaign_01_first_bend.json) | First Bend | capillary | 7 |
| [campaign_02_island_climb.json](../assets/levels/campaign_02_island_climb.json) | Island Climb | capillary | 6 |
| [campaign_03_two_chambers.json](../assets/levels/campaign_03_two_chambers.json) | Two Chambers | lymphatic | 7 |
| [campaign_04_twin_channels.json](../assets/levels/campaign_04_twin_channels.json) | Twin Channels | capillary | 7 |
| [campaign_05_ring_road.json](../assets/levels/campaign_05_ring_road.json) | Ring Road | organ_chamber | 8 |
| [campaign_06_circuit_board.json](../assets/levels/campaign_06_circuit_board.json) | Circuit Board | lymphatic | 8 |
| [campaign_07_chevron_sieve.json](../assets/levels/campaign_07_chevron_sieve.json) | Chevron Sieve | mucosal | 9 |
| [campaign_08_blob_field.json](../assets/levels/campaign_08_blob_field.json) | Blob Field | floodplain | 9 |
| [campaign_09_funnel_walls.json](../assets/levels/campaign_09_funnel_walls.json) | The Funnel | skin | 10 |
| [campaign_10_core_breach.json](../assets/levels/campaign_10_core_breach.json) | Core Breach | organ_chamber | 10 |

Non-campaign content remains reachable with `--level`, the gym's `level` command, and editor Open:

| Files | Use |
|---|---|
| [gym.json](../assets/levels/gym.json) | Five typed lanes and developer controls; see [Gym](GYM.md) |
| [lane_schema_test.json](../assets/levels/lane_schema_test.json) | Multi-lane schema/convergence fixture |
| [capillary_test.json](../assets/levels/capillary_test.json) | Small capillary fixture used by several tests/scripts |
| [floodplain_max_horde.json](../assets/levels/floodplain_max_horde.json) | Horde-capacity/performance content fixture |
| [level_1.json](../assets/levels/level_1.json) | Earlier six-wave content |
| [capillary_switchback.json](../assets/levels/capillary_switchback.json), [capillary_2_forking_vessels.json](../assets/levels/capillary_2_forking_vessels.json), [elbow_turn.json](../assets/levels/elbow_turn.json), [plaque_field.json](../assets/levels/plaque_field.json) | Capillary geometry/content maps |
| [lymphatic_1_nodal_junction.json](../assets/levels/lymphatic_1_nodal_junction.json) | Three-spawn lymphatic map |
| [mucosal_1_gut_lining.json](../assets/levels/mucosal_1_gut_lining.json), [floodplain_mucosal.json](../assets/levels/floodplain_mucosal.json) | Mucosal content maps |
| [organ_chamber_1_lymph_node_core.json](../assets/levels/organ_chamber_1_lymph_node_core.json), [organ_chamber_1b_infected.json](../assets/levels/organ_chamber_1b_infected.json) | Five-spawn organ-chamber maps |
| [skin_1_breach.json](../assets/levels/skin_1_breach.json) | Skin content map |

The built-in `LevelLoader::default_test_level()` is a code fixture, not a JSON catalog entry. Benchmark/script/screenshot setup can fall back to it when a requested file is missing or malformed; autoplay fails instead. Check stderr before trusting a report about a supplied level.

## Schema and coordinates

The loader accepts `schema: 1` and `schema: 2`. Missing or unsupported schema is an error. Schema-2 fields are optional; the writer chooses the lowest schema that expresses the loaded document. Unknown fields are ignored by the parser and **will not survive canonical formatting or editor Save**. Add new persistent fields to `LevelDef`, loader, writer, equality, and tests together.

Coordinates, widths, radii, and bounds are world units. Vessel point `w` is a **diameter**. `half_extents` is half the rectangle's local-axis size. JSON `rotation` is counterclockwise degrees; runtime structs store radians. Colors and movement tuning belong to config/runtime systems rather than level geometry.

A compact level looks like this:

```json
{
  "schema": 1,
  "name": "example_lane",
  "region": "capillary",
  "world": { "min": [0, 0], "max": [160, 90], "cell_size": 0.5 },
  "vessels": [
    { "id": "main", "points": [ { "p": [10, 45], "w": 32 }, { "p": [150, 45], "w": 32 } ] }
  ],
  "spawn_points": [ { "id": "p0", "pos": [10, 45], "radius": 3 } ],
  "objectives": [ { "id": "organ", "pos": [150, 45], "half_extents": [5, 5], "integrity": 100 } ],
  "waves": [
    { "name": "arrival", "prep_time": 8, "atp_reward": 50, "spawns": [
      { "family": "virus", "count": 40, "duration": 4, "spawn_point_id": "p0" }
    ] }
  ]
}
```

This illustrates the data shape. Validate and playtest a newly authored file before treating it as production content.

### Geometry fields

| Field | Shape and semantics |
|---|---|
| `world` | `min`, `max`, positive `cell_size` (default 0.5 when omitted) |
| `vessels[]` | Required for a valid level: unique `id`, at least two Catmull-Rom `points` with `p` and positive `w`; optional `lane_id`, `vessel_type`, `children[]` |
| `spawn_points[]` | Unique `id`, `pos`, positive `radius` (default 3), optional `lane_id` |
| `objectives[]` | Unique `id`, `pos`, positive `half_extents`, optional `rotation`, positive `integrity` (default 100) |
| `placement_zones[]` | Optional rectangles with `min`, `max`; optional `concentrated: false` and `priority: 1` planning hints |
| `squad_paths[]` | Optional unique `id`, `lane_id`, at least two `[x,y]` polyline points, positive `half_width` (default 6) |
| `ambient_drift` | Optional `[x,y]`; a nonzero level value overrides global ambient drift |

Legacy objective `radius: r` loads as `half_extents: [r,r]`, a square rather than a circular objective. The writer can retain the compact radius spelling for an axis-aligned square.

Multiple objectives participate in flow-field goal baking, but current chaff leakage uses only the first objective's oriented rectangle and one global integrity scalar. Instantiation creates objective ECS components with authored integrity values; it does not initialize that global scalar from those values, so the sim begins at its current global default of 100. Author a primary first objective and verify any multi-objective design in play; multiple independent health pools are not implemented.

A lane groups one or more vessels sharing a `lane_id`. Omitted vessel `lane_id` defaults to its `id`. Vessel types are `artery`, `vein`, `lymphatic`, `nerve_adjacent`, and `mucosal_fold`; unknown or omitted type currently falls back to artery. A spawn without a lane tag resolves to the vessel whose first control point is nearest. Explicit tags are easier to review on branching maps.

`children[]` records vessel references and is validated for dangling ids; it does not itself create physical connectivity. Spline lumens must actually join. Lane ownership is a parallel attribution/color grid, while traversal uses one shared walkable mask and flow field. Overlapping lanes have approximation/order effects in attribution; do not use lane tags as collision barriers.

When a lane has no authored squad paths, the loader derives a spread of paths from its geometry. Authoring one path disables derivation for that lane, which can collapse arrivals to a single route. Use **Level → Bake derived squad paths to authored** before hand-tuning a full spread. Path derivation/resampling/snapping is load-time work; it does not turn the authored polyline into new walkable geometry.

### Obstacles

Obstacles carve solid tissue out of the vessel lumen after rasterization. They are static geometry; the renderer, collision, placement checks, and flow field see them as vessel walls.

| `shape` | Authored geometry |
|---|---|
| `disc` | `pos`, positive `radius` |
| `capsule` | Two `points`, positive `radius` |
| `box` | `pos`, positive `half_extents`, optional `rotation` |
| `polygon` | At least three ordered `points`, or two with positive `inflate`; otherwise `inflate` is optional and nonnegative |
| `ridge` | At least two spline points `{ "p": [x,y], "w": positiveDiameter }` |

Obstacle `id` is optional and helps diagnostics. Unknown shape names fail parsing. Capsule/polygon points may be coordinate arrays; ridge widths require object points. An obstacle cannot swallow the center of a spawn or objective; geometry validation also detects sealed lanes after baking.

### Wave fields

`waves` is mandatory and nonempty. Each wave requires a nonempty `spawns` array. `region` is a grouping/presentation label; there is no runtime procedural wave generator or `waves.json` fallback.

| Field | Current parser default/meaning |
|---|---|
| Wave `name` | Optional text label |
| Wave `prep_time` | Seconds before spawning; default 20 |
| Wave `atp_reward` | Completion reward; default 0 |
| Wave `modifier` | `none`, `fever`, or `swarm`; omitted/empty means none |
| Spawn `family` | `virus`, `bacteria`, or `parasite`; default virus; unknown names fail |
| Spawn `count` | Number released; default 0 |
| Spawn `start_time` | Seconds after spawning begins; default 0 |
| Spawn `duration` | Release interval in seconds; default 1 |
| Spawn `spawn_point_id` | Empty cycles squads across markers in authored marker order; explicit id pins the entry |
| Spawn `elite_id` | 0 for ordinary chaff; nonzero selects a roster elite |
| Spawn `squad_size` | Optional override; 0 uses runtime squad tuning |
| Spawn `squad_paths[]` | Optional authored path-id filter for the entry |

Wave index is derived from array order and is not an independent authored field. Keep marker/path ids consistent when changing geometry. The editor wave ramp generator materializes ordinary `WaveDef` entries; it is an authoring convenience, not another runtime source of waves.

### Schema-2 metadata and rules

| Fields | Use |
|---|---|
| `display_name`, `description`, `author`, `difficulty`, `tags[]` | Metadata; display title and difficulty are used in level discovery; 0 difficulty means unrated |
| `camera.center`, `view_height` | Starting play framing; zero view height derives from bounds |
| `camera.min_view_height`, `max_view_height` | Optional zoom limits; zero leaves a limit unspecified |
| `economy.starting_atp` | Nonzero overrides the opening bankroll; 0 uses the global value |
| `economy.income_multiplier` | Multiplies passive income and density income; default 1 |
| `allowed_towers[]` | Type-name allowlist; empty means all types |
| `win.survive_seconds` | Positive survival duration is an alternative win condition; 0 uses wave clearing |
| `editor.grid_size`, `camera_center`, `camera_view_height`, `notes` | Persisted authoring annotations; separate from game-camera framing |

Interactive `App::apply_level_rules()` combines economy overrides with progression effects and installs the allowlist. Both stationary placement and direct-cell deployment can enforce an installed allowlist and authored zones. Zone checks require the unit footprint to fit in one zone; empty zones mean anywhere on valid tissue within the play bounds. The current headless autoplay/screenshot setup does not install every interactive schema-2 rule; see [Balance](BALANCE.md) before interpreting those runs as full campaign parity.

## Geometry bake and off-screen approaches

The shared `bake_geometry()` path rasterizes splines, carves obstacles, bakes the smooth render SDF and reconciles mask walkability with it, bakes the simulation distance field, stamps wall-proximity cost, and solves the flow field. `instantiate()` additionally wires objectives, spawn markers, squad paths, and sim bounds. The editor uses the same bake in separate buffers, and draws the real tissue renderer.

`world` is the camera/play area. `level_sim_bounds()` can expand the simulation to cover off-screen vessel approaches, spawn discs, and objective footprints, rounded outward to cells and capped at one world extent beyond each side. Units can walk in from outside the frame, but deployment is still restricted to the play area. Connect tissue all the way to an off-screen spawn. Larger simulation bounds and finer cells increase bake cost; a grid over roughly 500,000 cells produces a validator warning.

## Validation and canonical formatting

```powershell
$immuneExe = "$env:LOCALAPPDATA\horde-build\windows-release\bin\immune.exe"
& $immuneExe --level-check assets/levels
& $immuneExe --level-fmt assets/levels --check
& $immuneExe --level-fmt assets/levels/my_level.json
```

A directory target includes only its immediate `.json` files. Validation has parser guards, a lightweight loader validator, and the richer editor validator. Loading alone is not proof that the rich validation report is clean. `--level-check` uses the rich validator with baked geometry and no config-dependent wall-cost/smoothing values.

Errors include missing core collections, invalid world sizes/cell size, duplicate ids, dangling child/lane/spawn references, invalid sizes, and baked spawn/objective positions off tissue or unreachable spawn markers. Warnings include very thin lumens, large grids, off-screen markers/objectives, lanes without spawns, single authored path spreads, zones without tissue, and backwards prep/reward ramps. Errors fail the check; warnings are advisory. Review them as authored choices, not an automatic requirement to eliminate all of them.

Validation JSON contains `levels[]` entries with path, identity, `ok`, error/warning counts, issues, cell count, bake timing, and selected per-stage timings, plus aggregate `checked`, `failed`, `errors`, `warnings`, and `ok`. Issues can name an element, index/sub-index, and world anchor.

Formatting compares the exact file text with `level_to_json()`. It writes stable field order, compact geometry, omitted defaults, and a trailing newline while retaining authored array order. Before a rewrite it reloads the canonical text and checks `level_equal()`; a round-trip mismatch refuses the rewrite. `--check` writes nothing and returns 1 if any file differs. Formatting is not a substitute for semantic validation, and CLI formatting does not create the editor's `.bak` backup.

## Adding or changing content

1. Start from an editor template or a closely related existing map. Preserve meaningful level/element ids.
2. Author physical connectivity, markers, the objective, zones, camera framing, and an explicit wave table.
3. Inspect validation and the baked picture. Test all approaches and any deliberate off-screen entries.
4. Play the in-memory document, including later waves. Test with the relevant unlock/economy assumptions.
5. Save, check the actual file with `--level-check`, and check canonical formatting.
6. Run relevant level/editor/system tests. Use comparative autoplay reports for additional evidence, with the harness limitations recorded.
7. Update this catalog and any campaign-facing documentation when adding/removing/renaming content.
