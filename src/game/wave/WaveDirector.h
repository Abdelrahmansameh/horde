// game/wave/WaveDirector.h — wave composition and spawn scheduling.
// FROZEN CONTRACT. Owner: Wave 3A.
//
// The director is a pure function of (tick, wave table, RNG). It never reads
// wall-clock time and never spawns from a background thread, so a wave sequence
// replays identically under --sim-test.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune { class Rng; }
namespace immune::sim { class SimWorld; }

namespace immune::game {

/// One spawn instruction inside a wave.
struct SpawnEntry {
    PathogenFamily family = PathogenFamily::Virus;
    u16 elite_id = 0;         ///< 0 = chaff; non-zero spawns a named elite.
    u32 count = 0;
    f32 start_time = 0.0f;    ///< Seconds after wave start.
    f32 duration = 1.0f;      ///< Spread the count evenly over this window.
    std::string portal_id;    ///< Empty = any/first portal.
};

/// Curveball modifiers applied to a whole wave (DESIGN.md §6 allergen).
enum class WaveModifier : u8 {
    None = 0,
    AllergenOverreaction,  ///< Friendly-fire field; risk/reward.
    Fever,                 ///< Global tower fire-rate up, integrity drain.
    Swarm,                 ///< Counts multiplied, densities reduced.
};

struct WaveDef {
    u32 index = 0;
    std::string name;
    std::vector<SpawnEntry> spawns;
    WaveModifier modifier = WaveModifier::None;
    f32 prep_time = 20.0f;   ///< Seconds of build time before the wave starts.
    u32 atp_reward = 0;
};

enum class WavePhase : u8 { Prep = 0, Spawning = 1, Clearing = 2, Complete = 3 };

struct WaveStatus {
    u32 wave_index = 0;
    WavePhase phase = WavePhase::Prep;
    f32 phase_time_remaining = 0.0f;
    u32 remaining_to_spawn = 0;
    bool all_waves_complete = false;
};

class WaveDirector {
public:
    void set_waves(std::vector<WaveDef> waves);
    const std::vector<WaveDef>& waves() const { return waves_; }

    /// Generates a wave table for a region and difficulty index. Used by
    /// endless mode and by --bench scenarios that need pressure without content.
    static std::vector<WaveDef> generate(const std::string& region, u32 wave_count, Rng& rng);

    void start(sim::SimWorld& world);

    /// One fixed step. Spawns whatever this tick owes and advances the phase.
    void tick(sim::SimWorld& world, Rng& rng, f32 dt);

    /// Skips the remaining prep time (player pressed "start wave early").
    void request_early_start();

    WaveStatus status() const { return status_; }

    /// Preview of the next wave's composition, for the HUD.
    const WaveDef* next_wave() const;

    /// Returns and clears whatever WaveDef::atp_reward has accrued from waves
    /// that finished Clearing since the last call, so the caller (app/, which
    /// owns the Economy reference this class deliberately doesn't have) can
    /// credit it. Poll once per tick; 0 most ticks. Orchestrator amendment,
    /// not Wave 3A's original stub -- see WaveDirector.cpp's tick() for where
    /// it accrues.
    u32 take_pending_atp_reward();

private:
    std::vector<WaveDef> waves_;
    WaveStatus status_{};
    f32 wave_time_ = 0.0f;
    bool early_start_requested_ = false;
    /// Per-SpawnEntry running count for the current wave's Spawning phase,
    /// sized to waves_[status_.wave_index].spawns and reset on entry.
    std::vector<u32> spawned_so_far_;
    /// Seconds spent in the current wave's Clearing phase, so a few
    /// unreachable/leaked stragglers can never stall the sequence forever.
    f32 clearing_elapsed_ = 0.0f;
    /// Accrued by tick() when a wave finishes Clearing; drained by
    /// take_pending_atp_reward().
    u32 pending_atp_reward_ = 0;
};

} // namespace immune::game
