#!/usr/bin/env python3
"""Plan, execute and compare reproducible IMMUNE balance experiments.

Standard-library only. A report describes the configured controllers and seed
sample, never a measured human difficulty or enjoyment rating. Example:
  python tools/balance_lab.py run assets/balance/experiments/smoke.json \
      --exe path/to/immune.exe --out scratch/balance/smoke
  python tools/balance_lab.py report scratch/balance/smoke
"""
from __future__ import annotations

import argparse
import concurrent.futures
import copy
import hashlib
import html
import json
import math
import random
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
TERMINAL = {"cleared", "defeated", "completed", "stalled"}
SUCCESS = {"cleared", "completed"}
KNOWN_STATUSES = TERMINAL | {"timeout", "invalid"}
CONFIG_NAMES = ("towers", "enemies", "sim", "economy", "abilities", "meta")


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def merge(base: dict, patch: dict) -> dict:
    """Recursively merge objects; lists replace, so a purchase order is atomic."""
    out = copy.deepcopy(base)
    for key, value in patch.items():
        out[key] = merge(out[key], value) if isinstance(value, dict) and isinstance(out.get(key), dict) else copy.deepcopy(value)
    return out


def set_path(base: dict, path: str, value: Any) -> dict:
    """Set a dotted RunSpec path. Registry override paths belong inside patch."""
    out = copy.deepcopy(base)
    cursor = out
    keys = path.split(".")
    for key in keys[:-1]:
        if not isinstance(cursor.get(key, {}), dict):
            raise ValueError(f"Cannot descend into {path!r}")
        cursor = cursor.setdefault(key, {})
    cursor[keys[-1]] = copy.deepcopy(value)
    return out


def finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def get_path(value: dict, path: str, default=None):
    cursor: Any = value
    for key in path.split("."):
        if not isinstance(cursor, dict) or key not in cursor:
            return default
        cursor = cursor[key]
    return cursor


def config_documents(value: Any) -> dict:
    if isinstance(value, list) and len(value) == len(CONFIG_NAMES):
        return dict(zip(CONFIG_NAMES, value))
    return value if isinstance(value, dict) else {}


def slug(value: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_-]+", "-", value).strip("-")[:70] or "run"


def check_current_first_wave_reference(path: Path) -> None:
    """A checked-in current reference must track its authored gameplay inputs.

    Embedded historical documents remain immutable replay inputs and are not
    checked against current assets.
    """
    reference = ROOT / "assets/balance/levels/campaign_first_wave_reference.json"
    if path.resolve() != reference.resolve():
        return
    actual = read_json(reference)
    expected = read_json(ROOT / "assets/levels/campaign_01_first_bend.json")
    actual.pop("description", None)
    expected.pop("description", None)
    expected["waves"] = expected.get("waves", [])[:1]
    if actual != expected:
        raise ValueError("First-wave calibration fixture differs from current authored gameplay. Refresh the exact first-wave slice and requalify the controller; see docs/BALANCE_LAB.md")


def normalize_spec(spec: dict, repo: Path = ROOT) -> dict:
    """Resolve only filesystem fields, never mutate the experiment."""
    out = copy.deepcopy(spec)
    out.setdefault("schema", 1)
    for key in ("level",):
        if out.get(key):
            out[key] = str((repo / out[key]).resolve())
            if "level_document" not in out:
                check_current_first_wave_reference(Path(out[key]))
    if out.get("campaign"):
        out["campaign"]["levels"] = [str((repo / path).resolve()) for path in out["campaign"]["levels"]]
    if get_path(out, "progression.save"):
        out["progression"]["save"] = str((repo / out["progression"]["save"]).resolve())
    return out


def validate_experiment(experiment: dict) -> None:
    if experiment.get("schema") != 1:
        raise ValueError("Experiment schema must be 1")
    if experiment.get("kind", "paired") not in {"paired", "sweep", "search", "budget", "campaign"}:
        raise ValueError("Unknown experiment kind")
    if not isinstance(experiment.get("base"), dict):
        raise ValueError("Experiment requires a base RunSpec object")
    if experiment.get("evaluation_split", "evaluation") not in {"evaluation", "training", "heldout"}:
        raise ValueError("evaluation_split must be evaluation, training or heldout")
    pacing = experiment.get("progression_targets", {})
    supported_targets = {"median_attempts_min", "median_attempts_max", "maximum_attempts_per_level", "minimum_post_loss_purchase_rate"}
    if not isinstance(pacing, dict) or set(pacing) - supported_targets:
        raise ValueError("Unknown progression target")
    for key, value in pacing.items():
        if not finite_number(value) or value < 0 or (key == "minimum_post_loss_purchase_rate" and value > 1):
            raise ValueError(f"Invalid progression target {key}")
    if pacing.get("median_attempts_min", 0) > pacing.get("median_attempts_max", math.inf):
        raise ValueError("Minimum median attempts exceeds maximum")
    for name in ("seeds", "train_seeds", "heldout_seeds"):
        values = experiment.get(name)
        if values is not None and (not values or any(type(v) is not int or v < 0 for v in values) or len(set(values)) != len(values)):
            raise ValueError(f"{name} must contain distinct nonnegative integer seeds")
    if experiment.get("kind") == "search":
        if not experiment.get("train_seeds") or not experiment.get("heldout_seeds"):
            raise ValueError("Search requires training and held-out seeds")
        if set(experiment["train_seeds"]) & set(experiment["heldout_seeds"]):
            raise ValueError("Training and held-out seeds must be disjoint")
    if experiment.get("kind") == "budget":
        values = experiment.get("budgets", [])
        if not values or any(type(v) is not int or v < 0 for v in values) or len(set(values)) != len(values):
            raise ValueError("Budget search requires distinct nonnegative integer budgets")
        target = experiment.get("target_win_rate", 1.0)
        if not finite_number(target) or not 0 <= target <= 1:
            raise ValueError("target_win_rate must be between zero and one")
    policies = experiment.get("policies", [{"name": "competent", "controller": {"policy": "competent"}}])
    if not policies or len({p["name"] for p in policies}) != len(policies):
        raise ValueError("Policy names must be nonempty and distinct")
    variants = expand_variants(experiment)
    if not variants or len({v["name"] for v in variants}) != len(variants):
        raise ValueError("Variant names must be distinct")
    if "power_probe" in experiment:
        if experiment.get("kind") != "campaign":
            raise ValueError("Power probes require a campaign experiment")
        probe = experiment["power_probe"]
        base = probe.get("base", {})
        if base.get("income_enabled") is not False or "opening_atp" not in base or "spend_limit" not in base:
            raise ValueError("Power probes require explicit fixed opening_atp, spend_limit and income_enabled:false")
        if get_path(base, "progression.profile") == "all_unlocked":
            raise ValueError("Power probes must apply actual journey progression, not all_unlocked")
        scales = probe.get("pressure_scales", [])
        if not scales or any(not finite_number(s) or s <= 0 for s in scales) or len(set(scales)) != len(scales):
            raise ValueError("Power probe pressure_scales must be distinct positive numbers")
        seeds = probe.get("seeds", [])
        if not seeds or any(type(s) is not int or s < 0 for s in seeds) or len(set(seeds)) != len(seeds):
            raise ValueError("Power probes require distinct nonnegative seeds")
        rate = probe.get("minimum_clear_rate", 1.0)
        if not finite_number(rate) or not 0 <= rate <= 1:
            raise ValueError("Power probe minimum_clear_rate must be in [0,1]")
        if type(probe.get("every_n_clears", 1)) is not int or probe.get("every_n_clears", 1) < 1:
            raise ValueError("every_n_clears must be a positive integer")
        modes = probe.get("wallet_modes", [probe.get("wallet_mode", "fixed")])
        if not modes or any(mode not in {"fixed", "journey_opening"} for mode in modes) or len(set(modes)) != len(modes):
            raise ValueError("Power probe wallet modes must be fixed or journey_opening, without duplicates")
        if "snapshot_ids" in probe and (not probe["snapshot_ids"] or len(set(probe["snapshot_ids"])) != len(probe["snapshot_ids"])):
            raise ValueError("Power probe snapshot_ids must be nonempty and distinct")
    for key, target in experiment.get("growth_targets", {}).items():
        if key in {"enemy_factor_per_level", "player_factor_per_level"} and (not finite_number(target) or target <= 0):
            raise ValueError(f"Growth target {key} must be positive")
    calibration = experiment.get("controller_calibration", {})
    if not isinstance(calibration, dict) or calibration.get("schema", 1) != 1 or not isinstance(calibration.get("references", []), list):
        raise ValueError("Controller calibration must be a schema-1 object with references")
    for reference in calibration.get("references", []):
        if not isinstance(reference, dict):
            raise ValueError("Controller references must be objects")
        if not reference.get("name") or not reference.get("level") or not reference.get("source"):
            raise ValueError("Controller references require name, level and evidence source")
        if reference.get("profile", "fresh") not in {"fresh", "any"}:
            raise ValueError("Controller reference profile must be fresh or any")
        if "expected_status" in reference and reference["expected_status"] not in {"cleared", "defeated"}:
            raise ValueError("Controller reference expected_status must be cleared or defeated")
        if type(reference.get("minimum_waves_cleared", 1)) is not int or reference.get("minimum_waves_cleared", 1) < 0:
            raise ValueError("Controller reference minimum_waves_cleared must be a nonnegative integer")
        if type(reference.get("minimum_samples", 1)) is not int or reference.get("minimum_samples", 1) < 1:
            raise ValueError("Controller reference minimum_samples must be positive")
        rate = reference.get("minimum_success_rate", 1.0)
        if not finite_number(rate) or not 0 <= rate <= 1:
            raise ValueError("Controller reference minimum_success_rate must be in [0,1]")


def expand_variants(experiment: dict) -> list[dict]:
    kind = experiment.get("kind", "paired")
    variants = copy.deepcopy(experiment.get("variants", [{"name": "baseline", "patch": {}}]))
    if kind == "sweep":
        for sweep in experiment.get("sweeps", []):
            for value in sweep["values"]:
                if "override" in sweep:
                    patch = {"overrides": {sweep["override"]: value}}
                elif "tree" in sweep:
                    patch = {"progression": {"grant_tree": {sweep["tree"]: value}}}
                else:
                    patch = set_path({}, sweep["path"], value)
                variants.append({"name": f"{sweep['name']}={value}", "patch": patch, "axis": sweep["name"], "value": value})
    if kind == "budget":
        variants = [{"name": f"budget={value}", "patch": set_path({}, experiment.get("budget_path", "opening_atp"), value), "budget": value}
                    for value in sorted(experiment["budgets"])]
    return variants


