#!/usr/bin/env python3
"""Discover every IMMUNE upgrade and generate controlled real-engine probes.

  python tools/balance_catalog.py generate --exe path/to/immune.exe --out scratch/catalog
  python tools/balance_catalog.py run scratch/catalog --exe path/to/immune.exe --node 'goblet.*'
  python tools/balance_catalog.py report scratch/catalog
  python tools/balance_catalog.py self-test [--catalog scratch/catalog/catalog.json]

Generation does not play the game. Execution delegates to balance_lab.py, which
invokes the real BalanceRun session. Synthetic grants isolate direct effects;
the manifest records real acquisition prices separately from those effects.
"""
from __future__ import annotations

import argparse
import copy
import fnmatch
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import balance_lab as lab

ROOT = Path(__file__).resolve().parents[1]


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


class Catalog:
    def __init__(self, document: dict):
        if document.get("schema") != 1 or document.get("kind") != "balance_catalog":
            raise ValueError("Expected a schema-1 --balance-catalog document")
        self.document = document
        self.nodes = {node["key"]: node for node in document["tree"]["nodes"]}
        self.cells = {cell["key"]: cell for cell in document["cells"]}
        self.abilities = {ability["key"]: ability for ability in document["abilities"]}
        self.threshold = document["tree"]["capstone_threshold"]
        if len(self.nodes) != len(document["tree"]["nodes"]):
            raise ValueError("Catalog contains duplicate stable node keys")
        for node in self.nodes.values():
            if node["parent"] is not None and node["parent"] not in self.nodes:
                raise ValueError(f"Unknown parent for {node['key']}")
            prices = node["rank_prices"]
            if len(prices) != node["max_rank"] or [p["rank"] for p in prices] != list(range(1, node["max_rank"] + 1)):
                raise ValueError(f"Incomplete price curve for {node['key']}")
            self.ancestors(node["key"])

    def ancestors(self, key: str) -> list[str]:
        parents, seen = [], {key}
        current = self.nodes[key]["parent"]
        while current is not None:
            if current in seen:
                raise ValueError(f"Cycle in upgrade prerequisites at {key}")
            seen.add(current)
            parents.append(current)
            current = self.nodes[current]["parent"]
        return list(reversed(parents))

    def grant(self, key: str, grants: dict[str, int], blocked: str | None = None) -> None:
        for ancestor in [*self.ancestors(key), key]:
            if ancestor == blocked:
                raise ValueError(f"Support prerequisite would change the isolated target {blocked}")
            grants[ancestor] = max(1, grants.get(ancestor, 0))

    def branch_points(self, grants: dict[str, int], branch: str) -> int:
        return sum(rank for key, rank in grants.items()
                   if self.nodes[key]["branch"] == branch and self.nodes[key]["kind"] == "stat")

    def prerequisites(self, node: dict, composition: dict[str, float]) -> dict[str, int]:
        target = node["key"]
        grants = {parent: 1 for parent in self.ancestors(target)}
        for cell, weight in composition.items():
            root = self.cells[cell]["root"]
            if weight > 0 and root is not None and root != target:
                self.grant(root, grants, blocked=target)
        if node["kind"] == "capstone":
            while self.branch_points(grants, node["branch"]) < self.threshold:
                candidates = [n for n in self.nodes.values()
                              if n["kind"] == "stat" and n["branch"] == node["branch"]
                              and grants.get(n["key"], 0) < n["max_rank"]
                              and (n["parent"] is None or grants.get(n["parent"], 0) > 0)]
                if not candidates:
                    raise ValueError(f"Capstone threshold cannot be reached for {target}")
                chosen = min(candidates, key=lambda n: (n["rank_prices"][grants.get(n["key"], 0)]["memory_cells"], n["id"]))
                grants[chosen["key"]] = grants.get(chosen["key"], 0) + 1
        grants[target] = 0
        return dict(sorted(grants.items()))

    def cost(self, grants: dict[str, int]) -> dict:
        memory, antibodies = 0, 0
        for key, rank in grants.items():
            if rank:
                price = self.nodes[key]["rank_prices"][rank - 1]
                memory += price["cumulative_memory_cells"]
                antibodies += price["cumulative_antibodies"]
        return {"memory_cells": memory, "antibodies": antibodies}

    def validate_grants(self, grants: dict[str, int]) -> None:
        for key, rank in grants.items():
            node = self.nodes[key]
            if type(rank) is not int or not 0 <= rank <= node["max_rank"]:
                raise ValueError(f"Invalid rank for {key}")
            if not rank:
                continue
            if node["parent"] is not None and grants.get(node["parent"], 0) < 1:
                raise ValueError(f"Missing parent for {key}")
            if node["kind"] == "capstone" and self.branch_points(grants, node["branch"]) < self.threshold:
                raise ValueError(f"Missing branch points for {key}")


def composition(catalog: Catalog, node: dict) -> dict[str, float]:
    cell, ability = node.get("cell"), node.get("ability")
    if node["key"] == catalog.document["tree"]["root"]:
        weights = {"neutrophil": 1.0}
    elif ability in {"fever", "clot"}:
        # These diagnostic contexts use a small, explicitly paid army. A large
        # support army erased injury and concentration opportunities entirely.
        weights = {"neutrophil": 1.0}
    elif ability:
        weights = {"neutrophil": 0.65, "macrophage": 0.35}
    elif cell == "neutrophil":
        weights = {"neutrophil": 1.0}
    elif cell in {"goblet_cell", "fibroblast"}:
        # Slow/weakness and inflammation need allied damage to expose support.
        weights = {"neutrophil": 0.65, cell: 0.35}
    elif cell:
        weights = {"neutrophil": 0.25, cell: 0.75}
    else:
        weights = {"neutrophil": 0.45, "macrophage": 0.25, "cytotoxic_t": 0.2,
                   "goblet_cell": 0.05, "fibroblast": 0.05}
    if node["key"] == "hub.homeostasis":
        weights = {}
    return {key: weights.get(key, 0.0) for key in catalog.cells}


