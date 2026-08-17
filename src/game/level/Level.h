// game/level/Level.h — spline-authored level format. FROZEN CONTRACT.
// Owner: Wave 2D (loader), Wave 4A (content), Wave 4D (multi-lane schema).
//
// RATIONALE (plan decision 3)
// Levels are authored as JSON *splines with per-point width*, not as painted
// bitmaps. Agents can write and diff splines in text; nobody can author a mask
// PNG without a visual editor, and this project ships no binary assets anyway.
// At load, splines rasterize into a TissueMask -> DistanceField -> FlowField.
// That gives organic, branching, variable-width vessels (DESIGN.md §4) from a
// text file. tools/preview_level.py renders one to PNG for eyeballing.
//
// MULTI-LANE SCHEMA (Wave 4D, DESIGN.md §4.1/§4.6/§9.2)
// DESIGN.md wants 2-4 *named, typed* lanes per level (each a distinct vessel
// type -- artery, lymph, nerve-adjacent, ... -- with its own hue, DESIGN.md
// §9.2), converging on shared or private objectives. Structurally a level was
// already `vessels: [...]` (plural), but nothing tagged which lane a Vessel
// belonged to, and rasterize_vessels() (TissueRaster.h, frozen, DO NOT EDIT)
// merges every spline into one flat TissueMask with no memory of origin.
// This version adds:
//   - `Vessel::lane_id` + `Vessel::type` (VesselType): the identity data.
//     A "lane" is one or more Vessel entries sharing a lane_id (e.g. a lane's
//     trunk plus a same-type branch); lane_id defaults to the vessel's own
//     `id` when omitted, so every pre-existing single-vessel level is
//     trivially a well-formed single-lane level with no JSON changes needed.
//   - `SpawnPortal::lane_id`, resolved via LevelLoader::resolve_portal_lane_id()
//     so a multi-lane level's waves know which portal feeds which lane.
//   - `LaneOwnershipMap` + `LevelLoader::build_lane_ownership_map()`: a
//     parallel, coarse per-cell "which lane owns this point" grid, since the
//     real TissueMask/FlowField (frozen) stay lane-blind by design. See the
//     .cpp for how it's built and its known approximation at lane overlaps.
//   - `PlacementZoneTag` (concentrated/priority), index-aligned with the
//     existing `placement_zones` vector -- an authorable hint for DESIGN.md
//     §4.3/§4.7's "generous at bends, tight along straights" rule. Reading it
//     to actually validate placement is TowerSystem's job, not this file's.
// None of this changes rasterize_vessels()/TissueMask/FlowField (still frozen)
// and none of it changes instantiate()'s existing single-lane behavior.
//
// JSON SCHEMA (assets/levels/*.json) — version 1:
// {
//   "schema": 1,
//   "name": "capillary_chokepoint",
//   "region": "capillary",
//   "world": { "min": [0,0], "max": [256,144], "cell_size": 0.5 },
//   "vessels": [
//     { "id": "main",
//       "lane_id": "main",             // optional; defaults to "id"
//       "vessel_type": "artery",       // optional; defaults to "artery".
//                                      // one of: artery | vein | lymphatic |
//                                      // nerve_adjacent | mucosal_fold
//       "points": [ {"p":[8,72],"w":6.0}, {"p":[60,70],"w":5.0}, ... ],
//       "children": ["branch_a"] }
//   ],
//   "portals":   [ { "id":"p0", "pos":[8,72], "radius":3.0,
//                     "lane_id":"main" } ],   // lane_id optional; see
//                                             // resolve_portal_lane_id()
//   "objectives":[ { "id":"organ", "pos":[248,72], "radius":5.0, "integrity":100 } ],
//   "placement_zones": [
//     { "min":[20,50], "max":[200,100], "concentrated": false, "priority": 1.0 }
//   ],
//   "ambient_drift": [0.0, 0.0],
//   "waves": [                                 // optional; see below
//     { "name": "flood_1", "prep_time": 20.0, "atp_reward": 60,
//       "modifier": "none",                    // none|allergen|fever|swarm
//       "spawns": [
//         { "family": "virus", "count": 900, "start_time": 0.0,
//           "duration": 6.0, "portal_id": "p0", "elite_id": 0 }
//       ] }
//   ]
// }
// Unknown fields are ignored; a missing "schema" is an error, not a default.
//
// AUTHORED WAVES (optional)
// `waves` is absent from every level authored before it existed, and absence
// keeps the previous behavior exactly: the app asks WaveDirector::generate()
// for a region-shaped table. When present it *replaces* that table, because a
// generated table is a function of the region string alone and therefore
// cannot express a level whose whole point is its own pressure curve (a
// stress/floodplain level, a scripted set piece). Loading is the only thing
// this file does with it — scheduling and spawning stay WaveDirector's job,
// and the parsed vector is a plain std::vector<WaveDef>, the same type
// generate() returns, so callers pick one source or the other and nothing
// downstream can tell the difference.
// Per-field defaults match WaveDef/SpawnEntry's own defaults; `index` is not
// authorable (it is assigned from array position, so it cannot disagree with
// the order the director walks).
//
// WORKED MULTI-LANE EXAMPLE (see assets/levels/lane_schema_test.json for the
// full runnable fixture): three lanes -- an artery, a lymph channel, and a
// nerve-adjacent duct -- each with its own portal, converging on one shared
// objective:
// {
//   "schema": 1, "name": "lane_schema_test", "region": "organ_chamber",
//   "world": { "min": [0,0], "max": [160,140], "cell_size": 0.5 },
//   "vessels": [
//     { "id": "artery_main", "lane_id": "artery_main", "vessel_type": "artery",
//       "points": [ {"p":[8,20],"w":10}, {"p":[80,45],"w":8}, {"p":[140,72],"w":7} ] },
//     { "id": "lymph_main", "lane_id": "lymph_main", "vessel_type": "lymphatic",
//       "points": [ {"p":[8,72],"w":9}, {"p":[80,72],"w":8}, {"p":[140,72],"w":7} ] },
//     { "id": "nerve_main", "lane_id": "nerve_main", "vessel_type": "nerve_adjacent",
//       "points": [ {"p":[8,124],"w":8}, {"p":[80,99],"w":7}, {"p":[140,72],"w":7} ] }
//   ],
//   "portals": [
//     { "id": "p_artery", "pos": [8,20],  "radius": 5, "lane_id": "artery_main" },
//     { "id": "p_lymph",  "pos": [8,72],  "radius": 5, "lane_id": "lymph_main" },
//     { "id": "p_nerve",  "pos": [8,124], "radius": 5, "lane_id": "nerve_main" }
//   ],
//   "objectives": [ { "id": "organ", "pos": [140,72], "radius": 8, "integrity": 100 } ]
// }
// All three lanes' final control point coincides at (140,72) with the same
// width -- a deliberate physical overlap, so this file also doubles as the
// fixture for LaneOwnershipMap's first-claim-wins convergence behavior.
#pragma once

