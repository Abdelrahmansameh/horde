// game/level/LevelWriter.cpp — the serializer half of the level format.
// See LevelWriter.h for the four canonicalisation rules and the identity
// exception; this file is their implementation.
//
// WHY THE TEXT IS BUILT BY HAND
// nlohmann can order keys (ordered_json) but it cannot express "this array is
// one element per line, that one is inline" -- dump(2) explodes every array,
// which turns a 95-line level into 328. Since layout-by-edit-unit is the whole
// point (rule 4), the emitter below writes text directly. Strings still go
// through nlohmann for escaping, which is the one part worth not hand-rolling.
#include "game/level/LevelWriter.h"

#include "core/Math.h"
#include "platform/FileIO.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace immune::game {
namespace {

/// Three decimals, trailing zeros and a bare '.' trimmed: 287.378, 0.5, 76.
/// Negative zero is normalised to "0" -- a rotation dragged just past 0 must
/// not produce a "-0" that shows up in every subsequent diff.
std::string num(f32 v) {
    if (!std::isfinite(v)) return "0";
    f32 r = std::round(v * 1000.0f) / 1000.0f;
    if (r == 0.0f) r = 0.0f;   // collapses -0.0f
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<f64>(r));
    std::string s(buf);
    const usize dot = s.find('.');
    if (dot != std::string::npos) {
        usize last = s.find_last_not_of('0');
        if (last == dot) --last;
        s.erase(last + 1);
    }
    if (s == "-0") s = "0";
    return s;
}

std::string num(u32 v) { return std::to_string(v); }
std::string num(i32 v) { return std::to_string(v); }

std::string vec2(Vec2 v) { return "[" + num(v.x) + ", " + num(v.y) + "]"; }

/// Escaping only. nlohmann is the authority on what a JSON string literal is,
/// and re-deriving that by hand is exactly the sort of thing that is fine until
/// a level name contains a quote.
std::string str(const std::string& s) { return nlohmann::json(s).dump(); }

/// Accumulates `"key": value` pairs and joins them, so every caller can just
/// ask for a field and let the object decide about separators. `add` is the
/// omit-defaults gate: nothing reaches the output unless the caller passed a
/// value that differs from its parser-side default.
class Fields {
public:
    void add(const char* key, std::string value) {
        parts_.push_back(std::string("\"") + key + "\": " + std::move(value));
    }
    void add_if(bool condition, const char* key, std::string value) {
        if (condition) add(key, std::move(value));
    }
    bool empty() const { return parts_.empty(); }

    /// Single line: `{ "a": 1, "b": 2 }`.
    std::string inline_object() const {
        if (parts_.empty()) return "{}";
        std::string out = "{ ";
        for (usize i = 0; i < parts_.size(); ++i) {
            if (i) out += ", ";
            out += parts_[i];
        }
        out += " }";
        return out;
    }

    /// One field per line at `indent`, for objects that carry a nested array.
    std::string block_object(const std::string& indent) const {
        if (parts_.empty()) return "{}";
        std::string out = "{\n";
        for (usize i = 0; i < parts_.size(); ++i) {
            out += indent + "  " + parts_[i];
            if (i + 1 < parts_.size()) out += ",";
            out += "\n";
        }
        out += indent + "}";
        return out;
    }

private:
    std::vector<std::string> parts_;
};

/// A bracketed list with one item per line and the closing bracket at `indent`.
/// Used for every collection whose elements are the things an author drags one
/// of -- so a diff shows one changed line per changed thing.
std::string array_body(const std::vector<std::string>& items, const std::string& indent) {
    if (items.empty()) return "[]";
    std::string out = "[\n";
    for (usize i = 0; i < items.size(); ++i) {
        out += indent + "  " + items[i];
        if (i + 1 < items.size()) out += ",";
        out += "\n";
    }
    out += indent + "]";
    return out;
}

std::string array_field(const char* key, const std::vector<std::string>& items,
                        const std::string& indent) {
    return std::string("\"") + key + "\": " + array_body(items, indent);
}