def opportunity_gates(node: dict) -> list[dict]:
    key, ability = node["key"], node.get("ability")
    gates = []
    if ability:
        gates.append({"metric": "summary.abilities_used", "minimum": 2 if key.endswith(".cooldown") else 1,
                      "arms": "variants_only" if node["kind"] == "ability_root" else "all",
                      "reason": "Ability must actually be used; cooldown benefits need repeated casts"})
    if ability in {"fever", "clot"}:
        gates.append({"metric": "units.neutrophil.deployed", "minimum": 10 if ability == "fever" else 3,
                      "reason": "Diagnostic defenders must be deployed through the paid player action API"})
    if key == "ability.clot.duration":
        gates.append({"metric": "enemies.bacteria.lifecycle.retired_lifetime.observations", "minimum": 1,
                      "reason": "Barrier delay needs observed enemy passage/retirement, not only a successful cast"})
    if key == "hub.homeostasis":
        gates.append({"metric": "summary.leaks", "minimum": 1, "reason": "No leaks means no leak-damage opportunity"})
    elif key == "hub.efficient_clearance":
        gates.append({"metric": "economy.damage_income", "minimum": 1, "reason": "Income upgrade needs actual kill earnings"})
    elif key == "fibroblast.scar_health":
        gates.append({"metric": "combat.hostile_requested_structure_damage", "minimum": 0.001,
                      "reason": "Scar health needs hostile structure hits; requested damage may exceed applied damage"})
        gates.append({"metric": "state.scars_lost", "minimum": 1, "arms": "baseline_only",
                      "reason": "Baseline must lose an actual scar to hostile damage; upgraded scars need not die to prove protection"})
    elif key in {"hub.membrane_resilience", "hub.elite_response", "macrophage.wall"} or any(word in key for word in ("health", "vitality", "stamina")) or ability == "fever":
        cell = node.get("cell")
        metric = f"units.{cell}.lifecycle.damage_taken" if cell else "combat.hostile_actual_cell_damage"
        gates.append({"metric": metric, "minimum": 0.001,
                      "reason": "Durability needs actual damage to the upgraded role; damage to supporting cells is insufficient" if cell else
                                "Global durability/healing needs actual damage to friendly cells"})
    if node.get("cell") == "fibroblast":
        gates.append({"metric": "state.scars_built", "minimum": 1,
                      "arms": "variants_only" if node["kind"] == "tower_root" else "all",
                      "reason": "Builder upgrades need constructed scars; an unlock baseline correctly has no builder"})
    if node.get("cell") == "goblet_cell":
        gates.append({"metric": "summary.sampled_slow_enemy_seconds", "minimum": 0.001,
                      "arms": "variants_only" if node["kind"] == "tower_root" else "all",
                      "reason": "Mucus upgrades need observed slow exposure; an unlock baseline correctly has no mucus cell"})
    return gates


def endurance_level(document: dict, ability: dict) -> tuple[dict, int]:
    """Repeated authored arrivals create cooldown opportunities in real play.

    These are explicit diagnostic content changes shared by every rank arm,
    not a prediction that the defenses will survive or the controller will cast.
    """
    level = copy.deepcopy(document)
    level["name"] = f"catalog_{ability['key']}_endurance"
    level["display_name"] = f"Catalog: {ability['name']} endurance"
    level.pop("win", None)
    templates = level["waves"][0]["spawns"]
    if ability["key"] in {"complement", "histamine", "clot"}:
        # A strong supporting army removed the old sparse arrivals before the
        # tactical controller could observe a cast-worthy concentration. Use
        # a compact, non-replicating pulse instead. Three pulses separated by
        # 75% of the base cooldown create two baseline opportunities and an
        # earlier opportunity for a 30% cooldown reduction, without requiring
        # a long route whose uncontrolled replication can fill the enemy pool.
        # The shared zero defensive wallet is set in build_experiment below.
        template = copy.deepcopy(templates[0])
        template.update({"family": "bacteria", "count": 40, "duration": 0.5,
                         "squad_size": 40})
        spawn_id = template.get("spawn_point_id")
        if not spawn_id and level.get("spawn_points"):
            spawn_id = level["spawn_points"][0]["id"]
            template["spawn_point_id"] = spawn_id
        for point in level.get("spawn_points", []):
            if point.get("id") == spawn_id:
                point["radius"] = min(3.0, float(point.get("radius", 3.0)))
        gap = max(1.0, float(ability["cooldown_seconds"]) * 0.75)
        entries = [{**copy.deepcopy(template), "start_time": batch * gap}
                   for batch in range(3)]
        level["description"] = ("Synthetic direct-ability diagnostic: zero defensive budget and three "
            "compact non-replicating bacterial pulses. Measures cast opportunities and direct "
            "effects; this is not a campaign difficulty or affordability estimate.")
        if ability["key"] == "clot":
            level["description"] = ("Synthetic Clot diagnostic: three compact non-replicating bacterial "
                "pulses against a small army paid from the experiment's fixed wallet. Measures legal "
                "cast opportunities and delay, not campaign difficulty or affordability.")
        level["waves"] = [{"name": "isolated_ability_pulses", "prep_time": 4,
                            "atp_reward": 0, "spawns": entries}]
        return level, math.ceil((2 * gap + 120.0) * 60)
    batches = max(3, math.ceil(float(ability["cooldown_seconds"]) * 2.25 / 35.0))
    entries = []
    for batch in range(batches):
        for original in templates:
            entry = copy.deepcopy(original)
            entry["count"] = max(60, int(entry["count"]))
            entry["duration"] = min(6.0, max(1.0, float(entry.get("duration", 1))))
            entry["start_time"] = batch * 35.0 + float(entry.get("start_time", 0))
            entries.append(entry)
    level["waves"] = [{"name": "repeated_visible_pressure", "prep_time": 4,
                        "atp_reward": 0, "spawns": entries}]
    last = max(entry["start_time"] + entry["duration"] for entry in entries)
    return level, math.ceil((last + 120.0) * 60)