def plan_runs(experiment: dict, phase: str | None = None, selected: set[tuple[str, str]] | None = None) -> list[dict]:
    validate_experiment(experiment)
    kind = experiment.get("kind", "paired")
    phase = phase or experiment.get("evaluation_split", "evaluation")
    seed_key = ("train_seeds" if phase == "training" else "heldout_seeds" if phase == "heldout" else "seeds") if kind == "search" else "seeds"
    seeds = experiment.get(seed_key, [101, 202, 303])
    policies = experiment.get("policies", [{"name": "competent", "controller": {"policy": "competent"}}])
    runs = []
    for policy in policies:
        for variant in expand_variants(experiment):
            if selected is not None and (policy["name"], variant["name"]) not in selected:
                continue
            for seed in seeds:
                spec = merge(experiment["base"], {"controller": policy.get("controller", {})})
                spec = merge(spec, variant.get("patch", {}))
                spec["seed"] = seed
                spec = normalize_spec(spec)
                identity = {"policy": policy["name"], "variant": variant["name"], "seed": seed, "phase": phase, "spec": spec}
                digest = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:12]
                runs.append({"id": f"{slug(phase)}-{slug(policy['name'])}-{slug(variant['name'])}-{seed}-{digest}",
                             "policy": policy["name"], "variant": variant["name"], "seed": seed, "phase": phase,
                             "budget": variant.get("budget"), "spec": spec, "kind": kind})
    return runs


def command(executable: Path, spec: Path, output: Path) -> list[str]:
    return [str(executable.resolve()), "--balance-run", str(spec.resolve()), "--out", str(output.resolve())]


def checked_report(report: Any, exit_code: int = 0, process_timeout: bool = False) -> tuple[bool, str, str]:
    if process_timeout:
        return False, "timeout", "Runner exceeded the wall-clock timeout; no terminal outcome was established"
    if not isinstance(report, dict) or report.get("schema") != 1 or not isinstance(report.get("summary"), dict):
        return False, "invalid", "Missing or unsupported report schema/summary"
    summary = report["summary"]
    status = summary.get("status", "invalid")
    if status not in KNOWN_STATUSES:
        return False, "invalid", f"Unknown runner status: {status!r}"
    if exit_code != 0:
        return False, "invalid", f"Runner exited with code {exit_code}"
    if summary.get("valid") is not True or status == "invalid":
        return False, "invalid", "Runner marked the run invalid"
    return True, status, ""