#include "core/Types.h"
#include "game/wave/WaveDirector.h"   // WaveDef, for optional authored waves.

#include <string>
#include <vector>

namespace immune::sim { class SimWorld; }

namespace immune::game {

struct VesselPoint {
    Vec2 position{0.0f, 0.0f};
    f32 width = 4.0f;   ///< Vessel diameter in world units at this point.
};

/// DESIGN.md §9.2's named vessel archetypes. Purely a data tag here -- the
/// per-type hue/tempo (fast arterial pulse, slow lymphatic drift, ...) is a
/// rendering wave's job, not this loader's. Unknown/omitted JSON strings fall
/// back to Artery (see vessel_type_from_string()).
enum class VesselType : u8 {
    Artery = 0,
    Vein = 1,
    Lymphatic = 2,
    NerveAdjacent = 3,
    MucosalFold = 4,
};

/// "artery" | "vein" | "lymphatic" | "nerve_adjacent" | "mucosal_fold".
const char* vessel_type_to_string(VesselType t);
/// Unrecognized or empty input returns VesselType::Artery.
VesselType vessel_type_from_string(const std::string& s);

struct Vessel {
    std::string id;
    /// Groups Vessel entries into one named lane (DESIGN.md §4.1). Defaults to
    /// `id` when the JSON omits it, so a single-vessel level is automatically
    /// a well-formed single-lane level. A lane's trunk-plus-branch can share a
    /// lane_id to stay one visual identity across a bifurcation.
    std::string lane_id;
    VesselType type = VesselType::Artery;
    std::vector<VesselPoint> points;   ///< Catmull-Rom control points.
    std::vector<std::string> children; ///< Ids of vessels branching off the end.
};

struct SpawnPortal {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    f32 radius = 3.0f;
    /// Which lane this portal feeds. Empty means "unspecified" -- callers
    /// should resolve it via LevelLoader::resolve_portal_lane_id() rather than
    /// reading this field directly, since it is only an author-supplied hint.
    std::string lane_id;
};

struct ObjectivePoint {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    f32 radius = 5.0f;
    f32 integrity = 100.0f;
};

/// Per-placement-zone authoring hint for DESIGN.md §4.3/§4.7: a "concentrated"
/// zone is meant to be generous buildable margin near a bend/intersection/
/// chokepoint; a non-concentrated zone is meant to be tight or absent along a
/// long straight. Index-aligned with LevelDef::placement_zones (tag[i]
/// describes placement_zones[i]) rather than folded into Rect, since Rect
/// (core/Types.h) is a shared frozen type used well beyond level geometry.
/// Reading this to actually validate/steer placement is TowerSystem's job.
struct PlacementZoneTag {
    bool concentrated = false;
    f32 priority = 1.0f;
};

struct LevelDef {
    i32 schema = 0;
    std::string name;
    std::string region;
    Rect world_bounds{};
    f32 cell_size = 0.5f;
    std::vector<Vessel> vessels;
    std::vector<SpawnPortal> portals;
    std::vector<ObjectivePoint> objectives;
    std::vector<Rect> placement_zones;
    /// Same size and order as placement_zones; see PlacementZoneTag.
    std::vector<PlacementZoneTag> placement_zone_tags;
    Vec2 ambient_drift{0.0f, 0.0f};
    /// Optional authored wave table (see the header comment). Empty means "no
    /// opinion" -- the caller falls back to WaveDirector::generate().
    std::vector<WaveDef> waves;
};

/// Coarse per-cell "which lane owns this point" grid, built by
/// LevelLoader::build_lane_ownership_map(). Parallel to (same grid geometry
/// as) the TissueMask instantiate() bakes, but this file never sees a live
/// TissueMask -- it recomputes the same width/height/cell_size/origin from
/// the LevelDef so it can be built standalone from `def` alone. A cell is
/// unowned (kNoLane) if it belongs to no lane at all, or if it was claimed by
/// more than one lane's lumen (see the .cpp for the first-claim-wins caveat).
struct LaneOwnershipMap {
    static constexpr u8 kNoLane = 0xFFu;

