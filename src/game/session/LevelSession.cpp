// game/session/LevelSession.cpp — see the header for why this order lives in
// one place. The body below is App::tick_sim's, moved verbatim; every comment
// explaining a sequencing decision came with it and is load-bearing.
#include "game/session/LevelSession.h"

#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/gym/GymCommands.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"

namespace immune::game {

SessionOutcome step_level(const LevelSystems& s, Profiler* profiler) {
    if (s.world == nullptr) return SessionOutcome::InProgress;
    sim::SimWorld& world = *s.world;

    // Before the wave director, so a console-queued spawn and an authored wave
    // spawning into the same spawn point on the same tick resolve in a fixed order.
    if (s.spawns != nullptr) s.spawns->tick(world);
    if (s.waves != nullptr) s.waves->tick(world, world.rng(), kFixedDt);
    world.tick(profiler);
    // Before the win/loss check below reads the snapshot, so an enabled hold
    // actually prevents the loss instead of racing it.
    if (s.toggles != nullptr) s.toggles->apply(world);

    if (s.economy != nullptr) {
        // Passive income only accrues during the round itself (Spawning/
        // Clearing), not during Prep -- the player shouldn't watch ATP tick up
        // while idling in the build phase between rounds.
        const bool in_prep = s.waves != nullptr && s.waves->status().phase == WavePhase::Prep;
        if (!in_prep) s.economy->tick(kFixedDt);
        s.economy->credit_kills(world.last_damage_stats().density_removed);
        if (s.waves != nullptr) s.economy->credit_bounty(s.waves->take_pending_atp_reward());
    }
    if (s.abilities != nullptr) s.abilities->tick(kFixedDt);

    // Win/lose: checked every tick so the transition fires the moment either
    // condition becomes true, not on some later poll. Fail takes priority --
    // an integrity breach on the same tick the last wave clears is still a
    // loss, not a photo-finish win.
    const sim::SimSnapshot snap = world.snapshot();
    if (snap.objective_integrity <= 0.0f) return SessionOutcome::ObjectiveDestroyed;
    // Schema 2's survive-N-seconds objective, when the level authors one.
    // Checked from the sim's own tick counter rather than a wall clock, so it
    // stays deterministic and replays identically.
    if (s.survive_seconds > 0.0f) {
        const f64 elapsed = static_cast<f64>(world.tick_index()) * kFixedDtSeconds;
        if (elapsed >= static_cast<f64>(s.survive_seconds)) return SessionOutcome::Cleared;
        // A survive level is NOT also cleared by exhausting its table: running
        // out of waves early would end it before the clock, which is the
        // opposite of what "survive for two minutes" asks for.
        return SessionOutcome::InProgress;
    }
    if (s.waves != nullptr && s.waves->status().all_waves_complete && snap.chaff_count == 0) {
        return SessionOutcome::Cleared;
    }
    return SessionOutcome::InProgress;
}

} // namespace immune::game
