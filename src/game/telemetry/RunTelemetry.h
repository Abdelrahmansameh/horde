// game/telemetry/RunTelemetry.h — everything one played level is worth
// recording, for balance. NEW MODULE.
//
// RATIONALE
// Balance questions are comparative ("is the Macrophage worth 150 ATP next to
// a Neutrophil?", "which wave is the difficulty cliff?", "is the economy so
// generous that price is not a constraint?"), and none of them can be answered
// from a win/loss bit. This collector turns a run into the numbers those
// questions are actually about, and writes them out as one JSON document.
//
// WHERE THE NUMBERS COME FROM
//   per-tower damage  -> sim::DamageAttribution, fed by both damage paths
//                        (sim/Attribution.h). Nothing else can attribute an
//                        aggregate density field to the cell that cast it.
//   per-family counts -> SimSnapshot's per-family lifetime tallies.
//   spend             -> reported by whoever bought the thing, because the
//                        Economy only knows a total.
//   everything else   -> sampled from the snapshot once per tick.
//
// DERIVED, NOT STORED
// Rates and ratios (ATP per density, uptime, DPS) are computed in to_json()
// from the raw tallies rather than accumulated, so a rounding choice is never
// baked into the record and a new derived metric never needs a re-run.
//
// COST
// One pass over a few dozen attribution entries per tick plus a timeline
// sample every 30 ticks. Nothing here runs inside SimWorld::tick, and a run
// with no collector attached is bit-identical to one with (the sink is the
// only thing that touches the sim, and it only reads).
#pragma once

#include "core/Types.h"
#include "game/level/Level.h"
#include "sim/Attribution.h"

#include <string>
#include <vector>

namespace immune::sim { class SimWorld; }

namespace immune::game {

class Economy;
class TowerSystem;
class WaveDirector;

/// What a tower did for what it cost. One per placement, retained after a sell
/// so a run's record is complete rather than only describing what survived.
struct TowerTelemetry {
    EntityId id{};
    TowerType type = TowerType::Neutrophil;
    u8 tier = 1;
    u8 peak_tier = 1;
    Vec2 position{0.0f, 0.0f};
    std::string lane;            ///< Lane the site sits in, or "" if unowned.
    u64 built_tick = 0;
    u64 removed_tick = 0;        ///< 0 while still standing.
    /// Tick each tier was reached; index 0 is the build. 0 means "not reached".
    u64 tier_tick[3] = {};
    u32 invested_atp = 0;        ///< Build + every upgrade, ignoring refunds.
    u32 refunded_atp = 0;

    f64 density_removed[kFamilyCount] = {};
    u32 chaff_killed[kFamilyCount] = {};
    f64 named_damage = 0.0;
    u32 named_killed = 0;
    u32 active_ticks = 0;        ///< Ticks it removed anything at all.
    u32 alive_ticks = 0;

    f64 total_density_removed() const {
        f64 sum = 0.0;
        for (u32 f = 0; f < kFamilyCount; ++f) sum += density_removed[f];
        return sum;
    }
};

/// One wave, start to finish. `*_at_start` fields are captured on the tick the
/// wave's Prep phase begins, so a wave's cost is a difference, not a guess.
struct WaveTelemetry {
    u32 index = 0;
    std::string name;
    std::string modifier;
    u64 start_tick = 0;
    u64 end_tick = 0;
    u32 prep_ticks = 0;
    u32 spawning_ticks = 0;
    u32 clearing_ticks = 0;

    f32 integrity_at_start = 0.0f;
    f32 integrity_at_end = 0.0f;
    u32 atp_at_start = 0;
    u32 atp_at_end = 0;
    u32 atp_earned = 0;
    u32 atp_spent = 0;
    u32 towers_at_start = 0;
    u32 towers_at_end = 0;

