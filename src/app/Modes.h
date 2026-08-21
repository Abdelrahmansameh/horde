// app/Modes.h — the three non-interactive entry points. FROZEN CONTRACT.
//
// These are the project's verification substrate. Their exit codes and their
// stdout formats are consumed by tools and by every later sub-agent, so treat
// both as a public API.
//
//   run_bench       -> 0 always (unless setup failed); prints the timing JSON.
//   run_sim_test    -> 0 if every assertion passed, 1 otherwise.
//   run_screenshot  -> 0 if the PNG was written, 1 otherwise.
//
// Logging goes to stderr in all three so stdout stays machine-parsable.
#pragma once

#include "app/Cli.h"
#include "config/ConfigStore.h"
#include "core/JobSystem.h"
#include "core/Types.h"
#include "game/config/GameConfig.h"

#include <memory>
#include <string>
#include <vector>

namespace immune::app {

/// A named headless benchmark configuration.
struct BenchScenario {
    std::string name;
    std::string description;
    u32 chaff_count = 0;       ///< Agents spawned at t=0.
    u32 named_count = 0;       ///< Named ECS agents spawned at t=0.
    u32 damage_fields = 0;     ///< Persistent damage fields active.
    usize max_chaff = 16384;
};

/// All registered scenarios, in a stable order.
const std::vector<BenchScenario>& bench_scenarios();
const BenchScenario* find_bench_scenario(const std::string& name);

/// Directory the tuning JSON is read from: --config if given, else
/// assets/config resolved through platform::asset_path.
std::string resolve_config_dir(const Options& options);

/// Writes the live tuning values to --dump-config <dir> as a complete set of
/// JSON files. Exit 0 on success. This is the bootstrap that generates
/// assets/config from the code's own numbers instead of by transcription.
int run_dump_config(const Options& options);

/// Tuning for a headless run, plus the hash that pins it.
///
/// The headless modes are this project's verification substrate, so they must
/// run on the SAME tuning the interactive game does -- otherwise a passing
/// --sim-test proves nothing about what a player sees. Hot reload is never
/// enabled here: the config is a determinism input, and every headless report
/// prints `hash` so a run records exactly which tuning produced it.
///
/// Shared (rather than private to Modes.cpp) because --autoplay needs the same
/// guarantee for the same reason, and a second copy of this would be a second
/// chance for a balance run to be tuned differently from the game.
struct HeadlessConfig {
    config::ConfigStore store;
    game::GameConfig cfg;
    bool ok = false;
    u64 hash = 0;
};

HeadlessConfig load_headless_config(const Options& options);

/// Job system honouring --threads. `--threads 1` is explicitly serial, which
/// is what a run whose output must be bit-reproducible wants.
std::unique_ptr<JobSystem> make_jobs(const Options& options);

int run_bench(const Options& options);
int run_sim_test(const Options& options);
int run_screenshot(const Options& options);
int run_list_scenarios();

} // namespace immune::app
