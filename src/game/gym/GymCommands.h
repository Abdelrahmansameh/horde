// game/gym/GymCommands.h — the gym's command layer. NEW MODULE.
//
// RATIONALE
// Every subsystem in this project is individually testable headlessly (--bench,
// --sim-test, --screenshot) and individually reachable from the HUD, but there
// was no single place a human could stand inside a running level and say "now
// do the thing I want to look at". Waiting out a prep timer to see wave 5's
// swarm modifier, or building six towers by hand to compare their muzzle
// flashes, is not testing -- it is bookkeeping.
//
// So: one text command language, one executor, and deliberately NO front end of
// its own. The executor is a pure function of (GymContext, line) and touches
// nothing it was not handed, which is what lets the same command string run
// from three places:
//   - the gym level's control window (ui/GymPanel.h), against the live App;
//   - a --sim-test script action ({"type":"cmd","cmd":"spawn virus 500"}),
//     against a headless SimWorld, so a gym command can be a regression test;
//   - a unit test (tests/test_gym_commands.cpp).
//
// WHAT IT IS NOT
// Not a cheat menu shipped to players, and not a second gameplay path. Commands
// mutate the same systems through the same public APIs app/ uses for player
// intents -- there is no back door into SimWorld here. The one thing that IS
// special is that placement/economy gates are bypassable (`tower` places for
// free), because paying 100 ATP to look at a Tesla arc is bookkeeping too.
//
// DETERMINISM
// Commands run OUTSIDE SimWorld::tick(), between ticks, exactly like a player
// intent. They allocate, log, and throw-free-parse strings, all of which is
// fine there and forbidden inside a tick. A command that spawns draws from
// world.rng(), so a run driven by a fixed command script at fixed ticks is
// still bit-reproducible -- that is the property --sim-test relies on.
//
// ADDING A COMMAND
// Add one entry to the kCommands table in the .cpp and one branch in
// gym_execute(). The table is what `help` and the panel's controls render, so
// a command with no table entry is invisible and a table entry with no branch
// reports "not implemented" -- both are loud, neither is silent.
#pragma once

#include "core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace immune { class FixedClock; }
namespace immune::render { class Camera; }
namespace immune::sim { class SimWorld; }

namespace immune::game {

class LevelDoc;

class ActiveAbilitySystem;
class Economy;
class EnemyRoster;
class TowerSystem;
class WaveDirector;

/// Pending streamed spawns.
///
/// A single spawn_burst() can only place as many agents as the tissue under it
/// can hold; ask for more and the surplus lands off the lumen and despawns
/// within a second (see burst_capacity() in the .cpp). The wave director already
/// solves this by spreading a wave's count over seconds, and this is the same
/// answer for a typed command: `spawn virus 4000` places what fits now and hands
/// the remainder here, which releases another burst each tick as the previous
/// one flows away.
///
/// Owned by whoever ticks the sim (app/, --sim-test, a test), never by the
/// context -- GymContext is rebuilt per frame and would drop the queue.
/// Deterministic: entries drain in insertion order, one pass per tick, with no
/// wall-clock input, so a scripted run replays identically.
class GymSpawnQueue {
public:
    /// `group` asks each release to open fresh squads (sim/squad/Squads.h) and
    /// put them on their own paths, rather than dumping one ungrouped burst.
    ///
    /// A flag rather than a fixed squad id, deliberately: a streamed command is
    /// spread over many ticks precisely BECAUSE it is too big to land at once,
    /// and something too big to land at once is by definition several squads.
    /// Pinning one id would produce a single blob of hundreds, which is not a
    /// squad and does not behave like one.
    /// `on_path` places each squad's burst on that squad's own path rather than
    /// at `at`; see spawn_grouped() in the .cpp for when that is and is not
    /// appropriate.
    void enqueue(PathogenFamily family, Vec2 at, f32 radius, u32 count, bool group = false,
                 bool on_path = true);

    /// Releases everything due this tick. Call exactly once per sim tick, from
    /// outside SimWorld::tick(). Returns how many agents were spawned.
    u32 tick(sim::SimWorld& world);

    /// Agents still waiting across all entries.
    u32 pending() const;
    void clear() { entries_.clear(); }

private:
    struct Entry {
        PathogenFamily family = PathogenFamily::Virus;
        Vec2 at{0.0f, 0.0f};
        f32 radius = 3.0f;
        u32 remaining = 0;
        bool group = false;
        bool on_path = true;
    };
    std::vector<Entry> entries_;
};

/// Gym state that has to be re-applied every tick, as opposed to commands that
/// execute once.
///
/// Owned by whoever ticks the sim (app/, --sim-test, a test), like
/// GymSpawnQueue and for the same reason: GymContext is rebuilt every frame and
/// would drop it.
struct GymToggles {
    /// While set, apply() restores the objective's integrity after every tick.
    /// A leak still despawns the agent and still counts in chaff_leaked_total --
    /// only the consequence is suspended, so what you are testing (does the
    /// horde reach the organ?) stays observable while the run cannot end under
    /// you mid-experiment.
    ///
    /// Defaults OFF here so every headless path keeps the real loss condition;
    /// app/ turns it on for the gym level, where a run that ends while you are
    /// setting up an experiment is pure friction.
    bool objective_invulnerable = false;
    /// The value integrity is held at. Captured at level load, so a level that
    /// authors 250 integrity is pinned at 250 rather than at a hardcoded 100.
    f32 hold_integrity = 100.0f;

