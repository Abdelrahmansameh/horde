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
//   immune --dump-config <dir>                 Write the live tuning values as JSON.
//   immune --autoplay --level <f.json>         Bot-played balance run, JSON report.
//
// Global options: --seed N, --level <path>, --width N, --height N, --verbose,
//                 --quiet, --threads N, --list-scenarios, --help
//
// --exec "<gym commands>" runs game/gym commands against the world
// before a screenshot's ticks, so any state a human can set up by hand in the
// in-game console is also reachable from a script and from CI.
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
    Autoplay,
    ListScenarios,
    DumpConfig,
    Help,
    Invalid,
};

struct Options {
    Mode mode = Mode::Play;

    std::string scenario;       ///< --bench <scenario> / --scenario <scenario> (screenshot-only)
    std::string script_path;    ///< --sim-test <script.json>
    std::string level;          ///< --screenshot <level> / --level <path>
    std::string out_path = "shot.png";  ///< --out
    /// --towers: place one of every tower type before capturing, so the shot
    /// shows real combat (projectiles + particles) instead of an unopposed
    /// horde. Off by default so existing screenshot regressions keep framing
    /// exactly what they framed before.
    bool place_towers = false;
    /// --tower <name>: with --towers, restrict placement to just this type
    /// (parse_tower_type() name, e.g. "neutrophil"). Empty means every type.
    /// Isolating one tower per shot is what makes a per-tower visual bug
    /// diagnosable instead of guessed at in a crowd of six effects at once.
    std::string tower_filter;
    /// --view-height <world units>: camera framing for --screenshot. Defaults
    /// to the whole level. Needed to inspect per-agent art at all: at full-level
    /// framing a chaff agent covers about six pixels, which is the size the
    /// shader is tuned for but far too small to review.
    f32 view_height = 0.0f;
    /// --focus <x,y>: camera centre for --screenshot. Defaults to level centre.
    Vec2 focus{0.0f, 0.0f};
    bool has_focus = false;

    /// --exec "<gym commands>": semicolon-separated game/gym commands, run
    /// once against the world at startup -- before --screenshot advances any
    /// ticks, and just after the level loads in interactive play (which is what
    /// makes `--exec "autoplay on; time 8"` a way to watch the balance bot). The
    /// point is that a visual bug found by typing into the console can be
    /// reproduced by a command line, and captured in a regression shot, without
    /// anyone first inventing a bespoke CLI flag for it.
    std::string exec;

    /// --config <dir>: directory holding the tuning JSON. Empty means the
    /// default, assets/config, resolved through platform::asset_path. Passing
    /// it explicitly also pins the config: hot reload stays off, so a run
    /// driven from a script cannot be retuned underneath itself.
    std::string config_dir;
    bool config_pinned = false;
    /// --dump-config <dir>: write the live tuning values out as a complete set
    /// of JSON files and exit. This is how assets/config is generated from the
    /// code rather than transcribed by hand.
    std::string dump_config_dir;

    // --- --autoplay (app/AutoplayMode.h) -----------------------------------
    /// --profile <name>: which strategy the bot plays. Empty means the
    /// default, "greedy-cheapest". See game/autoplay/AutoPlayer.h.
    std::string profile;
    /// --report PATH: where the balance report JSON goes. Empty means stdout.
    std::string report_path;
    /// --max-ticks N: give up and report "tick_limit" after this many ticks.
    /// 0 means the harness default (30 simulated minutes).
    u64 max_ticks = 0;

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
