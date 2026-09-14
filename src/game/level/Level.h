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
//   - `SpawnPoint::lane_id`, resolved via LevelLoader::resolve_spawn_point_lane_id()
//     so a multi-lane level's waves know which spawn point feeds which lane.
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
//   "name": "capillary_switchback",
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
//   "obstacles": [                             // OPTIONAL; see ObstacleDef
//     { "id":"plaque", "shape":"disc",    "pos":[60,72], "radius":4.0 },
//     { "shape":"capsule", "points":[[80,60],[100,66]], "radius":3.0 },
//     { "shape":"box",     "pos":[120,72], "half_extents":[8,2],
//                          "rotation":30 },      // DEGREES, CCW
//     { "shape":"polygon", "points":[[150,60],[164,66],[152,78]],
//                          "inflate":0.0 },      // optional outward grow
//     { "shape":"ridge",   "points":[ {"p":[180,60],"w":5},
//                                     {"p":[200,72],"w":3} ] }
//   ],
//   "spawn_points":   [ { "id":"p0", "pos":[8,72], "radius":3.0,
//                     "lane_id":"main" } ],   // lane_id optional; see
//                                             // resolve_spawn_point_lane_id()
//   "objectives":[ { "id":"organ", "pos":[248,72],   // oriented rectangle:
//                     "half_extents":[8,5], "rotation":30,   // degrees
//                     "integrity":100 } ],                   // legacy: "radius":5
//                                                            // = a square 5x5
//   "placement_zones": [
//     { "min":[20,50], "max":[200,100], "concentrated": false, "priority": 1.0 }
//   ],
//   "squad_paths": [                           // OPTIONAL; derived when absent
//     { "id": "high_road",
//       "lane_id": "main",                     // optional; nearest lane if omitted
//       "points": [ [8,72], [60,60], [140,58], [248,72] ],
//       "half_width": 6.0 }                    // optional, default 6.0
//   ],
//   "ambient_drift": [0.0, 0.0],
//   "waves": [                                 // REQUIRED, non-empty; see below
//     { "name": "flood_1", "prep_time": 20.0, "atp_reward": 60,
//       "modifier": "none",                    // none|fever|swarm
//       "spawns": [
//         { "family": "virus", "count": 900, "start_time": 0.0,
//           "duration": 6.0, "spawn_point_id": "p0", "elite_id": 0 }
//       ] }
//   ]
// }
// Unknown fields are ignored; a missing "schema" is an error, not a default.
//
// AUTHORED WAVES (required)
// A level's wave table is authored here and nowhere else. There used to be a
// second source -- a procedural generator keyed on the `region` string, tuned
// globally in assets/config/waves.json -- which every level that omitted
// `waves` fell back to. It is gone, along with waves.json and
// WaveDirector::generate(): a table derived from the region string alone
// cannot express two levels in the same region that are meant to play
// differently, which is most of them. Every shipped level now carries its own
// explicit table, and `region` is once again purely a presentation/grouping
// tag.
// So `waves` is mandatory and must be non-empty; a level without one fails to
// load rather than silently running an empty table. (The one exception is
// LevelLoader::default_test_level(), the built-in fallback for headless runs
// with no level file, which builds its table in code.)
// Loading is still the only thing this file does with it — scheduling and
// spawning stay WaveDirector's job, and the parsed vector is a plain
// std::vector<WaveDef> handed straight to WaveDirector::set_waves().
// Per-field defaults match WaveDef/SpawnEntry's own defaults; `index` is not
// authorable (it is assigned from array position, so it cannot disagree with
// the order the director walks).
//
// IN-LANE OBSTACLES (optional `obstacles`)
// A lane used to be exactly the union of its splines' lumens, so the only
// shaping tool an author had was the centerline and the per-point width: every
// interesting piece of cover had to be built by threading two vessels around
// each other. `obstacles` adds the missing subtractive half -- five primitives
// (disc, capsule, oriented box, polygon, width-varying ridge) carved back out
// of the mask after rasterization and before the SDF bake.
// The deliberate design point is that an obstacle is NOT a new kind of thing:
// downstream, a carved cell is a cell no vessel ever covered. It shades as
// vessel wall, collides as vessel wall, reroutes the flow field, and is
// unbuildable, with no code in the renderer, the steering, or TowerSystem that
// knows the concept exists. sim/flowfield/ObstacleRaster.h carries the full
// argument; instantiate() owns the ordering.
// They are fully static: nothing at runtime spawns, moves or destroys one.
// validate() rejects an obstacle that swallows a spawn point or an objective,
// and instantiate() fails the load if carving leaves a spawn point unable to
// reach any objective -- an author can wall a lane off, but not silently.
//
// WORKED MULTI-LANE EXAMPLE (see assets/levels/lane_schema_test.json for the
// full runnable fixture): three lanes -- an artery, a lymph channel, and a
// nerve-adjacent duct -- each with its own spawn point, converging on one
// shared objective:
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
//   "spawn_points": [
//     { "id": "p_artery", "pos": [8,20],  "radius": 5, "lane_id": "artery_main" },
//     { "id": "p_lymph",  "pos": [8,72],  "radius": 5, "lane_id": "lymph_main" },
//     { "id": "p_nerve",  "pos": [8,124], "radius": 5, "lane_id": "nerve_main" }
//   ],
//   "objectives": [ { "id": "organ", "pos": [140,72], "half_extents": [8,8],
//                     "integrity": 100 } ]
// }
// All three lanes' final control point coincides at (140,72) with the same
// width -- a deliberate physical overlap, so this file also doubles as the
// fixture for LaneOwnershipMap's first-claim-wins convergence behavior.
#pragma once