def paid_ability_context(catalog: Catalog, node: dict) -> tuple[dict, dict]:
    """Inputs shared by every rank; casts still obey normal unlock/cooldown rules.

    The stationary injury fixture isolates healing from the controller's
    aggregate-health trigger. The sustained choke fixture isolates barrier
    lifetime from its crowd-density trigger. Neither predicts campaign value.
    """
    ability = node.get("ability")
    if ability not in {"fever", "clot"}:
        return {}, {}
    fields = catalog.document.get("registry", {}).get("fields", [])
    cost = int(float(next((f["value"] for f in fields
                          if f["path"] == "towers.neutrophil.stats.build_cost"), 15)))
    quantity = 10 if ability == "fever" else 3
    wallet = quantity * cost
    patch = {"opening_atp": wallet, "spend_limit": wallet}
    fixture = {"kind": "paid_compact_ability_pulses", "defensive_wallet": wallet,
               "paid_cell": "neutrophil", "paid_quantity": quantity,
               "interpretation": "Controlled direct effect only; synthetic unlock grants do not establish campaign affordability"}
    if ability == "fever":
        level = read_json(ROOT / "assets/balance/levels/open_lane.json")
        level.update({"name": "catalog_fever_real_injury", "display_name": "Catalog: Fever real-injury diagnostic",
            "description": "Explicit diagnostic overrides sustain genuine toxin injury on ten paid stationary low-damage cells. No debug damage/heal or cooldown bypass. Not campaign tuning.",
            "win": {"survive_seconds": 180}})
        level["spawn_points"][0].update({"pos": [30, 60], "radius": 3})
        entry = {"family": "bacteria", "count": 40, "duration": 0.5,
                 "spawn_point_id": "west", "squad_size": 40, "squad_paths": ["main_route"]}
        level["waves"] = [{"name": "continuous_real_toxin", "prep_time": 0.1, "atp_reward": 0,
                           "spawns": [entry, {**entry, "count": 1, "start_time": 179, "duration": 0.1}]}]
        overrides = {"towers.neutrophil.swarm.speed": 0,
            "towers.neutrophil.payload.round_damage": 0.002,
            "enemies.families.bacteria.attack.toxin_damage": 0.0015,
            "enemies.families.bacteria.attack.toxin_range": 32,
            "enemies.speed_tiers.fast.max_speed": 0.1, "enemies.speed_tiers.fast.jitter": 0}
        actions = [{"tick": 0, "kind": "deploy", "tower": "neutrophil", "position": [45, 60], "quantity": quantity},
                   {"tick": 0, "kind": "start_wave"}] + [
            {"tick": tick, "kind": "cast", "ability": "fever", "required": False}
            for tick in range(600, 10800, 120)]
        patch.update({"level_document": level, "overrides": overrides, "actions": actions,
                      "controller": {"policy": "none", "use_abilities": False}})
        fixture.update({"kind": "paid_real_injury", "geometry_source": "assets/balance/levels/open_lane.json",
            "overrides": overrides, "survival_seconds": 180, "shared_legal_cast_attempts": True,
            "cast_attempt_interval_seconds": 2,
            "interpretation": "Actual injury, deaths, censored survivors and remaining HP expose healing. Optional scripted attempts let the normal API reject locked/cooling casts, separating the effect from bot healing thresholds. No campaign inference."})
    elif node["key"] == "ability.clot.duration":
        source = read_json(ROOT / "assets/balance/levels/choke.json")
        level, _ = endurance_level(source, catalog.abilities["clot"])
        level.update({"name": "catalog_clot_sustained_choke", "display_name": "Catalog: Clot sustained passage",
            "description": "Three sustained bacterial streams through the standard diagnostic choke. Three paid defenders and identical legal cast inputs. Lower leak damage avoids early termination while measuring delay; not campaign tuning."})
        for entry in level["waves"][0]["spawns"]:
            entry.update({"count": 60, "duration": 20, "squad_size": 20})
        second_cast = 360 + math.ceil((catalog.abilities["clot"]["cooldown_seconds"] + 1) * 60)
        overrides = {"sim.globals.objective_damage_per_leak": 0.1}
        patch.update({"level_document": level, "overrides": overrides,
            "controller": {"policy": "none", "use_abilities": False},
            "actions": [{"tick": 0, "kind": "deploy", "tower": "neutrophil", "position": [150, 60], "quantity": quantity},
                        {"tick": 0, "kind": "start_wave"},
                        {"tick": 360, "kind": "cast", "ability": "clot", "position": [125, 60]},
                        {"tick": second_cast, "kind": "cast", "ability": "clot", "position": [125, 60]}]})
        fixture.update({"kind": "paid_sustained_choke_passage", "geometry_source": "assets/balance/levels/choke.json",
            "overrides": overrides, "pulse_count": 3, "enemies_per_pulse": 60, "release_seconds": 20,
            "shared_legal_cast_attempts": True,
            "interpretation": "Enemy retirement lifetime measures barrier delay. Equal deaths or leaks do not establish combat survival value; objective leak damage is explicitly reduced only to observe all passages."})
    else:
        fixture.update({"family": "bacteria", "pulse_count": 3, "enemies_per_pulse": 40,
                        "release_seconds": 0.5,
                        "pulse_interval_seconds": catalog.abilities["clot"]["cooldown_seconds"] * 0.75})
    return patch, fixture


