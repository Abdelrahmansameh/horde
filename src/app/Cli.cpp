#include "app/Cli.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace immune::app {
namespace {

bool next_value(int argc, char** argv, int& i, std::string_view flag, std::string& out,
                std::string& error) {
    if (i + 1 >= argc) {
        error = std::string("missing value for ") + std::string(flag);
        return false;
    }
    out = argv[++i];
    return true;
}

bool next_u64(int argc, char** argv, int& i, std::string_view flag, u64& out,
              std::string& error) {
    std::string s;
    if (!next_value(argc, argv, i, flag, s, error)) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (end == s.c_str() || (end && *end != '\0')) {
        error = std::string("expected an integer after ") + std::string(flag) + ", got '" + s + "'";
        return false;
    }
    out = static_cast<u64>(v);
    return true;
}

} // namespace

const char* usage_text() {
    return
"IMMUNE - tower defense with a 10,000-agent horde simulation\n"
"\n"
"USAGE\n"
"  immune                                              play interactively\n"
"  immune --bench <scenario> [--ticks N]               headless sim, JSON timings on stdout\n"
"  immune --sim-test <script.json>                     scripted run, exit 0 pass / 1 fail\n"
"  immune --screenshot <level> --tick N --out <f.png>  deterministic frame capture\n"
"      --towers                                        place every tower type first,\n"
"                                                      so the shot shows real combat\n"
"  immune --list-scenarios                             print available bench scenarios\n"
"\n"
"OPTIONS\n"
"  --ticks N        ticks to simulate (bench; default 600)\n"
"  --tick N         tick to advance to before capture (screenshot; default 0)\n"
"  --out PATH       output PNG path (screenshot; default shot.png)\n"
"  --level PATH     level JSON to load (default: built-in test level)\n"
"  --seed N         PRNG seed; identical seeds give identical runs\n"
"  --width N        framebuffer width (default 1600)\n"
"  --height N       framebuffer height (default 900)\n"
"  --threads N      worker threads; 1 forces a fully serial run\n"
"  --no-vsync       disable vsync in interactive mode\n"
"  --verbose        log at debug level\n"
"  --quiet          suppress all logging (stdout stays pure JSON)\n"
"  --help           this text\n";
}

Options parse_args(int argc, char** argv) {
    Options o;
    bool tick_set = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        std::string s;
        u64 v = 0;

        if (a == "--help" || a == "-h") {
            o.mode = Mode::Help;
            return o;
        } else if (a == "--list-scenarios") {
            o.mode = Mode::ListScenarios;
        } else if (a == "--bench") {
            if (!next_value(argc, argv, i, a, o.scenario, o.error)) break;
            o.mode = Mode::Bench;
        } else if (a == "--sim-test") {
            if (!next_value(argc, argv, i, a, o.script_path, o.error)) break;
            o.mode = Mode::SimTest;
        } else if (a == "--screenshot") {
            if (!next_value(argc, argv, i, a, o.level, o.error)) break;
            o.mode = Mode::Screenshot;
            if (!tick_set) o.ticks = 0;   // screenshots default to tick 0
        } else if (a == "--ticks" || a == "--tick") {
            if (!next_u64(argc, argv, i, a, o.ticks, o.error)) break;
            tick_set = true;
        } else if (a == "--out" || a == "-o") {
            if (!next_value(argc, argv, i, a, o.out_path, o.error)) break;
        } else if (a == "--towers") {
            o.place_towers = true;
        } else if (a == "--level") {
            if (!next_value(argc, argv, i, a, o.level, o.error)) break;
        } else if (a == "--scenario") {
            // Unlike --bench, does not force Mode::Bench — lets --screenshot
            // populate agents from a bench_scenarios() entry before capture.
            if (!next_value(argc, argv, i, a, o.scenario, o.error)) break;
        } else if (a == "--seed") {
            if (!next_u64(argc, argv, i, a, o.seed, o.error)) break;
        } else if (a == "--width") {
            if (!next_u64(argc, argv, i, a, v, o.error)) break;
            o.width = static_cast<i32>(v);
        } else if (a == "--height") {
            if (!next_u64(argc, argv, i, a, v, o.error)) break;
            o.height = static_cast<i32>(v);
        } else if (a == "--threads") {
            if (!next_u64(argc, argv, i, a, v, o.error)) break;
            o.threads = static_cast<i32>(v);
        } else if (a == "--no-vsync") {
            o.vsync = false;
        } else if (a == "--verbose") {
            o.verbose = true;
        } else if (a == "--quiet") {
            o.quiet = true;
        } else {
            o.error = "unknown argument: " + std::string(a);
            break;
        }
    }

    if (!o.error.empty()) {
        o.mode = Mode::Invalid;
        return o;
    }

    if (o.mode == Mode::Bench && o.scenario.empty()) {
        o.mode = Mode::Invalid;
        o.error = "--bench requires a scenario name";
    }
    if (o.mode == Mode::Screenshot && o.out_path.empty()) {
        o.mode = Mode::Invalid;
        o.error = "--screenshot requires --out <file.png>";
    }
    if (o.width <= 0 || o.height <= 0) {
        o.mode = Mode::Invalid;
        o.error = "--width/--height must be positive";
    }
    return o;
}

} // namespace immune::app