def execute_one(run: dict, executable: Path, directory: Path, timeout: float) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    spec_path = directory / f"{run['id']}.spec.json"
    report_path = directory / f"{run['id']}.report.json"
    record_path = directory / f"{run['id']}.record.json"
    spec = copy.deepcopy(run["spec"])
    if run.get("pinned_config"):
        config_dir = directory / f"{run['id']}.config"
        for name, document in config_documents(run["pinned_config"]).items():
            write_json(config_dir / f"{name}.json", document)
        spec["config_dir"] = str(config_dir.resolve())
    write_json(spec_path, spec)
    # Never accidentally reuse an older successful report after a failed rerun.
    if report_path.exists():
        report_path.unlink()
    started = time.monotonic()
    report, exit_code, timed_out, error = None, None, False, ""
    stdout, stderr = "", ""
    try:
        result = subprocess.run(command(executable, spec_path, report_path), cwd=ROOT,
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        exit_code, stdout, stderr = result.returncode, result.stdout, result.stderr
        if report_path.is_file():
            report = read_json(report_path)
        else:
            # Supports runner builds that emit JSON only on stdout.
            report = json.loads(stdout)
            write_json(report_path, report)
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        stdout = exc.stdout.decode("utf-8", "replace") if isinstance(exc.stdout, bytes) else exc.stdout or ""
        stderr = exc.stderr.decode("utf-8", "replace") if isinstance(exc.stderr, bytes) else exc.stderr or ""
    except (OSError, ValueError) as exc:
        error = str(exc)
    valid, status, validation_error = checked_report(report, exit_code or 0, timed_out)
    if valid:
        for key in ("executable_hash", "base_config_hash"):
            expected = run.get(f"expected_{key}")
            if expected is not None and get_path(report, f"provenance.{key}") != expected:
                valid, status = False, "invalid"
                validation_error = f"Pinned {key} mismatch: expected {expected}, got {get_path(report, f'provenance.{key}')}"
                break
    if error:
        valid, status = False, "invalid"
    (directory / f"{run['id']}.stdout.log").write_text(stdout, encoding="utf-8")
    (directory / f"{run['id']}.stderr.log").write_text(stderr, encoding="utf-8")
    record = {key: value for key, value in run.items() if key not in {"spec", "pinned_config"}}
    record.update({"valid": valid, "status": status, "error": error or validation_error,
                   "process_timeout": timed_out, "exit_code": exit_code,
                   "wall_seconds": round(time.monotonic() - started, 4),
                   "spec_path": f"runs/{spec_path.name}", "report_path": f"runs/{report_path.name}",
                   "stderr_path": f"runs/{run['id']}.stderr.log", "report": report})
    write_json(record_path, record)
    return record


def wilson(successes: int, count: int, z: float = 1.959963984540054) -> list[float] | None:
    if count == 0:
        return None
    p = successes / count
    denom = 1 + z * z / count
    center = (p + z * z / (2 * count)) / denom
    half = z * math.sqrt(p * (1 - p) / count + z * z / (4 * count * count)) / denom
    return [max(0.0, center - half), min(1.0, center + half)]


def percentile(values: list[float], fraction: float) -> float:
    values = sorted(values)
    index = (len(values) - 1) * fraction
    low = int(index)
    high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (index - low)


def mean_stats(values: list[float]) -> dict:
    if not values:
        return {"n": 0, "mean": None, "min": None, "max": None, "stddev": None, "ci95": None}
    ci = None
    if len(values) >= 2:
        rng = random.Random(1729)
        samples = [statistics.mean(rng.choices(values, k=len(values))) for _ in range(2000)]
        ci = [percentile(samples, .025), percentile(samples, .975)]
    return {"n": len(values), "mean": statistics.mean(values), "min": min(values), "max": max(values),
            "stddev": statistics.stdev(values) if len(values) >= 2 else None, "ci95": ci}


def is_terminal(record: dict) -> bool:
    return record.get("valid") is True and record.get("status") in TERMINAL


def metric_value(record: dict, metric: str) -> float | None:
    value = get_path(record.get("report") or {}, metric if "." in metric else f"summary.{metric}")
    return float(value) if finite_number(value) else None


def grouped(records: list[dict]) -> dict[tuple[str, str, str], list[dict]]:
    groups: dict[tuple[str, str, str], list[dict]] = {}
    for record in records:
        groups.setdefault((record["phase"], record["policy"], record["variant"]), []).append(record)
    return groups


def campaign_metrics(record: dict) -> dict:
    report = record.get("report") or {}
    attempts = report.get("attempts", [])
    terminal_attempts = [a for a in attempts if a.get("summary", {}).get("valid") is True and a.get("summary", {}).get("status") in {"cleared", "defeated"}]
    by_level: dict[str, dict] = {}
    for attempt in attempts:
        level = str(attempt.get("level_index", get_path(attempt, "provenance.level", "unknown")))
        row = by_level.setdefault(level, {"attempts": 0, "clears": 0, "terminal_attempts": 0, "memory_cells": 0, "antibodies": 0})
        row["attempts"] += 1
        row["clears"] += attempt.get("summary", {}).get("status") == "cleared"
        if attempt in terminal_attempts:
            row["terminal_attempts"] += 1
            reward = attempt.get("reward", {})
            row["memory_cells"] += reward.get("memory_cells", 0)
            row["antibodies"] += reward.get("antibodies", 0)
    simulated_seconds = sum(float(a["summary"].get("duration_seconds", a["summary"].get("duration_ticks", 0) / 60)) for a in attempts if isinstance(a.get("summary"), dict))
    memory_cells = sum(row["memory_cells"] for row in by_level.values())
    return {"attempts": len(attempts), "terminal_attempts": len(terminal_attempts),
            "levels_cleared": sum(row["clears"] > 0 for row in by_level.values()),
            "retries": sum(max(0, row["attempts"] - 1) for row in by_level.values()),
            "memory_cells_earned": memory_cells,
            "antibodies_earned": sum(row["antibodies"] for row in by_level.values()),
            "simulated_seconds": simulated_seconds,
            "memory_cells_per_simulated_minute": memory_cells * 60 / simulated_seconds if simulated_seconds > 0 else None,
            "purchases": report.get("purchases", []), "levels": by_level,
            "final_progression": report.get("final_progression", {})}


def progression_journeys(records: list[dict], experiment: dict) -> list[dict]:
    """Keep entry difficulty, rewarded retries and discoveries in their actual order.

    A purchased rank is not proof of a useful combat effect. This view joins
    the reward/purchase evidence; controlled rank probes establish usefulness.
    """
    journeys = []
    targets = experiment.get("progression_targets", {})
    for record in records:
        report = record.get("report") or {}
        if report.get("kind") != "campaign":
            continue
        attempts = report.get("attempts", [])
        levels = {}
        for index, attempt in enumerate(attempts):
            level_index = attempt.get("level_index", 0)
            level = get_path(attempt, "replay_spec.level_document", get_path(attempt, "spec.level_document", {}))
            total_waves = len(level.get("waves", []))
            row = levels.setdefault(level_index, {"level_index": level_index, "level": level.get("name", str(level_index)),
                "total_waves": total_waves, "attempts": [], "loss_rewards": [], "first_clear_antibodies": 0,
                "discovery_offers": [], "tools_bought_before_next_level": []})
            summary = attempt.get("summary", {})
            terminal = summary.get("valid") is True and summary.get("status") in {"cleared", "defeated"}
            lifecycles = lifecycle_metrics([{**record, "report": attempt, "valid": terminal, "status": summary.get("status")}])
            row["attempts"].append({"attempt": attempt.get("level_attempt", len(row["attempts"]) + 1),
                "valid_terminal": terminal, "status": summary.get("status", "invalid"),
                "waves_cleared": summary.get("waves_cleared", 0), "integrity": summary.get("objective_integrity"),
                "friendly_deaths": summary.get("friendly_killed"), "atp_spent": summary.get("atp_spent"),
                "opening_atp": get_path(attempt, "economy.opening_atp"), "ttk": lifecycles})
            if not terminal:
                continue
            reward = attempt.get("reward", {})
            following = attempts[index + 1] if index + 1 < len(attempts) else None
            if summary["status"] == "defeated":
                purchases = following.get("purchases", []) if following is not None else []
                row["loss_rewards"].append({"after_attempt": len(row["attempts"]),
                    "memory_cells": reward.get("memory_cells", 0),
                    "next_attempt_observed": following is not None,
                    "purchases_before_next_attempt": purchases,
                    "memory_spent_before_next_attempt": sum(p.get("memory_cells", 0) for p in purchases),
                    "scope": "Purchases are observed after the loss; retained currency may also fund them. Rank probes measure combat value."})
            elif reward.get("first_clear") is True:
                row["first_clear_antibodies"] = reward.get("antibodies", 0)
                row["discovery_offers"] = get_path(attempt, "discovery.offers", [])
                if following is not None and following.get("level_index") != level_index:
                    row["tools_bought_before_next_level"] = [p for p in following.get("purchases", []) if p.get("antibodies", 0) > 0]
        counts, unresolved = [], []
        for row in levels.values():
            observed = row["attempts"]
            row["cleared"] = any(a["valid_terminal"] and a["status"] == "cleared" for a in observed)
            row["unresolved"] = any(not a["valid_terminal"] for a in observed)
            row["entry_waves_cleared"] = observed[0]["waves_cleared"]
            row["entry_wave_fraction"] = row["entry_waves_cleared"] / row["total_waves"] if row["total_waves"] else None
            row["attempts_to_clear"] = next((a["attempt"] for a in observed if a["valid_terminal"] and a["status"] == "cleared"), None)
            if row["cleared"]:
                counts.append(row["attempts_to_clear"])
            if row["unresolved"] or not row["cleared"]:
                unresolved.append(row["level_index"])
        median = statistics.median(counts) if counts else None
        expected_levels = get_path(report, "summary.levels_total", len(levels))
        complete = (record.get("valid") is True and report.get("summary", {}).get("status") == "completed"
                    and len(levels) == expected_levels and not unresolved)
        losses = [loss for row in levels.values() for loss in row["loss_rewards"] if loss["next_attempt_observed"]]
        purchase_rate = sum(bool(loss["purchases_before_next_attempt"]) for loss in losses) / len(losses) if losses else None
        checks = []
        for name, passed in [
            ("all_levels_completed", complete),
            ("median_attempts_min", median is not None and median >= targets.get("median_attempts_min", 0)),
            ("median_attempts_max", median is not None and median <= targets.get("median_attempts_max", math.inf)),
            ("maximum_attempts_per_level", complete and max(counts, default=math.inf) <= targets.get("maximum_attempts_per_level", math.inf)),
            ("minimum_post_loss_purchase_rate", purchase_rate is not None and purchase_rate >= targets.get("minimum_post_loss_purchase_rate", 0)),
        ]:
            if name == "all_levels_completed" or name in targets:
                checks.append({"target": name, "passed": bool(passed)})
        journeys.append({"id": record["id"], "phase": record["phase"], "policy": record["policy"],
            "variant": record["variant"], "seed": record["seed"], "complete": complete,
            "expected_levels": expected_levels, "median_attempts_to_clear": median,
            "first_attempt_clears": sum(r["attempts_to_clear"] == 1 for r in levels.values()),
            "observed_loss_followups": len(losses), "post_loss_purchase_rate": purchase_rate,
            "unresolved_or_uncleared_levels": unresolved, "targets": targets, "checks": checks,
            "matches_declared_targets": complete and bool(targets) and all(c["passed"] for c in checks),
            "levels": list(levels.values()),
            "scope": "Observed retry pacing under this purchase strategy and controller. First-try clears are allowed; targets describe a journey, not forced failure or measured human enjoyment."})
    return journeys


def paired_comparisons(records: list[dict], baseline: str, metric: str) -> list[dict]:
    groups = grouped(records)
    comparisons = []
    for (phase, policy, variant), rows in groups.items():
        if variant == baseline:
            continue
        controls = {r["seed"]: r for r in groups.get((phase, policy, baseline), [])}
        differences, wins, used, omitted = [], [], [], []
        for row in rows:
            control = controls.get(row["seed"])
            if not control or not is_terminal(row) or not is_terminal(control):
                omitted.append(row["seed"])
                continue
            used.append(row["seed"])
            wins.append(float(row["status"] in SUCCESS) - float(control["status"] in SUCCESS))
            a, b = metric_value(row, metric), metric_value(control, metric)
            if a is not None and b is not None:
                differences.append(a - b)
        comparisons.append({"phase": phase, "policy": policy, "variant": variant, "baseline": baseline,
                            "paired_seeds": used, "omitted_seeds": omitted,
                            "metric": metric, "metric_delta": mean_stats(differences), "clear_rate_delta": mean_stats(wins)})
    return comparisons


def distribution_stats(distribution: dict) -> dict:
    """Preserve percentile bin bounds; never invent exact TTK quantiles."""
    count = distribution.get("observations", 0)
    if not isinstance(count, int) or count <= 0:
        return {"observations": 0, "mean_seconds": None, "min_seconds": None,
                "max_seconds": None, "p50_seconds": None, "p90_seconds": None}
    mean = distribution.get("mean_seconds")
    if not finite_number(mean):
        mean = distribution.get("sum_ticks", 0) / count / 60
    def bounds(fraction: float):
        name = "p50" if fraction == .5 else "p90"
        lo, hi = distribution.get(f"{name}_lower_seconds"), distribution.get(f"{name}_upper_seconds")
        if finite_number(lo) and finite_number(hi):
            return [lo, hi]
        target, cumulative = math.ceil(count * fraction), 0
        for index, observations in enumerate(distribution.get("bins", [])):
            cumulative += observations
            if cumulative >= target:
                return [0.0, 0.0] if index == 0 else [2 ** (index - 1) / 60, (2 ** index - 1) / 60]
        return None
    return {"observations": count, "mean_seconds": mean,
            "min_seconds": distribution.get("min_ticks", 0) / 60,
            "max_seconds": distribution.get("max_ticks", 0) / 60,
            "p50_seconds": bounds(.5), "p90_seconds": bounds(.9)}


def lifecycle_metrics(records: list[dict]) -> list[dict]:
    rows = []
    for record in records:
        # Journey attempts have different ranks and money, so preserve their
        # measurements in raw attempt reports rather than pool them as one unit.
        if not is_terminal(record) or (record.get("report") or {}).get("kind") == "campaign":
            continue
        report = record.get("report") or {}
        subjects = [("enemy", name, node) for name, node in get_path(report, "combat.enemy_lifetimes", {}).items()]
        subjects += [("friendly", name, node["lifecycle"]) for name, node in report.get("units", {}).items() if isinstance(node, dict) and "lifecycle" in node]
        for side, name, node in subjects:
            rows.append({"run_id": record["id"], "phase": record["phase"], "policy": record["policy"],
                         "variant": record["variant"], "seed": record["seed"], "side": side, "subject": name,
                         "spawned": node.get("spawned", 0), "entered_alive": node.get("entered_alive", 0),
                         "alive": node.get("alive", 0), "hit_alive": node.get("hit_alive", 0),
                         "first_hits": node.get("first_hits"),
                         "damage_deaths": node.get("damage_deaths", 0),
                         "leaked": node.get("leaked", 0), "out_of_bounds": node.get("out_of_bounds", 0),
                         "consumed": node.get("consumed", 0), "expired": node.get("expired", 0), "unknown": node.get("unknown", 0),
                         "damage_ttk": distribution_stats(node.get("damage_ttk", {})),
                         "damage_lifetime": distribution_stats(node.get("damage_lifetime", {})),
                         "retired_lifetime": distribution_stats(node.get("retired_lifetime", {}))})
    return rows


def lifecycle_pairs(rows: list[dict], baseline: str) -> list[dict]:
    out = []
    subjects = {(row["side"], row["subject"]) for row in rows}
    for side, subject in sorted(subjects):
        candidates = [row for row in rows if row["side"] == side and row["subject"] == subject]
        controls = {(row["phase"], row["policy"], row["seed"]): row for row in candidates if row["variant"] == baseline}
        keys = {(row["phase"], row["policy"], row["variant"]) for row in candidates if row["variant"] != baseline}
        for phase, policy, variant in sorted(keys):
            pairs = [(row, controls.get((phase, policy, row["seed"]))) for row in candidates
                     if (row["phase"], row["policy"], row["variant"]) == (phase, policy, variant)]
            diffs, used, omitted = [], [], []
            for row, control in pairs:
                a = row["damage_ttk"]["mean_seconds"]
                b = control["damage_ttk"]["mean_seconds"] if control else None
                if a is None or b is None:
                    omitted.append(row["seed"])
                else:
                    diffs.append(a - b)
                    used.append(row["seed"])
            out.append({"phase": phase, "policy": policy, "variant": variant, "baseline": baseline,
                        "side": side, "subject": subject, "paired_seeds": used,
                        "omitted_no_damage_death_seeds": omitted, "damage_ttk_mean_seconds_delta": mean_stats(diffs)})
    return out


def wave_pressure_metrics(records: list[dict]) -> list[dict]:
    rows = []
    for record in records:
        report = record.get("report") or {}
        level = get_path(report, "replay_spec.level_document", get_path(report, "spec.level_document", {}))
        authored = level.get("waves", [])
        cumulative_ticks = 0
        for index, measured in enumerate(report.get("waves", [])):
            if index >= len(authored):
                continue
            spawns = authored[index].get("spawns", [])
            changes: dict[float, float] = {}
            density_changes: dict[float, float] = {}
            count, density, bursts, max_end = 0, 0.0, 0, 0.0
            for spawn in spawns:
                amount = spawn.get("count", 0)
                start = float(spawn.get("start_time", 0))
                duration = float(spawn.get("duration", 0))
                effective = config_documents(get_path(report, "provenance.effective_config"))
                base_density = get_path(effective, f"enemies.families.{spawn.get('family')}.behavior.base_density")
                count += amount
                max_end = max(max_end, start + duration)
                if finite_number(base_density):
                    density += amount * base_density
                if duration > 0:
                    rate = amount / duration
                    changes[start] = changes.get(start, 0.0) + rate
                    changes[start + duration] = changes.get(start + duration, 0.0) - rate
                    if finite_number(base_density):
                        density_changes[start] = density_changes.get(start, 0.0) + rate * base_density
                        density_changes[start + duration] = density_changes.get(start + duration, 0.0) - rate * base_density
                else:
                    bursts += amount
            running = peak = density_running = density_peak = 0.0
            for tick in sorted(changes):
                running += changes[tick]
                density_running += density_changes.get(tick, 0.0)
                peak, density_peak = max(peak, running), max(density_peak, density_running)
            begin = measured.get("start_tick", cumulative_ticks)
            end = measured.get("end_tick", begin + measured.get("ticks", 0))
            cumulative_ticks = end
            deploys = [a for a in report.get("actions", []) if a.get("kind") == "deploy" and begin <= a.get("tick", -1) < end]
            duration_seconds = measured.get("ticks", 0) / 60
            samples = [s for s in report.get("timeline", []) if begin <= s.get("tick", -1) <= end]
            income = {}
            if len(samples) >= 2:
                for key in ("damage_income", "wave_income", "spent"):
                    first, last = get_path(samples[0], f"economy.{key}"), get_path(samples[-1], f"economy.{key}")
                    if finite_number(first) and finite_number(last):
                        income[key] = last - first
            rows.append({"run_id": record["id"], "phase": record["phase"], "policy": record["policy"], "variant": record["variant"],
                         "seed": record["seed"], "wave_index": index, "authored_count": count,
                         "authored_release_span_seconds": max_end, "authored_peak_agents_per_second": peak,
                         "authored_density": density if density_changes else None,
                         "authored_peak_density_per_second": density_peak if density_changes else None,
                         "instantaneous_burst_agents": bursts, "observed_interval_seconds": duration_seconds,
                         "interval_density_removed_per_second": measured.get("density_removed", 0) / duration_seconds if duration_seconds else None,
                         "cells_bought_in_interval": sum(a.get("deployed", 0) for a in deploys),
                         "atp_spent_in_interval": sum(a.get("atp_spent", 0) for a in deploys),
                         "sampled_economy_deltas": income, "clearing_timeout": measured.get("clearing_timeout", False),
                         "scope": "Authored release rates are continuous scheduling targets, not measured contact arrival. Interval combat includes prep and carryover; sampled economy deltas can miss boundary ticks."})
    return rows


def progression_snapshots(record: dict, probe: dict) -> list[dict]:
    attempts = (record.get("report") or {}).get("attempts", [])
    if not attempts:
        return []
    initial = attempts[0].get("progression_before")
    snapshots = [{"id": "initial", "level_index": -1, "x": 0,
                  "basis": "before first campaign attempt", "progression": initial,
                  "opening_atp": get_path(attempts[0], "economy.opening_atp")}] if initial is not None else []
    clears = [(index, a) for index, a in enumerate(attempts) if get_path(a, "summary.status") == "cleared" and get_path(a, "summary.valid") is True]
    every = probe.get("every_n_clears", 1)
    for clear_number, (attempt_index, attempt) in enumerate(clears, 1):
        if clear_number % every and clear_number != len(clears):
            continue
        progression = attempt.get("progression_after")
        opening_atp = get_path(attempt, "economy.opening_atp")
        basis = "after clear, before future purchases"
        if probe.get("snapshot_basis", "next_attempt") == "next_attempt" and attempt_index + 1 < len(attempts):
            progression = attempts[attempt_index + 1].get("progression_before", progression)
            opening_atp = get_path(attempts[attempt_index + 1], "economy.opening_atp")
            basis = "before next attempt, including actual selected purchases"
        if progression is None:
            continue
        level = attempt.get("level_index", clear_number - 1)
        snapshots.append({"id": f"after-level-{level}", "level_index": level, "x": level + 1,
                          "basis": basis, "progression": progression, "opening_atp": opening_atp})
    return [s for s in snapshots if "snapshot_ids" not in probe or s["id"] in probe["snapshot_ids"]]


def plan_power_runs(records: list[dict], experiment: dict) -> list[dict]:
    probe = experiment.get("power_probe")
    if not probe:
        return []
    base = normalize_spec(probe["base"])
    canonical = base.get("level_document")
    if canonical is None:
        canonical = read_json(Path(base["level"]))
    runs = []
    for journey in records:
        if (journey.get("report") or {}).get("kind") != "campaign" or journey.get("valid") is not True:
            continue
        attempts = journey["report"].get("attempts", [])
        pinned_config = get_path(attempts[0], "provenance.base_config") if attempts else None
        for snapshot in progression_snapshots(journey, probe):
            for wallet_mode in probe.get("wallet_modes", [probe.get("wallet_mode", "fixed")]):
                if wallet_mode == "journey_opening" and (not finite_number(snapshot.get("opening_atp")) or snapshot["opening_atp"] < 0):
                    raise ValueError("journey_opening power probes require actual economy.opening_atp in every selected attempt")
                wallet = int(snapshot["opening_atp"]) if wallet_mode == "journey_opening" else base["opening_atp"]
                spend_limit = wallet if wallet_mode == "journey_opening" else base["spend_limit"]
                for scale in sorted(probe["pressure_scales"]):
                    # JSON/argparse may represent an equivalent scale as 1 or
                    # 1.0. Keep its run identity stable during grid refinement.
                    scale = int(scale) if float(scale).is_integer() else float(scale)
                    document = copy.deepcopy(canonical)
                    for wave in document.get("waves", []):
                        for spawn in wave.get("spawns", []):
                            # Preserve positive route use and authored timings.
                            # Only population changes, not density or geometry.
                            spawn["count"] = max(1, int(math.floor(spawn.get("count", 0) * scale + .5))) if spawn.get("count", 0) else 0
                    for seed in probe["seeds"]:
                        spec = copy.deepcopy(base)
                        spec.pop("level", None)
                        spec["level_document"] = document
                        spec["seed"] = seed
                        spec["opening_atp"] = wallet
                        spec["spend_limit"] = spend_limit
                        spec["progression_snapshot"] = snapshot["progression"]
                        spec["progression"] = {}  # Ranks come from the real snapshot.
                        identity = {"journey": journey["id"], "snapshot": snapshot["id"], "wallet_mode": wallet_mode, "scale": scale, "seed": seed, "spec": spec}
                        digest = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:12]
                        runs.append({"id": f"power-{slug(journey['id'])}-{snapshot['id']}-{wallet_mode}-{scale}-{seed}-{digest}",
                                     "kind": "power_probe", "phase": "power", "policy": f"{get_path(base, 'controller.policy', 'competent')} probe / {journey['policy']} journey",
                                     "variant": f"{journey['variant']}:{snapshot['id']}:{wallet_mode}:pressure={scale}", "seed": seed, "spec": spec,
                                     "pinned_config": pinned_config,
                                     "expected_executable_hash": get_path(journey["report"], "provenance.executable_hash"),
                                     "expected_base_config_hash": get_path(journey["report"], "provenance.base_config_hash") if not base.get("overrides") else None,
                                     "probe": {"journey_id": journey["id"], "snapshot_id": snapshot["id"], "pressure_scale": scale,
                                               "level_index": snapshot["level_index"], "x": snapshot["x"], "basis": snapshot["basis"],
                                               "wallet_mode": wallet_mode, "controller": base.get("controller", {}),
                                               "opening_atp": wallet, "spend_limit": spend_limit,
                                               "source_executable_hash": get_path(journey["report"], "provenance.executable_hash"),
                                               "source_build_stamp": get_path(journey["report"], "provenance.build_stamp"),
                                               "source_base_config_hash": get_path(journey["report"], "provenance.base_config_hash")}})
    return runs


