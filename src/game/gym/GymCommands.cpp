// game/gym/GymCommands.cpp — parser and executor for the gym command language.
// See GymCommands.h for why this layer exists and what it deliberately is not.
#include "game/gym/GymCommands.h"

#include "game/editor/LevelDoc.h"
#include "game/editor/LevelTemplates.h"
#include "game/editor/LevelValidate.h"

#include "core/Clock.h"
#include "core/Math.h"
#include "core/Rng.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/enemies/EnemyRoster.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "render/Camera.h"
#include "sim/SimWorld.h"
#include "sim/squad/Squads.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace immune::game {
namespace {

// ---------------------------------------------------------------------------
// Small string helpers. Command parsing runs between ticks, never inside one,
// so ordinary readable std::string code is the right call here (CONVENTIONS.md
// §3: "do not micro-optimize a level loader" — the same applies to a parser).
// ---------------------------------------------------------------------------

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        const usize start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > start) out.emplace_back(line.substr(start, i - start));
    }
    return out;
}

/// printf into a std::string. Every command's message goes through this so the
/// panel reports numbers the way the rest of the project prints them.
template <typename... Args>
std::string fmt(const char* format, Args... args) {
    char buf[768];
    const int n = std::snprintf(buf, sizeof(buf), format, args...);
    if (n <= 0) return std::string{};
    const usize len = math::min<usize>(static_cast<usize>(n), sizeof(buf) - 1);
    return std::string(buf, len);
}

GymResult fail(std::string message) { return GymResult{false, std::move(message)}; }
GymResult okay(std::string message) { return GymResult{true, std::move(message)}; }

bool parse_f32(const std::string& s, f32& out) {
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || end == nullptr || *end != '\0') return false;
    out = v;
    return true;
}

bool parse_i64(const std::string& s, i64& out) {
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || end == nullptr || *end != '\0') return false;
    out = static_cast<i64>(v);
    return true;
}

// ---------------------------------------------------------------------------
// Enum <-> name. The family strings are the same ones the level JSON and
// --sim-test scripts use (Level.cpp's parse_family), on purpose: one spelling
// of each family name across the whole project.
// ---------------------------------------------------------------------------

const char* const kFamilyNames[kFamilyCount] = {"virus", "bacteria"};

const char* family_name(PathogenFamily f) {
    const u32 i = static_cast<u32>(f);
    return i < kFamilyCount ? kFamilyNames[i] : "?";
}

bool parse_family(const std::string& s, PathogenFamily& out) {
    for (u32 i = 0; i < kFamilyCount; ++i) {
        if (s == kFamilyNames[i]) {
            out = static_cast<PathogenFamily>(i);
            return true;
        }
    }
    return false;
}

bool parse_ability(const std::string& s, AbilityId& out) {
    if (s == "complement" || s == "cascade" || s == "burst") {
        out = AbilityId::ComplementCascadeBurst;
        return true;
    }
    if (s == "histamine" || s == "flare") { out = AbilityId::HistamineFlare; return true; }
    if (s == "fever") { out = AbilityId::FeverResponse; return true; }
    return false;
}

const char* placement_result_name(PlacementResult r) {
    switch (r) {
        case PlacementResult::Ok: return "ok";
        case PlacementResult::NotOnTissue: return "not on tissue";
        case PlacementResult::InsufficientClearance: return "insufficient clearance";
        case PlacementResult::Overlapping: return "overlapping another tower";
        case PlacementResult::WouldBlockAllPaths: return "would block every path";
        case PlacementResult::CannotAfford: return "cannot afford";
        case PlacementResult::OutsidePlacementZone: return "outside placement zone";
        case PlacementResult::TowerNotAllowed: return "tower type not allowed on this level";
    }
    return "unknown";
}

const char* wave_phase_name(WavePhase p) {
    switch (p) {
        case WavePhase::Prep: return "prep";
        case WavePhase::Spawning: return "spawning";
        case WavePhase::Clearing: return "clearing";
        case WavePhase::Complete: return "complete";
    }
    return "?";
}

struct CombatEventName {
    const char* name;
    sim::CombatEventType type;
};

const CombatEventName kCombatEventNames[] = {
    {"muzzle", sim::CombatEventType::MuzzleFlash},
    {"impact", sim::CombatEventType::ProjectileImpact},
    {"expired", sim::CombatEventType::ProjectileExpired},
    {"explosion", sim::CombatEventType::Explosion},
    {"beam", sim::CombatEventType::BeamFired},
    {"chain", sim::CombatEventType::ChainArc},
    {"cone", sim::CombatEventType::ConePulse},
    {"freeze", sim::CombatEventType::Freeze},
    {"shatter", sim::CombatEventType::Shatter},
    {"slash", sim::CombatEventType::BladeSlash},
    {"splash", sim::CombatEventType::FluidSplash},
    {"death", sim::CombatEventType::ChaffDeath},
};

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

