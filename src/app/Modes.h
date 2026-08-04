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
#include "core/Types.h"

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

int run_bench(const Options& options);
int run_sim_test(const Options& options);
int run_screenshot(const Options& options);
int run_list_scenarios();

} // namespace immune::app