def build_experiment(catalog: Catalog, node: dict, level: dict, config_dir: Path,
                     seeds: list[int], budget: int, max_ticks: int) -> tuple[dict, dict]:
    weights = composition(catalog, node)
    grants = catalog.prerequisites(node, weights)
    allowed = [cell for cell, weight in weights.items() if weight > 0]
    ability = node.get("ability")
    probe_level = copy.deepcopy(level)
    if ability:
        probe_level, ability_ticks = endurance_level(probe_level, catalog.abilities[ability])
        max_ticks = max(max_ticks, ability_ticks)
    base = {"schema": 1, "level_document": probe_level, "config_dir": str(config_dir.resolve()),
            "max_ticks": max_ticks, "sample_ticks": 30,
            "progression": {"grant_tree": grants}, "income_enabled": False,
            "opening_atp": budget, "spend_limit": budget, "allowed_cells": allowed,
            "allowed_abilities": [ability] if ability else [],
            "controller": {"policy": "tactical" if ability else "specialist",
                           "use_abilities": bool(ability), "weights": weights}}
    ability_patch, ability_fixture = paid_ability_context(catalog, node)
    base = lab.merge(base, ability_patch)
    economy_mode = "fixed_wallet"
    if ability in {"complement", "histamine"}:
        base["opening_atp"] = 0
        base["spend_limit"] = 0
        economy_mode = "zero_defense_direct_ability_probe"
    elif node["key"] == "hub.bone_marrow_reserve":
        base.pop("opening_atp")
        base.pop("spend_limit")
        economy_mode = "authored_opening_plus_tree_bonus"
    elif node["key"] == "hub.efficient_clearance":
        base["income_enabled"] = True
        base.pop("spend_limit")
        economy_mode = "real_kill_income_and_reinvestment"
    elif node["key"] == "hub.homeostasis":
        base["opening_atp"] = 0
        base["spend_limit"] = 0
        base["controller"]["policy"] = "none"
    metrics = ["density_removed", "atp_spent", "kills", "leaks", "friendly_killed",
               "abilities_used", "combat.hostile_actual_cell_damage"]
    if ability in {"fever", "clot"}:
        metrics += ["friendly_alive", "units.neutrophil.remaining_hp",
                    "units.neutrophil.lifecycle.damage_ttk.mean_seconds",
                    "units.neutrophil.lifecycle.damage_ttk.observations",
                    "enemies.bacteria.lifecycle.retired_lifetime.mean_seconds"]
    experiment = {"schema": 1, "name": f"Catalog: {node['name']} ({node['key']})",
        "kind": "paired", "seeds": seeds, "baseline": "baseline", "metric": "objective_integrity",
        "metrics": metrics,
        "policies": [{"name": "role_context", "controller": {}}], "base": base,
        "variants": [{"name": "baseline", "patch": {}}] + [
            {"name": f"rank_{rank}", "patch": {"progression": {"grant_tree": {node["key"]: rank}}}}
            for rank in range(1, node["max_rank"] + 1)],
        "expectations": {"scope": "Synthetic grants measure this node's direct effect with identical other grants; acquisition cost is separate",
                         "opportunity_gates": opportunity_gates(node),
                         "interpretation": "An unexercised mechanic or unresolved run is inconclusive, never proof of no value"}}
    metadata = {"key": node["key"], "name": node["name"], "effect": node["effect"],
        "kind": node["kind"], "max_rank": node["max_rank"], "applicability": node["applicability"],
        "common_grants": grants, "common_prerequisite_cost": catalog.cost(grants),
        "branch_points_before_target": catalog.branch_points(grants, node["branch"]),
        "rank_prices": node["rank_prices"], "rank_effects": node.get("rank_effects", []),
        "composition": weights, "allowed_abilities": base["allowed_abilities"],
        "economy_mode": economy_mode, "opportunity_gates": opportunity_gates(node),
        "synthetic_progression": True,
        "cost_scope": "Real loaded catalog acquisition prices; direct-effect runs grant prerequisites and target ranks, so they do not establish campaign affordability"}
    if "runtime_constraint" in node:
        metadata["runtime_constraint"] = node["runtime_constraint"]
    if ability_fixture:
        metadata["diagnostic_fixture"] = ability_fixture
        metadata["economy_mode"] = "paid_controlled_ability_diagnostic"
    if ability in {"complement", "histamine"}:
        metadata["diagnostic_fixture"] = {"kind": "isolated_ability_pulses",
            "defensive_wallet": 0, "family": "bacteria", "pulse_count": 3,
            "enemies_per_pulse": 40, "release_seconds": 0.5,
            "pulse_interval_seconds": max(1.0, float(catalog.abilities[ability]["cooldown_seconds"]) * 0.75),
            "interpretation": "Direct ability and cooldown opportunity probe; no allied-defense, campaign-difficulty or affordability claim"}
    validate_generated(catalog, node, experiment)
    return experiment, metadata


def validate_generated(catalog: Catalog, node: dict, experiment: dict) -> None:
    lab.validate_experiment(experiment)
    target = node["key"]
    baseline = experiment["base"]["progression"]["grant_tree"]
    if baseline.get(target) != 0:
        raise ValueError(f"Target is already bought in baseline: {target}")
    catalog.validate_grants(baseline)
    for rank, variant in enumerate(experiment["variants"]):
        spec = lab.merge(experiment["base"], variant["patch"])
        grants = spec["progression"]["grant_tree"]
        changed = {key for key in baseline if grants.get(key, 0) != baseline[key]}
        if changed != ({target} if rank else set()) or grants.get(target) != rank:
            raise ValueError(f"Probe changes more than the isolated target: {target} rank {rank}")
        catalog.validate_grants(grants)
        if spec["allowed_abilities"] != experiment["base"]["allowed_abilities"]:
            raise ValueError("Ability capability mask must remain fixed across rank arms")
    if node["kind"] == "capstone" and catalog.branch_points(baseline, node["branch"]) < catalog.threshold:
        raise ValueError("Capstone baseline lacks real branch prerequisites")


def fetch_catalog(args) -> tuple[dict, str]:
    if args.catalog:
        return read_json(args.catalog), ""
    if not args.exe or not args.exe.is_file():
        raise ValueError("Provide --catalog or an existing --exe")
    command = [str(args.exe.resolve()), "--balance-catalog", "--quiet"]
    if args.config:
        command += ["--config", str(args.config.resolve())]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=120)
    if result.returncode:
        raise ValueError(f"Catalog executable failed ({result.returncode}): {result.stderr.strip()}")
    return json.loads(result.stdout), result.stderr