/// Comma-separated on one line: `[a, b, c]`.
std::string inline_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (usize i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += items[i];
    }
    return out + "]";
}

// ---------------------------------------------------------------------------
// Per-element emitters. Each mirrors exactly one parse_* in Level.cpp, and the
// defaults below are that function's defaults -- if one moves, both move.
// ---------------------------------------------------------------------------

std::string vessel_point(const VesselPoint& p) {
    Fields f;
    f.add("p", vec2(p.position));
    f.add("w", num(p.width));   // identity: a point with no width says nothing
    return f.inline_object();
}

std::string vessel(const Vessel& v, const std::string& indent) {
    Fields f;
    f.add("id", str(v.id));
    // lane_id defaults to id (parse_vessel), so a single-lane level stays free
    // of a field it never needed.
    f.add_if(!v.lane_id.empty() && v.lane_id != v.id, "lane_id", str(v.lane_id));
    f.add_if(v.type != VesselType::Artery, "vessel_type", str(vessel_type_to_string(v.type)));

    std::vector<std::string> pts;
    pts.reserve(v.points.size());
    for (const VesselPoint& p : v.points) pts.push_back(vessel_point(p));
    f.add("points", array_body(pts, indent + "  "));

    if (!v.children.empty()) {
        std::vector<std::string> ids;
        ids.reserve(v.children.size());
        for (const std::string& c : v.children) ids.push_back(str(c));
        f.add("children", inline_array(ids));
    }
    return f.block_object(indent);
}

std::string obstacle(const ObstacleDef& o, const std::string& indent) {
    Fields f;
    f.add_if(!o.id.empty(), "id", str(o.id));
    f.add("shape", str(obstacle_shape_to_string(o.shape)));

    // `pos` is only read for the two shapes that have a centre. Writing it for
    // a polygon would be harmless to the parser and misleading to a reader.
    if (o.shape == ObstacleShape::Disc || o.shape == ObstacleShape::Box) {
        f.add("pos", vec2(o.position));
    }

    if (!o.points.empty()) {
        // Bare [x,y] pairs unless some point actually carries a width -- which
        // is the ridge case, and is keyed off the data rather than the shape so
        // a width authored on any other shape survives the round trip.
        bool any_width = false;
        for (const VesselPoint& p : o.points) any_width = any_width || p.width != 0.0f;

        std::vector<std::string> pts;
        pts.reserve(o.points.size());
        for (const VesselPoint& p : o.points) {
            pts.push_back(any_width ? vessel_point(p) : vec2(p.position));
        }
        f.add("points", any_width ? array_body(pts, indent + "  ") : inline_array(pts));
    }

    switch (o.shape) {
    case ObstacleShape::Disc:
    case ObstacleShape::Capsule:
        f.add("radius", num(o.radius));   // required positive; never a default
        break;
    case ObstacleShape::Box:
        f.add("half_extents", vec2(o.half_extents));
        // Radians in the struct, DEGREES in the file. This conversion is the
        // single easiest thing in the whole format to get backwards.
        f.add_if(o.rotation != 0.0f, "rotation", num(o.rotation * (180.0f / math::kPi)));
        break;
    case ObstacleShape::Polygon:
        // Same struct field as `radius`, spelled the way a polygon means it.
        f.add_if(o.radius != 0.0f, "inflate", num(o.radius));
        break;
    case ObstacleShape::Ridge:
        break;   // all geometry lives in the per-point widths above
    }

    // Ridges (and any other width-carrying shape) put their points on their own
    // lines, so the object has to be a block; everything else fits on one.
    bool multiline = false;
    for (const VesselPoint& p : o.points) multiline = multiline || p.width != 0.0f;
    return multiline ? f.block_object(indent) : f.inline_object();
}

std::string spawn_point(const SpawnPoint& p) {
    Fields f;
    f.add("id", str(p.id));
    f.add("pos", vec2(p.position));
    f.add_if(p.radius != 3.0f, "radius", num(p.radius));
    f.add_if(!p.lane_id.empty(), "lane_id", str(p.lane_id));
    return f.inline_object();
}