#include "core/Types.h"
#include "game/level/RenderSdf.h"     // RenderSdf, the drawn field bake_geometry() also writes.
#include "game/wave/WaveDirector.h"   // WaveDef, for optional authored waves.
#include "sim/squad/Squads.h"         // SquadPath, produced by build_squad_paths().

#include <cmath>
#include <string>
#include <vector>

namespace immune::sim { class SimWorld; class TissueMask; class DistanceField; class FlowField; }

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

/// Which primitive an ObstacleDef draws. Parsed from the JSON `shape` string;
/// unlike VesselType an unrecognized string is a load ERROR rather than a
/// silent fallback, because a typo'd shape would carve a plausible-looking but
/// wrong obstacle and the level would ship with it.
enum class ObstacleShape : u8 {
    Disc = 0,      ///< `pos` + `radius`.
    Capsule = 1,   ///< `points` (2) + `radius`: a stadium/thick segment.
    Box = 2,       ///< `pos` + `half_extents` + `rotation` (authored in degrees).
    Polygon = 3,   ///< `points` (>= 3), convex or concave, + optional `inflate`.
    Ridge = 4,     ///< `points` with per-point `w`: a Catmull-Rom solid septum.
};

/// "disc" | "capsule" | "box" | "polygon" | "ridge".
const char* obstacle_shape_to_string(ObstacleShape s);
/// Returns false (leaving `out` untouched) for an unrecognized string.
bool obstacle_shape_from_string(const std::string& s, ObstacleShape& out);

/// A solid island of tissue standing INSIDE a lane (DESIGN.md §4: lanes are
/// meant to be shaped, not just wide). Rasterized by carving the TissueMask
/// after the vessel splines have filled it and before the SDF is baked
/// (sim/flowfield/ObstacleRaster.h has the full rationale), so an obstacle is
/// the same material as the lane's outer wall in every system that reads the
/// mask or the SDF: it shades identically, blocks movement identically, reroutes
/// the flow field, and refuses towers -- none of which is special-cased for it.
///
/// Fully static. Nothing at runtime creates, moves, or destroys one; a tower's
/// footprint is the only thing that edits the mask after load, and it
/// snapshots/restores exactly its own cells.
///
/// Geometry lives in three shared fields rather than a variant, because the
/// loader has to validate whichever ones the shape actually uses anyway and a
/// flat struct keeps LevelDef trivially copyable-ish and diff-friendly. Which
/// fields a shape reads is documented on ObstacleShape.
struct ObstacleDef {
    std::string id;                  ///< Optional; for diagnostics only.
    ObstacleShape shape = ObstacleShape::Disc;
    Vec2 position{0.0f, 0.0f};       ///< Disc/Box center.
    /// Capsule endpoints, Polygon vertices in order, or Ridge control points.
    std::vector<VesselPoint> points;
    f32 radius = 0.0f;               ///< Disc/Capsule radius; Polygon outward inflate.
    Vec2 half_extents{0.0f, 0.0f};   ///< Box only.
    f32 rotation = 0.0f;             ///< Box only, RADIANS (the JSON authors degrees).
};

