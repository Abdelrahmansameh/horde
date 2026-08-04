// game/level/Level.h — spline-authored level format. FROZEN CONTRACT.
// Owner: Wave 2D (loader), Wave 4A (content).
//
// RATIONALE (plan decision 3)
// Levels are authored as JSON *splines with per-point width*, not as painted
// bitmaps. Agents can write and diff splines in text; nobody can author a mask
// PNG without a visual editor, and this project ships no binary assets anyway.
// At load, splines rasterize into a TissueMask -> DistanceField -> FlowField.
// That gives organic, branching, variable-width vessels (DESIGN.md §4) from a
// text file. tools/preview_level.py renders one to PNG for eyeballing.
//
// JSON SCHEMA (assets/levels/*.json) — version 1:
// {
//   "schema": 1,
//   "name": "capillary_chokepoint",
//   "region": "capillary",
//   "world": { "min": [0,0], "max": [256,144], "cell_size": 0.5 },
//   "vessels": [
//     { "id": "main",
//       "points": [ {"p":[8,72],"w":6.0}, {"p":[60,70],"w":5.0}, ... ],
//       "children": ["branch_a"] }
//   ],
//   "portals":   [ { "id":"p0", "pos":[8,72], "radius":3.0 } ],
//   "objectives":[ { "id":"organ", "pos":[248,72], "radius":5.0, "integrity":100 } ],
//   "placement_zones": [ { "min":[20,50], "max":[200,100] } ],
//   "ambient_drift": [0.0, 0.0]
// }
// Unknown fields are ignored; a missing "schema" is an error, not a default.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::sim { class SimWorld; }

namespace immune::game {

struct VesselPoint {
    Vec2 position{0.0f, 0.0f};
    f32 width = 4.0f;   ///< Vessel diameter in world units at this point.
};

struct Vessel {
    std::string id;
    std::vector<VesselPoint> points;   ///< Catmull-Rom control points.
    std::vector<std::string> children; ///< Ids of vessels branching off the end.
};

struct SpawnPortal {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    f32 radius = 3.0f;
};

struct ObjectivePoint {
    std::string id;
    Vec2 position{0.0f, 0.0f};
    f32 radius = 5.0f;
    f32 integrity = 100.0f;
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
    Vec2 ambient_drift{0.0f, 0.0f};
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
    /// only path by which a SimWorld acquires geometry.
    LevelLoadResult instantiate(const LevelDef& def, sim::SimWorld& world) const;

    /// Built-in fallback level used when no level file is supplied. Keeps
    /// --bench and --screenshot runnable before any content exists.
    static LevelDef default_test_level();
};

} // namespace immune::game