const sim::SpawnPointRuntime* find_spawn_point(const sim::SimWorld& world, const std::string& id) {
    for (const sim::SpawnPointRuntime& p : world.spawn_points()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

/// World position of the objective, recovered from the flow field's zero-cost
/// cells. SimWorld exposes no goal getter (the setter lives on ChaffSystem and
/// the goal cells live in the bake desc, neither of which survives as queryable
/// state), but a goal cell is exactly a cell whose cost-to-goal is zero, and
/// `costs()` is public. The flow field and the tissue mask are baked on the
/// same grid, so the mask converts cell -> world. O(cells); console-only.
bool objective_position(const sim::SimWorld& world, Vec2& out) {
    const sim::FlowField& flow = world.flow();
    const sim::TissueMask& mask = world.tissue();
    const f32* costs = flow.costs();
    if (costs == nullptr || flow.width() <= 0 || flow.height() <= 0) return false;
    if (mask.width() != flow.width() || mask.height() != flow.height()) return false;

    Vec2 sum{0.0f, 0.0f};
    u32 n = 0;
    for (i32 y = 0; y < flow.height(); ++y) {
        for (i32 x = 0; x < flow.width(); ++x) {
            const usize idx = static_cast<usize>(y) * static_cast<usize>(flow.width()) +
                              static_cast<usize>(x);
            if (costs[idx] <= 0.0f) {
                sum += mask.cell_to_world(x, y);
                ++n;
            }
        }
    }
    if (n == 0) return false;
    out = sum / static_cast<f32>(n);
    return true;
}

/// Parses an optional `at <where>` clause starting at token `i`, advancing `i`
/// past whatever it consumed. Accepted forms:
///     at 120,66     at 120 66     at p_lymph     at cursor
///     at objective  at center
/// An absent clause leaves `pos` at the caller's default and returns true, so
/// every command can call this unconditionally.
bool parse_at_clause(const GymContext& ctx, const std::vector<std::string>& tok, usize& i,
                     Vec2& pos, std::string& error) {
    if (i >= tok.size() || lower(tok[i]) != "at") return true;
    ++i;
    if (i >= tok.size()) {
        error = "'at' needs a target: x,y | <spawn_point id> | cursor | objective";
        return false;
    }

    const std::string where = lower(tok[i]);
    if (where == "cursor") {
        if (!ctx.has_cursor) { error = "no cursor in this context"; return false; }
        pos = ctx.cursor;
        ++i;
        return true;
    }
    if (where == "objective" || where == "organ" || where == "goal") {
        if (ctx.world == nullptr || !objective_position(*ctx.world, pos)) {
            error = "could not locate the objective";
            return false;
        }
        ++i;
        return true;
    }
    if (where == "center" || where == "centre") {
        if (ctx.world == nullptr) { error = "no world in this context"; return false; }
        pos = ctx.world->desc().world_bounds.center();
        ++i;
        return true;
    }

    // "x,y" in one token.
    const std::string& raw = tok[i];
    const usize comma = raw.find(',');
    if (comma != std::string::npos) {
        f32 x = 0.0f, y = 0.0f;
        if (!parse_f32(raw.substr(0, comma), x) || !parse_f32(raw.substr(comma + 1), y)) {
            error = "could not parse coordinate '" + raw + "'";
            return false;
        }
        pos = Vec2{x, y};
        ++i;
        return true;
    }

    // "x y" as two tokens.
    f32 x = 0.0f;
    if (parse_f32(raw, x)) {
        f32 y = 0.0f;
        if (i + 1 >= tok.size() || !parse_f32(tok[i + 1], y)) {
            error = "'at " + raw + "' needs a second coordinate";
            return false;
        }
        pos = Vec2{x, y};
        i += 2;
        return true;
    }

    // Otherwise: a spawn point id.
    if (ctx.world != nullptr) {
        if (const sim::SpawnPointRuntime* p = find_spawn_point(*ctx.world, tok[i])) {
            pos = p->position;
            ++i;
            return true;
        }
    }
    error = "unknown target '" + tok[i] + "' (not a spawn point id, coordinate, or keyword)";
    return false;
}

/// Where a spawn goes when the line says nothing: the first spawn point if the
/// level has one, else the middle of the world.
Vec2 default_spawn_point(const GymContext& ctx, f32& radius) {
    radius = 3.0f;
    if (ctx.world == nullptr) return Vec2{0.0f, 0.0f};
    if (!ctx.world->spawn_points().empty()) {
        radius = ctx.world->spawn_points()[0].radius;
        return ctx.world->spawn_points()[0].position;
    }
    return ctx.world->desc().world_bounds.center();
}

/// Optional trailing `radius <r>` / `r <r>` clause.
bool parse_radius_clause(const std::vector<std::string>& tok, usize& i, f32& radius,
                         std::string& error) {
    if (i >= tok.size()) return true;
    const std::string key = lower(tok[i]);
    if (key != "radius" && key != "r") return true;
    ++i;
    if (i >= tok.size() || !parse_f32(tok[i], radius) || radius <= 0.0f) {
        error = "'radius' needs a positive number";
        return false;
    }
    ++i;
    return true;
}

// ---------------------------------------------------------------------------
// Command table. Order is help order. Every entry must have a branch in
// gym_execute() and vice versa (see GymCommands.h, "adding a command").
// ---------------------------------------------------------------------------

const std::vector<GymCommandInfo>& command_table() {
    static const std::vector<GymCommandInfo> table = {
        {"help", "[command]", "List commands, or explain one."},
        {"spawn", "<family|all> <count> [at <x,y|spawn_point|cursor>] [radius <r>]",
         "Spawn chaff. 'all' spawns every family at once."},
        {"elite", "<name|id|all|list> [at <x,y|spawn_point|cursor>]",
         "Spawn a named elite."},
        {"flood", "[count-per-spawn-point]",
         "Every family out of every spawn point at once. The stress button."},
        {"kill", "[family|all]",
         "Flag chaff for removal, with real kill accounting."},
        {"tower", "<type|all|list> [at <x,y|cursor>] [tier <1-3>]",
         "Place a tower for free. 'all' spreads one of each across the level."},
        {"upgrade", "[all]", "Upgrade the last-placed tower, or every tower, one tier."},
        {"sell", "[all]", "Sell the last-placed tower, or every tower."},
        {"fire", "", "Trigger every placed tower's active ability."},
        {"cast", "<complement|histamine|fever> [at <x,y|cursor>]", "Cast a player ability."},
        {"ready", "", "Clear every ability cooldown."},
        {"atp", "<amount|+amount>", "Set or add ATP."},
        {"wave", "[start|next|status|<index>]",
         "Skip prep, jump waves, or print the director's state."},
        {"field", "<radius> <kill_rate> [duration] [at <x,y|cursor>]",
         "Submit a raw damage field. Tests aggregate damage in isolation."},
        {"vfx", "<event|all|list> [at <x,y|cursor>]",
         "Raise a combat event so the particle layer draws it."},
        {"time", "<scale>", "Set the time scale (0 pauses)."},
        {"step", "[ticks]", "Advance the sim by N ticks. Works while paused."},
        {"cam", "<x,y|spawn_point|objective|fit> [height]", "Move the camera."},
        {"overlay", "<debug|threat|squads> [on|off]", "Toggle a HUD overlay."},
        {"squads", "[on|off|list|paths]", "Inspect or toggle the squad layer."},
        {"invuln", "[on|off]",
         "Hold the objective's integrity, so a leak cannot end the run."},
        {"autoplay", "[on|off] [profile]",
         "Let the balance bot play this level. Pair with `time 8` to watch it fast."},
        {"stats", "", "Print the sim snapshot, economy, and wave state."},
        {"spawn_points", "", "List this level's spawn points and its objective."},
        {"level", "<name|path>", "Load another level."},
        {"restart", "", "Reload the current level from scratch."},
        {"config", "<get|set|list|reload|dump> [path] [value]",
         "Read or retune assets/config/*.json live. 'list' takes a filter."},
        {"edit",
         "<new|list|vessel|point|obstacle|spawn|objective|zone|undo|redo|validate|save|revert>",
         "Drive the level editor's document (docs/LEVEL_EDITOR.md). Only in the editor."},
    };
    return table;
}

// ---------------------------------------------------------------------------
// Individual commands
// ---------------------------------------------------------------------------

GymResult cmd_help(const std::vector<std::string>& tok) {
    if (tok.size() > 1) {
        const std::string want = lower(tok[1]);
        for (const GymCommandInfo& c : command_table()) {
            if (want == c.name) return okay(fmt("%s %s\n    %s", c.name, c.args, c.help));
        }
        return fail("no such command: " + tok[1]);
    }
    std::string out = "gym commands ('help <name>' for one):";
    for (const GymCommandInfo& c : command_table()) {
        out += fmt("\n  %-9s %s", c.name, c.args);
    }
    return okay(std::move(out));
}

/// Lane of whichever spawn point is nearest `at`.
///
/// The gym can spawn anywhere, including points no spawn point owns, so there
/// is no authored lane to read. Nearest spawn point is the cheapest answer
/// that is right in the case that matters (`spawn ... at <spawn_point>`) and
/// harmless otherwise -- an unknown lane simply falls back to the level's
/// whole path list.
const std::string& nearest_lane_id(const sim::SimWorld& world, Vec2 at) {
    static const std::string kNone;
    const auto& spawn_points = world.spawn_points();
    if (spawn_points.empty()) return kNone;
    usize best = 0;
    f32 best_d2 = math::length_sq(spawn_points[0].position - at);
    for (usize i = 1; i < spawn_points.size(); ++i) {
        const f32 d2 = math::length_sq(spawn_points[i].position - at);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return spawn_points[best].lane_id;
}

/// How many agents ChaffSystem::spawn_burst() can place at `at` before its disc
/// grows past the tissue there.
///
/// spawn_burst() sizes its disc to hold the whole count at contact spacing
/// (about contact_d * sqrt(n) * 0.75) and grows past the requested radius to do
/// it. One 4,000-agent burst therefore asks for a disc far wider than any lane,
/// and the outer ring lands off the lumen, drifts out of the world, and
/// despawns -- `spawn 4000` silently leaving 2,900 alive makes every count in a
/// gym session a lie, and the counts are most of why the gym exists. So the
/// executor never asks for more than fits; the rest is streamed (GymSpawnQueue).
u32 burst_capacity(sim::SimWorld& world, PathogenFamily family, Vec2 at, f32 radius) {
    const sim::ChaffFamilyParams& fp =
        world.chaff_system().tuning().family[static_cast<u32>(family)];
    const f32 contact_d = math::max(0.05f, fp.radius * fp.contact_spacing);
    // 0.9 of the local clearance, since the disc is only approximately that
    // size and the boundary is where agents get lost.
    const f32 budget = math::max(radius, world.sdf().sample(at)) * 0.9f;
    const f32 ratio = math::max(1.0f, budget / (0.75f * contact_d));
    return math::clamp(static_cast<u32>(ratio * ratio), 1u, 4096u);
}

/// Places up to `count` agents at `at`, split into squads on their own paths.
///
/// Shared by the immediate path (release_spawn) and the streaming queue, so a
/// command that lands in one tick and one that trickles over fifty produce the
/// same thing: squads of SquadTuning::target_squad_size, each dropped on the
/// next path of the lane rather than all in one disc. Returns how many landed.
///
/// `on_path` decides where each squad's burst actually lands. True puts it on
/// the squad's own path, which is what makes a bare `spawn` produce what a wave
/// produces. False keeps every burst at `at` and lets cohesion gather them over
/// the next second instead.
///
/// The distinction exists because `spawn ... at 104,66` is a promise about a
/// coordinate: the gym's whole value is placing a horde exactly where you want
/// it and then firing something at it, and silently relocating the agents onto
/// a nearby path would quietly invalidate every such test. So an EXPLICIT `at`
/// is honoured literally and only the implicit default gets path placement.
///
/// Ungrouped -- one burst at `at`, exactly the pre-squad behaviour -- when the
/// layer is off or the level authored no paths.
u32 spawn_grouped(sim::SimWorld& world, PathogenFamily family, Vec2 at, f32 radius, u32 count,
                  bool on_path) {
    sim::SquadRegistry& reg = world.squads();
    if (!reg.tuning().enabled || reg.paths().empty()) {
        return world.chaff_system().spawn_burst(world.chaff(), family, at, radius, count,
                                                world.rng());
    }

    const std::string& lane = nearest_lane_id(world, at);
    const u32 squad_size = math::max(reg.tuning().target_squad_size, 1u);
    u32 placed = 0;
    u32 left = count;
    while (left > 0) {
        // A fresh squad per chunk: this is one command's worth of horde all
        // arriving at once, so every chunk is genuinely its own cohort. (The
        // wave director instead carries a cursor across ticks and closes it via
        // SquadRegistry::accepting(), because its arrivals are spread in time.)
        u16 path = 0;
        const u16 squad =
            reg.next_path_for_lane(lane, path) ? reg.create_squad(path, at) : sim::kNoSquad;
        const u32 chunk = math::min(left, squad_size);

        // On the squad's own path, so it starts grouped instead of on top of
        // whatever else is already there. Radius 0 lets spawn_burst size the
        // disc to exactly what this chunk needs at contact spacing -- which is
        // a squad-sized clump, not a chunk smeared over the whole spawn point.
        Vec2 spot = at;
        f32 spot_radius = radius;
        if (squad != sim::kNoSquad && on_path) {
            spot = reg.get(squad).anchor;
            spot_radius = 0.0f;
        }
        const u32 now = world.chaff_system().spawn_burst(world.chaff(), family, spot, spot_radius,
                                                         chunk, world.rng(), squad);
        placed += now;
        left -= chunk;
        if (now < chunk) break;   // chaff buffer full; stop rather than spin
    }
    return placed;
}

/// Spawns as much of `count` as fits at `at` right now and hands the remainder
/// to the context's spawn queue (if it has one). Returns what went in now.
///
/// The budget is still measured at the REQUESTED point, not per squad: it is
/// the honest answer to "how much horde does this stretch of lane hold", and
/// keeping it there is what stops an oversized command from opening fifty
/// squads in a single tick that would all land stacked on the same few metres
/// of path. The surplus streams, and the squads come out spread over time.
u32 release_spawn(GymContext& ctx, PathogenFamily family, Vec2 at, f32 radius, u32 count,
                  bool on_path) {
    const u32 fits = math::min(count, burst_capacity(*ctx.world, family, at, radius));
    const u32 now = spawn_grouped(*ctx.world, family, at, radius, fits, on_path);
    if (now < count && ctx.spawns != nullptr) {
        const bool group = ctx.world->squads().tuning().enabled &&
                           !ctx.world->squads().paths().empty();
        ctx.spawns->enqueue(family, at, radius, count - now, group, on_path);
    }
    return now;
}

GymResult cmd_spawn(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    if (tok.size() < 2) return fail("usage: spawn <family|all> <count> [at ...] [radius <r>]");

    const std::string fam_token = lower(tok[1]);
    const bool every_family = (fam_token == "all");
    PathogenFamily family = PathogenFamily::Virus;
    if (!every_family && !parse_family(fam_token, family)) {
        return fail("unknown family '" + tok[1] + "' (virus bacteria)");
    }

    i64 count = 100;
    usize i = 2;
    if (i < tok.size() && parse_i64(tok[i], count)) ++i;
    if (count <= 0) return fail("count must be positive");

    f32 radius = 3.0f;
    Vec2 at = default_spawn_point(ctx, radius);
    std::string error;
    const usize before_at = i;
    if (!parse_at_clause(ctx, tok, i, at, error)) return fail(error);
    // An explicit `at` is a promise about a coordinate; only the implicit
    // default is free to be moved onto a squad path. See spawn_grouped().
    const bool on_path = (i == before_at);
    if (!parse_radius_clause(tok, i, radius, error)) return fail(error);

    if (every_family) {
        const u32 per = static_cast<u32>(count);
        u32 now = 0;
        for (u32 f = 0; f < kFamilyCount; ++f) {
            now += release_spawn(ctx, static_cast<PathogenFamily>(f), at, radius, per,
                                 on_path);
        }
        const u32 queued = per * kFamilyCount - now;
        return okay(queued == 0
                        ? fmt("spawned %u agents (%u per family) at (%.1f, %.1f) r=%.1f", now, per,
                              at.x, at.y, radius)
                        : fmt("spawning %u agents (%u per family) at (%.1f, %.1f): %u now, %u "
                              "streaming in",
                              per * kFamilyCount, per, at.x, at.y, now, queued));
    }

    const u32 asked = static_cast<u32>(count);
    const u32 now = release_spawn(ctx, family, at, radius, asked, on_path);
    if (now == asked) {
        return okay(fmt("spawned %u %s at (%.1f, %.1f) r=%.1f", now, family_name(family), at.x,
                        at.y, radius));
    }
    if (ctx.spawns == nullptr) {
        return okay(fmt("spawned %u/%u %s at (%.1f, %.1f) — that is all this spot holds in one "
                        "burst, and this context has no spawn queue to stream the rest",
                        now, asked, family_name(family), at.x, at.y));
    }
    return okay(fmt("spawning %u %s at (%.1f, %.1f): %u now, %u streaming in over ~%.1fs",
                    asked, family_name(family), at.x, at.y, now, asked - now,
                    static_cast<f32>(asked - now) / math::max(1.0f, static_cast<f32>(now)) *
                        kFixedDt));
}

GymResult cmd_elite(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    if (ctx.enemies == nullptr) return fail("no enemy roster in this context");
    const std::vector<EliteDef>& elites = ctx.enemies->elites();
    if (elites.empty()) return fail("the roster has no elites loaded");

    if (tok.size() < 2 || lower(tok[1]) == "list") {
        std::string out = "elites:";
        for (const EliteDef& e : elites) {
            out += fmt("\n  %u %-18s %-12s hp=%.0f", static_cast<unsigned>(e.id), e.name,
                       family_name(e.family), e.max_health);
        }
        return okay(std::move(out));
    }

    const std::string want = lower(tok[1]);
    usize i = 2;
    f32 ignored_radius = 0.0f;
    Vec2 at = default_spawn_point(ctx, ignored_radius);
    std::string error;
    if (!parse_at_clause(ctx, tok, i, at, error)) return fail(error);

    if (want == "all") {
        // Fanned along a short line so five elites do not stack into one
        // unreadable pile on the spawn point.
        u32 n = 0;
        for (usize k = 0; k < elites.size(); ++k) {
            const f32 t = static_cast<f32>(k) - 0.5f * static_cast<f32>(elites.size() - 1);
            if (ctx.enemies->spawn_elite(*ctx.world, elites[k].id, at + Vec2{0.0f, t * 4.0f})
                    .valid()) {
                ++n;
            }
        }
        return okay(fmt("spawned %u/%zu elites at (%.1f, %.1f)", n, elites.size(), at.x, at.y));
    }

    const EliteDef* def = ctx.enemies->find_elite(want);
    if (def == nullptr) {
        i64 id = 0;
        if (parse_i64(tok[1], id)) {
            for (const EliteDef& e : elites) {
                if (static_cast<i64>(e.id) == id) { def = &e; break; }
            }
        }
    }
    if (def == nullptr) return fail("unknown elite '" + tok[1] + "' (try: elite list)");

    if (!ctx.enemies->spawn_elite(*ctx.world, def->id, at).valid()) {
        return fail(fmt("spawning '%s' failed", def->name));
    }
    return okay(fmt("spawned elite %s at (%.1f, %.1f)", def->name, at.x, at.y));
}

GymResult cmd_flood(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    i64 per_spawn_point = 600;
    if (tok.size() > 1 && !parse_i64(tok[1], per_spawn_point)) {
        return fail("usage: flood [count-per-spawn-point]");
    }
    if (per_spawn_point <= 0) return fail("count must be positive");

    const std::vector<sim::SpawnPointRuntime>& spawn_points = ctx.world->spawn_points();
    if (spawn_points.empty()) return fail("this level has no spawn points");

    const u32 per_family = math::max(1u, static_cast<u32>(per_spawn_point) / kFamilyCount);
    u32 spawned = 0;
    for (const sim::SpawnPointRuntime& p : spawn_points) {
        for (u32 f = 0; f < kFamilyCount; ++f) {
            // Spawn-point-sourced, so path placement is right here: `flood` is
            // "what a wave would do, now", not a request for a coordinate.
            spawned += release_spawn(ctx, static_cast<PathogenFamily>(f), p.position, p.radius,
                                     per_family, /*on_path*/ true);
        }
    }
    const u32 asked = per_family * kFamilyCount * static_cast<u32>(spawn_points.size());
    return okay(fmt("flood: %u of %u agents from %zu spawn point(s) now%s; chaff %zu/%zu", spawned,
                    asked, spawn_points.size(),
                    spawned < asked ? ", the rest streaming in" : "",
                    ctx.world->chaff().count(), ctx.world->chaff().capacity()));
}

GymResult cmd_kill(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    sim::ChaffBuffers& chaff = ctx.world->chaff();

    const bool all = tok.size() < 2 || lower(tok[1]) == "all";
    PathogenFamily family = PathogenFamily::Virus;
    if (!all && !parse_family(lower(tok[1]), family)) {
        return fail("unknown family '" + tok[1] + "' (or 'all')");
    }

    // Flagged, not erased: removal goes through compact() on the next tick, so
    // the kill counters, the economy credit, and the per-family counts all move
    // the way a real kill moves them. chaff().clear() skips every one of those.
    usize n = 0;
    for (usize i = 0; i < chaff.count(); ++i) {
        if (!all && chaff.family[i] != static_cast<u8>(family)) continue;
        chaff.kill(i);
        ++n;
    }
    return okay(fmt("flagged %zu %s agent(s) for removal", n, all ? "chaff" : family_name(family)));
}

/// Shared by `tower <type>` and `tower all`: tries the requested point, then a
/// few nudges along Y, because a gym user who typed a coordinate that landed in
/// a vessel wall means "near here", not "exactly here or nothing".
EntityId place_near(GymContext& ctx, TowerType type, Vec2 want, Vec2& out_at,
                    PlacementResult& out_why_not) {
    out_why_not = PlacementResult::Ok;
    for (const f32 dy : {0.0f, 3.0f, -3.0f, 6.0f, -6.0f, 9.0f, -9.0f}) {
        const Vec2 p{want.x, want.y + dy};
        const PlacementQuery q = ctx.towers->validate(*ctx.world, type, p, 0xFFFF'FFFFu);
        if (!q.valid()) {
            out_why_not = q.result;
            continue;
        }
        const EntityId e = ctx.towers->place(*ctx.world, type, p);
        if (e.valid()) {
            out_at = q.snapped_position;
            return e;
        }
    }
    return EntityId{};
}

GymResult cmd_tower(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    if (ctx.towers == nullptr) return fail("no tower system in this context");

    if (tok.size() < 2 || lower(tok[1]) == "list") {
        std::string out = "tower types:";
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            const TowerType type = static_cast<TowerType>(t);
            const TowerStats& st = ctx.towers->stats(type, 1);
            out += fmt("\n  %-11s range=%.1f rate=%.2fs dmg=%.0f cost=%u", tower_type_name(type),
                       st.range, st.fire_interval, st.damage, st.build_cost);
        }
        return okay(std::move(out));
    }

    const std::string type_token = lower(tok[1]);
    const bool every_type = (type_token == "all");
    TowerType type = TowerType::Neutrophil;
    if (!every_type && !parse_tower_type(type_token, type)) {
        return fail("unknown tower '" + tok[1] + "' (try: tower list)");
    }

    usize i = 2;
    Vec2 at = ctx.has_cursor ? ctx.cursor : ctx.world->desc().world_bounds.center();
    std::string error;
    if (!parse_at_clause(ctx, tok, i, at, error)) return fail(error);

    i64 tier = 1;
    if (i < tok.size() && lower(tok[i]) == "tier") {
        ++i;
        if (i >= tok.size() || !parse_i64(tok[i], tier)) return fail("'tier' needs 1-3");
        ++i;
        tier = math::clamp<i64>(tier, 1, 3);
    }
    const auto upgrade_to_tier = [&](EntityId e) {
        for (i64 t = 1; t < tier; ++t) ctx.towers->upgrade(*ctx.world, e);
    };

    if (every_type) {
        // Spread across the level the same way --screenshot --towers does, so a
        // gym session and a regression capture frame the same arrangement.
        const Rect b = ctx.world->desc().world_bounds;
        u32 placed = 0;
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            const f32 frac = (static_cast<f32>(t) + 1.0f) / static_cast<f32>(kTowerTypeCount + 1);
            Vec2 got{};
            PlacementResult why_not = PlacementResult::Ok;
            const EntityId e = place_near(ctx, static_cast<TowerType>(t),
                                          Vec2{b.min.x + b.size().x * frac, at.y}, got, why_not);
            if (e.valid()) {
                upgrade_to_tier(e);
                ++placed;
            }
        }
        return placed == 0
                   ? fail("placed nothing — no valid spot on that row (try 'tower all at <x,y>')")
                   : okay(fmt("placed %u/%u tower types at tier %lld", placed, kTowerTypeCount,
                              static_cast<long long>(tier)));
    }

    Vec2 got{};
    PlacementResult why_not = PlacementResult::Ok;
    const EntityId e = place_near(ctx, type, at, got, why_not);
    if (!e.valid()) {
        return fail(fmt("cannot place %s near (%.1f, %.1f): %s", tower_type_name(type), at.x, at.y,
                        placement_result_name(why_not)));
    }
    upgrade_to_tier(e);
    return okay(fmt("placed %s at (%.1f, %.1f) tier %lld (free)", tower_type_name(type), got.x,
                    got.y, static_cast<long long>(tier)));
}

GymResult cmd_upgrade(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr || ctx.towers == nullptr) {
        return fail("no tower system in this context");
    }
    const std::vector<EntityId>& placed = ctx.towers->placed_towers();
    if (placed.empty()) return fail("no towers placed");

    if (tok.size() > 1 && lower(tok[1]) == "all") {
        u32 n = 0;
        for (const EntityId e : placed) {
            if (ctx.towers->upgrade(*ctx.world, e) != 0) ++n;
        }
        return okay(fmt("upgraded %u/%zu tower(s)", n, placed.size()));
    }
    const u8 tier = ctx.towers->upgrade(*ctx.world, placed.back());
    return tier == 0 ? fail("that tower is already at max tier")
                     : okay(fmt("upgraded the last tower to tier %u", static_cast<unsigned>(tier)));
}

GymResult cmd_sell(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr || ctx.towers == nullptr) {
        return fail("no tower system in this context");
    }
    if (ctx.towers->placed_towers().empty()) return fail("no towers placed");

    if (tok.size() > 1 && lower(tok[1]) == "all") {
        // sell() mutates placed_towers(), so iterate a copy.
        const std::vector<EntityId> all = ctx.towers->placed_towers();
        u32 refunded = 0;
        for (const EntityId e : all) refunded += ctx.towers->sell(*ctx.world, e);
        if (ctx.economy != nullptr) ctx.economy->credit_bounty(refunded);
        return okay(fmt("sold %zu tower(s) for %u ATP", all.size(), refunded));
    }
    const u32 refunded = ctx.towers->sell(*ctx.world, ctx.towers->placed_towers().back());
    if (ctx.economy != nullptr) ctx.economy->credit_bounty(refunded);
    return okay(fmt("sold the last tower for %u ATP", refunded));
}

