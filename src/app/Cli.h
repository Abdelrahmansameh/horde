// app/Cli.h — command-line parsing. FROZEN CONTRACT.
//
// The three headless modes are how every later sub-agent proves its work
// without a human looking at a screen. Their command lines and their output
// formats are part of the project's contract; do not change them casually.
//
//   immune                                    Interactive play.
//   immune --bench <scenario> --ticks N        Headless sim, JSON timings -> stdout.
//   immune --sim-test <script.json>            Scripted scenario, exit 0 pass / 1 fail.
//   immune --screenshot <level> --tick N --out <file.png>
//
// Global options: --seed N, --level <path>, --width N, --height N, --verbose,
//                 --quiet, --threads N, --list-scenarios, --help
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::app {

enum class Mode : u8 {
    Play = 0,
    Bench,
    SimTest,
    Screenshot,
    ListScenarios,
    Help,
    Invalid,
};

struct Options {
    Mode mode = Mode::Play;

    std::string scenario;       ///< --bench <scenario> / --scenario <scenario> (screenshot-only)
    std::string script_path;    ///< --sim-test <script.json>
    std::string level;          ///< --screenshot <level> / --level <path>
    std::string out_path = "shot.png";  ///< --out

    u64 ticks = 600;            ///< --ticks / --tick
    u64 seed = 0x1234'5678'9abc'def0ULL;
    i32 width = 1600;
    i32 height = 900;
    /// 0 = auto (hardware_concurrency - 1). Forced to 0 workers with --threads 1.
    i32 threads = 0;

    bool verbose = false;
    bool quiet = false;
    bool vsync = true;

    std::string error;          ///< Non-empty when mode == Invalid.
};

/// Parses argv. Never throws; malformed input yields Mode::Invalid + error.
Options parse_args(int argc, char** argv);

/// Usage text written to stdout by --help and to stderr on a parse error.
const char* usage_text();

} // namespace immune::app
