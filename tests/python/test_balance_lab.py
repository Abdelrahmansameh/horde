"""Evidence contracts for the experiment orchestrator; no game executable needed."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "balance_lab.py"
SPEC = importlib.util.spec_from_file_location("balance_lab", MODULE_PATH)
lab = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lab)


def experiment(**extra):
    result = {"schema": 1, "name": "Test experiment", "kind": "paired",
              "base": {"schema": 1, "level": "assets/levels/campaign_01_first_bend.json", "max_ticks": 6000},
              "seeds": [11, 22, 33], "metric": "objective_integrity",
              "policies": [{"name": "competent", "controller": {"policy": "competent", "reserve_atp": 20}}],
              "variants": [{"name": "baseline", "patch": {}},
                           {"name": "upgrade", "patch": {"progression": {"grant_tree": {"neutrophil.damage": 1}}}}]}
    result.update(extra)
    return result


def record(variant="baseline", seed=11, status="cleared", integrity=80, valid=True, phase="evaluation", policy="competent", **extra):
    result = {"id": f"{policy}-{variant}-{seed}", "variant": variant, "seed": seed, "phase": phase,
              "policy": policy, "valid": valid, "status": status, "error": "",
              "spec_path": "runs/spec.json", "report_path": "runs/report.json", "stderr_path": "runs/stderr.log",
              "report": {"schema": 1, "summary": {"valid": valid, "status": status, "objective_integrity": integrity},
                         "timeline": [{"tick": 0, "objective_integrity": 100}, {"tick": 600, "objective_integrity": integrity}]}}
    result.update(extra)
    return result


class PlanningTests(unittest.TestCase):
    def test_paired_design_keeps_seeds_controller_and_base_unchanged(self):
        exp = experiment()
        original = json.dumps(exp, sort_keys=True)
        runs = lab.plan_runs(exp)
        self.assertEqual(len(runs), 6)
        self.assertEqual(len({r["id"] for r in runs}), 6)
        self.assertEqual(json.dumps(exp, sort_keys=True), original)
        for seed in exp["seeds"]:
            pair = [r for r in runs if r["seed"] == seed]
            self.assertEqual(pair[0]["spec"]["controller"], pair[1]["spec"]["controller"])
            self.assertEqual(pair[0]["spec"]["level"], pair[1]["spec"]["level"])
        self.assertEqual(runs, lab.plan_runs(exp))

    def test_nested_patch_preserves_other_controller_settings(self):
        result = lab.merge({"controller": {"policy": "competent", "weights": [1, 2]}, "a": 1},
                           {"controller": {"weights": [3]}})
        self.assertEqual(result, {"controller": {"policy": "competent", "weights": [3]}, "a": 1})

    def test_sensitivity_sweep_is_one_axis_at_a_time(self):
        exp = experiment(kind="sweep", variants=[{"name": "baseline", "patch": {}}],
                         sweeps=[{"name": "density", "override": "enemies.families.virus.behavior.base_density", "values": [1, 2]},
                                 {"name": "wallet", "path": "opening_atp", "values": [100, 200]}])
        variants = lab.expand_variants(exp)
        self.assertEqual(len(variants), 5)
        self.assertEqual(variants[-1]["patch"], {"opening_atp": 200})
        self.assertEqual(variants[1]["patch"], {"overrides": {"enemies.families.virus.behavior.base_density": 1}})

    def test_tree_sweep_preserves_stable_key_with_dots(self):
        exp = experiment(kind="sweep", variants=[{"name": "baseline", "patch": {}}],
                         sweeps=[{"name": "damage", "tree": "neutrophil.round_damage", "values": [1, 3]}])
        self.assertEqual(lab.expand_variants(exp)[1]["patch"], {"progression": {"grant_tree": {"neutrophil.round_damage": 1}}})

    def test_duplicate_seeds_and_train_test_leakage_rejected(self):
        with self.assertRaises(ValueError):
            lab.plan_runs(experiment(seeds=[11, 11]))
        with self.assertRaises(ValueError):
            lab.plan_runs(experiment(kind="search", train_seeds=[11, 22], heldout_seeds=[22, 33]), "training")

    def test_command_preserves_spaces_as_independent_arguments(self):
        result = lab.command(Path("C:/game build/immune.exe"), Path("C:/run data/spec.json"), Path("C:/run data/report.json"))
        self.assertEqual(result[1], "--balance-run")
        self.assertEqual(result[3], "--out")
        self.assertEqual(len(result), 5)
        self.assertIn("run data", result[2])

    def test_campaign_level_and_save_paths_are_resolved(self):
        result = lab.normalize_spec({"campaign": {"levels": ["a.json", "b.json"]}, "progression": {"save": "save.json"}}, Path.cwd())
        self.assertTrue(all(Path(p).is_absolute() for p in result["campaign"]["levels"]))
        self.assertTrue(Path(result["progression"]["save"]).is_absolute())

    def test_curated_first_wave_slices_match_authored_gameplay(self):
        authored = lab.read_json(lab.ROOT / "assets/levels/campaign_01_first_bend.json")
        authored.pop("description", None)
        authored["waves"] = authored["waves"][:1]
        for document in [lab.read_json(lab.ROOT / "assets/balance/levels/campaign_first_wave_reference.json"),
                         lab.read_json(lab.ROOT / "assets/balance/references/fresh_first_wave_paid_actions.json")["level_document"]]:
            document.pop("description", None)
            self.assertEqual(document, authored, "Refresh the first-wave fixture and requalify the controller when campaign gameplay changes")

    def test_planning_rejects_drifted_current_reference_but_keeps_historical_documents(self):
        path = lab.ROOT / "assets/balance/levels/campaign_first_wave_reference.json"
        with mock.patch.object(lab, "read_json", side_effect=[{"waves": [{}], "economy": {"starting_atp": 1}},
                                                            {"waves": [{}, {}], "economy": {"starting_atp": 300}}]):
            with self.assertRaisesRegex(ValueError, "Refresh the exact first-wave slice"):
                lab.normalize_spec({"level": str(path)})
        historical = {"level": str(path.resolve()), "level_document": {"name": "campaign_01_first_bend", "waves": []}}
        with mock.patch.object(lab, "read_json", side_effect=AssertionError("Historical document must not reload current assets")):
            self.assertEqual(lab.normalize_spec(historical), {**historical, "schema": 1})


class StatisticsTests(unittest.TestCase):
    def test_wilson_interval_does_not_claim_certainty_for_perfect_small_sample(self):
        low, high = lab.wilson(3, 3)
        self.assertAlmostEqual(low, .4385029682)
        self.assertAlmostEqual(high, 1)
        self.assertIsNone(lab.wilson(0, 0))
        self.assertGreater(lab.wilson(0, 3)[1], .5)

    def test_bootstrap_mean_and_single_sample_contract(self):
        stats = lab.mean_stats([2, 2, 2])
        self.assertEqual(stats["mean"], 2)
        self.assertEqual(stats["ci95"], [2, 2])
        self.assertIsNone(lab.mean_stats([2])["ci95"])
        self.assertIsNone(lab.mean_stats([])["mean"])
        self.assertEqual(lab.mean_stats([1, 2, 3]), lab.mean_stats([1, 2, 3]))

    def test_invalid_and_timeouts_are_omitted_from_pairs_never_turned_into_wins(self):
        rows = [record(seed=11), record(seed=22), record(seed=33),
                record("upgrade", 11, integrity=85), record("upgrade", 22, status="timeout"),
                record("upgrade", 33, valid=False, status="invalid")]
        out = lab.summarize(rows, experiment())
        upgraded = next(g for g in out["groups"] if g["variant"] == "upgrade")
        self.assertEqual((upgraded["terminal"], upgraded["timeouts"], upgraded["invalid"]), (1, 1, 1))
        self.assertEqual(upgraded["clear_rate_lower_bound_all_attempted"], 1 / 3)
        paired = out["paired"][0]
        self.assertEqual(paired["paired_seeds"], [11])
        self.assertEqual(paired["omitted_seeds"], [22, 33])
        self.assertEqual(paired["metric_delta"]["mean"], 5)

    def test_pairs_do_not_mix_policies_or_phases(self):
        rows = [record(), record("upgrade", integrity=85),
                record("upgrade", policy="novice", integrity=90),
                record("upgrade", phase="heldout", integrity=99)]
        paired = lab.paired_comparisons(rows, "baseline", "objective_integrity")
        self.assertEqual(sum(len(p["paired_seeds"]) for p in paired), 1)

    def test_nonfinite_metric_cannot_pollute_summary(self):
        rows = [record(integrity=float("nan"))]
        self.assertEqual(lab.summarize(rows, experiment())["groups"][0]["metric_stats"]["n"], 0)


class ValidationTests(unittest.TestCase):
    def test_unknown_status_missing_valid_exit_failure_rejected(self):
        report = record()["report"]
        self.assertEqual(lab.checked_report(report), (True, "cleared", ""))
        self.assertFalse(lab.checked_report(report, exit_code=1)[0])
        report["summary"]["status"] = "probably-won"
        self.assertEqual(lab.checked_report(report)[1], "invalid")
        report["summary"]["status"] = "cleared"
        del report["summary"]["valid"]
        self.assertFalse(lab.checked_report(report)[0])

    def test_process_timeout_cannot_accept_successful_partial_report(self):
        valid, status, error = lab.checked_report(record()["report"], process_timeout=True)
        self.assertFalse(valid)
        self.assertEqual(status, "timeout")
        self.assertIn("wall-clock", error)

    def test_stale_report_removed_before_failed_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            run = lab.plan_runs(experiment())[0]
            lab.write_json(directory / f"{run['id']}.report.json", record()["report"])
            with mock.patch.object(lab.subprocess, "run", side_effect=OSError("cannot execute")):
                result = lab.execute_one(run, Path("immune.exe"), directory, 1)
            self.assertEqual(result["status"], "invalid")
            self.assertFalse((directory / f"{run['id']}.report.json").exists())

    def test_stdout_json_fallback_and_spec_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = lab.plan_runs(experiment())[0]
            process = subprocess.CompletedProcess([], 0, json.dumps(record()["report"]), "diagnostic")
            with mock.patch.object(lab.subprocess, "run", return_value=process) as call:
                result = lab.execute_one(run, Path("immune.exe"), Path(tmp), 3)
            self.assertTrue(result["valid"])
            self.assertEqual(call.call_args.kwargs["timeout"], 3)
            self.assertIsInstance(call.call_args.args[0], list)
            self.assertTrue((Path(tmp) / f"{run['id']}.spec.json").is_file())

    def test_pinned_config_documents_are_materialized_per_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = lab.plan_runs(experiment())[0]
            run["pinned_config"] = [{"schema": 1, "label": name} for name in lab.CONFIG_NAMES]
            process = subprocess.CompletedProcess([], 0, json.dumps(record()["report"]), "")
            with mock.patch.object(lab.subprocess, "run", return_value=process):
                result = lab.execute_one(run, Path("immune.exe"), Path(tmp), 3)
            saved = lab.read_json(Path(tmp) / f"{run['id']}.spec.json")
            self.assertEqual(lab.read_json(Path(saved["config_dir"]) / "enemies.json")["label"], "enemies")
            self.assertNotIn("pinned_config", result)

    def test_different_executable_cannot_silently_satisfy_pinned_experiment(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = lab.plan_runs(experiment())[0]
            run["expected_executable_hash"] = 123
            report = record()["report"]
            report["provenance"] = {"executable_hash": 456}
            process = subprocess.CompletedProcess([], 0, json.dumps(report), "")
            with mock.patch.object(lab.subprocess, "run", return_value=process):
                result = lab.execute_one(run, Path("immune.exe"), Path(tmp), 3)
            self.assertFalse(result["valid"])
            self.assertEqual(result["status"], "invalid")
            self.assertIn("Pinned executable_hash mismatch", result["error"])


class SearchTests(unittest.TestCase):
    def test_strategy_selection_rejects_incomplete_winner_and_holds_out_only_selected(self):
        exp = experiment(kind="search", train_seeds=[11, 22], heldout_seeds=[101, 102])
        rows = [record(seed=11, integrity=80, phase="training"), record(seed=22, integrity=70, phase="training"),
                record("upgrade", 11, integrity=100, phase="training"), record("upgrade", 22, status="timeout", phase="training")]
        selected, rankings = lab.search_selection(rows, exp)
        self.assertEqual(selected, {("competent", "baseline")})
        heldout = lab.plan_runs(exp, "heldout", selected)
        self.assertEqual({r["seed"] for r in heldout}, {101, 102})
        self.assertEqual({r["variant"] for r in heldout}, {"baseline"})

    def test_budget_threshold_is_inconclusive_on_timeout_and_detects_nonmonotonicity(self):
        exp = experiment(kind="budget", seeds=[11, 22], budgets=[100, 200, 300, 400], target_win_rate=1)
        rows = []
        outcomes = {100: ["defeated", "defeated"], 200: ["cleared", "cleared"],
                    300: ["defeated", "cleared"], 400: ["cleared", "timeout"]}
        for budget, statuses in outcomes.items():
            rows.extend(record(f"budget={budget}", seed, status=status, budget=budget) for seed, status in zip([11, 22], statuses))
        result = lab.budget_thresholds(rows, exp)[0]
        self.assertEqual(result["lowest_tested_passing_budget"], 200)
        self.assertEqual(result["highest_tested_failing_budget_below"], 100)
        self.assertEqual(result["inconclusive_budgets"], [400])
        self.assertEqual(result["monotonic_violations"], [{"seed": 11, "won_budget": 200, "lost_budget": 300}])


class JourneyAndReportTests(unittest.TestCase):
    def test_ttk_zero_is_observed_and_censored_missing_is_not_zero(self):
        instant = lab.distribution_stats({"observations": 2, "sum_ticks": 0, "min_ticks": 0, "max_ticks": 0, "bins": [2, 0, 0]})
        self.assertEqual(instant["mean_seconds"], 0)
        self.assertEqual(instant["p90_seconds"], [0, 0])
        empty = lab.distribution_stats({"observations": 0})
        self.assertIsNone(empty["mean_seconds"])
        long = lab.distribution_stats({"observations": 3, "sum_ticks": 8, "min_ticks": 2, "max_ticks": 4, "bins": [0, 0, 2, 1]})
        self.assertEqual(long["p50_seconds"], [2 / 60, 3 / 60])
        self.assertEqual(long["p90_seconds"], [4 / 60, 7 / 60])

    def test_lifecycle_table_and_pairs_preserve_damage_death_conditioning(self):
        baseline = record()
        baseline["report"]["combat"] = {"enemy_lifetimes": {"virus": {"damage_deaths": 2, "alive": 8, "hit_alive": 4,
            "damage_ttk": {"observations": 2, "sum_ticks": 120, "min_ticks": 30, "max_ticks": 90, "bins": [0] * 6 + [1, 1]}}}}
        upgraded = record("upgrade")
        upgraded["report"]["combat"] = {"enemy_lifetimes": {"virus": {"alive": 10, "hit_alive": 5, "damage_ttk": {"observations": 0}}}}
        summary = lab.summarize([baseline, upgraded], experiment())
        self.assertEqual(summary["lifecycles"][0]["hit_alive"], 4)
        self.assertIsNone(summary["paired_ttk"][0]["damage_ttk_mean_seconds_delta"]["mean"])
        self.assertEqual(summary["paired_ttk"][0]["omitted_no_damage_death_seeds"], [11])
        self.assertIn("censored", lab.render_html(summary, [baseline, upgraded]))

    def test_release_rate_is_separate_from_total_population(self):
        rows = []
        for variant, count, duration in [("baseline", 600, 30), ("compressed", 600, 15), ("longer", 1200, 60)]:
            r = record(variant)
            r["report"].update({"replay_spec": {"level_document": {"waves": [{"spawns": [{"family": "virus", "count": count, "duration": duration}]}]}},
                "waves": [{"ticks": 6000, "density_removed": 300}],
                "provenance": {"effective_config": {"enemies": {"families": {"virus": {"behavior": {"base_density": .6}}}}}},
                "actions": [{"kind": "deploy", "tick": 100, "deployed": 2, "atp_spent": 30}]})
            rows.append(r)
        pressure = lab.wave_pressure_metrics(rows)
        self.assertEqual([p["authored_peak_agents_per_second"] for p in pressure], [20, 40, 20])
        self.assertEqual([p["authored_count"] for p in pressure], [600, 600, 1200])
        self.assertEqual(pressure[0]["cells_bought_in_interval"], 2)
        self.assertEqual(pressure[0]["atp_spent_in_interval"], 30)
        self.assertEqual(pressure[0]["interval_density_removed_per_second"], 3)
        array_report = json.loads(json.dumps(rows[0]))
        named = array_report["report"]["provenance"]["effective_config"]
        array_report["report"]["provenance"]["effective_config"] = [named.get(key, {}) for key in lab.CONFIG_NAMES]
        actual = lab.wave_pressure_metrics([array_report])[0]
        self.assertEqual(actual["authored_peak_density_per_second"], 12)

    def test_journey_counts_only_real_terminal_rewards(self):
        row = record(status="completed")
        row["report"].update({"kind": "campaign", "attempts": [
            {"level_index": 0, "summary": {"status": "defeated", "valid": True}, "reward": {"memory_cells": 25}},
            {"level_index": 0, "summary": {"status": "cleared", "valid": True}, "reward": {"memory_cells": 154, "antibodies": 1}},
            {"level_index": 1, "summary": {"status": "timeout", "valid": True}, "reward": {"memory_cells": 999}},
        ], "purchases": [{"key": "neutrophil.round_damage", "levels": 1}], "final_progression": {"memory_cells": 159}})
        metrics = lab.campaign_metrics(row)
        self.assertEqual(metrics["attempts"], 3)
        self.assertEqual(metrics["terminal_attempts"], 2)
        self.assertEqual(metrics["memory_cells_earned"], 179)
        self.assertEqual(metrics["antibodies_earned"], 1)
        self.assertEqual(metrics["retries"], 1)
        self.assertEqual(metrics["levels_cleared"], 1)
        out = lab.summarize([row], experiment(kind="campaign"))
        self.assertEqual(len(out["campaigns"]), 1)
        self.assertIn("Campaign journeys", lab.render_html(out, [row]))

    def test_report_is_standalone_has_artifact_links_and_escapes_content(self):
        row = record(variant='<script>alert("x")</script>')
        result = lab.render_html(lab.summarize([row], experiment()), [row])
        self.assertIn("<svg", result)
        self.assertIn('href="runs/spec.json"', result)
        self.assertIn("Seed variation is not human skill variation", result)
        self.assertNotIn('<script>alert("x")</script>', result)
        self.assertIn("&lt;script&gt;", result)
        self.assertNotIn("https://", result)

    def test_disk_report_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            lab.write_json(directory / "experiment.json", experiment())
            lab.write_json(directory / "runs" / "a.record.json", record())
            summary = lab.report_directory(directory)
            self.assertEqual(summary["groups"][0]["wins"], 1)
            self.assertTrue((directory / "report.html").exists())
            self.assertEqual(lab.read_json(directory / "summary.json"), summary)


class GrowthTests(unittest.TestCase):
    def probe(self):
        return {"base": {"schema": 1, "level_document": {"schema": 2, "name": "probe",
                    "waves": [{"spawns": [{"family": "virus", "count": 100, "duration": 10}]}]},
                    "controller": {"policy": "competent", "use_abilities": False},
                    "opening_atp": 300, "spend_limit": 300, "income_enabled": False},
                "pressure_scales": [1, 2, 4], "seeds": [91, 92], "minimum_clear_rate": 1}

    def journey(self):
        row = record(status="stalled")
        row["report"].update({"kind": "campaign", "attempts": [
            {"level_index": 0, "summary": {"status": "cleared", "valid": True},
             "progression_before": {"tree": {}}, "progression_after": {"tree": {"neutrophil.round_damage": 1}}},
            {"level_index": 1, "summary": {"status": "defeated", "valid": True},
             "progression_before": {"tree": {"neutrophil.round_damage": 2}},
             "progression_after": {"tree": {"neutrophil.round_damage": 2}}}],
            "provenance": {"level_documents": [{"name": f"level-{i}", "waves": [{"spawns": [
                {"family": "virus", "count": 100 * 2 ** i, "duration": 10}]}]} for i in range(3)]}})
        return row

    def calibration_records(self, journey, all_pass=False):
        result = []
        for x in [0, 1]:
            for pressure in [1, 2, 4]:
                for seed in [91, 92]:
                    status = "cleared" if all_pass or pressure <= 2 ** x else "defeated"
                    row = record(variant=f"stage{x}-{pressure}", seed=seed, status=status, phase="power", kind="power_probe")
                    row["probe"] = {"journey_id": journey["id"], "snapshot_id": "initial" if x == 0 else "after-level-0",
                                    "x": x, "level_index": x - 1, "pressure_scale": pressure, "basis": "actual purchased progression"}
                    result.append(row)
        return result

    def test_probe_planning_uses_actual_purchases_and_only_changes_counts(self):
        journey, probe = self.journey(), self.probe()
        exp = experiment(kind="campaign", power_probe=probe)
        before = json.dumps(journey, sort_keys=True)
        runs = lab.plan_power_runs([journey], exp)
        self.assertEqual(len(runs), 12)
        self.assertEqual(json.dumps(journey, sort_keys=True), before)
        stage = next(r for r in runs if r["probe"]["snapshot_id"] == "after-level-0" and r["probe"]["pressure_scale"] == 4)
        self.assertEqual(stage["spec"]["progression_snapshot"]["tree"]["neutrophil.round_damage"], 2)
        spawn = stage["spec"]["level_document"]["waves"][0]["spawns"][0]
        self.assertEqual(spawn["count"], 400)
        self.assertEqual(spawn["duration"], 10)
        self.assertEqual(stage["spec"]["opening_atp"], 300)
        self.assertFalse(stage["spec"]["income_enabled"])

    def test_snapshot_selection_limits_cost_without_replacing_actual_progression(self):
        journey, probe = self.journey(), self.probe()
        last = journey["report"]["attempts"][-1]
        last["summary"]["status"] = "cleared"
        probe["snapshot_ids"] = ["initial", "after-level-0"]
        stages = lab.progression_snapshots(journey, probe)
        self.assertEqual([s["id"] for s in stages], ["initial", "after-level-0"])
        self.assertEqual(stages[1]["progression"]["tree"]["neutrophil.round_damage"], 2)

    def test_integral_pressure_scales_have_same_run_identity_as_json_floats(self):
        journey, probe = self.journey(), self.probe()
        integers = lab.plan_power_runs([journey], experiment(kind="campaign", power_probe=probe))
        probe["pressure_scales"] = [float(scale) for scale in probe["pressure_scales"]]
        floating = lab.plan_power_runs([journey], experiment(kind="campaign", power_probe=probe))
        self.assertEqual([r["id"] for r in integers], [r["id"] for r in floating])
        self.assertEqual(integers, floating)

    def test_fixed_probe_wallet_required(self):
        exp = experiment(kind="campaign", power_probe=self.probe())
        lab.validate_experiment(exp)
        exp["power_probe"]["base"]["income_enabled"] = True
        with self.assertRaises(ValueError):
            lab.validate_experiment(exp)

    def test_wallet_modes_use_recorded_journey_money_and_do_not_pool_stages(self):
        journey, probe = self.journey(), self.probe()
        journey["report"]["attempts"][0]["economy"] = {"opening_atp": 300}
        journey["report"]["attempts"][1]["economy"] = {"opening_atp": 460}
        probe["wallet_modes"] = ["fixed", "journey_opening"]
        runs = lab.plan_power_runs([journey], experiment(kind="campaign", power_probe=probe))
        self.assertEqual(len(runs), 24)
        for run in runs:
            if run["probe"]["snapshot_id"] == "after-level-0":
                expected = 460 if run["probe"]["wallet_mode"] == "journey_opening" else 300
                self.assertEqual(run["spec"]["opening_atp"], expected)
                self.assertEqual(run["spec"]["spend_limit"], expected)
        rows = self.calibration_records(journey)
        additional = json.loads(json.dumps(rows))
        for row in additional:
            row["probe"]["wallet_mode"] = "journey_opening"
        result = lab.power_calibrations(rows + additional, experiment(power_probe=probe))
        self.assertEqual(len(result), 4)
        self.assertTrue(all(r["capacity_estimate_scale"] is not None for r in result))

    def test_calibration_refinement_pins_historical_canonical_and_copies_only_journey(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp) / "old", Path(tmp) / "refined"
            probe = self.probe()
            canonical = probe["base"].pop("level_document")
            probe["base"]["level"] = "a-source-file-that-no-longer-exists.json"
            exp = experiment(kind="campaign", power_probe=probe)
            lab.write_json(source / "experiment.json", exp)
            journey = self.journey()
            lab.write_json(source / "runs" / "journey.record.json", journey)
            lab.write_json(source / journey["spec_path"], {"schema": 1})
            lab.write_json(source / journey["report_path"], journey["report"])
            (source / journey["stderr_path"]).write_text("", encoding="utf-8")
            prior = record(phase="power", kind="power_probe")
            prior["probe"] = {"pressure_scale": 1}
            prior["report"]["replay_spec"] = {"level_document": canonical}
            lab.write_json(source / "runs" / "old-power.record.json", prior)
            new_exp, copied = lab.calibration_inputs(source, output, [.125, .25])
            self.assertEqual(len(copied), 1)
            self.assertEqual(new_exp["power_probe"]["base"]["level_document"], canonical)
            self.assertEqual(new_exp["power_probe"]["pressure_scales"], [.125, .25])
            self.assertTrue((output / journey["spec_path"]).is_file())
            self.assertEqual(len(list((output / "runs").glob("*.record.json"))), 1)
            self.assertEqual(lab.read_json(source / "experiment.json"), exp)

    def test_log_fit_distinguishes_multiplicative_growth(self):
        fit = lab.log_growth([(0, 100), (1, 200), (2, 400)])
        self.assertAlmostEqual(fit["geometric_factor_per_level"], 2)
        self.assertAlmostEqual(fit["log_linear_factor_per_level"], 2)
        self.assertAlmostEqual(fit["log_fit_r_squared"], 1)
        self.assertEqual([r["factor"] for r in fit["ratios"]], [2, 2])
        self.assertIsNone(lab.log_growth([(0, 100)])["geometric_factor_per_level"])

    def test_empirical_power_brackets_and_relative_growth(self):
        journey = self.journey()
        exp = experiment(kind="campaign", power_probe=self.probe(),
                         growth_targets={"enemy_factor_per_level": 1.5, "player_factor_per_level": 1.5})
        rows = [journey] + self.calibration_records(journey)
        summary = lab.summarize(rows, exp)
        initial, later = sorted(summary["power_calibrations"], key=lambda r: r["x"])
        self.assertEqual((initial["capacity_lower_scale"], initial["capacity_upper_scale"]), (1, 2))
        self.assertEqual((later["capacity_lower_scale"], later["capacity_upper_scale"]), (2, 4))
        self.assertEqual(initial["relative_to_initial_estimate"], 1)
        self.assertEqual((initial["relative_to_initial_lower"], initial["relative_to_initial_upper"]), (1, 1))
        self.assertAlmostEqual(later["relative_to_initial_estimate"], 2)
        growth = summary["growth"][0]
        self.assertEqual(growth["hypotheses"]["player"]["comparison"], "at_or_above_hypothesis")
        self.assertEqual(len(growth["enemy_levels"]), 3)  # Includes unreached authored level.
        self.assertAlmostEqual(growth["player_pressure_growth"]["geometric_factor_per_level"], 2)
        self.assertEqual(len(summary["groups"]), 1)  # Calibration runs do not inflate journey success rates.
        self.assertIn("logarithmic y scale", lab.render_html(summary, rows))

    def test_all_passing_grid_is_censored_not_exact_power_or_target_success(self):
        journey = self.journey()
        rows = [journey] + self.calibration_records(journey, all_pass=True)
        exp = experiment(kind="campaign", power_probe=self.probe(), growth_targets={"player_factor_per_level": 1.5})
        growth = lab.summarize(rows, exp)["growth"][0]
        self.assertTrue(all(r["right_censored"] for r in growth["power_calibration"]))
        self.assertTrue(all(r["capacity_estimate_scale"] is None for r in growth["power_calibration"]))
        self.assertEqual(growth["hypotheses"]["player"]["comparison"], "inconclusive")

    def test_timeout_and_nonmonotonic_pressure_block_power_estimate(self):
        journey = self.journey()
        rows = self.calibration_records(journey)
        rows[0]["status"] = "defeated"  # Seed91 fails at1, succeeds at2 below.
        rows[2]["status"] = "cleared"
        rows[-1]["status"] = "timeout"
        results = lab.power_calibrations(rows, experiment(power_probe=self.probe()))
        self.assertTrue(results[0]["monotonic_violations"])
        self.assertIsNone(results[0]["capacity_estimate_scale"])
        self.assertEqual(results[1]["unresolved_scales"], [4])
        self.assertIsNone(results[1]["capacity_estimate_scale"])


class ControllerAdequacyTests(unittest.TestCase):
    def calibration(self):
        return {"schema": 1, "references": [{"name": "Known achievable fresh first wave", "level": "campaign_01_first_bend",
            "source": "Player reference", "profile": "fresh", "policies": ["competent"],
            "minimum_waves_cleared": 1, "minimum_samples": 2, "minimum_success_rate": 1}]}

    def observed(self, seed, waves=0, status="defeated"):
        row = record(seed=seed, status=status)
        row["report"]["summary"]["waves_cleared"] = waves
        row["report"]["spec"] = {"progression": {}}
        row["report"]["replay_spec"] = {"level_document": {"name": "campaign_01_first_bend"}}
        row["report"]["provenance"] = {"seed": seed, "progression": {"tree": {}},
            "synthetic_progression": False, "experimental_rules": False}
        return row

    def test_loss_without_reference_is_uncalibrated_and_never_generates_nerf_advice(self):
        summary = lab.summarize([self.observed(11)], experiment())
        assessment = summary["controller_adequacy"]
        self.assertEqual(assessment["status"], "uncalibrated")
        self.assertIsNone(assessment["automatic_gameplay_tuning_recommendation"])
        rendered = lab.render_html(summary, [self.observed(11)])
        self.assertLess(rendered.index("Controller adequacy:"), rendered.index("Outcomes by controller"))

    def test_known_achievable_failure_is_controller_calibration_failure(self):
        rows = [self.observed(11), self.observed(22)]
        summary = lab.summarize(rows, experiment(controller_calibration=self.calibration()))
        assessment = summary["controller_adequacy"]
        self.assertEqual(assessment["status"], "reference_failed")
        self.assertIn("controller/input calibration failure", assessment["interpretation"])
        self.assertIn("gameplay balance remains inconclusive", assessment["interpretation"])
        self.assertFalse(assessment["reference_scope_comparisons_supported"])
        self.assertIsNone(assessment["automatic_gameplay_tuning_recommendation"])

    def test_cleared_first_wave_can_pass_reference_even_when_later_wave_loses(self):
        rows = [self.observed(11, waves=1), self.observed(22, waves=2)]
        result = lab.controller_adequacy(rows, experiment(controller_calibration=self.calibration()))
        self.assertEqual(result["status"], "reference_passed")
        self.assertTrue(result["reference_scope_comparisons_supported"])
        self.assertIsNone(result["automatic_gameplay_tuning_recommendation"])
        self.assertEqual([s["level"] for s in result["qualified_scopes"]], ["campaign_01_first_bend"])

    def test_synthetic_profiles_modified_wallets_and_scripts_do_not_qualify_automatic_fresh_play(self):
        synthetic, modified, scripted = self.observed(11, 1), self.observed(22, 1), self.observed(33, 1)
        synthetic["report"]["provenance"]["synthetic_progression"] = True
        modified["report"]["provenance"]["experimental_rules"] = True
        scripted["report"]["spec"]["actions"] = []
        result = lab.controller_adequacy([synthetic, modified, scripted], experiment(controller_calibration=self.calibration()))
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["reference_checks"][0]["terminal"], 0)

    def test_training_and_pressure_probes_do_not_fill_reference_coverage(self):
        training, probe = self.observed(11, 1), self.observed(22, 1)
        training["phase"] = "training"
        probe["kind"] = "power_probe"
        result = lab.controller_adequacy([training, probe], experiment(controller_calibration=self.calibration()))
        self.assertEqual(result["status"], "inconclusive")

    def test_posthoc_reference_annotation_is_preserved_without_rewriting_original_experiment(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            exp = experiment()
            lab.write_json(directory / "experiment.json", exp)
            for seed in [11, 22]:
                lab.write_json(directory / "runs" / f"{seed}.record.json", self.observed(seed))
            result = lab.report_directory(directory, self.calibration())
            self.assertEqual(result["controller_adequacy"]["status"], "reference_failed")
            self.assertEqual(lab.read_json(directory / "experiment.json"), exp)
            self.assertEqual(lab.report_directory(directory)["controller_adequacy"], result["controller_adequacy"])
            invalid = self.calibration()
            invalid["references"][0]["expected_status"] = "probably-won"
            with self.assertRaises(ValueError):
                lab.report_directory(directory, invalid)
            self.assertEqual(lab.read_json(directory / "analysis-calibration.json"), self.calibration())

    def test_curated_automatic_reference_has_no_golden_action_script(self):
        exp = lab.read_json(lab.ROOT / "assets/balance/experiments/controller_calibration.json")
        paid = lab.read_json(lab.ROOT / "assets/balance/references/fresh_first_wave_paid_actions.json")
        self.assertNotIn("actions", exp["base"])
        self.assertTrue(paid["actions"])
        self.assertFalse(set(exp["seeds"]) & set(exp["controller_calibration"]["references"][0]["evidence"]["observed_seeds"]))


class RetryProgressionTests(unittest.TestCase):
    def test_campaign_split_labels_preserve_declared_seeds_without_search_selection(self):
        for split in ["training", "heldout"]:
            exp = experiment(kind="campaign", evaluation_split=split)
            runs = lab.plan_runs(exp)
            self.assertEqual({r["phase"] for r in runs}, {split})
            self.assertEqual({r["seed"] for r in runs}, set(exp["seeds"]))
        with self.assertRaisesRegex(ValueError, "progression target"):
            lab.validate_experiment(experiment(progression_targets={"minimum_post_loss_purchase_rate": 1.2}))

    def journey(self):
        attempts = []
        for level in range(2):
            for number, status in [(1, "defeated"), (2, "cleared")]:
                attempts.append({"kind": "run", "level_index": level, "level_attempt": number,
                    "replay_spec": {"level_document": {"name": f"level-{level}", "waves": [{}, {}, {}]}},
                    "summary": {"valid": True, "status": status, "waves_cleared": 1 if number == 1 else 3,
                                "objective_integrity": 0 if number == 1 else 80},
                    "reward": {"memory_cells": 30 if number == 1 else 100, "antibodies": int(number == 2), "first_clear": number == 2},
                    "discovery": {"offers": [{"key": "new-tool", "cost": {"memory_cells": 0, "antibodies": 1}}]} if number == 2 else {},
                    "purchases": ([{"key": "new-tool", "memory_cells": 0, "antibodies": 1}] if level and number == 1 else
                                  [{"key": "reload", "memory_cells": 20, "antibodies": 0}] if number == 2 else [])})
        row = record(status="completed")
        row["report"] = {"kind": "campaign", "summary": {"status": "completed", "levels_total": 2}, "attempts": attempts}
        return row

    def test_ordered_losses_purchases_discoveries_and_entry_difficulty_are_preserved(self):
        exp = experiment(progression_targets={"median_attempts_min": 2, "median_attempts_max": 4,
                                             "maximum_attempts_per_level": 5, "minimum_post_loss_purchase_rate": 1})
        original = self.journey()
        before = json.dumps(original, sort_keys=True)
        result = lab.progression_journeys([original], exp)[0]
        self.assertTrue(result["matches_declared_targets"])
        self.assertEqual(result["median_attempts_to_clear"], 2)
        self.assertEqual(result["observed_loss_followups"], 2)
        first = result["levels"][0]
        self.assertAlmostEqual(first["entry_wave_fraction"], 1 / 3)
        self.assertEqual(first["loss_rewards"][0]["memory_cells"], 30)
        self.assertEqual(first["loss_rewards"][0]["memory_spent_before_next_attempt"], 20)
        self.assertEqual(first["tools_bought_before_next_level"][0]["key"], "new-tool")
        self.assertEqual(len(first["discovery_offers"]), 1)
        self.assertEqual(json.dumps(original, sort_keys=True), before)

    def test_invalid_or_unreached_levels_never_satisfy_journey_pacing(self):
        original = self.journey()
        original["report"]["attempts"][-1]["summary"]["valid"] = False
        result = lab.progression_journeys([original], experiment())[0]
        self.assertFalse(result["complete"])
        self.assertIsNone(result["levels"][-1]["attempts_to_clear"])
        self.assertEqual(result["levels"][-1]["first_clear_antibodies"], 0)
        original = self.journey()
        original["report"]["summary"]["levels_total"] = 10
        result = lab.progression_journeys([original], experiment())[0]
        self.assertFalse(result["complete"])
        self.assertFalse(result["matches_declared_targets"])

    def test_missing_followup_does_not_invent_spending_or_tool_use(self):
        original = self.journey()
        original["status"] = "stalled"
        original["report"]["summary"]["status"] = "stalled"
        original["report"]["attempts"] = original["report"]["attempts"][:1]
        result = lab.progression_journeys([original], experiment())[0]
        loss = result["levels"][0]["loss_rewards"][0]
        self.assertFalse(loss["next_attempt_observed"])
        self.assertEqual(loss["purchases_before_next_attempt"], [])
        self.assertIsNone(result["post_loss_purchase_rate"])
        self.assertEqual(result["levels"][0]["tools_bought_before_next_level"], [])


if __name__ == "__main__":
    unittest.main()