def log_growth(points: list[tuple[float, float]]) -> dict:
    points = sorted((float(x), float(y)) for x, y in points if finite_number(x) and finite_number(y) and y > 0)
    ratios = [{"from": a[0], "to": b[0], "factor": b[1] / a[1],
               "factor_per_level": (b[1] / a[1]) ** (1 / (b[0] - a[0]))}
              for a, b in zip(points, points[1:]) if b[0] > a[0]]
    result = {"points": [{"level": x, "value": y} for x, y in points], "ratios": ratios,
              "geometric_factor_per_level": None, "log_linear_factor_per_level": None, "log_fit_r_squared": None}
    if len(points) < 2 or points[-1][0] <= points[0][0]:
        return result
    result["geometric_factor_per_level"] = (points[-1][1] / points[0][1]) ** (1 / (points[-1][0] - points[0][0]))
    xs, ys = [p[0] for p in points], [math.log(p[1]) for p in points]
    xm, ym = statistics.mean(xs), statistics.mean(ys)
    xx = sum((x - xm) ** 2 for x in xs)
    if xx == 0:
        return result
    slope = sum((x - xm) * (y - ym) for x, y in zip(xs, ys)) / xx
    residual = sum((y - (ym + slope * (x - xm))) ** 2 for x, y in zip(xs, ys))
    total = sum((y - ym) ** 2 for y in ys)
    result["log_linear_factor_per_level"] = math.exp(slope)
    result["log_fit_r_squared"] = 1 - residual / total if total > 0 else None
    return result


def power_calibrations(records: list[dict], experiment: dict) -> list[dict]:
    probe = experiment.get("power_probe", {})
    groups: dict[tuple[str, str, str], list[dict]] = {}
    for r in records:
        if "probe" in r:
            groups.setdefault((r["probe"]["journey_id"], r["probe"]["snapshot_id"], r["probe"].get("wallet_mode", "fixed")), []).append(r)
    results = []
    scales, seeds = sorted(probe.get("pressure_scales", [])), probe.get("seeds", [])
    required = probe.get("minimum_clear_rate", 1.0)
    for (journey_id, snapshot, wallet_mode), rows in groups.items():
        grid = []
        for scale in scales:
            samples = [r for r in rows if r["probe"]["pressure_scale"] == scale]
            terminal = [r for r in samples if is_terminal(r)]
            complete = len(terminal) == len(seeds) and {r["seed"] for r in terminal} == set(seeds)
            wins = sum(r["status"] in SUCCESS for r in terminal)
            rate = wins / len(terminal) if terminal else None
            grid.append({"pressure_scale": scale, "complete": complete, "terminal": len(terminal),
                         "clear_rate": rate, "ci95": wilson(wins, len(terminal)),
                         "passing": bool(complete and rate is not None and rate >= required)})
        passing = [g["pressure_scale"] for g in grid if g["passing"]]
        lower = max(passing) if passing else 0.0
        failing_above = [g["pressure_scale"] for g in grid if g["complete"] and not g["passing"] and g["pressure_scale"] > lower]
        upper = min(failing_above) if failing_above else None
        violations = []
        for seed in seeds:
            failed_at = None
            for r in sorted([r for r in rows if r["seed"] == seed and is_terminal(r)], key=lambda r: r["probe"]["pressure_scale"]):
                scale = r["probe"]["pressure_scale"]
                if r["status"] not in SUCCESS:
                    failed_at = scale
                elif failed_at is not None:
                    violations.append({"seed": seed, "failed_pressure": failed_at, "cleared_pressure": scale})
        unresolved = [g["pressure_scale"] for g in grid if not g["complete"]]
        bracketed = lower > 0 and upper is not None and not violations and not unresolved
        results.append({**rows[0]["probe"], "wallet_mode": wallet_mode, "grid": grid, "minimum_clear_rate": required,
                        "capacity_lower_scale": lower, "capacity_upper_scale": upper,
                        "capacity_estimate_scale": math.sqrt(lower * upper) if bracketed else None,
                        "right_censored": upper is None and bool(passing), "left_censored": not passing and upper is not None,
                        "unresolved_scales": unresolved, "monotonic_violations": violations,
                        "interpretation": "Empirical pressure capacity at the recorded wallet under the same probe controller; the geometric bracket midpoint is descriptive, not a measured exact threshold."})
    for journey_id, wallet_mode in {(r["journey_id"], r["wallet_mode"]) for r in results}:
        stages = sorted([r for r in results if r["journey_id"] == journey_id and r["wallet_mode"] == wallet_mode], key=lambda r: r["x"])
        initial = next((r for r in stages if r["snapshot_id"] == "initial"), None)
        for stage in stages:
            if stage is initial:
                # The numerator and denominator are the same measured stage;
                # treating its bracket as independent would invent uncertainty
                # around a relative baseline that is one by definition.
                baseline = 1.0 if stage["capacity_estimate_scale"] else None
                stage["relative_to_initial_estimate"] = baseline
                stage["relative_to_initial_lower"] = baseline
                stage["relative_to_initial_upper"] = baseline
                continue
            stage["relative_to_initial_estimate"] = (stage["capacity_estimate_scale"] / initial["capacity_estimate_scale"]
                if initial and initial["capacity_estimate_scale"] and stage["capacity_estimate_scale"] else None)
            stage["relative_to_initial_lower"] = stage["capacity_lower_scale"] / initial["capacity_upper_scale"] if initial and initial["capacity_upper_scale"] else None
            stage["relative_to_initial_upper"] = stage["capacity_upper_scale"] / initial["capacity_lower_scale"] if initial and initial["capacity_lower_scale"] and stage["capacity_upper_scale"] else None
    return results


