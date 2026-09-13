// game/editor/LevelValidate.cpp — see LevelValidate.h for what this exists for.
//
// Every rule below is one `add_error`/`add_warning` call, deliberately, so the
// list of rules reads as a list. Rules that need the bake are grouped at the
// end behind a single null check on `baked`.
#include "game/editor/LevelValidate.h"

#include "core/Math.h"
#include "sim/flowfield/FlowField.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace immune::game {
namespace {

/// The lane a vessel belongs to. Mirrors parse_vessel's rule (lane_id defaults
/// to id) rather than reading the raw field, which may legitimately be empty on
/// a LevelDef assembled in code.
std::string lane_of(const Vessel& v) { return v.lane_id.empty() ? v.id : v.lane_id; }

/// A representative point for an element, so the UI has somewhere to fly to.
bool vessel_anchor(const Vessel& v, Vec2& out) {
    if (v.points.empty()) return false;
    out = v.points[v.points.size() / 2].position;
    return true;
}

bool obstacle_anchor(const ObstacleDef& o, Vec2& out) {
    if (o.shape == ObstacleShape::Disc || o.shape == ObstacleShape::Box) {
        out = o.position;
        return true;
    }
    if (o.points.empty()) return false;
    Vec2 sum{0.0f, 0.0f};
    for (const VesselPoint& p : o.points) sum += p.position;
    out = sum / static_cast<f32>(o.points.size());
    return true;
}

class Report {
public:
    void error(std::string msg, ElementRef ref = {}) { push(Issue::Severity::Error, std::move(msg), ref); }
    void warn(std::string msg, ElementRef ref = {}) { push(Issue::Severity::Warning, std::move(msg), ref); }

    void error_at(std::string msg, ElementRef ref, Vec2 anchor) {
        push(Issue::Severity::Error, std::move(msg), ref, &anchor);
    }
    void warn_at(std::string msg, ElementRef ref, Vec2 anchor) {
        push(Issue::Severity::Warning, std::move(msg), ref, &anchor);
    }

    std::vector<Issue> take() {
        // Errors first, document order preserved within a severity -- so the
        // panel's top row is always the thing most worth fixing, and the list
        // does not reshuffle as you fix things.
        std::stable_sort(issues_.begin(), issues_.end(), [](const Issue& a, const Issue& b) {
            return static_cast<u8>(a.severity) < static_cast<u8>(b.severity);
        });
        return std::move(issues_);
    }

private:
    void push(Issue::Severity sev, std::string msg, ElementRef ref, const Vec2* anchor = nullptr) {
        Issue i;
        i.severity = sev;
        i.message = std::move(msg);
        i.ref = ref;
        if (anchor) {
            i.anchor = *anchor;
            i.has_anchor = true;
        }
        issues_.push_back(std::move(i));
    }
    std::vector<Issue> issues_;
};

/// Duplicate ids inside one collection. Currently unchecked anywhere: two spawn
/// points sharing an id makes WaveDirector::resolve_spawn_point() bind waves to
/// whichever one it finds first, so the horde comes out of a lane the author did
/// not pick and nothing says so.
template <typename Get>
void check_unique_ids(Report& rep, usize count, ElementKind kind, const char* what, Get get) {
    std::unordered_map<std::string, usize> seen;
    for (usize i = 0; i < count; ++i) {
        const std::string id = get(i);
        if (id.empty()) continue;   // optional ids (obstacles) are fine repeated-empty
        const auto it = seen.find(id);
        if (it == seen.end()) {
            seen.emplace(id, i);
            continue;
        }
        rep.error(std::string(what) + " id '" + id + "' is used twice (indices " +
                      std::to_string(it->second) + " and " + std::to_string(i) + ")",
                  ElementRef{kind, static_cast<i32>(i), -1});
    }
}

bool inside(const Rect& r, Vec2 p) {
    return p.x >= r.min.x && p.x <= r.max.x && p.y >= r.min.y && p.y <= r.max.y;
}

} // namespace

const char* severity_to_string(Issue::Severity s) {
    return s == Issue::Severity::Error ? "error" : "warning";
}