std::string objective(const ObjectivePoint& o) {
    Fields f;
    f.add("id", str(o.id));
    f.add("pos", vec2(o.position));
    // Always written, never as the legacy "radius": a rectangle cannot round-
    // trip through one number, and the loader still reads old files either way.
    f.add("half_extents", vec2(o.half_extents));
    f.add_if(o.rotation != 0.0f, "rotation", num(o.rotation * (180.0f / math::kPi)));
    f.add_if(o.integrity != 100.0f, "integrity", num(o.integrity));
    return f.inline_object();
}

/// The two index-aligned struct vectors (placement_zones + placement_zone_tags)
/// are ONE object in the file, which is the shape the parser reads.
std::string placement_zone(const Rect& r, const PlacementZoneTag& tag) {
    Fields f;
    f.add("min", vec2(r.min));
    f.add("max", vec2(r.max));
    f.add_if(tag.concentrated, "concentrated", "true");
    f.add_if(tag.priority != 1.0f, "priority", num(tag.priority));
    return f.inline_object();
}

std::string squad_path(const SquadPathDef& sp) {
    Fields f;
    f.add("id", str(sp.id));
    f.add_if(!sp.lane_id.empty(), "lane_id", str(sp.lane_id));
    std::vector<std::string> pts;
    pts.reserve(sp.points.size());
    for (Vec2 p : sp.points) pts.push_back(vec2(p));
    f.add("points", inline_array(pts));
    f.add_if(sp.half_width != 6.0f, "half_width", num(sp.half_width));
    return f.inline_object();
}

const char* family_name(PathogenFamily f) {
    // Matches parse_family's table; a family the parser cannot read back would
    // make the writer produce a file that no longer loads.
    switch (f) {
    case PathogenFamily::Virus: return "virus";
    case PathogenFamily::Bacteria: return "bacteria";
    default: return "virus";
    }
}

const char* modifier_name(WaveModifier m) {
    switch (m) {
    case WaveModifier::Fever: return "fever";
    case WaveModifier::Swarm: return "swarm";
    case WaveModifier::None:
    default: return "none";
    }
}

std::string spawn_entry(const SpawnEntry& e) {
    Fields f;
    f.add("family", str(family_name(e.family)));   // identity
    f.add("count", num(e.count));                  // identity
    f.add_if(e.elite_id != 0, "elite_id", num(static_cast<u32>(e.elite_id)));
    f.add_if(e.start_time != 0.0f, "start_time", num(e.start_time));
    f.add_if(e.duration != 1.0f, "duration", num(e.duration));
    f.add_if(!e.spawn_point_id.empty(), "spawn_point_id", str(e.spawn_point_id));
    // schema 2, both defaulting to the pre-v2 behaviour.
    f.add_if(e.squad_size != 0, "squad_size", num(e.squad_size));
    if (!e.squad_paths.empty()) {
        std::vector<std::string> ids;
        ids.reserve(e.squad_paths.size());
        for (const std::string& p : e.squad_paths) ids.push_back(str(p));
        f.add("squad_paths", inline_array(ids));
    }
    return f.inline_object();
}

std::string wave(const WaveDef& w, const std::string& indent) {
    Fields f;
    // `index` is deliberately absent: array position IS the index the director
    // walks, so writing it creates a second source that can disagree.
    f.add_if(!w.name.empty(), "name", str(w.name));
    f.add_if(w.prep_time != 20.0f, "prep_time", num(w.prep_time));
    f.add_if(w.atp_reward != 0, "atp_reward", num(w.atp_reward));
    f.add_if(w.modifier != WaveModifier::None, "modifier", str(modifier_name(w.modifier)));

    std::vector<std::string> spawns;
    spawns.reserve(w.spawns.size());
    for (const SpawnEntry& e : w.spawns) spawns.push_back(spawn_entry(e));
    f.add("spawns", array_body(spawns, indent + "  "));
    return f.block_object(indent);
}