def growth_analysis(records: list[dict], experiment: dict) -> list[dict]:
    calibrations = power_calibrations(records, experiment)
    out = []
    for record in records:
        report = record.get("report") or {}
        if report.get("kind") != "campaign":
            continue
        design_docs = get_path(report, "provenance.level_documents", [])
        if isinstance(design_docs, dict):
            design_docs = list(design_docs.values())
        enemy_rows = []
        for index, document in enumerate(design_docs):
            if isinstance(document, dict) and "document" in document:
                document = document["document"]
            total, peak = 0, 0.0
            for wave in document.get("waves", []):
                events: dict[float, float] = {}
                for spawn in wave.get("spawns", []):
                    count, duration = spawn.get("count", 0), spawn.get("duration", 0)
                    start = spawn.get("start_time", 0)
                    total += count
                    if duration > 0:
                        events[start] = events.get(start, 0.0) + count / duration
                        events[start + duration] = events.get(start + duration, 0.0) - count / duration
                rate = 0
                for tick in sorted(events):
                    rate += events[tick]
                    peak = max(peak, rate)
            reached = [a for a in report.get("attempts", []) if a.get("level_index") == index]
            last = reached[-1] if reached else {}
            enemy_rows.append({"level_index": index, "level": document.get("name", str(index)), "authored_population": total,
                               "authored_peak_release_agents_per_second": peak, "attempts": len(reached),
                               "cleared": any(get_path(a, "summary.status") == "cleared" for a in reached),
                               "observed_spawned": max((get_path(a, "state.spawned", 0) for a in reached), default=None),
                               "actual_opening_atp": get_path(last, "economy.opening_atp"),
                               "surviving_friendly_count": get_path(last, "summary.friendly_alive"),
                               "surviving_friendly_hp": get_path(last, "summary.friendly_remaining_hp"),
                               "surviving_units": {name: {"alive": unit.get("alive"), "remaining_hp": unit.get("remaining_hp")}
                                                   for name, unit in last.get("units", {}).items()},
                               "observed_peak_enemies": max((get_path(a, "summary.peak_enemies", 0) for a in reached), default=None)})
        # Older reports can still analyze reached levels, without claiming they
        # describe the unseen rest of a campaign.
        if not enemy_rows:
            for a in report.get("attempts", []):
                index = a.get("level_index", 0)
                if any(row["level_index"] == index for row in enemy_rows):
                    continue
                document = get_path(a, "replay_spec.level_document", {})
                pressure = wave_pressure_metrics([{**record, "report": a}])
                enemy_rows.append({"level_index": index, "level": document.get("name", str(index)),
                                   "authored_population": sum(p["authored_count"] for p in pressure),
                                   "authored_peak_release_agents_per_second": max((p["authored_peak_agents_per_second"] for p in pressure), default=0),
                                   "attempts": sum(b.get("level_index", 0) == index for b in report.get("attempts", [])),
                                   "observed_peak_enemies": get_path(a, "summary.peak_enemies"),
                                   "cleared": any(get_path(b, "summary.status") == "cleared" and b.get("level_index", 0) == index for b in report.get("attempts", []))})
        stages = sorted([c for c in calibrations if c["journey_id"] == record["id"]], key=lambda c: c["x"])
        enemies = log_growth([(row["level_index"], row["authored_population"]) for row in enemy_rows])
        arrival = log_growth([(row["level_index"], row["authored_peak_release_agents_per_second"]) for row in enemy_rows])
        powers = {mode: log_growth([(row["x"], row["capacity_estimate_scale"]) for row in stages
                                  if row["wallet_mode"] == mode and row["capacity_estimate_scale"] is not None])
                  for mode in {row["wallet_mode"] for row in stages}}
        primary_mode = "journey_opening" if "journey_opening" in powers else "fixed"
        power = powers.get(primary_mode, log_growth([]))
        comparisons = {}
        for name, fit, target_key in (("enemies", enemies, "enemy_factor_per_level"), ("player", power, "player_factor_per_level")):
            target = experiment.get("growth_targets", {}).get(target_key)
            observed = fit["geometric_factor_per_level"]
            censored = name == "player" and any(s["capacity_estimate_scale"] is None for s in stages if s["wallet_mode"] == primary_mode)
            comparisons[name] = {"hypothesis_factor_per_level": target, "observed_geometric_factor_per_level": observed,
                                 "comparison": "inconclusive" if target is None or observed is None or censored else "at_or_above_hypothesis" if observed >= target else "below_hypothesis",
                                 "incomplete_or_censored": censored}
        out.append({"journey_id": record["id"], "policy": record["policy"], "variant": record["variant"], "seed": record["seed"],
                    "enemy_levels": enemy_rows, "enemy_population_growth": enemies, "enemy_release_growth": arrival,
                    "player_pressure_growth": power, "power_calibration": stages, "hypotheses": comparisons,
                    "player_pressure_growth_by_wallet": powers, "primary_player_wallet_mode": primary_mode,
                    "scope": "Enemy growth is authored population and arrival targets. Fixed-wallet pressure capacity isolates upgrades/cost efficiency; journey-opening capacity includes actual opening resources. Neither is raw DPS or summed upgrade ranks. In-run reinvestment remains disabled in probes and is analyzed separately in real journeys."})
    return out


def controller_adequacy(records: list[dict], experiment: dict) -> dict:
    """Compare the controller with declared known-achievable anchors.

    Valid simulation, a passing easy synthetic grid, and human-like competence
    are separate questions. Loss alone cannot diagnose a game's balance.
    """
    references = experiment.get("controller_calibration", {}).get("references", [])
    observations = []
    for record in records:
        if record.get("kind") == "power_probe" or record.get("phase") == "training":
            continue
        report = record.get("report") or {}
        attempts = report.get("attempts", []) if report.get("kind") == "campaign" else [report]
        for index, attempt in enumerate(attempts):
            level = get_path(attempt, "replay_spec.level_document.name", get_path(attempt, "spec.level_document.name"))
            if level is None:
                level = Path(get_path(attempt, "spec.level", "unknown")).stem
            progression = attempt.get("progression_before", get_path(attempt, "provenance.progression", {}))
            tree = progression.get("tree") if isinstance(progression, dict) else None
            fresh = isinstance(tree, dict) and not any(finite_number(rank) and rank > 0 for rank in tree.values())
            fresh = fresh and get_path(attempt, "provenance.synthetic_progression") is not True
            fresh = fresh and get_path(attempt, "spec.progression.profile", "campaign") != "all_unlocked"
            observations.append({"record_id": record["id"], "attempt_index": index, "policy": record["policy"],
                                 "variant": record["variant"], "seed": get_path(attempt, "provenance.seed", record["seed"]),
                                 "level": level, "fresh": fresh, "summary": attempt.get("summary", {}),
                                 "automatic_input": "actions" not in attempt.get("spec", {}),
                                 "standard_rules": get_path(attempt, "provenance.experimental_rules") is False
                                     and not get_path(attempt, "spec.overrides", {})})
    checks = []
    for reference in references:
        requested_policies = reference.get("policies", sorted({r["policy"] for r in records if r.get("kind") != "power_probe"}))
        variants = reference.get("variants", [experiment.get("baseline", "baseline")])
        level = Path(reference["level"]).stem
        for policy in requested_policies:
            candidates = [o for o in observations if o["policy"] == policy and o["variant"] in variants and o["level"] == level]
            candidates = [o for o in candidates if reference.get("profile", "fresh") != "fresh" or o["fresh"]]
            if reference.get("automatic_controller_only", True):
                candidates = [o for o in candidates if o["automatic_input"]]
            if reference.get("require_standard_rules", True):
                candidates = [o for o in candidates if o["standard_rules"]]
            terminal = [o for o in candidates if o["summary"].get("valid") is True and o["summary"].get("status") in {"cleared", "defeated"}]
            def meets(observation):
                summary = observation["summary"]
                if summary.get("waves_cleared", 0) < reference.get("minimum_waves_cleared", 1):
                    return False
                return "expected_status" not in reference or summary.get("status") == reference["expected_status"]
            successes = sum(meets(o) for o in terminal)
            enough = len(terminal) >= reference.get("minimum_samples", 1)
            rate = successes / len(terminal) if terminal else None
            status = "inconclusive" if not enough or len(terminal) != len(candidates) else "passed" if rate >= reference.get("minimum_success_rate", 1.0) else "failed"
            checks.append({"name": reference["name"], "policy": policy, "level": level, "profile": reference.get("profile", "fresh"),
                           "source": reference["source"], "criteria": reference, "status": status, "observed": len(candidates),
                           "terminal": len(terminal), "successful": successes, "success_rate": rate,
                           "success_rate_ci95": wilson(successes, len(terminal)),
                           "observations": [{"record_id": o["record_id"], "attempt_index": o["attempt_index"], "seed": o["seed"],
                                             "status": o["summary"].get("status"), "waves_cleared": o["summary"].get("waves_cleared", 0)} for o in candidates]})
    if not references:
        status = "uncalibrated"
        interpretation = "No known-achievable reference anchors were supplied. Controller losses do not establish that the game or progression is too difficult."
    elif any(c["status"] == "failed" for c in checks):
        status = "reference_failed"
        interpretation = "The controller failed a declared known-achievable reference. Treat this as a controller/input calibration failure; gameplay balance remains inconclusive. Diagnose tactics and reproduce the player reference before recommending gameplay tuning."
    elif not checks or any(c["status"] == "inconclusive" for c in checks):
        status = "inconclusive"
        interpretation = "Reference coverage is incomplete or unresolved. Gameplay balance remains inconclusive."
    else:
        status = "reference_passed"
        interpretation = "The controller reproduced the declared reference criteria in this sample. This supports comparisons within that reference scope; it does not establish human skill calibration or justify an automatic balance change."
    return {"status": status, "interpretation": interpretation, "reference_checks": checks,
            "automatic_gameplay_tuning_recommendation": None,
            "reference_scope_comparisons_supported": status == "reference_passed",
            "qualified_scopes": [{"name": c["name"], "policy": c["policy"], "level": c["level"],
                                   "profile": c["profile"], "criteria": c["criteria"]} for c in checks if c["status"] == "passed"],
            "distinction": "Pressure-grid calibration measures this controller's capacity. Controller adequacy requires separate known-achievable gameplay references; valid simulation alone does not prove adequate play."}


def search_selection(records: list[dict], experiment: dict) -> tuple[set[tuple[str, str]], list[dict]]:
    metric = experiment.get("metric", "objective_integrity")
    maximize = experiment.get("maximize", True)
    rankings = []
    expected = len(experiment["train_seeds"])
    for (phase, policy, variant), rows in grouped(records).items():
        if phase != "training":
            continue
        terminal = [r for r in rows if is_terminal(r)]
        values = [v for r in terminal if (v := metric_value(r, metric)) is not None]
        complete = len(terminal) == expected and len(rows) == expected and len(values) == expected
        win_rate = sum(r["status"] in SUCCESS for r in terminal) / expected
        metric_mean = statistics.mean(values) if values else None
        rankings.append({"policy": policy, "variant": variant, "complete": complete,
                         "terminal": len(terminal), "win_rate": win_rate, "metric_mean": metric_mean})
    selected: set[tuple[str, str]] = set()
    for policy in {row["policy"] for row in rankings}:
        eligible = [row for row in rankings if row["policy"] == policy and row["complete"]]
        eligible.sort(key=lambda row: (-row["win_rate"], -(row["metric_mean"] or 0) if maximize else row["metric_mean"] or 0, row["variant"]))
        for rank, row in enumerate(eligible, 1):
            row["rank"] = rank
            if rank <= experiment.get("top_k", 1):
                selected.add((policy, row["variant"]))
    return selected, rankings


