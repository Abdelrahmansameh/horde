// game/session/LevelSession.h — the one authoritative "advance a level by one
// tick" step. NEW MODULE.
//
// RATIONALE
// The order in which a level's systems run (queued spawns, wave director, sim,
// gym toggles, economy, abilities, win/loss) is gameplay, not plumbing: move
// the economy credit before the sim tick and towers pay for kills a frame
// late; check the loss condition before the toggles apply and an invulnerable
// objective still dies. That order lived in exactly one place, App::tick_sim,
// and was therefore unavailable to anything without a window.
//
// The balance harness (game/autoplay) has to run the *same* game, not a
// plausible headless imitation of it -- a bot playing a subtly different tick
// order produces numbers that describe nothing. So the order moved here, App
// calls it, and the harness calls it. There is no second copy to drift.
//
// WHAT IT IS NOT
// Not an owner. Every pointer in LevelSystems belongs to the caller (App holds
// its systems as members; the harness holds its own), which is what lets App
// keep handing the same objects to the HUD, the renderer and the gym panel
// without indirection. Not a state machine either: it reports the outcome and
// leaves acting on it to the caller, because app/GameState.h's transitions are
// an app-layer concern and game/ must not depend on app/.
#pragma once

#include "core/Types.h"

namespace immune { class Profiler; }
namespace immune::sim { class SimWorld; }

namespace immune::game {

class ActiveAbilitySystem;
class Economy;
class EnemyRoster;
class GymSpawnQueue;
class TowerSystem;
class WaveDirector;
struct GymToggles;

/// Everything one tick of a level touches. All non-owning; `world` is the only
/// member that must be non-null.
///
/// The optional members are genuinely optional: a test that wants to watch a
/// wave table spawn does not need an Economy, and step_level() simply skips the
/// steps whose subsystem is absent rather than crashing or inventing one.
struct LevelSystems {
    sim::SimWorld* world = nullptr;
    TowerSystem* towers = nullptr;
    EnemyRoster* enemies = nullptr;
    WaveDirector* waves = nullptr;
    Economy* economy = nullptr;
    ActiveAbilitySystem* abilities = nullptr;
    GymSpawnQueue* spawns = nullptr;
    GymToggles* toggles = nullptr;

    /// Schema-2 alternative win condition: clear at this many SECONDS survived
    /// rather than by finishing the wave table. 0 -- the default, and every
    /// level that authors none -- keeps the wave-clear rule.
    ///
    /// Passed per-step rather than read off a level, because this layer owns
    /// nothing: every pointer above belongs to the caller, and LevelDef does
    /// not survive instantiate().
    f32 survive_seconds = 0.0f;
};

/// Why a level stopped, or that it has not. Mirrors app::LevelOutcome's two
/// terminal cases; the app layer maps it onto its own state machine, and
/// `Aborted` has no meaning here because only a front end can abort.
enum class SessionOutcome : u8 { InProgress = 0, Cleared, ObjectiveDestroyed };

/// Advances the level exactly one fixed 60 Hz step and reports the resulting
/// win/loss state. `profiler` may be null.
///
/// Fail takes priority over win: an integrity breach on the same tick the last
/// wave clears is a loss, not a photo-finish win.
SessionOutcome step_level(const LevelSystems& systems, Profiler* profiler = nullptr);

} // namespace immune::game