/// Does this document use anything schema 1 cannot express?
///
/// Governs the version stamp. Emitting 2 unconditionally would rewrite the
/// "schema" line of every shipped level for a feature none of them use, and
/// any reader pinned to v1 would then reject content that is still v1 in every
/// way that matters.
bool needs_schema_2(const LevelDef& d) {
    if (!d.display_name.empty() || !d.description.empty() || !d.author.empty()) return true;
    if (d.difficulty != 0 || !d.tags.empty() || !d.allowed_towers.empty()) return true;
    if (d.economy.starting_atp != 0 || d.economy.income_multiplier != 1.0f) return true;
    if (d.camera.has_center || d.camera.view_height != 0.0f ||
        d.camera.min_view_height != 0.0f || d.camera.max_view_height != 0.0f) {
        return true;
    }
    if (d.win.survive_seconds != 0.0f) return true;
    if (d.editor.present) return true;
    for (const WaveDef& w : d.waves) {
        for (const SpawnEntry& e : w.spawns) {
            if (e.squad_size != 0 || !e.squad_paths.empty()) return true;
        }
    }
    return false;
}

bool near_eq(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }
bool near_eq(Vec2 a, Vec2 b, f32 eps) { return near_eq(a.x, b.x, eps) && near_eq(a.y, b.y, eps); }

} // namespace