def budget_thresholds(records: list[dict], experiment: dict) -> list[dict]:
    target = experiment.get("target_win_rate", 1.0)
    out = []
    for policy in sorted({r["policy"] for r in records}):
        rows = []
        for budget in sorted(experiment["budgets"]):
            samples = [r for r in records if r["policy"] == policy and r.get("budget") == budget]
            terminal = [r for r in samples if is_terminal(r)]
            complete = len(terminal) == len(experiment.get("seeds", [101, 202, 303])) and len(samples) == len(terminal)
            rate = sum(r["status"] in SUCCESS for r in terminal) / len(terminal) if terminal else None
            rows.append({"budget": budget, "complete": complete, "clear_rate": rate,
                         "clear_rate_ci95": wilson(sum(r["status"] in SUCCESS for r in terminal), len(terminal)),
                         "meets_target": bool(complete and rate is not None and rate >= target)})
        passing = [r["budget"] for r in rows if r["meets_target"]]
        threshold = min(passing) if passing else None
        failed_below = [r["budget"] for r in rows if r["complete"] and not r["meets_target"] and (threshold is None or r["budget"] < threshold)]
        monotonic_violations = []
        by_seed: dict[int, list[dict]] = {}
        for r in records:
            if r["policy"] == policy and is_terminal(r):
                by_seed.setdefault(r["seed"], []).append(r)
        for seed, samples in by_seed.items():
            prior_success = None
            for sample in sorted(samples, key=lambda r: r["budget"]):
                if sample["status"] in SUCCESS:
                    prior_success = sample["budget"]
                elif prior_success is not None:
                    monotonic_violations.append({"seed": seed, "won_budget": prior_success, "lost_budget": sample["budget"]})
        out.append({"policy": policy, "target_win_rate": target, "lowest_tested_passing_budget": threshold,
                    "threshold_basis": "Observed terminal clear rate on this fixed seed grid, not a confidence-bound guarantee",
                    "highest_tested_failing_budget_below": max(failed_below) if failed_below else None,
                    "inconclusive_budgets": [r["budget"] for r in rows if not r["complete"]],
                    "monotonic_violations": monotonic_violations, "grid": rows,
                    "interpretation": "Empirical tested grid under this controller; not a proof of the minimum possible budget"})
    return out


def summarize(records: list[dict], experiment: dict) -> dict:
    metric = experiment.get("metric", "objective_integrity")
    primary_records = [r for r in records if r.get("kind") != "power_probe"]
    groups = []
    for (phase, policy, variant), rows in sorted(grouped(primary_records).items()):
        terminal = [r for r in rows if is_terminal(r)]
        wins = sum(r["status"] in SUCCESS for r in terminal)
        values = [v for r in terminal if (v := metric_value(r, metric)) is not None]
        groups.append({"phase": phase, "policy": policy, "variant": variant, "attempted": len(rows),
                       "terminal": len(terminal), "invalid": sum(not r.get("valid") and r["status"] != "timeout" for r in rows),
                       "timeouts": sum(r["status"] == "timeout" for r in rows), "wins": wins,
                       "clear_rate": wins / len(terminal) if terminal else None, "clear_rate_ci95": wilson(wins, len(terminal)),
                       "clear_rate_lower_bound_all_attempted": wins / len(rows), "metric": metric, "metric_stats": mean_stats(values),
                       "seeds": [r["seed"] for r in rows]})
    out = {"schema": 1, "experiment": experiment.get("name", "Unnamed experiment"), "kind": experiment.get("kind", "paired"),
           "interpretation": "Conditional on these controllers, settings and seeds. Seed variation is not human skill variation. Timeouts and invalid runs are unresolved, never implicit victories or ordinary defeats.",
           "groups": groups, "paired": paired_comparisons(primary_records, experiment.get("baseline", "baseline"), metric),
           "invalid_runs": [{"id": r["id"], "error": r.get("error", ""), "status": r["status"]} for r in records if not is_terminal(r)],
           "campaigns": [{"id": r["id"], "policy": r["policy"], "seed": r["seed"], **campaign_metrics(r)} for r in records if (r.get("report") or {}).get("kind") == "campaign"]}
    out["lifecycles"] = lifecycle_metrics(records)
    out["progression_journeys"] = progression_journeys(records, experiment)
    out["paired_ttk"] = lifecycle_pairs(out["lifecycles"], experiment.get("baseline", "baseline"))
    out["wave_pressure"] = wave_pressure_metrics(records)
    out["power_calibrations"] = power_calibrations(records, experiment)
    out["growth"] = growth_analysis(records, experiment)
    out["controller_adequacy"] = controller_adequacy(records, experiment)
    out["gameplay_balance_conclusion"] = "Controller outcomes alone do not establish a gameplay balance defect or a tuning recommendation."
    for growth in out["growth"]:
        growth["controller_adequacy_status"] = out["controller_adequacy"]["status"]
        growth["gameplay_balance_interpretation"] = "Controller-conditioned capacity measurements; controller adequacy and human reference scope must be assessed before gameplay conclusions."
    metrics = experiment.get("metrics", [])
    out["additional_metrics"] = [{"metric": m, "groups": [
        {"phase": phase, "policy": policy, "variant": variant,
         **mean_stats([v for r in rows if is_terminal(r) and (v := metric_value(r, m)) is not None])}
        for (phase, policy, variant), rows in sorted(grouped(primary_records).items())],
        "paired": paired_comparisons(primary_records, experiment.get("baseline", "baseline"), m)} for m in metrics if m != metric]
    if out["kind"] == "sweep":
        out["sensitivity_ranking"] = sorted(
            [row for row in out["paired"] if row["metric_delta"]["mean"] is not None],
            key=lambda row: (row["policy"], -abs(row["metric_delta"]["mean"])))
    if out["kind"] == "budget":
        out["budget_thresholds"] = budget_thresholds(records, experiment)
    if out["kind"] == "search":
        selected, rankings = search_selection(records, experiment)
        out["search"] = {"selected": [{"policy": p, "variant": v} for p, v in sorted(selected)], "training_rankings": rankings,
                         "warning": "Training ranks select candidates. Only heldout rows estimate selected-strategy performance. Do not pool both phases."}
    return out


def fmt(value, digits=2) -> str:
    return "—" if value is None else f"{value:.{digits}f}" if finite_number(value) else str(value)


def timeline_svg(report: dict) -> str:
    series = report.get("timeline", [])
    points = []
    for index, sample in enumerate(series):
        integrity = sample.get("objective_integrity", sample.get("integrity"))
        tick = sample.get("tick", index)
        if finite_number(integrity) and finite_number(tick):
            points.append((tick, integrity))
    if len(points) < 2:
        return ""
    max_tick = max(p[0] for p in points) or 1
    max_integrity = max(100, max(p[1] for p in points))
    path = " ".join(f"{20 + 600 * t / max_tick:.1f},{110 - 90 * y / max_integrity:.1f}" for t, y in points)
    return f'<svg viewBox="0 0 640 130" role="img" aria-label="Organ integrity over time"><path d="M20 15V110H625" fill="none" stroke="#455465"/><polyline points="{path}" fill="none" stroke="#6ed6af" stroke-width="2"/><text x="20" y="128">0</text><text x="550" y="128">{max_tick / 60:.1f} seconds</text></svg>'


def growth_svg(growth: dict) -> str:
    enemies = growth["enemy_population_growth"]["points"]
    first = enemies[0]["value"] if enemies else None
    enemy_points = [(p["level"], p["value"] / first) for p in enemies] if first else []
    fixed_points = [(p["x"], p["relative_to_initial_estimate"]) for p in growth["power_calibration"] if p["relative_to_initial_estimate"] is not None and p["wallet_mode"] == "fixed"]
    opening_points = [(p["x"], p["relative_to_initial_estimate"]) for p in growth["power_calibration"] if p["relative_to_initial_estimate"] is not None and p["wallet_mode"] == "journey_opening"]
    points = enemy_points + fixed_points + opening_points
    if len(points) < 2:
        return '<p>Insufficient uncensored points for a growth chart.</p>'
    max_x = max(p[0] for p in points) or 1
    logs = [math.log(p[1]) for p in points if p[1] > 0]
    low, high = min(0, min(logs)), max(math.log(1.1), max(logs))
    def path(values):
        return " ".join(f"{40 + 580 * x / max_x:.1f},{175 - 140 * (math.log(y) - low) / (high - low):.1f}" for x, y in values if y > 0)
    return f'<svg viewBox="0 0 650 245" role="img" aria-label="Log growth: enemy population and calibrated player capacity"><path d="M40 25V175H625" fill="none" stroke="#455465"/><polyline points="{path(enemy_points)}" fill="none" stroke="#ffb271" stroke-width="2"/><polyline points="{path(fixed_points)}" fill="none" stroke="#6ed6af" stroke-width="2"/><polyline points="{path(opening_points)}" fill="none" stroke="#8dbbff" stroke-width="2"/><text x="45" y="20">Relative growth, logarithmic y scale</text><text x="45" y="195">Level transitions / clears</text><text x="45" y="215" style="fill:#ffb271">Enemy population</text><text x="220" y="215" style="fill:#6ed6af">Fixed-wallet efficiency</text><text x="45" y="235" style="fill:#8dbbff">Actual-opening army capacity</text><text x="3" y="175">{math.exp(low):.1f}x</text><text x="3" y="35">{math.exp(high):.1f}x</text></svg>'