GymResult cmd_fire(GymContext& ctx) {
    if (ctx.world == nullptr || ctx.towers == nullptr) {
        return fail("no tower system in this context");
    }
    const std::vector<EntityId>& placed = ctx.towers->placed_towers();
    if (placed.empty()) return fail("no towers placed");
    u32 n = 0;
    for (const EntityId e : placed) {
        if (ctx.towers->trigger_ability(*ctx.world, e)) ++n;
    }
    return okay(fmt("triggered %u/%zu tower abilities (the rest are on cooldown or have none)", n,
                    placed.size()));
}

GymResult cmd_cast(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    if (ctx.abilities == nullptr) return fail("no ability system in this context");
    if (tok.size() < 2) return fail("usage: cast <complement|histamine|fever> [at ...]");

    AbilityId id = AbilityId::ComplementCascadeBurst;
    if (!parse_ability(lower(tok[1]), id)) {
        return fail("unknown ability '" + tok[1] + "' (complement, histamine, fever)");
    }

    usize i = 2;
    Vec2 at = ctx.has_cursor ? ctx.cursor : ctx.world->desc().world_bounds.center();
    std::string error;
    if (!parse_at_clause(ctx, tok, i, at, error)) return fail(error);

    if (!ctx.abilities->cast(*ctx.world, id, at)) {
        const AbilityStatus st = ctx.abilities->status(id);
        return fail(fmt("%s is on cooldown (%.1fs left) — 'ready' clears it", ability_name(id),
                        st.cooldown_remaining));
    }
    return okay(fmt("cast %s at (%.1f, %.1f)", ability_name(id), at.x, at.y));
}