std::string level_to_json(const LevelDef& def) {
    const std::string ind = "  ";
    std::vector<std::string> top;

    // Always emits the version the document ACTUALLY needs, rather than
    // stamping every file v2: a level using no v2 field stays a v1 file, so
    // adopting the schema does not rewrite content that did not change.
    top.push_back("\"schema\": " + num(needs_schema_2(def) ? 2 : 1));   // identity
    if (!def.name.empty()) top.push_back("\"name\": " + str(def.name));
    if (!def.display_name.empty()) top.push_back("\"display_name\": " + str(def.display_name));
    if (!def.description.empty()) top.push_back("\"description\": " + str(def.description));
    if (!def.author.empty()) top.push_back("\"author\": " + str(def.author));
    if (def.difficulty != 0) top.push_back("\"difficulty\": " + num(def.difficulty));
    if (!def.tags.empty()) {
        std::vector<std::string> t;
        t.reserve(def.tags.size());
        for (const std::string& x : def.tags) t.push_back(str(x));
        top.push_back("\"tags\": " + inline_array(t));
    }
    if (!def.region.empty()) top.push_back("\"region\": " + str(def.region));

    {
        Fields w;
        w.add("min", vec2(def.world_bounds.min));
        w.add("max", vec2(def.world_bounds.max));
        w.add_if(def.cell_size != 0.5f, "cell_size", num(def.cell_size));
        top.push_back("\"world\": " + w.inline_object());
    }
    if (def.camera.has_center || def.camera.view_height != 0.0f ||
        def.camera.min_view_height != 0.0f || def.camera.max_view_height != 0.0f) {
        Fields c;
        if (def.camera.has_center) c.add("center", vec2(def.camera.center));
        c.add_if(def.camera.view_height != 0.0f, "view_height", num(def.camera.view_height));
        c.add_if(def.camera.min_view_height != 0.0f, "min_view_height",
                 num(def.camera.min_view_height));
        c.add_if(def.camera.max_view_height != 0.0f, "max_view_height",
                 num(def.camera.max_view_height));
        top.push_back("\"camera\": " + c.inline_object());
    }
    if (def.economy.starting_atp != 0 || def.economy.income_multiplier != 1.0f) {
        Fields e;
        e.add_if(def.economy.starting_atp != 0, "starting_atp", num(def.economy.starting_atp));
        e.add_if(def.economy.income_multiplier != 1.0f, "income_multiplier",
                 num(def.economy.income_multiplier));
        top.push_back("\"economy\": " + e.inline_object());
    }
    if (!def.allowed_towers.empty()) {
        std::vector<std::string> t;
        t.reserve(def.allowed_towers.size());
        for (const std::string& x : def.allowed_towers) t.push_back(str(x));
        top.push_back("\"allowed_towers\": " + inline_array(t));
    }
    if (def.win.survive_seconds != 0.0f) {
        Fields w;
        w.add("survive_seconds", num(def.win.survive_seconds));
        top.push_back("\"win\": " + w.inline_object());
    }

    if (!def.vessels.empty()) {
        std::vector<std::string> items;
        items.reserve(def.vessels.size());
        for (const Vessel& v : def.vessels) items.push_back(vessel(v, ind + "  "));
        top.push_back(array_field("vessels", items, ind));
    }
    if (!def.obstacles.empty()) {
        std::vector<std::string> items;
        items.reserve(def.obstacles.size());
        for (const ObstacleDef& o : def.obstacles) items.push_back(obstacle(o, ind + "  "));
        top.push_back(array_field("obstacles", items, ind));
    }
    if (!def.spawn_points.empty()) {
        std::vector<std::string> items;
        items.reserve(def.spawn_points.size());
        for (const SpawnPoint& p : def.spawn_points) items.push_back(spawn_point(p));
        top.push_back(array_field("spawn_points", items, ind));
    }
    if (!def.objectives.empty()) {
        std::vector<std::string> items;
        items.reserve(def.objectives.size());
        for (const ObjectivePoint& o : def.objectives) items.push_back(objective(o));
        top.push_back(array_field("objectives", items, ind));
    }
    if (!def.placement_zones.empty()) {
        std::vector<std::string> items;
        items.reserve(def.placement_zones.size());
        for (usize i = 0; i < def.placement_zones.size(); ++i) {
            const PlacementZoneTag tag =
                i < def.placement_zone_tags.size() ? def.placement_zone_tags[i] : PlacementZoneTag{};
            items.push_back(placement_zone(def.placement_zones[i], tag));
        }
        top.push_back(array_field("placement_zones", items, ind));
    }
    if (!def.squad_paths.empty()) {
        std::vector<std::string> items;
        items.reserve(def.squad_paths.size());
        for (const SquadPathDef& sp : def.squad_paths) items.push_back(squad_path(sp));
        top.push_back(array_field("squad_paths", items, ind));
    }
    if (def.ambient_drift != Vec2{0.0f, 0.0f}) {
        top.push_back("\"ambient_drift\": " + vec2(def.ambient_drift));
    }
    {
        // Always written, even empty: the parser treats a missing `waves` as a
        // hard error, so omitting it would produce a file that cannot load.
        std::vector<std::string> items;
        items.reserve(def.waves.size());
        for (const WaveDef& w : def.waves) items.push_back(wave(w, ind + "  "));
        top.push_back(array_field("waves", items, ind));
    }

    if (def.editor.present) {
        Fields e;
        e.add_if(def.editor.grid_size != 0.0f, "grid_size", num(def.editor.grid_size));
        e.add_if(def.editor.camera_center != Vec2{0.0f, 0.0f}, "camera_center",
                 vec2(def.editor.camera_center));
        e.add_if(def.editor.camera_view_height != 0.0f, "camera_view_height",
                 num(def.editor.camera_view_height));
        e.add_if(!def.editor.notes.empty(), "notes", str(def.editor.notes));
        // Written even when every field is default: `present` is what records
        // that the author opened this level in the editor, and dropping the
        // block would lose that on the next save.
        top.push_back("\"editor\": " + e.inline_object());
    }

    std::string out = "{\n";
    for (usize i = 0; i < top.size(); ++i) {
        out += ind + top[i];
        if (i + 1 < top.size()) out += ",";
        out += "\n";
    }
    out += "}\n";
    return out;
}

bool level_save_file(const LevelDef& def, const std::string& path, std::string& err) {
    const std::string text = level_to_json(def);
    if (!platform::write_text_file(path, text)) {
        err = "cannot write level file '" + path + "'";
        return false;
    }
    err.clear();
    return true;
}