def indicator_svg(growth: dict, key: str, label: str, color: str) -> str:
    points = [(row["level_index"], row.get(key)) for row in growth["enemy_levels"] if finite_number(row.get(key))]
    if not points:
        return ""
    max_x, max_y = max(p[0] for p in points) or 1, max(p[1] for p in points) or 1
    coords = [(40 + 580 * x / max_x, 125 - 90 * y / max_y) for x, y in points]
    path = " ".join(f"{x:.1f},{y:.1f}" for x, y in coords)
    dots = "".join(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="3" fill="{color}"/>' for x, y in coords)
    return f'<svg viewBox="0 0 650 155" role="img" aria-label="{html.escape(label, quote=True)}"><path d="M40 25V125H625" fill="none" stroke="#455465"/><polyline points="{path}" fill="none" stroke="{color}" stroke-width="2"/>{dots}<text x="45" y="20">{html.escape(label)}</text><text x="3" y="35">{max_y:.0f}</text><text x="3" y="125">0</text><text x="45" y="150">Reached level index (0-based), latest actual attempt</text></svg>'


def render_html(summary: dict, records: list[dict]) -> str:
    esc = lambda value: html.escape(str(value), quote=True)
    group_rows = []
    for group in summary["groups"]:
        rate = group["clear_rate"]
        ci = group["clear_rate_ci95"]
        interval = f"{100 * ci[0]:.1f}–{100 * ci[1]:.1f}%" if ci else "—"
        bar = f'<span class="bar"><span style="width:{100 * rate:.1f}%"></span></span>' if rate is not None else ""
        group_rows.append(f'<tr><td>{esc(group["phase"])}</td><td>{esc(group["policy"])}</td><td>{esc(group["variant"])}</td><td>{group["wins"]}/{group["terminal"]} {bar}</td><td>{interval}</td><td>{group["timeouts"]}</td><td>{group["invalid"]}</td><td>{fmt(group["metric_stats"]["mean"])}</td></tr>')
    pairs = []
    for row in summary["paired"]:
        stats = row["metric_delta"]
        ci = stats["ci95"]
        pairs.append(f'<tr><td>{esc(row["phase"])}</td><td>{esc(row["policy"])}</td><td>{esc(row["variant"])}</td><td>{len(row["paired_seeds"])}</td><td>{fmt(stats["mean"])}</td><td>{fmt(ci[0]) + " to " + fmt(ci[1]) if ci else "insufficient sample"}</td><td>{len(row["omitted_seeds"])}</td></tr>')
    run_rows, charts = [], []
    for record in records:
        report_link = f'<a href="{esc(record["report_path"])}">raw report / actions</a>' if record.get("report") else "no report"
        run_rows.append(f'<tr><td>{esc(record["phase"])}</td><td>{esc(record["policy"])}</td><td>{esc(record["variant"])}</td><td>{record["seed"]}</td><td class="{esc(record["status"])}">{esc(record["status"])}</td><td><a href="{esc(record["spec_path"])}">replay spec</a> · {report_link} · <a href="{esc(record["stderr_path"])}">log</a></td><td>{esc(record.get("error", ""))}</td></tr>')
        chart = timeline_svg(record.get("report") or {})
        if chart:
            charts.append(f'<details><summary>{esc(record["policy"])} / {esc(record["variant"])} / seed {record["seed"]}: integrity</summary>{chart}</details>')
    campaigns = []
    for campaign in summary["campaigns"]:
        campaigns.append(f'<tr><td>{esc(campaign["policy"])}</td><td>{campaign["seed"]}</td><td>{campaign["levels_cleared"]}</td><td>{campaign["attempts"]}</td><td>{campaign["retries"]}</td><td>{campaign["memory_cells_earned"]}</td><td>{campaign["antibodies_earned"]}</td></tr>')
    extras = ""
    if summary.get("growth"):
        growth_parts = []
        for growth in summary["growth"]:
            stages = []
            for stage in growth["power_calibration"]:
                upper = fmt(stage["capacity_upper_scale"]) if stage["capacity_upper_scale"] is not None else "unbounded by tested grid"
                stages.append(f'<tr><td>{esc(stage["snapshot_id"])}</td><td>{esc(stage["wallet_mode"])}</td><td>{fmt(stage.get("opening_atp"),0)}</td><td>{esc(stage["basis"])}</td><td>{fmt(stage["capacity_lower_scale"])} to {upper}</td><td>{fmt(stage["capacity_estimate_scale"])}</td><td>{fmt(stage["relative_to_initial_lower"])} to {fmt(stage["relative_to_initial_upper"])}</td><td>{esc(stage["unresolved_scales"])}</td><td>{len(stage["monotonic_violations"])}</td></tr>')
            indicators = indicator_svg(growth, "actual_opening_atp", "Actual starting ATP per reached level", "#8dbbff") + indicator_svg(growth, "surviving_friendly_count", "Surviving friendly count at attempt end", "#6ed6af") + indicator_svg(growth, "surviving_friendly_hp", "Remaining friendly HP at attempt end (descriptive, not power)", "#dfb3ff")
            growth_parts.append(f'<h3>{esc(growth["policy"])} / {esc(growth["variant"])} / seed {growth["seed"]}</h3>{growth_svg(growth)}{indicators}<p>Enemy geometric factor/level: {fmt(growth["enemy_population_growth"]["geometric_factor_per_level"],3)}. Player bracket-midpoint factor/level ({esc(growth["primary_player_wallet_mode"])}): {fmt(growth["player_pressure_growth"]["geometric_factor_per_level"],3)}. Player estimates require finite resolved brackets; missing or censored capacity is not zero.</p><table><thead><tr><th>Snapshot</th><th>Wallet scope</th><th>Starting ATP</th><th>Progression basis</th><th>Tested pressure bracket</th><th>Geometric midpoint</th><th>Relative capacity bounds</th><th>Unresolved scales</th><th>Monotonicity violations</th></tr></thead><tbody>{"".join(stages)}</tbody></table><details><summary>Growth ratios, log fits, authored demand and design hypotheses</summary><pre>{esc(json.dumps(growth, indent=2))}</pre></details>')
        extras += '<h2>Multiplicative campaign growth</h2><p>Pressure probes keep map, composition policy and cadence fixed. Fixed-wallet probes isolate upgrade/cost efficiency; actual-journey-opening probes include growth in the available army budget. Both disable in-run reinvestment. Pressure scales increase enemy count at unchanged release timing. Targets are configurable design hypotheses. A top-grid clear is a censored lower bound; an all-failing grid is an upper bound. Neither establishes an exact power value. Log fits and geometric bracket midpoints are descriptive; actual journey earnings and spending capture the additional income feedback.</p>' + ''.join(growth_parts)
    if summary.get("wave_pressure"):
        pressure_rows = []
        for row in summary["wave_pressure"]:
            pressure_rows.append(f'<tr><td>{esc(row["policy"])}</td><td>{esc(row["variant"])}</td><td>{row["seed"]}</td><td>{row["wave_index"] + 1}</td><td>{row["authored_count"]}</td><td>{fmt(row["authored_release_span_seconds"])}</td><td>{fmt(row["authored_peak_agents_per_second"])}</td><td>{fmt(row["authored_peak_density_per_second"])}</td><td>{fmt(row["interval_density_removed_per_second"])}</td><td>{row["cells_bought_in_interval"]}</td><td>{row["atp_spent_in_interval"]}</td><td>{esc(row["clearing_timeout"])}</td></tr>')
        extras += '<h2>Release pressure and reinvestment</h2><p>Authored peak release is a continuous scheduling target, not measured arrivals at the defense. Density/s represents incoming density burden, not hostile attack DPS. Interval removal includes preparation, combat and possible carryover, so it is not a theoretical sustained kill capacity. Per-wave purchases use interval ticks; sampled income deltas in summary.json can miss boundary ticks.</p><details><summary>Wave population, release rate, observed damage removal and reinforcements</summary><table><thead><tr><th>Controller</th><th>Variant</th><th>Seed</th><th>Wave</th><th>Count</th><th>Release span s</th><th>Peak agents/s</th><th>Peak density/s</th><th>Interval density removed/s</th><th>Cells bought</th><th>ATP spent</th><th>Clearing timeout</th></tr></thead><tbody>' + ''.join(pressure_rows) + '</tbody></table></details>'
    if summary.get("lifecycles"):
        lifecycle_rows = []
        bound_text = lambda v: "—" if v is None else fmt(v[0], 3) + "–" + fmt(v[1], 3)
        for row in summary["lifecycles"]:
            ttk = row["damage_ttk"]
            lifecycle_rows.append(f'<tr><td>{esc(row["policy"])}</td><td>{esc(row["variant"])}</td><td>{row["seed"]}</td><td>{esc(row["side"])}</td><td>{esc(row["subject"])}</td><td>{ttk["observations"]}</td><td>{fmt(ttk["mean_seconds"],3)}</td><td>{bound_text(ttk["p50_seconds"])}</td><td>{bound_text(ttk["p90_seconds"])}</td><td>{fmt(row["damage_lifetime"]["mean_seconds"],3)}</td><td>{row["alive"]} / {row["hit_alive"]}</td><td>{row["leaked"]} / {row["consumed"]} / {row["expired"]}</td></tr>')
        extras += '<h2>Enemy and friendly time to kill</h2><p>TTK is first effective hit to damage death, in seconds. A zero is a valid first-hit kill. Percentiles are histogram bounds, not exact values. Means condition on observed damage deaths; alive/hit-alive survivors are censored. Leaks, consumption, expiration and other exit reasons are not damage-death samples. Read survival and exit counts alongside TTK. Only terminal encounters appear here; campaign attempt metrics remain in raw reports.</p><details><summary>Per-run damage-death TTK and lifetime measurements</summary><table><thead><tr><th>Controller</th><th>Variant</th><th>Seed</th><th>Side</th><th>Family / cell</th><th>Damage-death samples</th><th>Mean TTK s</th><th>p50 bounds s</th><th>p90 bounds s</th><th>Mean lifetime to damage death s</th><th>Alive / hit-alive</th><th>Leaks / consumed / expired</th></tr></thead><tbody>' + ''.join(lifecycle_rows) + '</tbody></table></details><details><summary>Paired changes in per-run mean TTK</summary><pre>' + esc(json.dumps(summary["paired_ttk"], indent=2)) + '</pre></details>'
    if summary.get("additional_metrics"):
        extras += '<h2>Additional metrics</h2><details><summary>Per-controller means, variation and paired changes</summary><pre>' + esc(json.dumps(summary["additional_metrics"], indent=2)) + '</pre></details>'
    if "search" in summary:
        extras += '<h2>Strategy selection</h2><p>' + esc(summary["search"]["warning"]) + '</p><pre>' + esc(json.dumps(summary["search"], indent=2)) + '</pre>'
    if "budget_thresholds" in summary:
        extras += '<h2>Budget threshold grid</h2><pre>' + esc(json.dumps(summary["budget_thresholds"], indent=2)) + '</pre>'
    if campaigns:
        extras += '<h2>Campaign journeys</h2><table><thead><tr><th>Controller</th><th>Seed</th><th>Levels cleared</th><th>Attempts</th><th>Retries</th><th>Memory Cells earned</th><th>Antibodies earned</th></tr></thead><tbody>' + ''.join(campaigns) + '</tbody></table>'
    if summary.get("progression_journeys"):
        parts = []
        for journey in summary["progression_journeys"]:
            rows = []
            for level in journey["levels"]:
                losses = ', '.join(str(loss["memory_cells"]) for loss in level["loss_rewards"]) or '—'
                tools = ', '.join(p["key"] for p in level["tools_bought_before_next_level"]) or '—'
                rows.append(f'<tr><td>{esc(level["level"])}</td><td>{level["entry_waves_cleared"]}/{level["total_waves"]}</td><td>{esc(level["attempts_to_clear"] if level["attempts_to_clear"] is not None else "unresolved")}</td><td>{losses}</td><td>{level["first_clear_antibodies"]}</td><td>{len(level["discovery_offers"])}</td><td>{esc(tools)}</td></tr>')
            parts.append(f'<h3>{esc(journey["phase"])} · {esc(journey["policy"])} · {esc(journey["variant"])} · seed {journey["seed"]}</h3><p>Complete: {esc(journey["complete"])}. Median attempts per cleared level: {fmt(journey["median_attempts_to_clear"])}. Losses followed by purchases: {fmt(journey["post_loss_purchase_rate"])}. Declared pacing targets matched: {esc(journey["matches_declared_targets"])}.</p><table><thead><tr><th>Level</th><th>Entry waves cleared</th><th>Attempt clearing level</th><th>Memory Cells per loss</th><th>First-clear Antibodies</th><th>Affordable discovery choices</th><th>Tools bought before next level</th></tr></thead><tbody>{"".join(rows)}</tbody></table><details><summary>Ordered attempts, rewards, purchases, TTK and target checks</summary><pre>{esc(json.dumps(journey, indent=2))}</pre></details>')
        extras += '<h2>Getting stronger through retries</h2><p>Loss rewards intentionally fund permanent growth. This view preserves each actual attempt and its following purchases; it does not assume every purchased rank improved combat. First-try clears are allowed. Enemy and friendly TTK remain conditional on damage deaths with survivors and other exits shown. Controller seed variation does not establish human enjoyment.</p>' + ''.join(parts)
    title = esc(summary["experiment"])
    adequacy = summary.get("controller_adequacy", {"status": "uncalibrated", "interpretation": "No controller adequacy assessment is available.", "reference_checks": []})
    anchor_rows = ''.join(f'<tr><td>{esc(r["name"])}</td><td>{esc(r["policy"])}</td><td>{esc(r["status"])}</td><td>{r["successful"]}/{r["terminal"]}</td><td>{esc(r["source"])}</td></tr>' for r in adequacy["reference_checks"])
    adequacy_table = '<table><thead><tr><th>Known-achievable reference</th><th>Controller</th><th>Check</th><th>Matches / terminal samples</th><th>Evidence source</th></tr></thead><tbody>' + anchor_rows + '</tbody></table>' if anchor_rows else ''
    return f'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>{title}</title>
<style>body{{font:15px system-ui,sans-serif;background:#111820;color:#dce7ed;margin:0 auto;padding:32px;max-width:1400px}}h1,h2{{color:#8ee4bc}}p{{max-width:1000px;line-height:1.6}}table{{border-collapse:collapse;width:100%;margin:20px 0}}th,td{{text-align:left;padding:10px;border-bottom:1px solid #34424d}}th{{background:#1a2834}}a{{color:#90c9ff}}.bar{{display:inline-block;width:90px;height:8px;background:#334454;margin-left:8px}}.bar span{{display:block;height:100%;background:#6ed6af}}.invalid,.timeout{{color:#ffbd73}}.cleared,.completed{{color:#8ee4bc}}.defeated,.stalled{{color:#ff9b9b}}details{{padding:8px}}svg{{width:min(100%,800px)}}svg text{{fill:#c5d5de;font:11px system-ui}}pre{{overflow:auto;background:#1a2834;padding:16px}}input{{padding:10px;background:#1a2834;color:white;border:1px solid #455465}}</style>
<h1>{title}</h1><section style="border:2px solid #d5ad75;padding:16px;background:#23272b"><h2 style="margin-top:0">Controller adequacy: {esc(adequacy['status'].replace('_',' '))}</h2><p>{esc(adequacy['interpretation'])}</p><p>Automatic gameplay tuning recommendation: none. Pressure-grid capacity and correct simulation do not establish that a controller plays adequately.</p>{adequacy_table}</section><p>{esc(summary['interpretation'])}</p><p>Clear rates use terminal outcomes only; always inspect unresolved counts. Wilson 95% intervals describe seed uncertainty within the fixed controller. Paired metric intervals use a deterministic percentile bootstrap; one sample has no interval. These are exploratory estimates, not a multiple-comparison-corrected significance test.</p><p><a href="summary.json">Machine-readable summary</a> · <a href="experiment.json">Experiment definition</a></p>
<h2>Outcomes by controller and variant</h2><table><thead><tr><th>Phase</th><th>Controller</th><th>Variant</th><th>Clears / terminal</th><th>95% interval</th><th>Timeouts</th><th>Invalid</th><th>{esc(summary['groups'][0]['metric'] if summary['groups'] else 'metric')} mean</th></tr></thead><tbody>{''.join(group_rows)}</tbody></table>
<h2>Paired changes from baseline</h2><p>Only matching seeds under the same controller and phase are compared. Omitted pairs have no established terminal result. Positive metric deltas mean the variant has a larger value.</p><table><thead><tr><th>Phase</th><th>Controller</th><th>Variant</th><th>Paired seeds</th><th>Mean delta</th><th>95% bootstrap interval</th><th>Omitted</th></tr></thead><tbody>{''.join(pairs)}</tbody></table>{extras}
<h2>Run artifacts</h2><p>Replay specs preserve exact inputs; raw reports contain recorded actions and provenance. Rerunning the same spec requires the matching game build and assets recorded in that report.</p><input id="filter" placeholder="Filter runs" aria-label="Filter run table"><table id="runs"><thead><tr><th>Phase</th><th>Controller</th><th>Variant</th><th>Seed</th><th>Status</th><th>Artifacts</th><th>Diagnostic</th></tr></thead><tbody>{''.join(run_rows)}</tbody></table>
<h2>Integrity timelines</h2>{''.join(charts) if charts else '<p>No sampled integrity series available.</p>'}
<script>document.getElementById('filter').addEventListener('input',e=>{{let q=e.target.value.toLowerCase();document.querySelectorAll('#runs tbody tr').forEach(r=>r.hidden=!r.textContent.toLowerCase().includes(q));}});</script></html>'''


def report_directory(directory: Path, calibration: dict | None = None) -> dict:
    experiment = read_json(directory / "experiment.json")
    explicit_calibration = calibration is not None
    if calibration is None and (directory / "analysis-calibration.json").is_file():
        calibration = read_json(directory / "analysis-calibration.json")
    if calibration is not None:
        experiment["controller_calibration"] = calibration
    validate_experiment(experiment)
    if explicit_calibration:
        write_json(directory / "analysis-calibration.json", calibration)
    records = [read_json(path) for path in sorted((directory / "runs").glob("*.record.json"))]
    summary = summarize(records, experiment)
    write_json(directory / "summary.json", summary)
    (directory / "report.html").write_text(render_html(summary, records), encoding="utf-8")
    return summary


def execute_batch(runs: list[dict], args) -> list[dict]:
    records = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(execute_one, run, args.exe, args.out / "runs", args.timeout): run for run in runs}
        for future in concurrent.futures.as_completed(futures):
            record = future.result()
            records.append(record)
            print(f"{len(records)}/{len(runs)} {record['policy']} {record['variant']} seed={record['seed']}: {record['status']}", flush=True)
    return records


def calibration_inputs(source: Path, output: Path, scales: list[float]) -> tuple[dict, list[dict]]:
    """Refine a pressure grid without replaying its campaign or mixing outcomes."""
    experiment = read_json(source / "experiment.json")
    if (source / "analysis-calibration.json").is_file():
        experiment["controller_calibration"] = read_json(source / "analysis-calibration.json")
    if not experiment.get("power_probe"):
        raise ValueError("Source experiment has no power_probe")
    experiment["power_probe"]["pressure_scales"] = scales
    base = experiment["power_probe"]["base"]
    records = [read_json(path) for path in sorted((source / "runs").glob("*.record.json"))]
    if "level_document" not in base:
        # A scale-one historical probe pins the exact canonical document even
        # when current checked-in assets have changed since its campaign.
        original = next((r for r in records if get_path(r, "probe.pressure_scale") == 1), None)
        pinned = get_path(original or {}, "report.replay_spec.level_document")
        if pinned is None:
            raise ValueError("No pinned canonical probe document; rerun a planned experiment with current schema")
        base["level_document"] = pinned
    validate_experiment(experiment)
    journeys = [r for r in records if (r.get("report") or {}).get("kind") == "campaign"]
    if not journeys:
        raise ValueError("Source contains no campaign report")
    (output / "runs").mkdir(parents=True, exist_ok=True)
    for record in journeys:
        for key in ("spec_path", "report_path", "stderr_path"):
            path = Path(record[key])
            src, dst = (source / path).resolve(), (output / path).resolve()
            if not src.is_relative_to(source.resolve()) or not dst.is_relative_to(output.resolve()):
                raise ValueError("Artifact paths must stay inside the experiment directories")
            if src.is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        write_json(output / "runs" / f"{record['id']}.record.json", record)
    experiment["calibration_source"] = str(source.resolve())
    return experiment, journeys


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "run"):
        cmd = sub.add_parser(name)
        cmd.add_argument("experiment", type=Path)
        cmd.add_argument("--out", type=Path, required=True)
        if name == "run":
            cmd.add_argument("--exe", type=Path, required=True)
            cmd.add_argument("--jobs", type=int, default=1, help="Independent processes; each runner remains deterministic")
            cmd.add_argument("--timeout", type=float, default=180, help="Wall-clock timeout per subprocess, seconds")
    cmd = sub.add_parser("report")
    cmd.add_argument("directory", type=Path)
    cmd.add_argument("--calibration", type=Path, help="Attach explicit known-achievable controller references without changing the original experiment")
    cmd = sub.add_parser("calibrate", help="Refine power probes from existing recorded journeys")
    cmd.add_argument("directory", type=Path)
    cmd.add_argument("--out", type=Path, required=True)
    cmd.add_argument("--exe", type=Path, required=True)
    cmd.add_argument("--pressure-scales", nargs="+", type=float, required=True)
    cmd.add_argument("--jobs", type=int, default=1)
    cmd.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args(argv)
    try:
        if args.command == "report":
            report_directory(args.directory, read_json(args.calibration) if args.calibration else None)
            print(args.directory / "report.html")
            return 0
        if args.command in {"run", "calibrate"} and (args.jobs < 1 or args.timeout <= 0 or not args.exe.is_file()):
            raise ValueError("Run requires an existing executable, jobs >= 1, timeout > 0")
        if args.command in {"run", "calibrate"} and (args.out / "runs").exists() and any((args.out / "runs").glob("*.record.json")):
            raise ValueError("Output already contains runs; choose a fresh directory to avoid mixing experiments")
        args.out.mkdir(parents=True, exist_ok=True)
        if args.command == "calibrate":
            experiment, records = calibration_inputs(args.directory, args.out, args.pressure_scales)
            write_json(args.out / "experiment.json", experiment)
            probes = plan_power_runs(records, experiment)
            write_json(args.out / "power-plan.json", {"schema": 1, "runs": probes})
            records.extend(execute_batch(probes, args))
            report_directory(args.out)
            print(args.out / "report.html")
            return 0 if records and all(is_terminal(r) for r in records) else 2
        experiment = read_json(args.experiment)
        validate_experiment(experiment)
        if experiment.get("power_probe"):
            base = experiment["power_probe"]["base"]
            if "level_document" not in base:
                base["level_document"] = read_json(ROOT / base["level"])
        write_json(args.out / "experiment.json", experiment)
        phase = "training" if experiment.get("kind") == "search" else experiment.get("evaluation_split", "evaluation")
        runs = plan_runs(experiment, phase)
        write_json(args.out / "plan.json", {"schema": 1, "runs": runs})
        if args.command == "plan":
            print(f"Planned {len(runs)} {phase} runs in {args.out / 'plan.json'}")
            return 0
        records = execute_batch(runs, args)
        if experiment.get("kind") == "search":
            selected, rankings = search_selection(records, experiment)
            write_json(args.out / "selection.json", {"selected": sorted(selected), "rankings": rankings})
            # A prespecified control provides paired held-out effect estimates;
            # it does not participate in any further strategy selection.
            controls = {(policy, experiment.get("baseline", "baseline")) for policy, _ in selected}
            heldout = plan_runs(experiment, "heldout", selected | controls)
            write_json(args.out / "heldout-plan.json", {"schema": 1, "runs": heldout})
            records.extend(execute_batch(heldout, args))
        if experiment.get("power_probe"):
            probes = plan_power_runs(records, experiment)
            write_json(args.out / "power-plan.json", {"schema": 1, "runs": probes})
            records.extend(execute_batch(probes, args))
        report_directory(args.out)
        print(args.out / "report.html")
        # A valid defeat is evidence. An invalid run or unresolved timeout is an
        # incomplete experiment and must not silently pass CI.
        return 0 if records and all(is_terminal(r) for r in records) else 2
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"balance_lab: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