    i32 width = 0;
    i32 height = 0;
    f32 cell_size = 0.5f;
    Vec2 world_origin{0.0f, 0.0f};

    /// Stable lane order = first appearance across LevelDef::vessels.
    std::vector<std::string> lane_ids;
    std::vector<VesselType> lane_types;   ///< Parallel to lane_ids.
    /// width*height cells, row-major (y * width + x); value is an index into
    /// lane_ids/lane_types, or kNoLane.
    std::vector<u8> owner;

    bool in_range(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    usize index(i32 x, i32 y) const {
        return static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x);
    }
    IVec2 world_to_cell(Vec2 p) const;

    /// Index into lane_ids/lane_types owning the cell containing `p`, or -1 if
    /// the point is out of range or unowned.
    i32 lane_at(Vec2 p) const;
};

struct LevelLoadResult {
    bool ok = false;
    std::string error;      ///< Empty when ok.
    u32 warnings = 0;
};

class LevelLoader {
public:
    /// Parses a level JSON file. Does not touch the sim.
    LevelLoadResult load_file(const std::string& path, LevelDef& out) const;
    LevelLoadResult load_string(const std::string& json, LevelDef& out) const;

    /// Schema/semantic validation: portals and objectives must lie on tissue,
    /// every portal must reach at least one objective, widths must be positive.
    LevelLoadResult validate(const LevelDef& def) const;

    /// Rasterizes splines into the world's TissueMask, bakes the distance field
    /// and the flow field, and configures the sim's bounds and goal. This is the
    /// only path by which a SimWorld acquires geometry. Lanes share one
    /// TissueMask/FlowField (they are one walkable region overall); for
    /// per-lane attribution after the fact, see build_lane_ownership_map().
    LevelLoadResult instantiate(const LevelDef& def, sim::SimWorld& world) const;

    /// Builds the per-cell lane-ownership grid described above by rasterizing
    /// each lane's vessels into a private scratch mask and unioning the
    /// touched cells into one owner grid. Pure function of `def`; independent
    /// of instantiate() (call in either order, or not at all for a level a
    /// caller doesn't need per-lane attribution for). O(cells * lane_count);
    /// level-load-time cost only, never called from a sim tick.
    LaneOwnershipMap build_lane_ownership_map(const LevelDef& def) const;

    /// Effective lane a portal feeds: `portal.lane_id` if the author set one,
    /// otherwise the lane_id of whichever vessel's first control point is
    /// nearest the portal's position. Always returns a non-empty string for
    /// any level with at least one vessel.
    std::string resolve_portal_lane_id(const LevelDef& def, const SpawnPortal& portal) const;

    /// Built-in fallback level used when no level file is supplied. Keeps
    /// --bench and --screenshot runnable before any content exists.
    static LevelDef default_test_level();
};

} // namespace immune::game