GymResult cmd_ready(GymContext& ctx) {
    if (ctx.abilities == nullptr) return fail("no ability system in this context");
    // load_defaults() re-seeds the tuning AND zeroes every cooldown, which is
    // exactly "make them all ready" — no debug-only setter needed on the frozen
    // ActiveAbilities header.
    ctx.abilities->load_defaults();
    return okay("every ability cooldown cleared");
}

GymResult cmd_atp(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.economy == nullptr) return fail("no economy in this context");
    if (tok.size() < 2) {
        return okay(fmt("atp: %u (usage: atp <amount|+amount>)", ctx.economy->atp()));
    }

    const bool relative = tok[1][0] == '+';
    i64 amount = 0;
    if (!parse_i64(relative ? tok[1].substr(1) : tok[1], amount) || amount < 0) {
        return fail("usage: atp <amount|+amount>");
    }
    if (!relative) {
        // Economy has no setter by design (income has exactly two sources), so
        // "set" is spend-to-zero then credit. Totals stay internally consistent.
        ctx.economy->spend(ctx.economy->atp());
    }
    ctx.economy->credit_bounty(static_cast<u32>(amount));
    return okay(fmt("atp: %u", ctx.economy->atp()));
}

GymResult cmd_wave(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.waves == nullptr) return fail("no wave director in this context");
    if (ctx.world == nullptr) return fail("no world in this context");

    const std::string sub = tok.size() > 1 ? lower(tok[1]) : std::string("status");
    const WaveStatus st = ctx.waves->status();

    if (sub == "status") {
        std::string out = fmt("wave %u/%zu  phase=%s  %.1fs left  %u still to spawn%s",
                              st.wave_index + 1, ctx.waves->waves().size(),
                              wave_phase_name(st.phase), st.phase_time_remaining,
                              st.remaining_to_spawn,
                              st.all_waves_complete ? "  [ALL COMPLETE]" : "");
        if (const WaveDef* next = ctx.waves->next_wave()) {
            u32 total = 0;
            for (const SpawnEntry& e : next->spawns) total += e.count;
            out += fmt("\nnext: %s — %zu entries, %u agents, prep %.0fs",
                       next->name.empty() ? "(unnamed)" : next->name.c_str(), next->spawns.size(),
                       total, next->prep_time);
        }
        return okay(std::move(out));
    }
    if (sub == "start" || sub == "now") {
        ctx.waves->request_early_start();
        return okay("prep skipped — the wave starts on the next tick");
    }

    // "wave next" / "wave <n>" re-seat the director at a chosen wave.
    // set_waves() + start() is the only public way to move the index, so this
    // hands it the tail of the same table starting at the target; the waves
    // before it are dropped, which is what "jump to wave 5" means in a gym.
    const std::vector<WaveDef>& table = ctx.waves->waves();
    if (table.empty()) return fail("this level has no wave table");

    i64 target = 0;
    if (sub == "next") {
        target = static_cast<i64>(st.wave_index) + 1;
    } else if (!parse_i64(sub, target)) {
        return fail("usage: wave [start|next|status|<index>]");
    } else {
        target -= 1;   // 1-based on the command line, 0-based in the director.
    }
    if (target < 0 || target >= static_cast<i64>(table.size())) {
        return fail(fmt("wave index out of range (1..%zu)", table.size()));
    }

    std::vector<WaveDef> rest(table.begin() + static_cast<std::ptrdiff_t>(target), table.end());
    for (usize k = 0; k < rest.size(); ++k) rest[k].index = static_cast<u32>(k);
    const std::string name = rest.front().name;
    ctx.waves->set_waves(std::move(rest));
    ctx.waves->start(*ctx.world);
    ctx.waves->request_early_start();
    return okay(fmt("jumped to wave %lld (%s); prep skipped", static_cast<long long>(target + 1),
                    name.empty() ? "unnamed" : name.c_str()));
}