bool level_equal(const LevelDef& a, const LevelDef& b, f32 eps) {
    // NOT `schema`: the writer stamps whichever version the content needs, so a
    // v1 document that gains a v2 field and one loaded straight from a v2 file
    // are the same level. Comparing the stamp would make the dirty flag fire on
    // a version bump that changed no content.
    if (a.name != b.name || a.region != b.region) return false;
    if (a.display_name != b.display_name || a.description != b.description) return false;
    if (a.author != b.author || a.difficulty != b.difficulty) return false;
    if (a.tags != b.tags || a.allowed_towers != b.allowed_towers) return false;
    if (a.economy.starting_atp != b.economy.starting_atp) return false;
    if (!near_eq(a.economy.income_multiplier, b.economy.income_multiplier, eps)) return false;
    if (a.camera.has_center != b.camera.has_center) return false;
    if (a.camera.has_center && !near_eq(a.camera.center, b.camera.center, eps)) return false;
    if (!near_eq(a.camera.view_height, b.camera.view_height, eps)) return false;
    if (!near_eq(a.camera.min_view_height, b.camera.min_view_height, eps)) return false;
    if (!near_eq(a.camera.max_view_height, b.camera.max_view_height, eps)) return false;
    if (!near_eq(a.win.survive_seconds, b.win.survive_seconds, eps)) return false;
    if (a.editor.present != b.editor.present) return false;
    if (a.editor.present) {
        if (a.editor.notes != b.editor.notes) return false;
        if (!near_eq(a.editor.grid_size, b.editor.grid_size, eps)) return false;
        if (!near_eq(a.editor.camera_center, b.editor.camera_center, eps)) return false;
        if (!near_eq(a.editor.camera_view_height, b.editor.camera_view_height, eps)) return false;
    }
    if (!near_eq(a.world_bounds.min, b.world_bounds.min, eps)) return false;
    if (!near_eq(a.world_bounds.max, b.world_bounds.max, eps)) return false;
    if (!near_eq(a.cell_size, b.cell_size, eps)) return false;
    if (!near_eq(a.ambient_drift, b.ambient_drift, eps)) return false;

    if (a.vessels.size() != b.vessels.size()) return false;
    for (usize i = 0; i < a.vessels.size(); ++i) {
        const Vessel& x = a.vessels[i];
        const Vessel& y = b.vessels[i];
        if (x.id != y.id || x.lane_id != y.lane_id || x.type != y.type) return false;
        if (x.children != y.children) return false;
        if (x.points.size() != y.points.size()) return false;
        for (usize k = 0; k < x.points.size(); ++k) {
            if (!near_eq(x.points[k].position, y.points[k].position, eps)) return false;
            if (!near_eq(x.points[k].width, y.points[k].width, eps)) return false;
        }
    }

    if (a.obstacles.size() != b.obstacles.size()) return false;
    for (usize i = 0; i < a.obstacles.size(); ++i) {
        const ObstacleDef& x = a.obstacles[i];
        const ObstacleDef& y = b.obstacles[i];
        if (x.id != y.id || x.shape != y.shape) return false;
        if (!near_eq(x.position, y.position, eps)) return false;
        if (!near_eq(x.radius, y.radius, eps)) return false;
        if (!near_eq(x.half_extents, y.half_extents, eps)) return false;
        // Rotation is compared in DEGREES: the file's unit is what the 3-decimal
        // tolerance was chosen for, and 1e-3 radians is a far tighter test than
        // the format can actually hold.
        if (!near_eq(x.rotation * (180.0f / math::kPi), y.rotation * (180.0f / math::kPi), eps)) {
            return false;
        }
        if (x.points.size() != y.points.size()) return false;
        for (usize k = 0; k < x.points.size(); ++k) {
            if (!near_eq(x.points[k].position, y.points[k].position, eps)) return false;
            if (!near_eq(x.points[k].width, y.points[k].width, eps)) return false;
        }
    }

    if (a.spawn_points.size() != b.spawn_points.size()) return false;
    for (usize i = 0; i < a.spawn_points.size(); ++i) {
        const SpawnPoint& x = a.spawn_points[i];
        const SpawnPoint& y = b.spawn_points[i];
        if (x.id != y.id || x.lane_id != y.lane_id) return false;
        if (!near_eq(x.position, y.position, eps) || !near_eq(x.radius, y.radius, eps)) return false;
    }

    if (a.objectives.size() != b.objectives.size()) return false;
    for (usize i = 0; i < a.objectives.size(); ++i) {
        const ObjectivePoint& x = a.objectives[i];
        const ObjectivePoint& y = b.objectives[i];
        if (x.id != y.id) return false;
        if (!near_eq(x.position, y.position, eps)) return false;
        if (!near_eq(x.half_extents, y.half_extents, eps)) return false;
        // Compared in degrees, the unit the file carries: two rotations that
        // serialize to the same text must compare equal, or the dirty flag
        // never clears.
        if (!near_eq(x.rotation * (180.0f / math::kPi), y.rotation * (180.0f / math::kPi), eps)) {
            return false;
        }
        if (!near_eq(x.integrity, y.integrity, eps)) return false;
    }

    if (a.placement_zones.size() != b.placement_zones.size()) return false;
    for (usize i = 0; i < a.placement_zones.size(); ++i) {
        if (!near_eq(a.placement_zones[i].min, b.placement_zones[i].min, eps)) return false;
        if (!near_eq(a.placement_zones[i].max, b.placement_zones[i].max, eps)) return false;
    }
    // Tags are index-aligned with zones but the vector may be shorter on a
    // LevelDef built in code; a missing tag means the default one.
    for (usize i = 0; i < a.placement_zones.size(); ++i) {
        const PlacementZoneTag x =
            i < a.placement_zone_tags.size() ? a.placement_zone_tags[i] : PlacementZoneTag{};
        const PlacementZoneTag y =
            i < b.placement_zone_tags.size() ? b.placement_zone_tags[i] : PlacementZoneTag{};
        if (x.concentrated != y.concentrated) return false;
        if (!near_eq(x.priority, y.priority, eps)) return false;
    }

    if (a.squad_paths.size() != b.squad_paths.size()) return false;
    for (usize i = 0; i < a.squad_paths.size(); ++i) {
        const SquadPathDef& x = a.squad_paths[i];
        const SquadPathDef& y = b.squad_paths[i];
        if (x.id != y.id || x.lane_id != y.lane_id) return false;
        if (!near_eq(x.half_width, y.half_width, eps)) return false;
        if (x.points.size() != y.points.size()) return false;
        for (usize k = 0; k < x.points.size(); ++k) {
            if (!near_eq(x.points[k], y.points[k], eps)) return false;
        }
    }

    if (a.waves.size() != b.waves.size()) return false;
    for (usize i = 0; i < a.waves.size(); ++i) {
        const WaveDef& x = a.waves[i];
        const WaveDef& y = b.waves[i];
        if (x.index != y.index || x.name != y.name || x.modifier != y.modifier) return false;
        if (x.atp_reward != y.atp_reward) return false;
        if (!near_eq(x.prep_time, y.prep_time, eps)) return false;
        if (x.spawns.size() != y.spawns.size()) return false;
        for (usize k = 0; k < x.spawns.size(); ++k) {
            const SpawnEntry& p = x.spawns[k];
            const SpawnEntry& q = y.spawns[k];
            if (p.family != q.family || p.elite_id != q.elite_id || p.count != q.count) return false;
            if (p.spawn_point_id != q.spawn_point_id) return false;
            if (p.squad_size != q.squad_size || p.squad_paths != q.squad_paths) return false;
            if (!near_eq(p.start_time, q.start_time, eps)) return false;
            if (!near_eq(p.duration, q.duration, eps)) return false;
        }
    }
    return true;
}

} // namespace immune::game
