#!/usr/bin/env python3
"""tools/balance_sweep.py — plays every level with every strategy across
several seeds and turns the pile of reports into something a balance pass can
read in one sitting.

One --autoplay run answers "did the bot survive this level with this
strategy". That is a sample, not a measurement: a bot that wins once on seed 7
tells you nothing about whether the Macrophage is overpriced. A sweep does,
because the comparisons that matter are all BETWEEN runs --

  * one strategy winning where five lose  -> a dominant strategy;
  * every strategy winning comfortably    -> the level is not asking anything;
  * a tower nobody ever buys              -> its price or its power is wrong;
  * one wave costing most of the integrity across every run -> a difficulty
    cliff that is the wave's fault, not the player's.

Deliberately stdlib-only, same rule as tools/bench_report.py: agents cannot
rely on `pip install` succeeding in a sandboxed build.

Outputs, into --out (default tools/balance/):
    runs/<level>__<profile>__seed<N>.json   the raw per-run reports
    summary.csv                             one row per run, for a spreadsheet
    summary.md                              the digest described above

Exit code: 0 if every run completed (won OR lost -- a loss is data, not a
failure), 1 if any run could not be executed at all.

Usage:
    python tools/balance_sweep.py
    python tools/balance_sweep.py --seeds 5 --profiles all
    python tools/balance_sweep.py --levels assets/levels/skin_1_breach.json
    python tools/balance_sweep.py --exe C:\\path\\to\\immune.exe --jobs 8
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = (Path(os.environ.get("LOCALAPPDATA", "")) / "horde-build" / "windows-release" /
               "bin" / "immune.exe")

TOWERS = ["neutrophil", "macrophage", "interferon", "cytotoxic_t", "goblet_cell", "nk_cell"]
CORE_PROFILES = ["greedy-cheapest", "spread-coverage", "save-for-tier3"]
SINGLE_PROFILES = [f"single-type:{t}" for t in TOWERS]
ALL_PROFILES = CORE_PROFILES + SINGLE_PROFILES

# Levels that exist to exercise the engine rather than to be played. Sweeping
# them produces rows that mean nothing and drown the ones that do.
SKIP_LEVELS = {"gym", "lane_schema_test", "capillary_test", "floodplain_max_horde"}

SUMMARY_FIELDS = [
    "level", "profile", "seed", "result", "seconds", "waves_reached",
    "final_integrity", "spawned", "killed", "leaked", "leak_rate",
    "atp_earned", "atp_spent", "mean_banked_atp", "towers_built",
]


def resolve_exe(cli_exe: str | None) -> Path:
    if cli_exe:
        return Path(cli_exe)
    env_exe = os.environ.get("IMMUNE")
    return Path(env_exe) if env_exe else DEFAULT_EXE


def discover_levels(args_levels: list[str]) -> list[Path]:
    if args_levels:
        out: list[Path] = []
        for entry in args_levels:
            p = Path(entry)
            if p.is_dir():
                out.extend(sorted(p.glob("*.json")))
            else:
                out.append(p)
        return out
    return [p for p in sorted((ROOT / "assets" / "levels").glob("*.json"))
            if p.stem not in SKIP_LEVELS]


def run_one(exe: Path, level: Path, profile: str, seed: int, out_dir: Path,
            max_ticks: int) -> tuple[str, dict | None, str]:
    """Runs one game. Returns (label, report, error)."""
    safe_profile = profile.replace(":", "_")
    label = f"{level.stem}__{safe_profile}__seed{seed}"
    report_path = out_dir / "runs" / f"{label}.json"
    cmd = [str(exe), "--autoplay", "--level", str(level), "--profile", profile,
           "--seed", str(seed), "--report", str(report_path), "--threads", "1", "--quiet"]
    if max_ticks:
        cmd += ["--max-ticks", str(max_ticks)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, timeout=900)
    except (OSError, subprocess.SubprocessError) as exc:
        return label, None, str(exc)
    if proc.returncode != 0:
        return label, None, (proc.stderr or "").strip()[-400:] or f"exit {proc.returncode}"
    try:
        return label, json.loads(report_path.read_text(encoding="utf-8")), ""
    except (OSError, json.JSONDecodeError) as exc:
        return label, None, f"unreadable report: {exc}"


def summary_row(level: Path, profile: str, seed: int, report: dict) -> dict:
    totals = report.get("totals", {})
    economy = report.get("economy", {})
    return {
        "level": level.stem,
        "profile": profile,
        "seed": seed,
        "result": report.get("result", "?"),
        "seconds": round(report.get("seconds", 0.0), 1),
        "waves_reached": report.get("waves_reached", 0),
        "final_integrity": round(totals.get("final_integrity", 0.0), 1),
        "spawned": totals.get("spawned", 0),
        "killed": totals.get("killed", 0),
        "leaked": totals.get("leaked", 0),
        "leak_rate": round(totals.get("leak_rate") or 0.0, 4),
        "atp_earned": totals.get("atp_earned", 0),
        "atp_spent": totals.get("atp_spent", 0),
        "mean_banked_atp": round(economy.get("mean_banked_atp") or 0.0, 1),
        "towers_built": len(report.get("towers", [])),
    }


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def write_digest(path: Path, rows: list[dict], reports: list[tuple[str, str, dict]]) -> None:
    """`reports` is [(level, profile, report)]."""
    lines: list[str] = ["# Balance sweep", ""]
    lines.append(f"{len(rows)} runs across {len({r['level'] for r in rows})} levels "
                 f"and {len({r['profile'] for r in rows})} strategies.")
    lines.append("")

    # --- Per level: how hard is it, and does any one strategy dominate?
    lines += ["## Levels", "",
              "| level | win rate | mean waves | mean final integrity | mean leak rate | "
              "strategies that won |", "|---|---|---|---|---|---|"]
    for level in sorted({r["level"] for r in rows}):
        lr = [r for r in rows if r["level"] == level]
        wins = [r for r in lr if r["result"] == "cleared"]
        winners = sorted({r["profile"] for r in wins})
        lines.append(
            f"| {level} | {len(wins)}/{len(lr)} | {mean([r['waves_reached'] for r in lr]):.1f} | "
            f"{mean([r['final_integrity'] for r in lr]):.1f} | "
            f"{mean([r['leak_rate'] for r in lr]):.3f} | "
            f"{', '.join(winners) if winners else '—'} |")
    lines.append("")

    # --- Per strategy: is one of them free money?
    lines += ["## Strategies", "",
              "| strategy | win rate | mean final integrity | mean banked ATP |",
              "|---|---|---|---|"]
    for profile in sorted({r["profile"] for r in rows}):
        pr = [r for r in rows if r["profile"] == profile]
        wins = sum(1 for r in pr if r["result"] == "cleared")
        lines.append(f"| {profile} | {wins}/{len(pr)} | "
                     f"{mean([r['final_integrity'] for r in pr]):.1f} | "
                     f"{mean([r['mean_banked_atp'] for r in pr]):.0f} |")
    lines.append("")

    # --- The tower ranking. ATP per density is the whole point of the harness:
    # it is the one number that compares a Neutrophil to a Macrophage without
    # first asking how many of each got built.
    agg: dict[str, dict[str, float]] = {}
    for _level, _profile, report in reports:
        for entry in report.get("towers_by_type", []):
            a = agg.setdefault(entry["type"], {"atp": 0.0, "density": 0.0, "kills": 0.0,
                                               "instances": 0.0, "uptime": 0.0, "n": 0.0})
            a["atp"] += entry.get("invested_atp", 0.0)
            a["density"] += entry.get("density_removed", 0.0)
            a["kills"] += entry.get("chaff_killed", 0)
            a["instances"] += entry.get("instances", 0)
            if entry.get("uptime") is not None:
                a["uptime"] += entry["uptime"]
                a["n"] += 1
    lines += ["## Towers", "",
              "Lower ATP-per-density is better value. A tower with zero instances was never "
              "worth buying to any strategy on any level, which is a pricing finding on its own.",
              "",
              "| tower | instances | ATP invested | density removed | ATP / density | kills | "
              "mean uptime |", "|---|---|---|---|---|---|---|"]
    ranked = sorted(agg.items(),
                    key=lambda kv: (kv[1]["atp"] / kv[1]["density"]) if kv[1]["density"] > 0
                    else float("inf"))
    for tower, a in ranked:
        value = f"{a['atp'] / a['density']:.2f}" if a["density"] > 0 else "never fired"
        uptime = f"{a['uptime'] / a['n']:.0%}" if a["n"] else "—"
        lines.append(f"| {tower} | {a['instances']:.0f} | {a['atp']:.0f} | {a['density']:.0f} | "
                     f"{value} | {a['kills']:.0f} | {uptime} |")
    lines.append("")

    # --- Difficulty cliffs: waves whose integrity cost is an outlier for their
    # level, averaged over every run that reached them.
    cliffs: dict[tuple[str, int], list[float]] = {}
    names: dict[tuple[str, int], str] = {}
    for level, _profile, report in reports:
        for wave in report.get("waves", []):
            key = (level, wave.get("index", 0))
            cliffs.setdefault(key, []).append(wave.get("integrity_cost", 0.0))
            names[key] = wave.get("name", "")
    flagged = []
    for level in {k[0] for k in cliffs}:
        level_waves = {k: v for k, v in cliffs.items() if k[0] == level}
        costs = [mean(v) for v in level_waves.values()]
        if len(costs) < 2:
            continue
        threshold = mean(costs) + (statistics.pstdev(costs) or 0.0)
        for key, values in sorted(level_waves.items()):
            avg = mean(values)
            if avg > threshold and avg > 0.0:
                flagged.append((level, key[1], names[key], avg, len(values)))
    lines += ["## Difficulty spikes", "",
              "Waves costing more integrity than one standard deviation above their own level's "
              "mean, averaged over the runs that reached them.", ""]
    if flagged:
        lines += ["| level | wave | name | mean integrity cost | runs |", "|---|---|---|---|---|"]
        for level, index, name, avg, n in sorted(flagged, key=lambda f: -f[3]):
            lines.append(f"| {level} | {index} | {name} | {avg:.1f} | {n} |")
    else:
        lines.append("None — no wave stands out against its own level.")
    lines.append("")

    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", help="path to immune.exe")
    ap.add_argument("--levels", nargs="*", default=[],
                    help="level files or directories (default: assets/levels)")
    ap.add_argument("--profiles", default="core",
                    help="'core', 'all', 'single', or a comma-separated list")
    ap.add_argument("--seeds", type=int, default=3, help="seeds per (level, profile)")
    ap.add_argument("--max-ticks", type=int, default=0, help="0 = the harness default")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    ap.add_argument("--out", default=str(ROOT / "tools" / "balance"))
    args = ap.parse_args()

    exe = resolve_exe(args.exe)
    if not exe.is_file():
        print(f"error: no executable at {exe} (pass --exe or set IMMUNE)", file=sys.stderr)
        return 1

    if args.profiles == "core":
        profiles = CORE_PROFILES
    elif args.profiles == "all":
        profiles = ALL_PROFILES
    elif args.profiles == "single":
        profiles = SINGLE_PROFILES
    else:
        profiles = [p.strip() for p in args.profiles.split(",") if p.strip()]

    levels = discover_levels(args.levels)
    if not levels:
        print("error: no levels to sweep", file=sys.stderr)
        return 1

    out_dir = Path(args.out)
    (out_dir / "runs").mkdir(parents=True, exist_ok=True)

    jobs = [(level, profile, seed)
            for level in levels for profile in profiles
            for seed in range(1, args.seeds + 1)]
    print(f"{len(jobs)} runs: {len(levels)} levels x {len(profiles)} strategies x "
          f"{args.seeds} seeds, {args.jobs} at a time")

    rows: list[dict] = []
    reports: list[tuple[str, str, dict]] = []
    failures = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_one, exe, level, profile, seed, out_dir, args.max_ticks)
                   for level, profile, seed in jobs]
        for (level, profile, seed), future in zip(jobs, futures):
            label, report, error = future.result()
            if report is None:
                failures += 1
                print(f"  FAILED {label}: {error}", file=sys.stderr)
                continue
            row = summary_row(level, profile, seed, report)
            rows.append(row)
            reports.append((level.stem, profile, report))
            print(f"  {label}: {row['result']} wave {row['waves_reached']} "
                  f"integrity {row['final_integrity']}")

    if not rows:
        print("error: every run failed", file=sys.stderr)
        return 1

    csv_path = out_dir / "summary.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    digest = out_dir / "summary.md"
    write_digest(digest, rows, reports)
    print(f"\nwrote {csv_path}\nwrote {digest}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