    u64 spawned[kFamilyCount] = {};
    u64 killed[kFamilyCount] = {};
    u64 leaked[kFamilyCount] = {};
    u64 peak_chaff = 0;
    f32 peak_density = 0.0f;
};

/// One periodic sample of the whole run's state. The shape a plot wants.
struct TimelineSample {
    u64 tick = 0;
    u32 wave_index = 0;
    u8 phase = 0;
    u32 atp = 0;
    u32 total_earned = 0;
    u32 total_spent = 0;
    u64 chaff_count = 0;
    f32 total_density = 0.0f;
    f32 integrity = 0.0f;
    u32 towers = 0;
};

/// How the run ended, as recorded in the report header.
enum class RunResult : u8 { InProgress = 0, Cleared, Lost, TickLimit };

class RunTelemetry {
public:
    /// Binds to a level and attaches the attribution sinks to `world`. Call
    /// once, after the level is instantiated and before the first tick.
    void begin(sim::SimWorld& world, const LevelDef& level, const LaneOwnershipMap& lanes);

    /// Detaches the sinks. Safe to call twice; called by the destructor.
    void end(sim::SimWorld& world);

    // --- Purchases. Reported by the buyer: Economy only knows a total. ------
    void on_tower_placed(EntityId id, TowerType type, Vec2 position, u32 cost, u64 tick);
    void on_tower_upgraded(EntityId id, u8 new_tier, u32 cost, u64 tick);
    void on_tower_sold(EntityId id, u32 refund, u64 tick);

    /// One sample. Call once per tick, after step_level().
    void sample(const sim::SimWorld& world, const WaveDirector& waves, const Economy& economy);

    /// Records the terminal state. Call once, when the run stops.
    void finish(RunResult result) { result_ = result; }

    RunResult result() const { return result_; }
    const std::vector<TowerTelemetry>& towers() const { return towers_; }
    const std::vector<WaveTelemetry>& waves() const { return waves_; }

    /// The whole report, including everything derived. `header` fields the
    /// collector cannot know (level path, seed, profile, config hash) are
    /// passed in rather than guessed.
    struct ReportHeader {
        std::string level_path;
        std::string level_name;
        std::string profile;
        u64 seed = 0;
        std::string config_hash;
        std::string config_dir;
    };
    std::string to_json(const ReportHeader& header, int indent = 2) const;

private:
    TowerTelemetry* find(EntityId id);
    void close_wave(u64 tick);

    sim::DamageAttribution attribution_;
    std::vector<TowerTelemetry> towers_;
    std::vector<WaveTelemetry> waves_;
    std::vector<TimelineSample> timeline_;
    const LaneOwnershipMap* lanes_ = nullptr;

    RunResult result_ = RunResult::InProgress;
    u64 last_tick_ = 0;
    u32 timeline_interval_ = 30;

    // Running state used to turn per-tick samples into per-wave differences.
    bool wave_open_ = false;
    u32 open_wave_index_ = 0;
    u8 last_phase_ = 0;
    u64 spawned_at_wave_start_[kFamilyCount] = {};
    u64 killed_at_wave_start_[kFamilyCount] = {};
    u64 leaked_at_wave_start_[kFamilyCount] = {};
    u32 earned_at_wave_start_ = 0;
    u32 spent_at_wave_start_ = 0;

    /// Latest per-family lifetime tallies, kept so to_json() can report the
    /// run's totals without being handed a world it has no other use for.
    u64 final_spawned_[kFamilyCount] = {};
    u64 final_killed_[kFamilyCount] = {};
    u64 final_leaked_[kFamilyCount] = {};
    u64 final_despawned_[kFamilyCount] = {};
    f32 final_integrity_ = 0.0f;
    u32 final_atp_ = 0;
    u32 total_earned_ = 0;
    u32 total_spent_ = 0;

    /// ATP-seconds the player sat on. A large number against a lost run means
    /// the bot could not spend fast enough; against a won run it means the
    /// economy is not a constraint. Either reading is a balance finding.
    f64 idle_atp_seconds_ = 0.0;
    f64 peak_atp_ = 0.0;
};

/// "cleared" | "lost" | "tick_limit" | "in_progress".
const char* run_result_name(RunResult r);

} // namespace immune::game