GymResult cmd_field(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    if (tok.size() < 3) return fail("usage: field <radius> <kill_rate> [duration] [at ...]");

    sim::DamageField f;
    f.shape = sim::FieldShape::Circle;
    if (!parse_f32(tok[1], f.radius) || f.radius <= 0.0f) return fail("radius must be > 0");
    if (!parse_f32(tok[2], f.kill_rate)) return fail("kill_rate must be a number");

    usize i = 3;
    f.lifetime = 2.0f;
    if (i < tok.size() && lower(tok[i]) != "at") {
        if (!parse_f32(tok[i], f.lifetime)) return fail("duration must be a number");
        ++i;
    }
    // DamageField.h: lifetime <= 0 means "persistent, re-submitted by its owner
    // every tick". Nothing here re-submits, so a gym field always gets a finite
    // lifetime rather than becoming an orphan that never evaluates.
    if (f.lifetime <= 0.0f) f.lifetime = kFixedDt;

    Vec2 at = ctx.has_cursor ? ctx.cursor : ctx.world->desc().world_bounds.center();
    std::string error;
    if (!parse_at_clause(ctx, tok, i, at, error)) return fail(error);
    f.origin = at;
    f.falloff = 1.0f;

    ctx.world->damage().submit(f);
    return okay(fmt("field r=%.1f rate=%.1f/s for %.2fs at (%.1f, %.1f)", f.radius, f.kill_rate,
                    f.lifetime, at.x, at.y));
}

GymResult cmd_vfx(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    if (tok.size() < 2 || lower(tok[1]) == "list") {
        std::string out = "combat events:";
        for (const CombatEventName& e : kCombatEventNames) out += fmt("\n  %s", e.name);
        return okay(std::move(out));
    }

    const std::string want = lower(tok[1]);
    usize i = 2;
    Vec2 at = ctx.has_cursor ? ctx.cursor : ctx.world->desc().world_bounds.center();
    std::string error;
    if (!parse_at_clause(ctx, tok, i, at, error)) return fail(error);

    const auto make = [](sim::CombatEventType type, Vec2 origin) {
        sim::CombatEvent e;
        e.type = type;
        e.origin = origin;
        e.secondary = origin + Vec2{10.0f, 4.0f};
        e.direction = Vec2{1.0f, 0.0f};
        e.radius = 6.0f;
        e.arc_radians = 0.6f;
        e.magnitude = 4.0f;
        e.source = TowerType::Neutrophil;
        e.target_family = PathogenFamily::Virus;
        return e;
    };

    if (want == "all") {
        // Spread along a row so ten simultaneous effects stay separable instead
        // of compositing into one blob — the whole point of firing them by hand.
        u32 n = 0;
        for (const CombatEventName& e : kCombatEventNames) {
            ctx.world->combat_events().push(
                make(e.type, at + Vec2{static_cast<f32>(n) * 9.0f - 40.0f, 0.0f}));
            ++n;
        }
        return okay(fmt("raised %u combat events across a row at y=%.1f", n, at.y));
    }

    for (const CombatEventName& e : kCombatEventNames) {
        if (want == e.name) {
            ctx.world->combat_events().push(make(e.type, at));
            return okay(fmt("raised %s at (%.1f, %.1f)", e.name, at.x, at.y));
        }
    }
    return fail("unknown event '" + tok[1] + "' (try: vfx list)");
}

GymResult cmd_time(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.clock == nullptr) return fail("no clock in this context");
    if (tok.size() < 2) return okay(fmt("time scale: %.2fx", ctx.clock->time_scale()));
    f32 scale = 1.0f;
    if (!parse_f32(tok[1], scale) || scale < 0.0f) return fail("usage: time <scale >= 0>");
    ctx.clock->set_time_scale(scale);
    return okay(fmt("time scale: %.2fx%s", scale, scale == 0.0f ? " (paused)" : ""));
}

GymResult cmd_step(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    i64 ticks = 1;
    if (tok.size() > 1 && !parse_i64(tok[1], ticks)) return fail("usage: step [ticks]");
    if (ticks <= 0 || ticks > 100'000) return fail("step takes 1..100000 ticks");

    // Ticks the sim directly rather than going through the clock, because the
    // point of `step` is to advance while the clock is stopped. Deliberately
    // does NOT advance the wave director or the economy: this steps the
    // simulation, not the session, which is what you want when inspecting a
    // single frame of movement.
    for (i64 i = 0; i < ticks; ++i) ctx.world->tick(nullptr);
    return okay(fmt("stepped %lld tick(s) -> tick %llu", static_cast<long long>(ticks),
                    static_cast<unsigned long long>(ctx.world->tick_index())));
}

GymResult cmd_cam(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.camera == nullptr) return fail("no camera in this context");
    if (ctx.world == nullptr) return fail("no world in this context");
    if (tok.size() < 2) {
        return okay(fmt("camera at (%.1f, %.1f) height %.1f", ctx.camera->center().x,
                        ctx.camera->center().y, ctx.camera->view_height()));
    }

    const Rect bounds = ctx.world->desc().world_bounds;
    if (lower(tok[1]) == "fit") {
        ctx.camera->set_center(bounds.center());
        ctx.camera->set_view_height(bounds.size().y);
        ctx.camera->clamp_to_bounds();
        return okay("camera framing the whole level");
    }

    // Reuse the `at` grammar by treating the words after `cam` as if one
    // preceded them, so "cam p_lymph" and "spawn ... at p_lymph" resolve targets
    // identically instead of drifting apart.
    std::vector<std::string> as_at{"at"};
    as_at.insert(as_at.end(), tok.begin() + 1, tok.end());
    usize i = 0;
    Vec2 at = ctx.camera->center();
    std::string error;
    if (!parse_at_clause(ctx, as_at, i, at, error)) return fail(error);

    f32 height = ctx.camera->view_height();
    if (i < as_at.size() && !parse_f32(as_at[i], height)) {
        return fail("trailing '" + as_at[i] + "' is not a view height");
    }
    ctx.camera->set_center(at);
    ctx.camera->set_view_height(math::max(4.0f, height));
    ctx.camera->clamp_to_bounds();
    return okay(fmt("camera at (%.1f, %.1f) height %.1f", ctx.camera->center().x,
                    ctx.camera->center().y, ctx.camera->view_height()));
}

