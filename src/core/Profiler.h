// core/Profiler.h — per-subsystem frame timing capture feeding `--bench` JSON.
// FROZEN CONTRACT.
//
// Every subsystem that appears in the perf budget (DESIGN.md §8.6) records one
// sample per tick under a fixed key. `--bench` reduces the collected samples to
// avg/p50/p99 in milliseconds and prints them as JSON on stdout.
//
// Adding a key is a contract change: the bench JSON schema is what later waves
// are graded against. Request new keys from the orchestrator.
#pragma once

#include "core/Clock.h"
#include "core/Types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace immune {

/// Canonical profiler keys. These exact strings appear in the bench JSON.
namespace prof_key {
inline constexpr const char* kChaffUpdate   = "chaff_update";
inline constexpr const char* kSpatialHash   = "spatial_hash";
inline constexpr const char* kSquadUpdate   = "squad_update";
/// The pathogen-vs-friendly pass (sim/hostile). An extra, like squad_update:
/// emitted after the five canonical keys, never in their place.
inline constexpr const char* kHostile       = "hostile_update";
/// Burrowing and slither (sim/burrow). Another extra, same footing.
inline constexpr const char* kBurrow        = "burrow_update";
inline constexpr const char* kEcsTick       = "ecs_tick";
inline constexpr const char* kRenderSubmit  = "render_submit";
inline constexpr const char* kFrameTotal    = "frame_total";
} // namespace prof_key

struct TimingStats {
    f64 avg_ms = 0.0;
    f64 p50_ms = 0.0;
    f64 p99_ms = 0.0;
    f64 min_ms = 0.0;
    f64 max_ms = 0.0;
    u64 samples = 0;
};

class Profiler {
public:
    /// Reserves sample storage so recording never allocates mid-run.
    void reserve(usize expected_samples);

    /// Records one millisecond sample for `key`.
    void record(const char* key, f64 ms);

    /// Reduces every key to avg/p50/p99. Sorting happens here, not in record().
    std::unordered_map<std::string, TimingStats> summarize() const;

    /// Emits the canonical `--bench` JSON document to a string.
    /// Shape (stable, machine-parsed by tools/bench_report.py):
    /// {
    ///   "scenario": "...", "ticks": 600, "seed": 1234,
    ///   "agents": {"chaff": 0, "named": 0},
    ///   "timings_ms": {
    ///     "chaff_update": {"avg":..,"p50":..,"p99":..,"min":..,"max":..,"samples":..},
    ///     ... one object per key ...
    ///   }
    /// }
    std::string to_json(const std::string& scenario, u64 ticks, u64 seed,
                        u64 chaff_count, u64 named_count) const;

    void clear();

    /// Scoped RAII sample. Prefer IMMUNE_PROFILE_SCOPE.
    class Scope {
    public:
        Scope(Profiler& p, const char* key) : prof_(p), key_(key) {}
        ~Scope() { prof_.record(key_, clock_.elapsed_ms()); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Profiler& prof_;
        const char* key_;
        WallClock clock_{};
    };

private:
    std::unordered_map<std::string, std::vector<f64>> samples_;
};

} // namespace immune

#define IMMUNE_PROFILE_CONCAT2(a, b) a##b
#define IMMUNE_PROFILE_CONCAT(a, b) IMMUNE_PROFILE_CONCAT2(a, b)
#define IMMUNE_PROFILE_SCOPE(profiler, key) \
    ::immune::Profiler::Scope IMMUNE_PROFILE_CONCAT(immune_prof_, __LINE__)((profiler), (key))