    /// Re-applies whatever is enabled. Call once per sim tick, after
    /// SimWorld::tick() and before anything reads the snapshot for win/loss.
    void apply(sim::SimWorld& world) const;
};

/// Everything a command may reach, all optional.
///
/// Every pointer is nullable and every command checks the ones it needs before
/// using them, because the headless callers legitimately have no camera, no
/// clock, and no economy. A command that needs a subsystem the caller did not
/// supply fails with "no <thing> in this context" -- a normal, reportable
/// result, not a crash and not a silent no-op.
struct GymContext {
    sim::SimWorld* world = nullptr;
    TowerSystem* towers = nullptr;
    EnemyRoster* enemies = nullptr;
    WaveDirector* waves = nullptr;
    Economy* economy = nullptr;
    ActiveAbilitySystem* abilities = nullptr;
    FixedClock* clock = nullptr;
    render::Camera* camera = nullptr;
    /// Where a spawn too large for one burst puts its remainder. Without one,
    /// an oversized `spawn` places what fits and says so.
    GymSpawnQueue* spawns = nullptr;
    /// Per-tick toggles (objective invulnerability). Without one, `invuln`
    /// reports that this context cannot hold the setting.
    GymToggles* toggles = nullptr;

    /// Mouse position in world space. Commands that take a target point use it
    /// as the default when the caller has one, so "cast fever" means "here".
    Vec2 cursor{0.0f, 0.0f};
    bool has_cursor = false;

    /// Loads a level by file path or by bare name ("gym", "skin_1_breach").
    /// Supplied by app/; absent headlessly.
    std::function<bool(const std::string&)> load_level;
    /// Reloads the current level from scratch.
    std::function<bool()> restart_level;
    /// Toggles a named HUD overlay ("debug", "threat"). Names it does not know
    /// must be reported as unknown by the implementer, not ignored.
    std::function<bool(const std::string&, bool)> set_overlay;

    /// The level editor's document, when one is open. Absent otherwise, in
    /// which case `edit` reports that rather than pretending.
    ///
    /// Exposing the editor through the SAME command language everything else
    /// uses is what keeps the GUI from becoming a second API: the panels are a
    /// typist for LevelDoc, `edit` is a typist for LevelDoc, and neither can
    /// drift from the other. It also makes an editor operation reachable from
    /// --exec, and therefore from --sim-test and screenshot regressions.
    LevelDoc* doc = nullptr;
    /// Re-bakes and re-validates after a document edit. Supplied by app/.
    std::function<void()> doc_changed;
    /// Writes the document. Empty path means its own source path.
    std::function<bool(const std::string& path, bool force, std::string& err)> doc_save;
    /// Re-reads the document from its source path, discarding edits.
    std::function<bool(std::string& err)> doc_revert;

    // --- Tuning config (assets/config/*.json) -------------------------------
    // Supplied by app/, which owns the ConfigStore. Absent in a context that
    // has no config, in which case `config` reports that rather than lying.
    //
    // These are what make a balance tweak a one-liner: `config set
    // towers.macrophage.3.damage 200` reaches the same bytes the JSON loader
    // writes, and `config dump` writes the live values back out so an
    // experiment that worked can be kept.

    /// Reads one dotted field path. False with a message in `out` on a miss.
    std::function<bool(const std::string& path, std::string& out)> config_get;
    /// Writes one dotted field path and re-applies the affected systems.
    std::function<bool(const std::string& path, const std::string& value, std::string& err)>
        config_set;
    /// Re-reads every config file from disk.
    std::function<bool(std::string& err)> config_reload;
    /// Writes the live values back to the config directory.
    std::function<bool(std::string& err)> config_dump;
    /// Every addressable field path, for `config list`.
    std::function<std::vector<std::string>()> config_paths;

    /// Turns the balance bot (game/autoplay) on or off for the current level,
    /// with an optional profile name. Supplied by app/; absent headlessly,
    /// where --autoplay drives the bot directly and needs no console.
    ///
    /// The point of exposing it here is verification: `autoplay on; time 8`
    /// runs the same bot the harness runs, in a window, at eight times speed,
    /// so its play can be watched rather than trusted.
    std::function<bool(bool enable, const std::string& profile, std::string& err)> set_autoplay;
};

/// Outcome of one command. `message` is always populated -- on success it is
/// the human-readable confirmation the panel prints ("spawned 500 virus at
/// (12.0, 66.0)"), because a command whose effect is 3,000 pixels off-screen is
/// otherwise indistinguishable from one that silently did nothing.
struct GymResult {
    bool ok = true;
    std::string message;
};

/// One row of the command table: what `help` prints and what the panel's
/// button palette is built from.
struct GymCommandInfo {
    const char* name = "";
    const char* args = "";     ///< Usage fragment, e.g. "<family> <count> [at <x,y>]".
    const char* help = "";     ///< One line.
};

/// The full command table, in help order. Stable for the process lifetime.
const std::vector<GymCommandInfo>& gym_commands();

/// The token spellings this language accepts for the two enumerations a UI
/// needs to offer as a list. Exposed so a front end can build a dropdown from
/// the same strings the parser matches, instead of keeping a second copy that
/// drifts. Family order is PathogenFamily order; event order is the order
/// `vfx all` fires them in.
const std::vector<const char*>& gym_family_names();
const std::vector<const char*>& gym_vfx_event_names();

/// Runs one command line. Blank lines and lines starting with '#' succeed as
/// no-ops so a pasted script with comments works. Never throws.
GymResult gym_execute(GymContext& ctx, std::string_view line);

/// Runs several newline- or semicolon-separated commands, stopping at the first
/// failure. The returned message is every line's message, newline-joined.
GymResult gym_execute_script(GymContext& ctx, std::string_view text);

} // namespace immune::game