GymResult cmd_squads(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.world == nullptr) return fail("no world in this context");
    sim::SquadRegistry& reg = ctx.world->squads();

    const std::string sub = tok.size() > 1 ? lower(tok[1]) : std::string("list");

    if (sub == "on" || sub == "off" || sub == "1" || sub == "0" || sub == "true" ||
        sub == "false") {
        const bool on = (sub == "on" || sub == "1" || sub == "true");
        sim::SquadTuning t = reg.tuning();
        t.enabled = on;
        reg.set_tuning(t);
        // Deliberately NOT clearing live squads on "off": leaving membership
        // intact means `squads off` then `squads on` resumes the same groups,
        // which is what makes an A/B comparison mid-wave actually comparable.
        return okay(fmt("squads %s (%u live, %u paths)", on ? "on" : "off", reg.active_count(),
                        static_cast<u32>(reg.paths().size())));
    }

    if (sub == "paths") {
        if (reg.paths().empty()) return okay("no squad paths on this level");
        std::string out;
        for (usize i = 0; i < reg.paths().size(); ++i) {
            const sim::SquadPath& p = reg.paths()[i];
            out += fmt("[%u] %s lane=%s pts=%u len=%.1f half_width=%.1f\n", static_cast<u32>(i),
                       p.id.c_str(), p.lane_id.c_str(), static_cast<u32>(p.points.size()),
                       p.length(), p.half_width);
        }
        if (!out.empty() && out.back() == '\n') out.pop_back();
        return okay(out);
    }

    if (sub == "list") {
        if (reg.active_count() == 0) return okay("no live squads");
        std::string out = fmt("%u live squads / %u paths, enabled=%s\n", reg.active_count(),
                              static_cast<u32>(reg.paths().size()),
                              reg.tuning().enabled ? "yes" : "no");
        for (u32 id = 0; id < static_cast<u32>(reg.squads().size()); ++id) {
            if (!reg.alive(static_cast<u16>(id))) continue;
            const sim::Squad& sq = reg.get(static_cast<u16>(id));
            const f32 drag = math::length(sq.anchor - sq.centroid);
            out += fmt(
                "  #%u path=%u n=%u arc=%.1f r=%.1f spread=%.1f centroid=(%.1f,%.1f) drag=%.1f\n",
                id, static_cast<u32>(sq.path_index), sq.member_count, sq.arc_pos, sq.radius,
                sq.spread, sq.centroid.x, sq.centroid.y, drag);
        }
        if (!out.empty() && out.back() == '\n') out.pop_back();
        return okay(out);
    }

    return fail("usage: squads [on|off|list|paths]");
}

GymResult cmd_overlay(GymContext& ctx, const std::vector<std::string>& tok) {
    if (!ctx.set_overlay) return fail("no HUD in this context");
    if (tok.size() < 2) return fail("usage: overlay <debug|threat|squads> [on|off]");
    bool on = true;
    if (tok.size() > 2) {
        const std::string v = lower(tok[2]);
        if (v == "off" || v == "0" || v == "false") {
            on = false;
        } else if (v != "on" && v != "1" && v != "true") {
            return fail("expected on or off");
        }
    }
    const std::string name = lower(tok[1]);
    if (!ctx.set_overlay(name, on))
        return fail("unknown overlay '" + tok[1] + "' (debug, threat, squads)");
    return okay(fmt("%s overlay %s", name.c_str(), on ? "on" : "off"));
}

GymResult cmd_invuln(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.toggles == nullptr) return fail("nothing in this context holds per-tick gym settings");

    bool on = !ctx.toggles->objective_invulnerable;   // bare `invuln` toggles
    if (tok.size() > 1) {
        const std::string v = lower(tok[1]);
        if (v == "on" || v == "1" || v == "true") on = true;
        else if (v == "off" || v == "0" || v == "false") on = false;
        else return fail("usage: invuln [on|off]");
    }
    ctx.toggles->objective_invulnerable = on;
    if (on && ctx.world != nullptr) {
        // Snap it back immediately rather than waiting for the next tick, so
        // turning this on mid-breach reads as "stop the bleeding now".
        ctx.world->set_objective_integrity(ctx.toggles->hold_integrity);
    }
    return okay(on ? fmt("objective integrity held at %.0f -- leaks still count, the run cannot end",
                         static_cast<double>(ctx.toggles->hold_integrity))
                   : std::string("objective integrity is live again"));
}

GymResult cmd_autoplay(GymContext& ctx, const std::vector<std::string>& tok) {
    if (!ctx.set_autoplay) return fail("nothing in this context can run the bot");

    bool on = true;   // bare `autoplay` turns it on; `autoplay off` is explicit
    usize profile_at = 1;
    if (tok.size() > 1) {
        const std::string v = lower(tok[1]);
        if (v == "on" || v == "1" || v == "true") {
            profile_at = 2;
        } else if (v == "off" || v == "0" || v == "false") {
            on = false;
            profile_at = 2;
        }
    }
    const std::string profile = tok.size() > profile_at ? tok[profile_at] : std::string{};

    std::string err;
    if (!ctx.set_autoplay(on, profile, err)) return fail(err);
    if (!on) return okay("autoplay off -- the towers it built stay where they are");
    return okay(profile.empty() ? std::string("autoplay on (greedy-cheapest)")
                                : "autoplay on (" + profile + ")");
}

GymResult cmd_stats(GymContext& ctx) {
    if (ctx.world == nullptr) return fail("no world in this context");
    const sim::SimSnapshot s = ctx.world->snapshot();
    std::string out = fmt("tick %llu  chaff %llu/%zu  named %llu  density %.1f  integrity %.1f",
                          static_cast<unsigned long long>(s.tick),
                          static_cast<unsigned long long>(s.chaff_count),
                          ctx.world->chaff().capacity(),
                          static_cast<unsigned long long>(s.named_count), s.total_density,
                          s.objective_integrity);
    out += fmt("\nkilled %llu  leaked %llu  hash %016llx",
               static_cast<unsigned long long>(s.chaff_killed_total),
               static_cast<unsigned long long>(s.chaff_leaked_total),
               static_cast<unsigned long long>(ctx.world->state_hash()));
    out += "\nby family:";
    for (u32 f = 0; f < kFamilyCount; ++f) out += fmt(" %s=%u", kFamilyNames[f], s.chaff_by_family[f]);
    if (ctx.economy != nullptr) {
        const EconomySnapshot e = ctx.economy->snapshot();
        out += fmt("\natp %u  earned %u  spent %u", e.atp, e.total_earned, e.total_spent);
    }
    if (ctx.towers != nullptr) out += fmt("\ntowers placed %zu", ctx.towers->placed_towers().size());
    if (ctx.spawns != nullptr && ctx.spawns->pending() > 0) {
        out += fmt("\nqueued spawns: %u still streaming in", ctx.spawns->pending());
    }
    if (ctx.toggles != nullptr && ctx.toggles->objective_invulnerable) {
        out += fmt("\nobjective integrity held at %.0f (invuln on)",
                   static_cast<double>(ctx.toggles->hold_integrity));
    }
    if (ctx.waves != nullptr) {
        const WaveStatus w = ctx.waves->status();
        out += fmt("\nwave %u/%zu %s %.1fs", w.wave_index + 1, ctx.waves->waves().size(),
                   wave_phase_name(w.phase), w.phase_time_remaining);
    }
    return okay(std::move(out));
}

GymResult cmd_spawn_points(GymContext& ctx) {
    if (ctx.world == nullptr) return fail("no world in this context");
    const std::vector<sim::SpawnPointRuntime>& spawn_points = ctx.world->spawn_points();
    if (spawn_points.empty()) return okay("this level has no spawn points");
    std::string out = fmt("%zu spawn point(s):", spawn_points.size());
    for (const sim::SpawnPointRuntime& p : spawn_points) {
        out += fmt("\n  %-10s (%.1f, %.1f) r=%.1f", p.id.c_str(), p.position.x, p.position.y,
                   p.radius);
    }
    Vec2 objective{};
    if (objective_position(*ctx.world, objective)) {
        out += fmt("\n  objective  (%.1f, %.1f)", objective.x, objective.y);
    }
    return okay(std::move(out));
}

GymResult cmd_level(GymContext& ctx, const std::vector<std::string>& tok) {
    if (!ctx.load_level) return fail("level loading is not available in this context");
    if (tok.size() < 2) return fail("usage: level <name|path>");
    return ctx.load_level(tok[1]) ? okay("loaded level '" + tok[1] + "'")
                                  : fail("could not load level '" + tok[1] + "'");
}

GymResult cmd_restart(GymContext& ctx) {
    if (!ctx.restart_level) return fail("restart is not available in this context");
    return ctx.restart_level() ? okay("level restarted") : fail("restart failed");
}

// ---------------------------------------------------------------------------
// `edit` -- the level editor's document, as text.
// ---------------------------------------------------------------------------

/// "x,y" in one token. The same spelling parse_at_clause() accepts, kept
/// separate because the edit commands take bare coordinate pairs in positions
/// where an "at" keyword would just be noise.
bool parse_xy(const std::string& raw, Vec2& out, std::string& err) {
    const usize comma = raw.find(',');
    if (comma == std::string::npos) {
        err = "expected x,y but got '" + raw + "'";
        return false;
    }
    f32 x = 0.0f, y = 0.0f;
    if (!parse_f32(raw.substr(0, comma), x) || !parse_f32(raw.substr(comma + 1), y)) {
        err = "could not parse coordinate '" + raw + "'";
        return false;
    }
    out = Vec2{x, y};
    return true;
}

