// app/AutoplayMode.h — the balance harness's entry point. NEW MODULE.
//
// Deliberately not in Modes.h: that header is a frozen contract covering the
// three verification modes, whose stdout formats other tools parse and whose
// exit codes mean pass/fail. This one is neither a test nor a benchmark -- it
// is a data run. It exits 0 whenever it managed to play the level at all,
// because "the bot lost" is a finding to read in the report, not a failure of
// the harness.
//
//   immune --autoplay --level assets/levels/skin_1_breach.json
//          [--profile greedy-cheapest] [--seed N] [--max-ticks N]
//          [--report out.json]
//
// The report schema lives in game/telemetry/RunTelemetry.h. Runs are
// bit-reproducible for a given (level, seed, profile, config hash), all four
// of which the report records in its header.
#pragma once

#include "app/Cli.h"

namespace immune::app {

/// Plays one level with the bot and writes the balance report.
/// Returns 0 on a completed run (won or lost), 1 only if setup failed.
int run_autoplay(const Options& options);

} // namespace immune::app
