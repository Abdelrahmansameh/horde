// app/LevelTools.h — the two level-authoring CLI modes. NEW MODULE.
//
// Deliberately NOT in app/Modes.h: that header is the frozen contract for the
// three sim-verification entry points (--bench, --sim-test, --screenshot) whose
// stdout formats other tools parse. These two are content tooling, they touch
// no SimWorld tick, and they should be free to grow without anyone worrying
// about the bench schema.
//
//   run_level_check -> 0 if every level is error-free, 1 otherwise.
//                      Prints a JSON report to stdout; logs go to stderr, so
//                      `immune --level-check assets/levels` is a CI step.
//   run_level_fmt   -> 0 on success. With --check, 1 if any file is not already
//                      canonical (and nothing is written).
//
// Both accept a single .json file or a directory, which is swept for *.json.
#pragma once

namespace immune::app {

struct Options;

int run_level_check(const Options& options);
int run_level_fmt(const Options& options);

} // namespace immune::app