bool parse_index(const std::string& s, i32& out) {
    i64 v = 0;
    if (!parse_i64(s, v) || v < 0) return false;
    out = static_cast<i32>(v);
    return true;
}

/// Point argument: an explicit "x,y", the literal "cursor", or the cursor by
/// default. Mirrors how every other command that takes a point behaves.
bool edit_point_arg(GymContext& ctx, const std::vector<std::string>& tok, usize from, Vec2& out,
                    std::string& err) {
    if (from < tok.size() && lower(tok[from]) == "at") ++from;
    if (from >= tok.size() || lower(tok[from]) == "cursor") {
        if (!ctx.has_cursor) {
            err = "no cursor in this context; give an explicit x,y";
            return false;
        }
        out = ctx.cursor;
        return true;
    }
    return parse_xy(tok[from], out, err);
}

GymResult cmd_edit(GymContext& ctx, const std::vector<std::string>& tok) {
    if (ctx.doc == nullptr) {
        return fail("no level document open; `edit` works in the level editor (--editor)");
    }
    LevelDoc& doc = *ctx.doc;
    const auto touched = [&ctx]() {
        if (ctx.doc_changed) ctx.doc_changed();
    };

    if (tok.size() < 2) return fail("usage: edit <new|list|vessel|point|obstacle|...>");
    const std::string sub = lower(tok[1]);
    std::string err;

    if (sub == "list") {
        const LevelDef& d = doc.def();
        std::string out = fmt("level '%s' (%s)", d.name.c_str(), d.region.c_str());
        out += fmt("\n  world  %.0f x %.0f @ cell %.2f", d.world_bounds.size().x,
                   d.world_bounds.size().y, d.cell_size);
        for (usize i = 0; i < d.vessels.size(); ++i) {
            out += fmt("\n  vessel %zu  %s [%s] %zu pts", i, d.vessels[i].id.c_str(),
                       vessel_lane(d.vessels[i]).c_str(), d.vessels[i].points.size());
        }
        for (usize i = 0; i < d.obstacles.size(); ++i) {
            out += fmt("\n  obstacle %zu  %s (%s)", i, d.obstacles[i].id.c_str(),
                       obstacle_shape_to_string(d.obstacles[i].shape));
        }
        for (usize i = 0; i < d.spawn_points.size(); ++i) {
            out += fmt("\n  spawn %zu  %s at (%.1f, %.1f)", i, d.spawn_points[i].id.c_str(),
                       d.spawn_points[i].position.x, d.spawn_points[i].position.y);
        }
        for (usize i = 0; i < d.objectives.size(); ++i) {
            out += fmt("\n  objective %zu  %s at (%.1f, %.1f)", i, d.objectives[i].id.c_str(),
                       d.objectives[i].position.x, d.objectives[i].position.y);
        }
        out += fmt("\n  %zu zones, %zu squad paths, %zu waves", d.placement_zones.size(),
                   d.squad_paths.size(), d.waves.size());
        return okay(out);
    }

    if (sub == "new") {
        TemplateParams params;
        LevelTemplate t = LevelTemplate::StraightLane;
        if (tok.size() > 2) {
            const std::string want = lower(tok[2]);
            bool found = false;
            for (LevelTemplate c : all_level_templates()) {
                std::string n = level_template_to_string(c);
                for (char& ch : n) ch = static_cast<char>(::tolower(ch));
                // Match on the first word, so "fork" and "switchback" work.
                if (n.rfind(want, 0) != 0) continue;
                t = c;
                found = true;
                break;
            }
            if (!found) return fail("unknown template '" + tok[2] + "'; try `edit new fork`");
        }
        if (tok.size() > 3) params.name = tok[3];
        doc.set_document(make_template(t, params));
        touched();
        return okay(fmt("new level from template '%s'", level_template_to_string(t)));
    }

    if (sub == "undo") {
        if (!doc.undo()) return fail("nothing to undo");
        touched();
        return okay("undone");
    }
    if (sub == "redo") {
        if (!doc.redo()) return fail("nothing to redo");
        touched();
        return okay("redone");
    }

    if (sub == "validate") {
        const std::vector<Issue> issues = validate_level(doc.def());
        if (issues.empty()) return okay("no issues");
        std::string out;
        u32 errors = 0;
        for (const Issue& i : issues) {
            if (i.severity == Issue::Severity::Error) ++errors;
            out += fmt("%s%s: %s", out.empty() ? "" : "\n", severity_to_string(i.severity),
                       i.message.c_str());
        }
        // Errors make this a FAILED command, so `--exec "edit validate"` is a
        // usable gate in a script rather than something you have to grep.
        return errors > 0 ? fail(out) : okay(out);
    }

    if (sub == "save") {
        if (!ctx.doc_save) return fail("saving is not available in this context");
        const std::string path = tok.size() > 2 ? tok[2] : std::string{};
        const bool force = tok.size() > 3 && lower(tok[3]) == "force";
        if (!ctx.doc_save(path, force, err)) return fail(err);
        return okay("saved " + (path.empty() ? doc.source_path() : path));
    }
    if (sub == "revert") {
        if (!ctx.doc_revert) return fail("revert is not available in this context");
        if (!ctx.doc_revert(err)) return fail(err);
        touched();
        return okay("reverted");
    }

    if (sub == "vessel") {
        if (tok.size() < 3 || lower(tok[2]) != "add") return fail("usage: edit vessel add <x,y> <x,y> [width]");
        Vec2 a{}, b{};
        if (tok.size() < 5) return fail("usage: edit vessel add <x,y> <x,y> [width]");
        if (!parse_xy(tok[3], a, err)) return fail(err);
        if (!parse_xy(tok[4], b, err)) return fail(err);
        f32 w = 12.0f;
        if (tok.size() > 5 && !parse_f32(tok[5], w)) return fail("width must be a number");
        const i32 idx = doc.add_vessel(a, b, w);
        touched();
        return okay(fmt("added vessel %d ('%s')", idx,
                        doc.def().vessels[static_cast<usize>(idx)].id.c_str()));
    }

    if (sub == "point") {
        // edit point add <vessel> at <x,y>   |   edit point <vessel> <i> w <width>
        if (tok.size() >= 4 && lower(tok[2]) == "add") {
            i32 v = 0;
            if (!parse_index(tok[3], v)) return fail("vessel index must be a number");
            Vec2 at{};
            if (!edit_point_arg(ctx, tok, 4, at, err)) return fail(err);
            const i32 k = doc.insert_vessel_point(v, -1, at);
            if (k < 0) return fail("no such vessel");
            touched();
            return okay(fmt("vessel %d now has %zu points", v,
                            doc.def().vessels[static_cast<usize>(v)].points.size()));
        }
        if (tok.size() >= 6 && lower(tok[4]) == "w") {
            i32 v = 0, k = 0;
            f32 w = 0.0f;
            if (!parse_index(tok[2], v) || !parse_index(tok[3], k)) return fail("indices must be numbers");
            if (!parse_f32(tok[5], w)) return fail("width must be a number");
            doc.set_vessel_point_width(v, k, w);
            touched();
            return okay(fmt("vessel %d point %d width %.2f", v, k, w));
        }
        return fail("usage: edit point add <vessel> at <x,y> | edit point <vessel> <i> w <width>");
    }

    if (sub == "obstacle") {
        if (tok.size() < 3) return fail("usage: edit obstacle <disc|capsule|box|polygon|ridge> at <x,y> [size]");
        ObstacleShape shape{};
        if (!obstacle_shape_from_string(lower(tok[2]), shape)) {
            return fail("unknown shape '" + tok[2] + "'");
        }
        Vec2 at{};
        usize i = 3;
        if (!edit_point_arg(ctx, tok, i, at, err)) return fail(err);
        f32 size = 8.0f;
        for (usize k = 3; k + 1 < tok.size(); ++k) {
            if (lower(tok[k]) == "r" || lower(tok[k]) == "size") {
                if (!parse_f32(tok[k + 1], size)) return fail("size must be a number");
            }
        }
        const i32 idx = doc.add_obstacle(shape, at, size);
        touched();
        return okay(fmt("added %s obstacle %d at (%.1f, %.1f)", obstacle_shape_to_string(shape),
                        idx, at.x, at.y));
    }

    if (sub == "spawn" || sub == "objective") {
        usize i = 2;
        if (i < tok.size() && lower(tok[i]) == "add") ++i;
        Vec2 at{};
        if (!edit_point_arg(ctx, tok, i, at, err)) return fail(err);
        const i32 idx = sub == "spawn" ? doc.add_spawn_point(at) : doc.add_objective(at);
        touched();
        return okay(fmt("added %s %d at (%.1f, %.1f)", sub.c_str(), idx, at.x, at.y));
    }

    if (sub == "zone") {
        if (tok.size() < 4) return fail("usage: edit zone <x,y> <x,y>");
        Vec2 a{}, b{};
        if (!parse_xy(tok[2], a, err)) return fail(err);
        if (!parse_xy(tok[3], b, err)) return fail(err);
        const i32 idx = doc.add_zone(Rect{a, b});
        touched();
        return okay(fmt("added zone %d", idx));
    }

    if (sub == "delete" || sub == "erase") {
        if (!doc.erase_selection()) return fail("nothing selected, or the last of its kind");
        touched();
        return okay("deleted");
    }

    return fail("unknown edit subcommand '" + tok[1] + "'");
}

} // namespace