/// One authored route across a lane, for the squad layer (sim/squad/Squads.h).
///
/// OPTIONAL BY DESIGN. A level that authors none still gets squads: at load,
/// LevelLoader::build_squad_paths() derives a few offset copies of each lane's
/// vessel centerline. That is what keeps the feature zero-authoring across the
/// existing content while leaving an author free to override any lane whose
/// automatic spread does not read the way they want.
///
/// `points` is a plain polyline in world space rather than a spline, because
/// unlike a vessel these are never rasterized -- nothing depends on their
/// curvature, only on where they run -- and a polyline is the form the arclength
/// table wants anyway.
struct SquadPathDef {
    std::string id;
    /// Which lane this path serves. Empty resolves to the lane of whichever
    /// vessel's centerline runs nearest the path's first point.
    std::string lane_id;
    std::vector<Vec2> points;
    /// Lateral tolerance: caps how wide a squad on this path spreads, and
    /// scales the per-squad offset that keeps squads sharing a path apart.
    f32 half_width = 6.0f;
};

struct SpawnPoint {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    f32 radius = 3.0f;
    /// Which lane this spawn point feeds. Empty means "unspecified" -- callers
    /// should resolve it via LevelLoader::resolve_spawn_point_lane_id() rather
    /// than reading this field directly, since it is only an author-supplied
    /// hint.
    std::string lane_id;
};

/// The organ the horde is trying to reach. Its footprint is an oriented
/// RECTANGLE, not a disc: an organ reads as a built structure sitting in the
/// tissue rather than as another round blob among the round agents, a flat
/// face gives the horde something to pile against instead of a curve that
/// slides them around it, and a rectangle can be sized and turned to sit along
/// whatever vessel it terminates.
///
/// Geometry matches ObstacleDef::Box exactly -- centre, half-extents, rotation
/// in radians authored as degrees -- so every rotated-rectangle test in the
/// codebase (picking, rasterizing, despawning) reads the same three fields the
/// same way.
struct ObjectivePoint {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    /// Half-size along the objective's OWN axes, before rotation. The footprint
    /// is `position` +/- these in the rotated frame.
    Vec2 half_extents{5.0f, 5.0f};
    /// RADIANS, CCW (the JSON authors degrees, like ObstacleDef::rotation).
    f32 rotation = 0.0f;
    f32 integrity = 100.0f;

    /// Point-in-footprint test, in world space. The one place the rectangle's
    /// meaning is defined; everything that needs it (loader, editor picking,
    /// validation) calls this rather than re-deriving the transform.
    bool contains(Vec2 p) const {
        const f32 c = std::cos(-rotation);
        const f32 s = std::sin(-rotation);
        const Vec2 d = p - position;
        const Vec2 local{d.x * c - d.y * s, d.x * s + d.y * c};
        return std::fabs(local.x) <= half_extents.x && std::fabs(local.y) <= half_extents.y;
    }
};

/// Per-placement-zone authoring hint for DESIGN.md §4.3/§4.7: a "concentrated"
/// zone is meant to be generous buildable margin near a bend/switchback/
/// convergence; a non-concentrated zone is meant to be tight or absent along a
/// long straight. Index-aligned with LevelDef::placement_zones (tag[i]
/// describes placement_zones[i]) rather than folded into Rect, since Rect
/// (core/Types.h) is a shared frozen type used well beyond level geometry.
/// Reading this to actually validate/steer placement is TowerSystem's job.
struct PlacementZoneTag {
    bool concentrated = false;
    f32 priority = 1.0f;
};

/// Per-level economy overrides (schema 2). Global in assets/config/economy.json
/// otherwise: a tutorial and a floodplain cannot want the same opening bankroll,
/// and "more starting ATP" is a difficulty lever that is not "more enemies".
/// A value of 0 / 1.0 means "use the global".
struct LevelEconomy {
    u32 starting_atp = 0;         ///< 0 = economy.json's value
    f32 income_multiplier = 1.0f;
};

/// Per-level camera framing (schema 2). Without it, framing is "the whole
/// level", which is wrong for a long capillary: the lane ends up a ribbon three
/// pixels tall. 0 means "derive it from the world bounds", i.e. today's rule.
struct LevelCamera {
    Vec2 center{0.0f, 0.0f};
    bool has_center = false;
    f32 view_height = 0.0f;       ///< 0 = whole level
    f32 min_view_height = 0.0f;   ///< 0 = no limit
    f32 max_view_height = 0.0f;
};

/// Alternative win condition (schema 2). 0 keeps today's behaviour: clear the
/// authored wave table.
struct LevelWin {
    f32 survive_seconds = 0.0f;
};

/// Editor-only annotations (schema 2). The loader parses and preserves these
/// and NOTHING else reads them.
///
/// It exists because the parser drops unknown keys and the writer cannot emit
/// what the LevelDef never held -- so before this block there was nowhere for a
/// note or a grid size to live that survived a Save.
struct LevelEditorState {
    f32 grid_size = 0.0f;         ///< 0 = the editor's default
    Vec2 camera_center{0.0f, 0.0f};
    f32 camera_view_height = 0.0f;
    std::string notes;
    bool present = false;         ///< False when the file authored no block
};