def generate(args) -> int:
    document, stderr = fetch_catalog(args)
    catalog = Catalog(document)
    output = args.out.resolve()
    if (output / "manifest.json").exists():
        raise ValueError("Choose a fresh --out directory; a catalog manifest already exists")
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "catalog.json", document)
    (output / "catalog.stderr.log").write_text(stderr, encoding="utf-8")
    config_dir = output / "config"
    names = document.get("config_files", ["towers.json", "enemies.json", "sim.json", "economy.json", "abilities.json", "meta.json"])
    if len(names) != len(document["base_config"]):
        raise ValueError("Catalog config document count differs from file names")
    for name, config in zip(names, document["base_config"]):
        if Path(name).name != name or not name.endswith(".json"):
            raise ValueError("Catalog config filenames must be simple JSON filenames")
        write_json(config_dir / name, config)
    level_path = args.level if args.level.is_absolute() else ROOT / args.level
    level = read_json(level_path)
    if not level.get("waves") or not level["waves"][0].get("spawns"):
        raise ValueError("Probe level needs a positive authored wave table")
    entries = []
    for node in catalog.nodes.values():
        key = node["key"]
        if not any(fnmatch.fnmatchcase(key, pattern) for pattern in args.node):
            entries.append({"key": key, "status": "not_selected", "applicability": node["applicability"]})
            continue
        if node["applicability"]["status"] == "requires_named_enemies":
            entries.append({"key": key, "name": node["name"], "status": "requires_named_enemies",
                            "applicability": node["applicability"], "rank_prices": node["rank_prices"]})
            continue
        experiment, metadata = build_experiment(catalog, node, level, config_dir,
                                                args.seeds, args.budget, args.max_ticks)
        relative = f"experiments/{lab.slug(key)}.json"
        write_json(output / relative, experiment)
        metadata.update({"status": "generated", "experiment": relative,
                         "result_directory": f"results/{lab.slug(key)}",
                         "planned_runs": len(lab.plan_runs(experiment)),
                         "command": [sys.executable, str(ROOT / "tools/balance_lab.py"), "run",
                                     str(output / relative), "--exe", str(args.exe.resolve()) if args.exe else "<immune.exe>",
                                     "--out", str(output / f"results/{lab.slug(key)}")]})
        entries.append(metadata)
    manifest = {"schema": 1, "kind": "balance_catalog_manifest", "catalog": "catalog.json",
        "config_hash": document.get("config_hash"), "config_directory": "config", "source_level": str(level_path.resolve()),
        "seeds": args.seeds, "budget": args.budget, "nodes": entries,
        "coverage": {"catalog_nodes": len(catalog.nodes), "generated_nodes": sum(e["status"] == "generated" for e in entries),
                     "requires_named_enemies": sum(e["status"] == "requires_named_enemies" for e in entries),
                     "planned_runs": sum(e.get("planned_runs", 0) for e in entries)},
        "interpretation": "Every generated comparison changes only one tree node. Other grants, encounter, composition, ability mask and seeds stay fixed. Grants isolate mechanics; real prices are metadata, not evidence that the player can afford them. Inspect opportunity coverage before interpreting a zero effect."}
    write_json(output / "manifest.json", manifest)
    write_json(output / "generation-checks.json", {"schema": 1, "status": "passed",
        "checks": ["stable keys", "all parent prerequisites", "capstone branch stat threshold", "one changed node per arm",
                   "complete rank prices", "fixed composition", "isolated ability mask", "real balance_lab planning"]})
    print(json.dumps({"manifest": str(output / "manifest.json"), **manifest["coverage"]}, indent=2))
    return 0


def report_directory(directory: Path) -> dict:
    manifest = read_json(directory / "manifest.json")
    coverage = []
    for entry in manifest["nodes"]:
        row = {key: entry[key] for key in ("key", "status", "rank_prices", "common_prerequisite_cost",
            "applicability", "runtime_constraint", "diagnostic_fixture") if key in entry}
        if entry["status"] != "generated":
            coverage.append(row)
            continue
        result_dir = directory / entry["result_directory"]
        records = [read_json(path) for path in sorted((result_dir / "runs").glob("*.record.json"))]
        terminal = [record for record in records if lab.is_terminal(record)]
        missing = []
        for gate in entry["opportunity_gates"]:
            variants = {r["variant"] for r in terminal}
            expected_variants = (set() if gate.get("arms") == "baseline_only" else
                                 {f"rank_{rank}" for rank in range(1, entry["max_rank"] + 1)})
            if gate.get("arms") != "variants_only":
                expected_variants.add("baseline")
            for variant in expected_variants:
                if variant not in variants or not any(
                    (value := lab.get_path(record.get("report") or {}, gate["metric"])) is not None
                    and isinstance(value, (int, float)) and value >= gate["minimum"]
                    for record in terminal if record["variant"] == variant):
                    missing.append({"variant": variant, **gate})
        row["coverage_state"] = ("not_run" if not records else "invalid_or_unresolved"
            if len(terminal) != entry["planned_runs"] else "runtime_capped"
            if entry["applicability"]["status"] == "runtime_capped" else "unexercised" if missing else "measured")
        row["attempted"] = len(records)
        row["terminal"] = len(terminal)
        row["missing_opportunities"] = missing
        if (result_dir / "summary.json").is_file():
            summary = read_json(result_dir / "summary.json")
            row["paired_direct_benefits"] = summary.get("paired", [])
            row["report"] = str((result_dir / "report.html").resolve())
        coverage.append(row)
    result = {"schema": 1, "kind": "balance_catalog_coverage", "nodes": coverage,
              "interpretation": "Measured means terminal evidence and at least one opportunity per required arm. Runtime-capped means the configured upgrade cannot increase its effective engine parameter; terminal comparisons do not make the advertised effect available. Unexercised mechanics are inconclusive. No state establishes universal utility or affordability. Costs remain loaded acquisition prices; support value is conditional on the fixed composition."}
    write_json(directory / "coverage.json", result)
    return result