// ---------------------------------------------------------------------------
// GymToggles
// ---------------------------------------------------------------------------

void GymToggles::apply(sim::SimWorld& world) const {
    // Restored rather than prevented: the leak itself is left alone, so the
    // despawn still happens and chaff_leaked_total still moves. Suppressing the
    // despawn instead would pile agents on the objective and quietly change
    // what the horde does, which is the thing under test.
    if (objective_invulnerable) world.set_objective_integrity(hold_integrity);
}

// ---------------------------------------------------------------------------
// GymSpawnQueue
// ---------------------------------------------------------------------------

void GymSpawnQueue::enqueue(PathogenFamily family, Vec2 at, f32 radius, u32 count, bool group,
                            bool on_path) {
    if (count == 0) return;
    // Merge into an identical pending entry rather than growing the list: a user
    // hammering the same quick-action button five times should get one stream,
    // not five interleaved ones releasing five bursts a tick into one spot.
    for (Entry& e : entries_) {
        if (e.family == family && e.radius == radius && e.at == at && e.group == group &&
            e.on_path == on_path) {
            e.remaining += count;
            return;
        }
    }
    entries_.push_back(Entry{family, at, radius, count, group, on_path});
}

u32 GymSpawnQueue::tick(sim::SimWorld& world) {
    if (entries_.empty()) return 0;
    u32 released = 0;
    for (Entry& e : entries_) {
        if (e.remaining == 0) continue;
        // Re-measured every tick: the agents released last tick have moved off,
        // so how much fits now is a live question, not a cached one.
        const u32 fits = math::min(e.remaining, burst_capacity(world, e.family, e.at, e.radius));
        const u32 got = e.group
                            ? spawn_grouped(world, e.family, e.at, e.radius, fits, e.on_path)
                            : world.chaff_system().spawn_burst(world.chaff(), e.family, e.at,
                                                               e.radius, fits, world.rng());
        e.remaining -= got;
        released += got;
        if (got == 0 && world.chaff().full()) e.remaining = 0;   // buffer full: drop, do not spin
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [](const Entry& e) { return e.remaining == 0; }),
                   entries_.end());
    return released;
}

u32 GymSpawnQueue::pending() const {
    u32 n = 0;
    for (const Entry& e : entries_) n += e.remaining;
    return n;
}

const std::vector<GymCommandInfo>& gym_commands() { return command_table(); }

const std::vector<const char*>& gym_family_names() {
    static const std::vector<const char*> names(kFamilyNames, kFamilyNames + kFamilyCount);
    return names;
}

const std::vector<const char*>& gym_vfx_event_names() {
    static const std::vector<const char*> names = [] {
        std::vector<const char*> v;
        for (const CombatEventName& e : kCombatEventNames) v.push_back(e.name);
        return v;
    }();
    return names;
}


/// `config` — the tuning surface, reachable from the panel, from --exec, and
/// from a --sim-test "cmd" action like everything else here.
///
/// Deliberately narrow: it addresses fields by the same dotted path the config
/// registry uses, and it never invents a value. `set` writes through to the
/// live systems immediately; `dump` is how an experiment that worked gets kept.
GymResult cmd_config(GymContext& ctx, const std::vector<std::string>& tok) {
    if (tok.size() < 2) return fail("usage: config <get|set|list|reload|dump> [path] [value]");
    const std::string sub = lower(tok[1]);

    if (sub == "reload") {
        if (!ctx.config_reload) return fail("this context has no config");
        std::string err;
        if (!ctx.config_reload(err)) return fail("config reload failed: " + err);
        return okay("config reloaded");
    }

    if (sub == "dump") {
        if (!ctx.config_dump) return fail("this context has no config");
        std::string err;
        if (!ctx.config_dump(err)) return fail("config dump failed: " + err);
        return okay("config written");
    }

    if (sub == "list") {
        if (!ctx.config_paths) return fail("this context has no config");
        const std::string filter = tok.size() > 2 ? lower(tok[2]) : std::string{};
        std::string out;
        u32 shown = 0;
        for (const std::string& path : ctx.config_paths()) {
            if (!filter.empty() && lower(path).find(filter) == std::string::npos) continue;
            // A bare `config list` would print several hundred lines into a
            // console panel; cap it and say so rather than flooding.
            if (shown >= 40u) {
                out += "... (filter with 'config list <text>')";
                break;
            }
            if (!out.empty()) out += "\n";
            out += path;
            ++shown;
        }
        if (out.empty()) return fail("no config field matches '" + filter + "'");
        return okay(out);
    }

    if (sub == "get") {
        if (!ctx.config_get) return fail("this context has no config");
        if (tok.size() < 3) return fail("usage: config get <path>");
        std::string out;
        if (!ctx.config_get(tok[2], out)) return fail(out);
        return okay(tok[2] + " = " + out);
    }

    if (sub == "set") {
        if (!ctx.config_set) return fail("this context has no config");
        if (tok.size() < 4) return fail("usage: config set <path> <value>");
        // Vec2/Vec4 fields are written "0.5,0.28"; tokenize() splits on spaces
        // only, so a value with spaces around the commas is rejoined here.
        std::string value = tok[3];
        for (usize i = 4; i < tok.size(); ++i) value += tok[i];
        std::string err;
        if (!ctx.config_set(tok[2], value, err)) return fail(err);
        return okay(tok[2] + " = " + value);
    }

    return fail("unknown config subcommand '" + tok[1] + "'");
}

GymResult gym_execute(GymContext& ctx, std::string_view line) {
    const std::vector<std::string> tok = tokenize(line);
    if (tok.empty() || tok[0][0] == '#') return okay("");

    const std::string cmd = lower(tok[0]);
    if (cmd == "help" || cmd == "?") return cmd_help(tok);
    if (cmd == "spawn") return cmd_spawn(ctx, tok);
    if (cmd == "elite") return cmd_elite(ctx, tok);
    if (cmd == "flood") return cmd_flood(ctx, tok);
    if (cmd == "kill") return cmd_kill(ctx, tok);
    if (cmd == "tower") return cmd_tower(ctx, tok);
    if (cmd == "upgrade") return cmd_upgrade(ctx, tok);
    if (cmd == "sell") return cmd_sell(ctx, tok);
    if (cmd == "fire") return cmd_fire(ctx);
    if (cmd == "cast") return cmd_cast(ctx, tok);
    if (cmd == "ready") return cmd_ready(ctx);
    if (cmd == "atp") return cmd_atp(ctx, tok);
    if (cmd == "wave") return cmd_wave(ctx, tok);
    if (cmd == "field") return cmd_field(ctx, tok);
    if (cmd == "vfx") return cmd_vfx(ctx, tok);
    if (cmd == "time") return cmd_time(ctx, tok);
    if (cmd == "step") return cmd_step(ctx, tok);
    if (cmd == "cam" || cmd == "camera") return cmd_cam(ctx, tok);
    if (cmd == "overlay") return cmd_overlay(ctx, tok);
    if (cmd == "squads") return cmd_squads(ctx, tok);
    if (cmd == "invuln" || cmd == "invulnerable" || cmd == "godmode") {
        return cmd_invuln(ctx, tok);
    }
    if (cmd == "autoplay" || cmd == "bot") return cmd_autoplay(ctx, tok);
    if (cmd == "stats") return cmd_stats(ctx);
    if (cmd == "spawn_points") return cmd_spawn_points(ctx);
    if (cmd == "level") return cmd_level(ctx, tok);
    if (cmd == "restart") return cmd_restart(ctx);
    if (cmd == "config") return cmd_config(ctx, tok);
    if (cmd == "edit") return cmd_edit(ctx, tok);

    return fail("unknown command '" + tok[0] + "' — type 'help'");
}

GymResult gym_execute_script(GymContext& ctx, std::string_view text) {
    std::string joined;
    usize start = 0;
    while (start <= text.size()) {
        usize end = text.find_first_of(";\n", start);
        if (end == std::string_view::npos) end = text.size();
        const GymResult r = gym_execute(ctx, text.substr(start, end - start));
        if (!r.message.empty()) {
            if (!joined.empty()) joined += '\n';
            joined += r.message;
        }
        if (!r.ok) return GymResult{false, std::move(joined)};
        if (end == text.size()) break;
        start = end + 1;
    }
    return GymResult{true, std::move(joined)};
}

} // namespace immune::game
