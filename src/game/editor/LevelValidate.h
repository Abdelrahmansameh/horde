// game/editor/LevelValidate.h — what "correct" means for a level. NEW MODULE.
//
// RATIONALE
// LevelLoader::validate() answers one bool and one message, which is all a
// loader needs and nowhere near enough for an editor: an editor has to draw a
// halo on the offending thing, fly the camera to it, and keep going after the
// first problem so the author sees all of them at once. So this file returns a
// LIST of issues, each carrying what to select and where to look.
//
// It is a strict SUPERSET of validate(), not a replacement. The parser keeps
// its own guards (`waves` non-empty, check_obstacles) because App::load_level
// and Modes::build_world go straight from load_file() to instantiate() and
// never call validate() at all -- a rule that only lives here would not protect
// a level the game actually loads.
//
// THE CHECKS THAT NEED THE BAKE
// Three of the most valuable rules cannot be answered from the JSON:
//   - is this spawn point actually on tissue?
//   - is this objective?
//   - can this spawn point reach any objective?
// The last one is the pinched-switchback catcher, and today it exists only
// inside instantiate(), gated on `!def.obstacles.empty()` so it cannot reject
// pre-existing content. Here it runs whenever a bake is supplied, which is what
// makes an editor able to say "this lane is sealed" while you are still
// dragging the thing that sealed it.
//
// HEADLESS BY CONSTRUCTION
// No ImGui, no GL, no window: this is the same executor/front-end split
// game/gym uses, and it is what lets --level-check run the editor's own
// validator in CI.
#pragma once

#include "core/Types.h"
#include "game/level/Level.h"

#include <string>
#include <vector>

namespace immune::sim { class TissueMask; class DistanceField; class FlowField; }

namespace immune::game {

/// Which element an Issue is about. Also the editor's selection currency --
/// LevelDoc's ops address elements the same way, so clicking a validation row
/// and clicking the thing in the viewport produce the same selection.
enum class ElementKind : u8 {
    None = 0,
    World,          ///< world_bounds / cell_size; index unused
    Vessel,         ///< index = vessel; sub = control point, or -1 for the whole vessel
    Obstacle,       ///< index = obstacle; sub = point, or -1
    SpawnPoint,
    Objective,
    Zone,           ///< index into placement_zones (and placement_zone_tags)
    SquadPath,      ///< index = path; sub = point, or -1
    Wave,           ///< index = wave; sub = spawn entry, or -1
};

struct ElementRef {
    ElementKind kind = ElementKind::None;
    i32 index = -1;
    i32 sub = -1;   ///< Point/entry within `index`, or -1 for the whole element.

    bool valid() const { return kind != ElementKind::None; }
    bool operator==(const ElementRef& o) const {
        return kind == o.kind && index == o.index && sub == o.sub;
    }
};

struct Issue {
    /// Error blocks Save (with an explicit override) and fails --level-check.
    /// Warning is always advisory: a level can ship with warnings, and several
    /// of them describe choices an author may have made deliberately.
    enum class Severity : u8 { Error = 0, Warning = 1 };

    Severity severity = Severity::Error;
    std::string message;
    ElementRef ref;             ///< What to select. May be invalid for whole-level issues.
    Vec2 anchor{0.0f, 0.0f};    ///< Where to point the camera. Zero when there is no place.
    bool has_anchor = false;
};

/// The geometry the bake-dependent checks need. All three pointers may be null
/// (then those checks are skipped and the JSON-only rules still run), but they
/// must all come from ONE bake of the level being validated -- mixing a mask
/// from one level with a flow field from another produces confident nonsense.
struct BakedGeometry {
    const sim::TissueMask* mask = nullptr;
    const sim::DistanceField* sdf = nullptr;
    const sim::FlowField* flow = nullptr;
};

/// Every problem with `def`, most severe first, then in document order.
///
/// Pure: reads `def` and `baked`, touches no files and no sim. Never throws.
/// An empty result means the level is clean; use has_errors() to decide whether
/// it can be saved or shipped.
std::vector<Issue> validate_level(const LevelDef& def, const BakedGeometry& baked = {});

/// True if any issue is an Error. The Save gate and --level-check's exit code.
bool has_errors(const std::vector<Issue>& issues);

/// "error" / "warning" -- for the JSON report and the UI.
const char* severity_to_string(Issue::Severity s);
/// "vessel", "obstacle", ... -- stable strings for the --level-check report.
const char* element_kind_to_string(ElementKind k);

} // namespace immune::game
