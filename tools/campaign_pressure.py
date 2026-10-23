#!/usr/bin/env python3
"""Inspect or deliberately retime authored campaign release pressure.

This is scheduling analysis, not a second combat simulation. Density/sec uses
the currently loaded family base densities and does not measure contact DPS.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIN_PORT_GAP = 1.25


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def family_densities(root: Path = ROOT) -> dict[str, float]:
    document = read_json(root / "assets/config/enemies.json")
    return {name: family["behavior"]["base_density"] for name, family in document["families"].items()}


def wave_metrics(wave: dict, densities: dict[str, float]) -> dict:
    events: dict[float, list[float]] = {}
    ports: dict[str, list[dict]] = {}
    density = count = 0
    for spawn in wave.get("spawns", []):
        start, duration = spawn.get("start_time", 0), spawn["duration"]
        if duration <= 0:
            raise ValueError("Campaign pressure requires positive-duration, explicit cohorts")
        population = spawn["count"]
        mass = population * densities[spawn["family"]]
        density += mass
        count += population
        for time, sign in ((start, 1), (start + duration, -1)):
            changes = events.setdefault(time, [0.0, 0.0])
            changes[0] += sign * mass / duration
            changes[1] += sign * population / duration
        ports.setdefault(spawn["spawn_point_id"], []).append(spawn)
    current_density = current_agents = peak_density = peak_agents = 0.0
    for time in sorted(events):
        current_density += events[time][0]
        current_agents += events[time][1]
        peak_density = max(peak_density, current_density)
        peak_agents = max(peak_agents, current_agents)
    gaps = []
    for cohorts in ports.values():
        ordered = sorted(cohorts, key=lambda s: s.get("start_time", 0))
        gaps.extend(b.get("start_time", 0) - a.get("start_time", 0) - a["duration"]
                    for a, b in zip(ordered, ordered[1:]))
    return {"name": wave.get("name"), "population": count, "authored_density": density,
            "peak_density_per_second": peak_density, "peak_agents_per_second": peak_agents,
            "release_span_seconds": max(events, default=0), "ports_used": len(ports),
            "minimum_same_port_gap_seconds": min(gaps) if gaps else None,
            "cohorts": copy.deepcopy(wave.get("spawns", []))}


def level_metrics(document: dict, densities: dict[str, float]) -> dict:
    waves = [wave_metrics(wave, densities) for wave in document["waves"]]
    return {"name": document["name"], "starting_atp": document["economy"]["starting_atp"],
            "income_multiplier": document["economy"].get("income_multiplier", 1.0),
            "population": sum(w["population"] for w in waves),
            "authored_density": sum(w["authored_density"] for w in waves),
            "peak_density_per_second": max(w["peak_density_per_second"] for w in waves), "waves": waves}


def inspect_campaign(root: Path = ROOT) -> dict:
    densities = family_densities(root)
    levels = [{"path": str(path.relative_to(root)).replace("\\", "/"),
               **level_metrics(read_json(path), densities)}
              for path in sorted((root / "assets/levels").glob("campaign_*.json"))]
    return {"schema": 1, "scope": "Authored release density/sec; not measured contact pressure, combat capacity or player difficulty.",
            "family_base_densities": densities, "levels": levels}


def curve_errors(curve: dict, minimum_gap: float = MIN_PORT_GAP) -> list[str]:
    errors = []
    previous_level = 0.0
    if len(curve["levels"]) != 10:
        errors.append("Expected all ten authored campaign levels")
    for level in curve["levels"]:
        if level["peak_density_per_second"] <= previous_level + 1e-6:
            errors.append(f"{level['name']}: level peak release density does not increase")
        previous_level = level["peak_density_per_second"]
        previous_wave = 0.0
        for wave in level["waves"]:
            if wave["peak_density_per_second"] <= previous_wave + 1e-6:
                errors.append(f"{wave['name']}: release density does not increase within level")
            previous_wave = wave["peak_density_per_second"]
            gap = wave["minimum_same_port_gap_seconds"]
            if gap is not None and gap + 1e-6 < minimum_gap:
                errors.append(f"{wave['name']}: same-port cohort gap below {minimum_gap} seconds")
    return errors


def compressed_wave(original: dict, scale: float, minimum_gap: float = MIN_PORT_GAP) -> dict:
    wave = copy.deepcopy(original)
    ports: dict[str, list[int]] = {}
    for index, entry in enumerate(original["spawns"]):
        ports.setdefault(entry["spawn_point_id"], []).append(index)
    for indices in ports.values():
        previous_end = None
        for index in sorted(indices, key=lambda i: original["spawns"][i].get("start_time", 0)):
            before, after = original["spawns"][index], wave["spawns"][index]
            duration = round(before["duration"] * scale, 4)
            if duration <= 0:
                raise ValueError("Compression produced an empty cohort duration")
            start = before.get("start_time", 0) * scale
            if previous_end is not None:
                start = max(start, previous_end + minimum_gap)
            start = round(start, 4)
            after["duration"] = duration
            if "start_time" in before or start:
                after["start_time"] = start
            previous_end = start + duration
    return wave


def retime_wave(original: dict, target: float, densities: dict[str, float]) -> dict:
    existing = wave_metrics(original, densities)["peak_density_per_second"]
    if target <= existing * (1 + 1e-10):
        return copy.deepcopy(original)
    # Fixed readable gaps can change which ports overlap as time compresses.
    # Sample first, then refine every crossing, rather than assume a globally
    # monotone objective and silently pick an incorrect binary-search branch.
    samples = []
    for index in range(257):
        scale = math.exp(math.log(.02) * (1 - index / 256))
        wave = compressed_wave(original, scale)
        pressure = wave_metrics(wave, densities)["peak_density_per_second"]
        samples.append((scale, pressure, wave))
    candidates = [s for s in samples if s[1] >= target]
    for left, right in zip(samples, samples[1:]):
        if (left[1] >= target) == (right[1] >= target):
            continue
        lo, hi = left[0], right[0]
        left_passes = left[1] >= target
        for _ in range(30):
            mid = (lo + hi) / 2
            wave = compressed_wave(original, mid)
            pressure = wave_metrics(wave, densities)["peak_density_per_second"]
            if pressure >= target:
                candidates.append((mid, pressure, wave))
            if (pressure >= target) == left_passes:
                lo = mid
            else:
                hi = mid
    if not candidates:
        raise ValueError("Could not reach the requested scheduling pressure")
    chosen = min(candidates, key=lambda s: (s[1] - target, -s[0]))
    if chosen[1] > target * 1.003:
        raise ValueError("Requested pressure cannot be reached closely while preserving cohort gaps")
    return chosen[2]


def retime_levels(documents: list[dict], densities: dict[str, float], factor: float = 1.32) -> list[dict]:
    if not 1 < factor < 2:
        raise ValueError("Level pressure factor must be greater than one and less than two")
    opening_fraction = (wave_metrics(documents[0]["waves"][0], densities)["peak_density_per_second"] /
                        level_metrics(documents[0], densities)["peak_density_per_second"])
    previous_level = 0.0
    result = []
    for level_index, original in enumerate(documents):
        document = copy.deepcopy(original)
        target_peak = max(level_metrics(original, densities)["peak_density_per_second"], previous_level * factor)
        previous_wave = 0.0
        for index, wave in enumerate(original["waves"]):
            phase = index / (len(original["waves"]) - 1) if len(original["waves"]) > 1 else 1
            envelope = target_peak * (opening_fraction + (1 - opening_fraction) * phase ** 1.15)
            target = max(envelope, previous_wave * 1.02)
            if level_index == 0 and index == 0:
                document["waves"][index] = copy.deepcopy(wave)
            else:
                document["waves"][index] = retime_wave(wave, target, densities)
            previous_wave = wave_metrics(document["waves"][index], densities)["peak_density_per_second"]
        previous_level = level_metrics(document, densities)["peak_density_per_second"]
        result.append(document)
    return result


def replace_schedule_text(text: str, before: dict, after: dict) -> str:
    """Retain compact authored geometry formatting; change only timing tokens."""
    original = [s for w in before["waves"] for s in w["spawns"]]
    revised = [s for w in after["waves"] for s in w["spawns"]]
    lines, index = [], 0
    numeric = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
    for line in text.splitlines(keepends=True):
        if '"family"' in line:
            old, new = original[index], revised[index]
            if old != new:
                for key in ("start_time", "duration"):
                    if key not in new:
                        continue
                    number = f"{new[key]:.4f}".rstrip("0").rstrip(".") or "0"
                    if f'"{key}"' in line:
                        line = re.sub(rf'("{key}"\s*:\s*){numeric}', lambda m: m[1] + number, line)
                    elif key == "start_time":
                        line = line.replace('"duration"', f'"start_time": {number}, "duration"', 1)
            index += 1
        lines.append(line)
    if index != len(revised):
        raise ValueError("Unexpected level formatting; no files were written")
    result = "".join(lines)
    if json.loads(result) != after:
        raise ValueError("Timing-only serialization changed other fields; no files were written")
    return result


def author_report(paths: list[Path], documents: list[dict], revised: list[dict], densities: dict[str, float], factor: float) -> dict:
    levels = []
    for path, before, after in zip(paths, documents, revised):
        levels.append({"path": str(path.relative_to(ROOT)).replace("\\", "/"),
                       "before": level_metrics(before, densities), "after": level_metrics(after, densities),
                       "before_document_sha256": hashlib.sha256(json.dumps(before, sort_keys=True).encode()).hexdigest(),
                       "after_document_sha256": hashlib.sha256(json.dumps(after, sort_keys=True).encode()).hexdigest()})
    return {"schema": 1, "scope": "Authored scheduling change; not measured player difficulty or gameplay acceptance.",
            "hypothesis": {"level_peak_density_factor": factor, "minimum_same_port_gap_seconds": MIN_PORT_GAP,
                           "within_level_envelope": "opening fraction plus remaining fraction times wave phase^1.15; no existing wave is weakened",
                           "first_campaign_wave_unchanged": True, "counts_families_routes_geometry_and_economy_unchanged": True},
            "family_base_densities": densities, "levels": levels}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail on campaign pressure troughs or cohort gap violations")
    parser.add_argument("--retime", action="store_true", help="Explicitly edit authored campaign timing; no combat stats or economy")
    parser.add_argument("--factor", type=float, default=1.32)
    parser.add_argument("--out", type=Path, help="Write exact machine-readable inspection or before/after report")
    args = parser.parse_args(argv)
    if args.retime:
        paths = sorted((ROOT / "assets/levels").glob("campaign_*.json"))
        documents = [read_json(path) for path in paths]
        densities = family_densities()
        revised = retime_levels(documents, densities, args.factor)
        texts = [replace_schedule_text(p.read_text(encoding="utf-8"), a, b) for p, a, b in zip(paths, documents, revised)]
        report = author_report(paths, documents, revised, densities, args.factor)
        curve = {"levels": [level["after"] for level in report["levels"]]}
        errors = curve_errors(curve)
        if errors:
            raise ValueError("; ".join(errors))
        for path, text in zip(paths, texts):
            path.write_text(text, encoding="utf-8")
    else:
        report = inspect_campaign()
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    else:
        print(json.dumps(report, indent=2))
    if args.check:
        errors = curve_errors(inspect_campaign())
        for error in errors:
            print(error)
        return 2 if errors else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