struct LevelDef {
    i32 schema = 0;
    std::string name;
    std::string region;
    // ---- schema 2 metadata -------------------------------------------------
    // Level select shows a filename-derived name without these.
    std::string display_name;
    std::string description;
    std::string author;
    i32 difficulty = 0;                 ///< 0 = unrated
    std::vector<std::string> tags;
    /// Empty = every tower is buildable. The biggest missing design lever: a
    /// level that is ABOUT the Goblet Cell. Names match parse_tower_type().
    std::vector<std::string> allowed_towers;
    LevelEconomy economy;
    LevelCamera camera;
    LevelWin win;
    LevelEditorState editor;
    Rect world_bounds{};
    f32 cell_size = 0.5f;
    std::vector<Vessel> vessels;
    /// Solid islands carved back out of the lumen; see ObstacleDef. Optional.
    std::vector<ObstacleDef> obstacles;
    std::vector<SpawnPoint> spawn_points;
    std::vector<ObjectivePoint> objectives;
    std::vector<Rect> placement_zones;
    /// Optional authored squad routes; empty means "derive them" (see
    /// SquadPathDef and build_squad_paths()).
    std::vector<SquadPathDef> squad_paths;
    /// Same size and order as placement_zones; see PlacementZoneTag.
    std::vector<PlacementZoneTag> placement_zone_tags;
    Vec2 ambient_drift{0.0f, 0.0f};
    /// The level's wave table (see the header comment). Never empty: the
    /// loader rejects a level that authors none, and there is no generator to
    /// fall back to.
    std::vector<WaveDef> waves;
};

/// The rect the SIMULATION covers, which is NOT always the rect the level is
/// framed by. `world_bounds` is the play area: what the camera clamps to, what
/// the editor draws as the level's edge, what a tower may be built inside. A
/// Spawn points, vessel ends, and objectives are allowed to sit OUTSIDE it for
/// off-frame composition. The tissue mask, flow field, spatial hash, and
/// out-of-bounds despawn test therefore have to reach that far or the geometry
/// would be clipped and spawned agents could be retired on their first tick.
///
/// So: world_bounds, grown to contain each off-frame spawn disc, vessel end,
/// and objective footprint plus a margin, rounded out to whole cells. Equal to
/// world_bounds for the (overwhelmingly common) all-in-frame level, which is
/// why nothing downstream needs a special case for ordinary content.
Rect level_sim_bounds(const LevelDef& def);

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

/// The SimDesc fields the geometry bake actually reads, lifted out so
/// bake_geometry() can run without a SimWorld. Defaults are the "no wall cost,
/// no smoothing" bake, which is what a caller that only wants the walkable
/// shape (the level editor's overlay, --level-check) wants; App fills them in
/// from config so instantiate() behaves exactly as it always has.
struct GeometryBakeDesc {
    f32 flow_smoothing_radius = 0.0f;
    f32 flow_wall_cost = 0.0f;
    f32 flow_wall_falloff = 0.0f;
    f32 flow_wall_exponent = 1.0f;
};

/// Per-stage wall-clock cost of one bake_geometry() call, in milliseconds.
///
/// Not profiling garnish: the editor re-bakes on every gesture, so WHICH stage
/// dominates decides what it can afford to do live and what has to go to a
/// worker. Measured on the shipped levels, the flow solve is the overwhelming
/// majority and is strongly topology-dependent rather than cell-count-dependent
/// -- a switchback costs an order of magnitude more than a straight lane of the
/// same grid size, because the cost-to-goal sweep has to propagate around every
/// hairpin. Wall-clock, so never read from sim logic.
struct GeometryBakeStats {
    f64 rasterize_ms = 0.0;
    f64 carve_ms = 0.0;
    /// The smooth field bake plus the walkability it writes back (see
    /// bake_geometry()).
    f64 render_sdf_ms = 0.0;
    f64 sdf_ms = 0.0;
    f64 wall_cost_ms = 0.0;
    f64 flow_ms = 0.0;
    f64 total_ms = 0.0;
};

class LevelLoader {
public:
    /// Parses a level JSON file. Does not touch the sim.
    LevelLoadResult load_file(const std::string& path, LevelDef& out) const;
    LevelLoadResult load_string(const std::string& json, LevelDef& out) const;

    /// Schema/semantic validation: spawn points and objectives must lie on
    /// tissue, every spawn point must reach at least one objective, widths
    /// must be positive.
    LevelLoadResult validate(const LevelDef& def) const;