def run_generated(args) -> int:
    manifest = read_json(args.directory / "manifest.json")
    failures = 0
    for entry in manifest["nodes"]:
        if entry["status"] != "generated" or not any(fnmatch.fnmatchcase(entry["key"], pattern) for pattern in args.node):
            continue
        command = [sys.executable, str(ROOT / "tools/balance_lab.py"), "run",
                   str((args.directory / entry["experiment"]).resolve()), "--exe", str(args.exe.resolve()),
                   "--out", str((args.directory / entry["result_directory"]).resolve()),
                   "--jobs", str(args.jobs), "--timeout", str(args.timeout)]
        print(f"Running {entry['key']}: {entry['planned_runs']} real-engine runs", flush=True)
        result = subprocess.run(command, cwd=ROOT)
        failures += result.returncode != 0
    report_directory(args.directory)
    print(args.directory / "coverage.json")
    return 2 if failures else 0


def self_test(document: dict | None) -> int:
    if document is None:
        def node(key, parent, kind, branch, ranks, index, ability=None, cell=None):
            prices, cumulative = [], 0
            for rank in range(1, ranks + 1):
                price = 20 + (rank - 1) * 15
                cumulative += price
                prices.append({"rank": rank, "memory_cells": price, "antibodies": 0,
                               "cumulative_memory_cells": cumulative, "cumulative_antibodies": 0})
            return {"key": key, "parent": parent, "kind": kind, "branch": branch, "max_rank": ranks,
                    "id": index, "ability": ability, "cell": cell, "rank_prices": prices,
                    "name": key, "effect": key, "applicability": {"status": "available"}}
        nodes = [node("neutrophil.round_damage", None, "stat", "neutrophil", 5, 0, cell="neutrophil"),
                 node("macrophage.unlock", "neutrophil.round_damage", "tower_root", "macrophage", 1, 1, cell="macrophage"),
                 node("macrophage.health", "macrophage.unlock", "stat", "macrophage", 4, 2, cell="macrophage"),
                 node("macrophage.arms", "macrophage.unlock", "stat", "macrophage", 1, 3, cell="macrophage"),
                 node("macrophage.search", "macrophage.unlock", "stat", "macrophage", 3, 4, cell="macrophage"),
                 node("macrophage.capstone", "macrophage.health", "capstone", "macrophage", 1, 5, cell="macrophage"),
                 node("ability.complement.unlock", "neutrophil.round_damage", "ability_root", "hub", 1, 6, ability="complement"),
                 node("ability.complement.cooldown", "ability.complement.unlock", "ability_stat", "hub", 3, 7, ability="complement"),
                 node("ability.fever.unlock", "macrophage.unlock", "ability_root", "hub", 1, 8, ability="fever"),
                 node("ability.fever.cooldown", "ability.fever.unlock", "ability_stat", "hub", 3, 9, ability="fever"),
                 node("ability.fever.magnitude", "ability.fever.unlock", "ability_stat", "hub", 3, 10, ability="fever"),
                 node("ability.clot.unlock", "macrophage.unlock", "ability_root", "hub", 1, 11, ability="clot"),
                 node("ability.clot.cooldown", "ability.clot.unlock", "ability_stat", "hub", 3, 12, ability="clot"),
                 node("ability.clot.duration", "ability.clot.unlock", "ability_stat", "hub", 3, 13, ability="clot")]
        document = {"schema": 1, "kind": "balance_catalog", "tree": {"root": "neutrophil.round_damage", "capstone_threshold": 6, "nodes": nodes},
                    "cells": [{"key": "neutrophil", "root": None}, {"key": "macrophage", "root": "macrophage.unlock"}],
                    "abilities": [{"key": key, "name": key, "cooldown_seconds": cooldown, "root": f"ability.{key}.unlock"}
                                  for key, cooldown in (("complement", 60), ("fever", 60), ("clot", 75))]}
    catalog = Catalog(document)
    level = read_json(ROOT / "assets/balance/levels/open_lane.json")

    class GenerationTests(unittest.TestCase):
        def test_all_generated_nodes_have_matching_legal_other_grants(self):
            for target in catalog.nodes.values():
                if target["applicability"]["status"] == "requires_named_enemies":
                    continue
                with self.subTest(node=target["key"]):
                    experiment, metadata = build_experiment(catalog, target, level, Path("unused-config"), [101, 202], 300, 18000)
                    validate_generated(catalog, target, experiment)
                    self.assertEqual(metadata["rank_prices"], target["rank_prices"])
                    self.assertEqual(len(experiment["variants"]), target["max_rank"] + 1)

        def test_capstones_use_advertised_prerequisites_without_inventing_stat_gates(self):
            for target in catalog.nodes.values():
                if target["kind"] != "capstone":
                    continue
                grants = catalog.prerequisites(target, composition(catalog, target))
                self.assertGreaterEqual(catalog.branch_points(grants, target["branch"]), catalog.threshold)
                if catalog.threshold == 0:
                    self.assertTrue(all(rank <= 1 for rank in grants.values()), "A discovery must not require grinding stat ranks")
                self.assertEqual(grants[target["key"]], 0)
                catalog.validate_grants({**grants, target["key"]: 1})

        def test_innate_root_baseline_does_not_buy_itself(self):
            root = catalog.nodes[catalog.document["tree"]["root"]]
            grants = catalog.prerequisites(root, composition(catalog, root))
            self.assertEqual(grants, {root["key"]: 0})

        def test_ability_masks_are_individual_and_support_is_shared(self):
            for target in catalog.nodes.values():
                if not target.get("ability"):
                    continue
                experiment, _ = build_experiment(catalog, target, level, Path("unused-config"), [101], 300, 18000)
                self.assertEqual(experiment["base"]["allowed_abilities"], [target["ability"]])
                if "actions" in experiment["base"]:
                    self.assertEqual(experiment["base"]["controller"]["policy"], "none")
                else:
                    self.assertTrue(experiment["base"]["controller"]["use_abilities"])
                if target["kind"] == "ability_stat":
                    self.assertEqual(experiment["base"]["progression"]["grant_tree"][catalog.abilities[target["ability"]]["root"]], 1)

        def test_cost_totals_use_catalog_rank_curve(self):
            for target in catalog.nodes.values():
                for rank in range(1, target["max_rank"] + 1):
                    cost = catalog.cost({target["key"]: rank})
                    self.assertEqual(cost["memory_cells"], sum(p["memory_cells"] for p in target["rank_prices"][:rank]))
                    self.assertEqual(cost["antibodies"], sum(p["antibodies"] for p in target["rank_prices"][:rank]))

        def test_direct_ability_pulses_share_zero_wallet_and_nonreplicating_pressure(self):
            original = copy.deepcopy(level)
            for target in catalog.nodes.values():
                if target.get("ability") not in {"complement", "histamine"}:
                    continue
                experiment, metadata = build_experiment(catalog, target, level, Path("unused-config"), [101, 202], 600, 18000)
                base = experiment["base"]
                self.assertEqual(base["opening_atp"], 0)
                self.assertEqual(base["spend_limit"], 0)
                self.assertEqual(base["controller"]["policy"], "tactical")
                self.assertTrue(base["controller"]["use_abilities"])
                pulses = base["level_document"]["waves"][0]["spawns"]
                self.assertEqual(len(pulses), 3)
                self.assertTrue(all(p["family"] == "bacteria" and p["count"] == 40 and p["duration"] == 0.5 for p in pulses))
                self.assertEqual(metadata["diagnostic_fixture"]["defensive_wallet"], 0)
            self.assertEqual(level, original)

        def test_paid_ability_contexts_keep_inputs_and_normal_api_shared_across_ranks(self):
            original = copy.deepcopy(level)
            for target in catalog.nodes.values():
                if target.get("ability") not in {"fever", "clot"}:
                    continue
                with self.subTest(node=target["key"]):
                    experiment, metadata = build_experiment(catalog, target, level, Path("unused-config"), [101], 600, 18000)
                    base = experiment["base"]
                    self.assertEqual(base["allowed_cells"], ["neutrophil"])
                    self.assertEqual(base["opening_atp"], base["spend_limit"])
                    self.assertGreater(base["opening_atp"], 0)
                    self.assertEqual(metadata["diagnostic_fixture"]["defensive_wallet"], base["opening_atp"])
                    self.assertNotIn("zero defensive budget", base["level_document"]["description"])
                    for variant in experiment["variants"]:
                        arm = lab.merge(base, variant["patch"])
                        arm["progression"]["grant_tree"][target["key"]] = 0
                        self.assertEqual(arm, base, "Only the named tree rank may vary")
                    actions = base.get("actions", [])
                    self.assertTrue(all(a["kind"] in {"deploy", "cast", "start_wave"} for a in actions))
                    if actions:
                        self.assertEqual(actions[0]["kind"], "deploy")
                        self.assertEqual(actions[0]["quantity"], metadata["diagnostic_fixture"]["paid_quantity"])
                        self.assertTrue(all(a["ability"] == target["ability"] for a in actions if a["kind"] == "cast"))
            self.assertEqual(level, original)

        def test_fever_injury_and_clot_duration_have_real_opportunity_contexts(self):
            fever = catalog.nodes.get("ability.fever.magnitude")
            clot = catalog.nodes.get("ability.clot.duration")
            self.assertIsNotNone(fever)
            self.assertIsNotNone(clot)
            experiment, _ = build_experiment(catalog, fever, level, Path("unused-config"), [101], 600, 18000)
            base = experiment["base"]
            self.assertEqual(base["overrides"]["enemies.families.bacteria.attack.toxin_damage"], 0.0015)
            self.assertGreater(base["overrides"]["towers.neutrophil.payload.round_damage"], 0)
            self.assertEqual(base["level_document"]["win"]["survive_seconds"], 180)
            self.assertTrue(all(not a["required"] for a in base["actions"] if a["kind"] == "cast"))
            self.assertTrue(any(g["metric"] == "combat.hostile_actual_cell_damage" for g in opportunity_gates(fever)))
            experiment, _ = build_experiment(catalog, clot, level, Path("unused-config"), [101], 600, 18000)
            base = experiment["base"]
            self.assertTrue(all(s["duration"] == 20 for s in base["level_document"]["waves"][0]["spawns"]))
            casts = [a for a in base["actions"] if a["kind"] == "cast"]
            self.assertGreater(casts[1]["tick"] - casts[0]["tick"], catalog.abilities["clot"]["cooldown_seconds"] * 60)
            self.assertTrue(all(a.get("required", True) for a in casts))
            self.assertIn("enemies.bacteria.lifecycle.retired_lifetime.mean_seconds", experiment["metrics"])
            self.assertTrue(any(g["metric"].endswith("retired_lifetime.observations") for g in opportunity_gates(clot)))

        def test_paid_ability_wallet_uses_loaded_cell_price(self):
            changed = copy.deepcopy(catalog.document)
            fields = changed.setdefault("registry", {}).setdefault("fields", [])
            fields[:] = [f for f in fields if f["path"] != "towers.neutrophil.stats.build_cost"]
            fields.append({"path": "towers.neutrophil.stats.build_cost", "value": "17"})
            priced = Catalog(changed)
            for key, quantity in (("ability.fever.unlock", 10), ("ability.clot.unlock", 3)):
                experiment, _ = build_experiment(priced, priced.nodes[key], level, Path("unused-config"), [101], 600, 18000)
                self.assertEqual(experiment["base"]["opening_atp"], 17 * quantity)
                self.assertEqual(experiment["base"]["spend_limit"], 17 * quantity)

        def test_runtime_constraints_survive_generation(self):
            target = copy.deepcopy(catalog.nodes[catalog.document["tree"]["root"]])
            target["runtime_constraint"] = {"configured_baseline": 4, "engine_ceiling": 3, "runtime_baseline": 3}
            target["applicability"] = {"status": "runtime_capped", "reason": "synthetic test ceiling"}
            _, metadata = build_experiment(catalog, target, level, Path("unused-config"), [101], 600, 18000)
            self.assertEqual(metadata["runtime_constraint"], target["runtime_constraint"])
            self.assertEqual(metadata["applicability"], target["applicability"])
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                entry = {**metadata, "status": "generated", "planned_runs": 1,
                         "result_directory": "results/capped", "opportunity_gates": []}
                write_json(directory / "manifest.json", {"nodes": [entry]})
                write_json(directory / "results/capped/runs/one.record.json",
                           {"valid": True, "status": "cleared", "variant": "baseline", "report": {}})
                coverage = report_directory(directory)["nodes"][0]
                self.assertEqual(coverage["coverage_state"], "runtime_capped")
                self.assertEqual(coverage["runtime_constraint"], target["runtime_constraint"])

        def test_scar_durability_gate_observes_scar_damage_not_friendly_cell_damage(self):
            gates = opportunity_gates({"key": "fibroblast.scar_health", "kind": "stat", "ability": None, "cell": "fibroblast"})
            self.assertTrue(any(g["metric"] == "combat.hostile_requested_structure_damage" for g in gates))
            self.assertTrue(any(g["metric"] == "state.scars_lost" and g["arms"] == "baseline_only" for g in gates))
            self.assertFalse(any(g["metric"] == "combat.hostile_actual_cell_damage" for g in gates))
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                entry = {"key": "fibroblast.scar_health", "status": "generated", "planned_runs": 2,
                         "result_directory": "results/scars", "max_rank": 1,
                         "applicability": {"status": "available"}, "opportunity_gates": gates}
                write_json(directory / "manifest.json", {"nodes": [entry]})
                for variant, lost in (("baseline", 1), ("rank_1", 0)):
                    write_json(directory / f"results/scars/runs/{variant}.record.json",
                               {"valid": True, "status": "cleared", "variant": variant,
                                "report": {"combat": {"hostile_requested_structure_damage": 10},
                                           "state": {"scars_built": 1, "scars_lost": lost}}})
                self.assertEqual(report_directory(directory)["nodes"][0]["coverage_state"], "measured")

        def test_cell_unlock_opportunities_apply_only_after_the_cell_is_owned(self):
            for cell, metric in [("fibroblast", "state.scars_built"), ("goblet_cell", "summary.sampled_slow_enemy_seconds")]:
                node = {"key": cell + ".unlock", "kind": "tower_root", "ability": None, "cell": cell}
                gate = next(g for g in opportunity_gates(node) if g["metric"] == metric)
                self.assertEqual(gate["arms"], "variants_only")

        def test_role_health_cannot_be_exercised_by_damage_to_supporting_neutrophils(self):
            node = {"key": "cytotoxic.tower_health", "kind": "stat", "cell": "cytotoxic_t"}
            gates = opportunity_gates(node)
            self.assertEqual(gates[0]["metric"], "units.cytotoxic_t.lifecycle.damage_taken")
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                entry = {"key": node["key"], "status": "generated", "planned_runs": 2,
                         "result_directory": "results/health", "max_rank": 1,
                         "applicability": {"status": "available"}, "opportunity_gates": gates}
                write_json(directory / "manifest.json", {"nodes": [entry]})
                for damage in (0, 1):
                    for variant in ("baseline", "rank_1"):
                        write_json(directory / f"results/health/runs/{variant}.record.json",
                                   {"valid": True, "status": "cleared", "variant": variant,
                                    "report": {"combat": {"hostile_actual_cell_damage": 100},
                                               "units": {"cytotoxic_t": {"lifecycle": {"damage_taken": damage}}}}})
                    self.assertEqual(report_directory(directory)["nodes"][0]["coverage_state"],
                                     "unexercised" if damage == 0 else "measured")

        def test_invalid_prerequisites_are_detected(self):
            target = next(n for n in catalog.nodes.values() if n["parent"] is not None)
            with self.assertRaises(ValueError):
                catalog.validate_grants({target["key"]: 1})

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(GenerationTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    generate_parser = commands.add_parser("generate")
    generate_parser.add_argument("--exe", type=Path)
    generate_parser.add_argument("--catalog", type=Path)
    generate_parser.add_argument("--config", type=Path)
    generate_parser.add_argument("--out", type=Path, required=True)
    generate_parser.add_argument("--level", type=Path, default=Path("assets/balance/levels/open_lane.json"))
    generate_parser.add_argument("--node", action="append", default=[])
    generate_parser.add_argument("--seeds", nargs="+", type=int, default=[101, 202])
    generate_parser.add_argument("--budget", type=int, default=300)
    generate_parser.add_argument("--max-ticks", type=int, default=18000)
    run_parser = commands.add_parser("run")
    run_parser.add_argument("directory", type=Path)
    run_parser.add_argument("--exe", type=Path, required=True)
    run_parser.add_argument("--node", action="append", default=[])
    run_parser.add_argument("--jobs", type=int, default=1)
    run_parser.add_argument("--timeout", type=float, default=180)
    report_parser = commands.add_parser("report")
    report_parser.add_argument("directory", type=Path)
    tests = commands.add_parser("self-test")
    tests.add_argument("--catalog", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "self-test":
            return self_test(read_json(args.catalog) if args.catalog else None)
        if args.command == "report":
            report_directory(args.directory)
            print(args.directory / "coverage.json")
            return 0
        args.node = args.node or ["*"]
        if args.command == "generate":
            if args.budget < 0 or args.max_ticks < 1 or not args.seeds or any(seed < 0 for seed in args.seeds) or len(set(args.seeds)) != len(args.seeds):
                raise ValueError("Use budget >=0, max-ticks >0 and distinct nonnegative seeds")
            return generate(args)
        if not args.exe.is_file() or args.jobs < 1 or args.timeout <= 0:
            raise ValueError("Run needs an existing executable, jobs >=1 and timeout >0")
        return run_generated(args)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"balance_catalog: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
