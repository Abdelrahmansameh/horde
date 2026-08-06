#!/usr/bin/env python3
"""tools/bench_report.py — runs every --bench scenario and grades it against
the perf budgets in docs/AGENT_BRIEF.md §5 / DESIGN.md §8.6, so a
content-heavy wave (new levels, new towers, new elites) can't silently push a
subsystem over budget between commits.

Owner: Wave 5D (tests/ + tools/). Deliberately stdlib-only — this project
ships no binary assets and agents cannot rely on `pip install` succeeding in a
sandboxed build environment (see tools/preview_level.py's own docstring for
the same rule).

What it does:
  1. Resolves the immune.exe to run: --exe PATH, else the IMMUNE env var, else
     the default build output path.
  2. Asks it for the scenario list via `--list-scenarios`.
  3. Runs `--bench <scenario> --ticks N --quiet` for each one and parses the
     Profiler JSON (schema: src/core/Profiler.cpp's to_json()).
  4. Grades each scenario's *average* timings against the four budget lines:
       chaff_update + render_submit < 4ms
       ecs_tick                     < 2ms
       spatial_hash                 < 1ms
       frame_total                  < 16.6ms
     (p99 is printed alongside for visibility but does not gate pass/fail —
     the budgets in the brief are stated as steady-state numbers, and a
     single-sample p99 spike is expected noise on a shared machine.)
  5. Prints a pass/fail table to stdout.
  6. Appends one row per scenario to a local history file (default
     tools/bench_history.csv, gitignored — see the --history flag to point
     elsewhere) so numbers are diffable across commits with a plain
     spreadsheet or `git log -p`.

Exit code: 0 if every scenario passes every budget, 1 otherwise (or if the
exe can't be run at all).

Usage:
    python tools/bench_report.py
    python tools/bench_report.py --exe C:\\path\\to\\immune.exe
    python tools/bench_report.py --ticks 1200 --scenario chaff10k --scenario mixed
    python tools/bench_report.py --no-history
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_EXE = Path(os.environ.get("LOCALAPPDATA", "")) / "horde-build" / "windows-release" / "bin" / "immune.exe"

# (bench json key(s), label, budget_ms). "chaff" combines two keys.
BUDGETS = [
    ("chaff", "chaff_update + render_submit", 4.0),
    ("ecs_tick", "ecs_tick", 2.0),
    ("spatial_hash", "spatial_hash", 1.0),
    ("frame_total", "frame_total", 16.6),
]

HISTORY_FIELDS = [
    "timestamp", "git_commit", "scenario", "ticks", "chaff_agents", "named_agents",
    "chaff_update_avg", "render_submit_avg", "chaff_combined_avg", "chaff_combined_p99",
    "ecs_tick_avg", "spatial_hash_avg", "frame_total_avg",
    "chaff_pass", "ecs_pass", "spatial_pass", "frame_pass", "overall_pass",
]


def resolve_exe(cli_exe: str | None) -> Path:
    if cli_exe:
        return Path(cli_exe)
    env_exe = os.environ.get("IMMUNE")
    if env_exe:
        return Path(env_exe)
    return DEFAULT_EXE


def git_commit() -> str:
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True,
                             text=True, timeout=5, cwd=Path(__file__).resolve().parent.parent)
        if out.returncode == 0:
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown"


def run_json(exe: Path, args: list[str]) -> dict | list:
    proc = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=120)
    if proc.returncode != 0:
        raise RuntimeError(f"{exe} {' '.join(args)} exited {proc.returncode}\nstderr:\n{proc.stderr}")
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"{exe} {' '.join(args)} did not print valid JSON on stdout: {e}\n"
                           f"stdout was:\n{proc.stdout}\nstderr was:\n{proc.stderr}") from e


def list_scenarios(exe: Path) -> list[str]:
    scenarios = run_json(exe, ["--list-scenarios"])
    return [s["name"] for s in scenarios]


def grade(bench: dict) -> dict:
    """Returns a flat dict of the numbers and pass/fail flags this tool cares about."""
    t = bench["timings_ms"]
    chaff_avg = t["chaff_update"]["avg"] + t["render_submit"]["avg"]
    chaff_p99 = t["chaff_update"]["p99"] + t["render_submit"]["p99"]
    ecs_avg = t["ecs_tick"]["avg"]
    spatial_avg = t["spatial_hash"]["avg"]
    frame_avg = t["frame_total"]["avg"]

    chaff_pass = chaff_avg < 4.0
    ecs_pass = ecs_avg < 2.0
    spatial_pass = spatial_avg < 1.0
    frame_pass = frame_avg < 16.6

    return {
        "scenario": bench["scenario"],
        "ticks": bench["ticks"],
        "chaff_agents": bench["agents"]["chaff"],
        "named_agents": bench["agents"]["named"],
        "chaff_update_avg": t["chaff_update"]["avg"],
        "render_submit_avg": t["render_submit"]["avg"],
        "chaff_combined_avg": chaff_avg,
        "chaff_combined_p99": chaff_p99,
        "ecs_tick_avg": ecs_avg,
        "spatial_hash_avg": spatial_avg,
        "frame_total_avg": frame_avg,
        "chaff_pass": chaff_pass,
        "ecs_pass": ecs_pass,
        "spatial_pass": spatial_pass,
        "frame_pass": frame_pass,
        "overall_pass": chaff_pass and ecs_pass and spatial_pass and frame_pass,
    }


def print_table(rows: list[dict]) -> None:
    def mark(ok: bool) -> str:
        return "PASS" if ok else "FAIL"

    header = (f"{'scenario':<18} {'chaff+render(ms)':>17} {'budget':>8}  {'ecs(ms)':>8} {'budget':>8}  "
              f"{'spatial(ms)':>11} {'budget':>8}  {'frame(ms)':>10} {'budget':>8}")
    print(header)
    print("-" * len(header))
    for r in rows:
        print(f"{r['scenario']:<18} "
              f"{r['chaff_combined_avg']:>17.3f} {mark(r['chaff_pass']):>8}  "
              f"{r['ecs_tick_avg']:>8.3f} {mark(r['ecs_pass']):>8}  "
              f"{r['spatial_hash_avg']:>11.3f} {mark(r['spatial_pass']):>8}  "
              f"{r['frame_total_avg']:>10.3f} {mark(r['frame_pass']):>8}")
    print()
    failed = [r["scenario"] for r in rows if not r["overall_pass"]]
    if failed:
        print(f"RESULT: FAIL - over budget in: {', '.join(failed)}")
    else:
        print(f"RESULT: PASS - all {len(rows)} scenario(s) within budget (avg timings).")


def append_history(path: Path, rows: list[dict], commit: str) -> None:
    ts = datetime.now(timezone.utc).isoformat(timespec="seconds")
    is_new = not path.exists()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=HISTORY_FIELDS)
        if is_new:
            writer.writeheader()
        for r in rows:
            writer.writerow({
                "timestamp": ts,
                "git_commit": commit,
                "scenario": r["scenario"],
                "ticks": r["ticks"],
                "chaff_agents": r["chaff_agents"],
                "named_agents": r["named_agents"],
                "chaff_update_avg": f"{r['chaff_update_avg']:.6f}",
                "render_submit_avg": f"{r['render_submit_avg']:.6f}",
                "chaff_combined_avg": f"{r['chaff_combined_avg']:.6f}",
                "chaff_combined_p99": f"{r['chaff_combined_p99']:.6f}",
                "ecs_tick_avg": f"{r['ecs_tick_avg']:.6f}",
                "spatial_hash_avg": f"{r['spatial_hash_avg']:.6f}",
                "frame_total_avg": f"{r['frame_total_avg']:.6f}",
                "chaff_pass": r["chaff_pass"],
                "ecs_pass": r["ecs_pass"],
                "spatial_pass": r["spatial_pass"],
                "frame_pass": r["frame_pass"],
                "overall_pass": r["overall_pass"],
            })


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", help="path to immune.exe (else $IMMUNE, else the default build output path)")
    ap.add_argument("--ticks", type=int, default=600, help="ticks per scenario (default 600)")
    ap.add_argument("--seed", type=int, default=None, help="forwarded to --bench --seed if given")
    ap.add_argument("--scenario", action="append", dest="scenarios", default=None,
                     help="run only this scenario (repeatable). Default: every scenario from "
                          "--list-scenarios")
    ap.add_argument("--history", default=str(Path(__file__).resolve().parent / "bench_history.csv"),
                     help="CSV file to append results to (default tools/bench_history.csv)")
    ap.add_argument("--no-history", action="store_true", help="skip writing to the history file")
    ap.add_argument("--json", action="store_true", help="also print the full graded rows as JSON")
    args = ap.parse_args()

    exe = resolve_exe(args.exe)
    if not exe.exists():
        print(f"error: immune executable not found at '{exe}'. Pass --exe PATH, set $IMMUNE, or build "
              f"the project first (tools\\build.bat all windows-release).", file=sys.stderr)
        return 1

    try:
        scenarios = args.scenarios if args.scenarios else list_scenarios(exe)
    except (RuntimeError, subprocess.SubprocessError, OSError) as e:
        print(f"error: could not list scenarios from '{exe}': {e}", file=sys.stderr)
        return 1

    if not scenarios:
        print("error: no scenarios reported by --list-scenarios", file=sys.stderr)
        return 1

    commit = git_commit()
    rows: list[dict] = []
    for name in scenarios:
        bench_args = ["--bench", name, "--ticks", str(args.ticks), "--quiet"]
        if args.seed is not None:
            bench_args += ["--seed", str(args.seed)]
        try:
            bench = run_json(exe, bench_args)
        except (RuntimeError, subprocess.SubprocessError, OSError) as e:
            print(f"error: bench scenario '{name}' failed to run: {e}", file=sys.stderr)
            return 1
        rows.append(grade(bench))

    print(f"immune executable: {exe}")
    print(f"git commit: {commit}    ticks/scenario: {args.ticks}\n")
    print_table(rows)

    if args.json:
        print()
        print(json.dumps(rows, indent=2))

    if not args.no_history:
        history_path = Path(args.history)
        append_history(history_path, rows, commit)
        print(f"\nappended {len(rows)} row(s) to {history_path}")

    return 0 if all(r["overall_pass"] for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