    /// Rasterizes splines into the world's TissueMask, bakes the distance field
    /// and the flow field, and configures the sim's bounds and goal. This is the
    /// only path by which a SimWorld acquires geometry. Lanes share one
    /// TissueMask/FlowField (they are one walkable region overall); for
    /// per-lane attribution after the fact, see build_lane_ownership_map().
    /// `out_render_sdf`, when given, receives the smooth field the tissue pass
    /// draws the level from (see bake_geometry()).
    LevelLoadResult instantiate(const LevelDef& def, sim::SimWorld& world,
                                RenderSdf* out_render_sdf = nullptr) const;

    /// The geometry half of instantiate(), against caller-owned buffers:
    /// rasterize vessels -> carve obstacles -> bake SDF -> stamp wall-proximity
    /// cost -> bake flow. instantiate() IS this call plus the SimWorld wiring
    /// (ECS objectives, spawn points, squad paths, chaff goal/bounds).
    ///
    /// Split out so a caller that wants only the walkable shape does not have
    /// to construct and destroy a whole SimWorld to get it -- SimWorld::init()
    /// is destructive, and re-running it per edit is exactly what an editor
    /// must not do. Keeping it as the ONE rasterizer is the point: a second
    /// preview-only bake would be free to drift from what ships.
    ///
    /// Does not resize `mask` beyond what level_sim_bounds(def)/cell_size imply, and
    /// does not consult any objective for reachability -- that check needs the
    /// baked flow field and belongs to the caller (instantiate() and
    /// validate_level() both run it).
    /// `stats` is optional; pass one to find out where the time went.
    ///
    /// THE MASK IS THE PICTURE. After the splines are stamped and the
    /// obstacles carved, the smooth field the renderer draws from
    /// (game/level/RenderSdf.h: analytic, concave corners filleted, lanes
    /// run off the world edge) is baked, and every cell it says is lumen is
    /// made walkable. That is what keeps the horde pressed against exactly
    /// the wall the player sees: the fillet at the inside of a bend is a
    /// place agents can go, not a painted margin they stop short of. The
    /// field is a superset of the stamped geometry by construction (it is
    /// the same splines and solids, evaluated exactly rather than at cell
    /// centres), so nothing an author drew is taken away; the only cells that
    /// close are sub-texel slivers the stamp caught by accident.
    /// `out_render_sdf` receives that field; pass one from any caller that
    /// will draw.
    LevelLoadResult bake_geometry(const LevelDef& def, const GeometryBakeDesc& desc,
                                  sim::TissueMask& mask, sim::DistanceField& sdf,
                                  sim::FlowField& flow,
                                  GeometryBakeStats* stats = nullptr,
                                  RenderSdf* out_render_sdf = nullptr) const;

    /// Resolves the level's squad routes (game/level SquadPathDef ->
    /// sim::SquadPath): resamples every authored path, DERIVES a spread of
    /// paths for any lane that authored none, snaps points that fall off the
    /// tissue back toward the lane centerline, and builds each arclength table.
    ///
    /// Takes the baked mask because the snap step needs to know what is
    /// walkable; call it after rasterize_vessels(). Pure function of its inputs
    /// and load-time only -- never called from a tick.
    ///
    /// `auto_paths_per_lane` is sim::SquadTuning::auto_paths_per_lane's job in
    /// spirit, but is passed explicitly so this stays independent of any live
    /// SimWorld and can be unit-tested on a LevelDef alone.
    std::vector<sim::SquadPath> build_squad_paths(const LevelDef& def,
                                                  const sim::TissueMask& mask,
                                                  u32 auto_paths_per_lane = 3) const;

    /// Builds the per-cell lane-ownership grid described above by rasterizing
    /// each lane's vessels into a private scratch mask and unioning the
    /// touched cells into one owner grid. Pure function of `def`; independent
    /// of instantiate() (call in either order, or not at all for a level a
    /// caller doesn't need per-lane attribution for). O(cells * lane_count);
    /// level-load-time cost only, never called from a sim tick.
    LaneOwnershipMap build_lane_ownership_map(const LevelDef& def) const;

    /// Effective lane a spawn point feeds: `spawn_point.lane_id` if the author
    /// set one, otherwise the lane_id of whichever vessel's first control
    /// point is nearest the spawn point's position. Always returns a
    /// non-empty string for any level with at least one vessel.
    std::string resolve_spawn_point_lane_id(const LevelDef& def, const SpawnPoint& spawn_point) const;

    /// Built-in fallback level used when no level file is supplied. Keeps
    /// --bench and --screenshot runnable before any content exists.
    static LevelDef default_test_level();
};

} // namespace immune::game