const char* element_kind_to_string(ElementKind k) {
    switch (k) {
    case ElementKind::World: return "world";
    case ElementKind::Vessel: return "vessel";
    case ElementKind::Obstacle: return "obstacle";
    case ElementKind::SpawnPoint: return "spawn_point";
    case ElementKind::Objective: return "objective";
    case ElementKind::Zone: return "placement_zone";
    case ElementKind::SquadPath: return "squad_path";
    case ElementKind::Wave: return "wave";
    case ElementKind::None:
    default: return "level";
    }
}

bool has_errors(const std::vector<Issue>& issues) {
    for (const Issue& i : issues) {
        if (i.severity == Issue::Severity::Error) return true;
    }
    return false;
}

std::vector<Issue> validate_level(const LevelDef& def, const BakedGeometry& baked) {
    Report rep;

    // ---- Structure ---------------------------------------------------------

    // Both versions the loader accepts; a schema-2 file is what LevelWriter
    // emits for any level using the v2 header (display_name, difficulty, ...).
    if (def.schema != 1 && def.schema != 2) {
        rep.error("unsupported or missing level schema " + std::to_string(def.schema) +
                      " (expected 1 or 2)",
                  ElementRef{ElementKind::World, -1, -1});
    }
    if (def.vessels.empty()) rep.error("level has no vessels");
    if (def.spawn_points.empty()) rep.error("level has no spawn points");
    if (def.objectives.empty()) rep.error("level has no objectives");
    if (def.waves.empty()) rep.error("level declares no waves");

    const Vec2 extent = def.world_bounds.size();
    // The rect the bake actually covers. Equal to world_bounds unless the level
    // put a spawn point outside the play area, in which case the grid -- and so
    // everything geometric below -- reaches out to meet it (level_sim_bounds()).
    const Rect sim_rect = level_sim_bounds(def);
    const Vec2 sim_extent = sim_rect.size();
    if (extent.x <= 0.0f || extent.y <= 0.0f) {
        rep.error("world bounds are empty or inverted", ElementRef{ElementKind::World, -1, -1});
    }
    if (def.cell_size <= 0.0f) {
        rep.error("cell_size must be positive", ElementRef{ElementKind::World, -1, -1});
    } else if (sim_extent.x > 0.0f && sim_extent.y > 0.0f) {
        // The bake is O(cells) several times over and the editor re-runs it on
        // every gesture, so grid size is an authoring decision with a felt cost.
        // Counted over the SIM rect: an off-map spawn point buys real cells.
        const f64 cells = static_cast<f64>(sim_extent.x / def.cell_size) *
                          static_cast<f64>(sim_extent.y / def.cell_size);
        if (cells > 500000.0) {
            rep.warn("world is " + std::to_string(static_cast<i64>(cells)) +
                         " cells at this cell_size; bakes and rebakes get slow past ~500k",
                     ElementRef{ElementKind::World, -1, -1});
        }
    }

    // ---- Unique ids --------------------------------------------------------

    check_unique_ids(rep, def.vessels.size(), ElementKind::Vessel, "vessel",
                     [&](usize i) { return def.vessels[i].id; });
    check_unique_ids(rep, def.obstacles.size(), ElementKind::Obstacle, "obstacle",
                     [&](usize i) { return def.obstacles[i].id; });
    check_unique_ids(rep, def.spawn_points.size(), ElementKind::SpawnPoint, "spawn point",
                     [&](usize i) { return def.spawn_points[i].id; });
    check_unique_ids(rep, def.objectives.size(), ElementKind::Objective, "objective",
                     [&](usize i) { return def.objectives[i].id; });
    check_unique_ids(rep, def.squad_paths.size(), ElementKind::SquadPath, "squad path",
                     [&](usize i) { return def.squad_paths[i].id; });

    // ---- Vessels -----------------------------------------------------------

    std::unordered_set<std::string> vessel_ids;
    std::unordered_set<std::string> lane_ids;
    for (const Vessel& v : def.vessels) {
        vessel_ids.insert(v.id);
        lane_ids.insert(lane_of(v));
    }

    for (usize i = 0; i < def.vessels.size(); ++i) {
        const Vessel& v = def.vessels[i];
        const ElementRef ref{ElementKind::Vessel, static_cast<i32>(i), -1};
        Vec2 anchor{};
        const bool has_anchor = vessel_anchor(v, anchor);

        if (v.id.empty()) rep.error("vessel " + std::to_string(i) + " has no id", ref);
        if (v.points.size() < 2) {
            rep.error("vessel '" + v.id + "' needs at least 2 control points", ref);
        }
        for (usize k = 0; k < v.points.size(); ++k) {
            const ElementRef pref{ElementKind::Vessel, static_cast<i32>(i), static_cast<i32>(k)};
            const VesselPoint& p = v.points[k];
            if (p.width <= 0.0f) {
                rep.error_at("vessel '" + v.id + "' point " + std::to_string(k) +
                                 " has non-positive width",
                             pref, p.position);
            } else if (def.cell_size > 0.0f && p.width < def.cell_size * 2.0f) {
                // Below ~2 cells the rasterizer produces a broken, speckled
                // lumen rather than a narrow one, and the flow field routes
                // through the gaps. Narrow-on-purpose is fine; narrower than
                // the grid is not a width, it is an artefact.
                rep.warn_at("vessel '" + v.id + "' point " + std::to_string(k) + " is width " +
                                std::to_string(p.width) + ", under 2 cells at cell_size " +
                                std::to_string(def.cell_size) + " -- the lumen will rasterize broken",
                            pref, p.position);
            }
            if (extent.x > 0.0f && !inside(sim_rect, p.position)) {
                // Against the SIM rect: running a lane off the edge to feed an
                // off-map spawn point is the supported way to do that, and the
                // grid grew to cover it. Past the grid there is no tissue to
                // rasterize into, but the rasterizer clips each stamped disc
                // to the grid, so the lane simply stops at the edge. Almost
                // always a mistyped coordinate, never a broken level.
                rep.warn_at("vessel '" + v.id + "' point " + std::to_string(k) +
                                " lies outside the simulated area",
                            pref, p.position);
            }
        }
        // `children` is parsed and currently drives nothing, but a dangling id
        // is still an authoring mistake, and the editor's branch-weld drag is
        // about to give the field a job.
        for (const std::string& c : v.children) {
            if (vessel_ids.count(c)) continue;
            if (has_anchor) {
                rep.error_at("vessel '" + v.id + "' names unknown child vessel '" + c + "'", ref,
                             anchor);
            } else {
                rep.error("vessel '" + v.id + "' names unknown child vessel '" + c + "'", ref);
            }
        }
    }

    // ---- Obstacles ---------------------------------------------------------

    for (usize i = 0; i < def.obstacles.size(); ++i) {
        const ObstacleDef& o = def.obstacles[i];
        const ElementRef ref{ElementKind::Obstacle, static_cast<i32>(i), -1};
        const std::string label = o.id.empty() ? ("obstacle " + std::to_string(i))
                                               : ("obstacle '" + o.id + "'");
        Vec2 anchor{};
        const bool has_anchor = obstacle_anchor(o, anchor);
        if (!has_anchor) continue;

        // Shape-specific geometry is already enforced by parse_obstacle and is
        // not re-derived here; what the parser cannot see is the world rect.
        bool out_of_bounds = false;
        if (o.shape == ObstacleShape::Disc || o.shape == ObstacleShape::Box) {
            out_of_bounds = !inside(sim_rect, o.position);
        }
        for (const VesselPoint& p : o.points) {
            out_of_bounds = out_of_bounds || !inside(sim_rect, p.position);
        }
        if (extent.x > 0.0f && out_of_bounds) {
            rep.error_at(label + " extends outside the simulated area", ref, anchor);
        }
    }

    // ---- Spawn points and objectives --------------------------------------

    for (usize i = 0; i < def.spawn_points.size(); ++i) {
        const SpawnPoint& p = def.spawn_points[i];
        const ElementRef ref{ElementKind::SpawnPoint, static_cast<i32>(i), -1};
        if (p.id.empty()) rep.error_at("spawn point " + std::to_string(i) + " has no id", ref, p.position);
        if (p.radius <= 0.0f) {
            rep.error_at("spawn point '" + p.id + "' has non-positive radius", ref, p.position);
        }
        if (extent.x > 0.0f && !inside(sim_rect, p.position)) {
            // level_sim_bounds() caps how far the grid will chase a spawn point
            // (one world extent per side). Past that it stopped growing, so the
            // point is on no grid at all and its burst dies on the tick it
            // spawns -- which is the one out-of-bounds case still an error.
            rep.error_at("spawn point '" + p.id +
                             "' is too far outside the world bounds to simulate -- the grid "
                             "grows at most one world extent past each edge",
                         ref, p.position);
        } else if (extent.x > 0.0f && !inside(def.world_bounds, p.position)) {
            // Deliberate off-map spawning: the horde walks in from off-screen.
            // Worth saying out loud (it is invisible if the camera is framed on
            // the play rect) but it is not a mistake.
            rep.warn_at("spawn point '" + p.id +
                            "' lies outside the world bounds -- the horde will walk in from "
                            "off-screen, and needs tissue running out to it",
                        ref, p.position);
        }
        if (!p.lane_id.empty() && !lane_ids.count(p.lane_id)) {
            rep.error_at("spawn point '" + p.id + "' names unknown lane '" + p.lane_id + "'", ref,
                         p.position);
        }
    }

    for (usize i = 0; i < def.objectives.size(); ++i) {
        const ObjectivePoint& o = def.objectives[i];
        const ElementRef ref{ElementKind::Objective, static_cast<i32>(i), -1};
        if (o.id.empty()) rep.error_at("objective " + std::to_string(i) + " has no id", ref, o.position);
        if (o.half_extents.x <= 0.0f || o.half_extents.y <= 0.0f) {
            rep.error_at("objective '" + o.id + "' has non-positive radius", ref, o.position);
        }
        if (o.integrity <= 0.0f) {
            rep.error_at("objective '" + o.id + "' has non-positive integrity", ref, o.position);
        }
        if (extent.x > 0.0f && !inside(def.world_bounds, o.position)) {
            rep.error_at("objective '" + o.id + "' lies outside the world bounds", ref, o.position);
        }
    }

    // ---- Placement zones ---------------------------------------------------

    for (usize i = 0; i < def.placement_zones.size(); ++i) {
        const Rect& r = def.placement_zones[i];
        const ElementRef ref{ElementKind::Zone, static_cast<i32>(i), -1};
        const Vec2 center = (r.min + r.max) * 0.5f;
        if (r.max.x <= r.min.x || r.max.y <= r.min.y) {
            rep.error_at("placement zone " + std::to_string(i) + " is empty or inverted", ref, center);
            continue;
        }
        if (extent.x > 0.0f && (!inside(def.world_bounds, r.min) || !inside(def.world_bounds, r.max))) {
            rep.warn_at("placement zone " + std::to_string(i) + " extends outside the world bounds",
                        ref, center);
        }
    }

    // ---- Squad paths -------------------------------------------------------

    std::unordered_map<std::string, u32> authored_paths_per_lane;
    for (usize i = 0; i < def.squad_paths.size(); ++i) {
        const SquadPathDef& sp = def.squad_paths[i];
        const ElementRef ref{ElementKind::SquadPath, static_cast<i32>(i), -1};
        const Vec2 anchor = sp.points.empty() ? Vec2{0.0f, 0.0f} : sp.points[sp.points.size() / 2];

        if (sp.points.size() < 2) {
            rep.error("squad path '" + sp.id + "' needs at least 2 points", ref);
        }
        if (sp.half_width <= 0.0f) {
            rep.error_at("squad path '" + sp.id + "' has non-positive half_width", ref, anchor);
        }
        if (!sp.lane_id.empty()) {
            if (lane_ids.count(sp.lane_id)) {
                authored_paths_per_lane[sp.lane_id] += 1;
            } else {
                // build_squad_paths() would silently re-home this onto the
                // nearest lane, which is right for an OMITTED lane_id and wrong
                // for a typo'd one.
                rep.error_at("squad path '" + sp.id + "' names unknown lane '" + sp.lane_id + "'",
                             ref, anchor);
            }
        }
    }
    // Authoring even ONE path on a lane disables derivation for that whole lane
    // (build_squad_paths). A 3-path lane hand-tuned down to 1 changes how every
    // wave on it reads, and nothing in the file says so.
    for (const auto& [lane, count] : authored_paths_per_lane) {
        if (count > 1) continue;
        rep.warn("lane '" + lane +
                 "' authors a single squad path, which disables the derived spread for the whole "
                 "lane -- bake the derived paths to authored ones first if that was not intended");
    }

    // ---- Lanes without a spawn point --------------------------------------

    {
        std::unordered_set<std::string> fed;
        LevelLoader loader;
        for (const SpawnPoint& p : def.spawn_points) {
            fed.insert(loader.resolve_spawn_point_lane_id(def, p));
        }
        for (const std::string& lane : lane_ids) {
            if (fed.count(lane)) continue;
            rep.warn("lane '" + lane + "' has no spawn point feeding it");
        }
    }

    // ---- Waves -------------------------------------------------------------

    std::unordered_set<std::string> spawn_point_ids;
    for (const SpawnPoint& p : def.spawn_points) spawn_point_ids.insert(p.id);

    f32 prev_prep = -1.0f;
    u32 prev_atp = 0;
    for (usize i = 0; i < def.waves.size(); ++i) {
        const WaveDef& w = def.waves[i];
        const ElementRef ref{ElementKind::Wave, static_cast<i32>(i), -1};
        if (w.spawns.empty()) rep.error("wave '" + w.name + "' has no spawn entries", ref);

        u32 total = 0;
        for (usize k = 0; k < w.spawns.size(); ++k) {
            const SpawnEntry& e = w.spawns[k];
            const ElementRef eref{ElementKind::Wave, static_cast<i32>(i), static_cast<i32>(k)};
            total += e.count;
            if (e.spawn_point_id.empty() || spawn_point_ids.count(e.spawn_point_id)) continue;
            // WaveDirector falls back to spawn_points[0] at runtime, so the
            // wave would silently come out of the wrong lane.
            rep.error("wave '" + w.name + "' spawns from unknown spawn point '" + e.spawn_point_id +
                          "'",
                      eref);
        }
        if (total == 0) rep.warn("wave '" + w.name + "' spawns nothing", ref);

        // A ramp that goes backwards is usually a copy-paste, not a design.
        if (i > 0 && w.prep_time > prev_prep + 0.001f) {
            rep.warn("wave '" + w.name + "' gives more prep time than the wave before it", ref);
        }
        if (i > 0 && w.atp_reward < prev_atp) {
            rep.warn("wave '" + w.name + "' rewards less ATP than the wave before it", ref);
        }
        prev_prep = w.prep_time;
        prev_atp = w.atp_reward;
    }

    // ---- Everything that needs the bake ------------------------------------

    if (baked.mask && baked.flow) {
        const sim::TissueMask& mask = *baked.mask;
        const sim::FlowField& flow = *baked.flow;

        auto on_tissue = [&mask](Vec2 p) {
            const IVec2 c = mask.world_to_cell(p);
            return mask.walkable(c.x, c.y);
        };

        for (usize i = 0; i < def.spawn_points.size(); ++i) {
            const SpawnPoint& p = def.spawn_points[i];
            const ElementRef ref{ElementKind::SpawnPoint, static_cast<i32>(i), -1};
            if (!on_tissue(p.position)) {
                // The single most common authoring error, and invisible in the
                // text: the number looks plausible, the point is in the wall.
                rep.error_at("spawn point '" + p.id + "' is not on tissue", ref, p.position);
                continue;   // reachability from off-tissue says nothing useful
            }
            if (!flow.reachable(p.position)) {
                rep.error_at("spawn point '" + p.id +
                                 "' cannot reach any objective -- its lane is sealed",
                             ref, p.position);
            }
        }

        for (usize i = 0; i < def.objectives.size(); ++i) {
            const ObjectivePoint& o = def.objectives[i];
            const ElementRef ref{ElementKind::Objective, static_cast<i32>(i), -1};
            if (!on_tissue(o.position)) {
                rep.error_at("objective '" + o.id + "' is not on tissue", ref, o.position);
            }
        }

        // A zone over solid ground is buildable area that does not exist. Not an
        // error -- a generous rect that overhangs a lane is normal authoring --
        // but a zone with NO tissue under it at all is always a mistake.
        for (usize i = 0; i < def.placement_zones.size(); ++i) {
            const Rect& r = def.placement_zones[i];
            if (r.max.x <= r.min.x || r.max.y <= r.min.y) continue;
            const f32 step = math::max(def.cell_size, 0.25f);
            bool any = false;
            for (f32 y = r.min.y; y <= r.max.y && !any; y += step) {
                for (f32 x = r.min.x; x <= r.max.x && !any; x += step) {
                    any = on_tissue(Vec2{x, y});
                }
            }
            if (any) continue;
            rep.warn_at("placement zone " + std::to_string(i) + " covers no tissue",
                        ElementRef{ElementKind::Zone, static_cast<i32>(i), -1},
                        (r.min + r.max) * 0.5f);
        }
    }

    return rep.take();
}

} // namespace immune::game
