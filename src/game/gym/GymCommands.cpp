// game/gym/GymCommands.cpp — parser and executor for the gym command language.
// See GymCommands.h for why this layer exists and what it deliberately is not.
#include "game/gym/GymCommands.h"

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
// of "fungal_spore" across the whole project.
// ---------------------------------------------------------------------------

const char* const kFamilyNames[kFamilyCount] = {
    "virus", "bacteria", "fungal_spore", "parasite", "cancer_cell", "allergen"};

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
    // Convenience aliases — typing "fungus" should not be a syntax error.
    if (s == "fungus" || s == "spore") { out = PathogenFamily::FungalSpore; return true; }
    if (s == "cancer") { out = PathogenFamily::CancerCell; return true; }
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
};

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

const sim::SpawnPortalRuntime* find_portal(const sim::SimWorld& world, const std::string& id) {
    for (const sim::SpawnPortalRuntime& p : world.portals()) {
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
        error = "'at' needs a target: x,y | <portal_id> | cursor | objective";
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

    // Otherwise: a portal id.
    if (ctx.world != nullptr) {
        if (const sim::SpawnPortalRuntime* p = find_portal(*ctx.world, tok[i])) {
            pos = p->position;
            ++i;
            return true;
        }
    }
    error = "unknown target '" + tok[i] + "' (not a portal id, coordinate, or keyword)";
    return false;
}

/// Where a spawn goes when the line says nothing: the first portal if the level
/// has one, else the middle of the world.
Vec2 default_spawn_point(const GymContext& ctx, f32& radius) {
    radius = 3.0f;
    if (ctx.world == nullptr) return Vec2{0.0f, 0.0f};
    if (!ctx.world->portals().empty()) {
        radius = ctx.world->portals()[0].radius;
        return ctx.world->portals()[0].position;
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
        {"spawn", "<family|all> <count> [at <x,y|portal|cursor>] [radius <r>]",
         "Spawn chaff. 'all' spawns every family at once."},
        {"elite", "<name|id|all|list> [at <x,y|portal|cursor>]",
         "Spawn a named elite."},
        {"flood", "[count-per-portal]",
         "Every family out of every portal at once. The stress button."},
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
        {"cam", "<x,y|portal|objective|fit> [height]", "Move the camera."},
        {"overlay", "<debug|threat> [on|off]", "Toggle a HUD overlay."},
        {"invuln", "[on|off]",
         "Hold the objective's integrity, so a leak cannot end the run."},
        {"stats", "", "Print the sim snapshot, economy, and wave state."},
        {"portals", "", "List this level's spawn portals and its objective."},
        {"level", "<name|path>", "Load another level."},
        {"restart", "", "Reload the current level from scratch."},
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

/// Spawns as much of `count` as fits at `at` right now and hands the remainder
/// to the context's spawn queue (if it has one). Returns what went in now.
u32 release_spawn(GymContext& ctx, PathogenFamily family, Vec2 at, f32 radius, u32 count) {
    const u32 fits = math::min(count, burst_capacity(*ctx.world, family, at, radius));
    const u32 now = ctx.world->chaff_system().spawn_burst(ctx.world->chaff(), family, at, radius,
                                                          fits, ctx.world->rng());
    if (now < count && ctx.spawns != nullptr) {
        ctx.spawns->enqueue(family, at, radius, count - now);
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
        return fail("unknown family '" + tok[1] +
                    "' (virus bacteria fungal_spore parasite cancer_cell allergen)");
    }

    i64 count = 100;
    usize i = 2;
    if (i < tok.size() && parse_i64(tok[i], count)) ++i;
    if (count <= 0) return fail("count must be positive");

    f32 radius = 3.0f;
    Vec2 at = default_spawn_point(ctx, radius);
    std::string error;
    if (!parse_at_clause(ctx, tok, i, at, error)) return fail(error);
    if (!parse_radius_clause(tok, i, radius, error)) return fail(error);

    if (every_family) {
        const u32 per = static_cast<u32>(count);
        u32 now = 0;
        for (u32 f = 0; f < kFamilyCount; ++f) {
            now += release_spawn(ctx, static_cast<PathogenFamily>(f), at, radius, per);
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
    const u32 now = release_spawn(ctx, family, at, radius, asked);
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
    i64 per_portal = 600;
    if (tok.size() > 1 && !parse_i64(tok[1], per_portal)) {
        return fail("usage: flood [count-per-portal]");
    }
    if (per_portal <= 0) return fail("count must be positive");

    const std::vector<sim::SpawnPortalRuntime>& portals = ctx.world->portals();
    if (portals.empty()) return fail("this level has no portals");

    const u32 per_family = math::max(1u, static_cast<u32>(per_portal) / kFamilyCount);
    u32 spawned = 0;
    for (const sim::SpawnPortalRuntime& p : portals) {
        for (u32 f = 0; f < kFamilyCount; ++f) {
            spawned += release_spawn(ctx, static_cast<PathogenFamily>(f), p.position, p.radius,
                                     per_family);
        }
    }
    const u32 asked = per_family * kFamilyCount * static_cast<u32>(portals.size());
    return okay(fmt("flood: %u of %u agents from %zu portal(s) now%s; chaff %zu/%zu", spawned,
                    asked, portals.size(),
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

GymResult cmd_overlay(GymContext& ctx, const std::vector<std::string>& tok) {
    if (!ctx.set_overlay) return fail("no HUD in this context");
    if (tok.size() < 2) return fail("usage: overlay <debug|threat> [on|off]");
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
    if (!ctx.set_overlay(name, on)) return fail("unknown overlay '" + tok[1] + "' (debug, threat)");
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

GymResult cmd_portals(GymContext& ctx) {
    if (ctx.world == nullptr) return fail("no world in this context");
    const std::vector<sim::SpawnPortalRuntime>& portals = ctx.world->portals();
    if (portals.empty()) return okay("this level has no portals");
    std::string out = fmt("%zu portal(s):", portals.size());
    for (const sim::SpawnPortalRuntime& p : portals) {
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

void GymSpawnQueue::enqueue(PathogenFamily family, Vec2 at, f32 radius, u32 count) {
    if (count == 0) return;
    // Merge into an identical pending entry rather than growing the list: a user
    // hammering the same quick-action button five times should get one stream,
    // not five interleaved ones releasing five bursts a tick into one spot.
    for (Entry& e : entries_) {
        if (e.family == family && e.radius == radius && e.at == at) {
            e.remaining += count;
            return;
        }
    }
    entries_.push_back(Entry{family, at, radius, count});
}

u32 GymSpawnQueue::tick(sim::SimWorld& world) {
    if (entries_.empty()) return 0;
    u32 released = 0;
    for (Entry& e : entries_) {
        if (e.remaining == 0) continue;
        // Re-measured every tick: the agents released last tick have moved off,
        // so how much fits now is a live question, not a cached one.
        const u32 fits = math::min(e.remaining, burst_capacity(world, e.family, e.at, e.radius));
        const u32 got = world.chaff_system().spawn_burst(world.chaff(), e.family, e.at, e.radius,
                                                         fits, world.rng());
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
    if (cmd == "invuln" || cmd == "invulnerable" || cmd == "godmode") {
        return cmd_invuln(ctx, tok);
    }
    if (cmd == "stats") return cmd_stats(ctx);
    if (cmd == "portals") return cmd_portals(ctx);
    if (cmd == "level") return cmd_level(ctx, tok);
    if (cmd == "restart") return cmd_restart(ctx);

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
