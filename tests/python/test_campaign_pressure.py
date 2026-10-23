"""Authored campaign pressure and readable-cohort contracts; no executable."""
import copy
import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("campaign_pressure", ROOT / "tools/campaign_pressure.py")
pressure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pressure)


class PressureAnalysisTests(unittest.TestCase):
    def wave(self):
        return {"name": "fixture", "prep_time": 10, "atp_reward": 45, "spawns": [
            {"family": "virus", "count": 100, "duration": 4, "spawn_point_id": "a", "squad_paths": ["a-route"]},
            {"family": "bacteria", "count": 80, "start_time": 1, "duration": 4, "spawn_point_id": "b", "squad_paths": ["b-route"]},
            {"family": "bacteria", "count": 60, "start_time": 5.25, "duration": 4, "spawn_point_id": "a", "squad_paths": ["a-route"]}]}

    def test_peak_uses_concurrent_density_and_separates_total_population(self):
        wave = self.wave()
        measured = pressure.wave_metrics(wave, {"virus": .6, "bacteria": .9})
        self.assertAlmostEqual(measured["peak_density_per_second"], 33)
        self.assertAlmostEqual(measured["authored_density"], 186)
        self.assertEqual(measured["population"], 240)
        self.assertAlmostEqual(measured["minimum_same_port_gap_seconds"], 1.25)
        longer = copy.deepcopy(wave)
        for spawn in longer["spawns"]:
            spawn["count"] *= 2
            spawn["duration"] *= 2
            spawn["start_time"] = spawn.get("start_time", 0) * 2
        doubled = pressure.wave_metrics(longer, {"virus": .6, "bacteria": .9})
        self.assertEqual(doubled["population"], 480)
        self.assertAlmostEqual(doubled["peak_density_per_second"], measured["peak_density_per_second"])

    def test_compression_retains_explicit_cohorts_and_readable_per_port_gaps(self):
        original = self.wave()
        revised = pressure.compressed_wave(original, .4)
        measured = pressure.wave_metrics(revised, {"virus": .6, "bacteria": .9})
        self.assertGreaterEqual(measured["minimum_same_port_gap_seconds"], 1.25 - 1e-8)
        for old, new in zip(original["spawns"], revised["spawns"]):
            for key in ("family", "count", "spawn_point_id", "squad_paths"):
                self.assertEqual(old[key], new[key])
            self.assertLess(new["duration"], old["duration"])
        self.assertEqual(original, self.wave())

    def test_retime_hits_pressure_without_touching_rewards_or_weakening_cohorts(self):
        original = self.wave()
        densities = {"virus": .6, "bacteria": .9}
        revised = pressure.retime_wave(original, 60, densities)
        measured = pressure.wave_metrics(revised, densities)
        self.assertGreaterEqual(measured["peak_density_per_second"], 60)
        self.assertLess(measured["peak_density_per_second"], 60.2)
        self.assertEqual(revised["atp_reward"], original["atp_reward"])
        self.assertEqual(revised["prep_time"], original["prep_time"])
        self.assertEqual(pressure.retime_wave(original, 10, densities), original)

    def test_curve_check_detects_pressure_troughs_and_cohort_overlap(self):
        curve = {"levels": [{"name": str(i), "peak_density_per_second": i + 1,
            "waves": [{"name": f"{i}-wave", "peak_density_per_second": i + 1,
                       "minimum_same_port_gap_seconds": 1.25}]} for i in range(10)]}
        self.assertFalse(pressure.curve_errors(curve))
        curve["levels"][5]["peak_density_per_second"] = 1
        curve["levels"][5]["waves"][0]["minimum_same_port_gap_seconds"] = 1
        self.assertEqual(len(pressure.curve_errors(curve)), 2)


class ShippedCampaignContracts(unittest.TestCase):
    def test_all_shipped_levels_and_waves_raise_release_density_without_family_overlap(self):
        curve = pressure.inspect_campaign(ROOT)
        self.assertEqual(pressure.curve_errors(curve), [])
        for first, second in zip(curve["levels"], curve["levels"][1:]):
            ratio = second["peak_density_per_second"] / first["peak_density_per_second"]
            self.assertGreaterEqual(ratio, 1.30)
            self.assertLessEqual(ratio, 1.35)

    def test_before_after_artifact_matches_shipped_schedule_and_preserves_payloads(self):
        report = pressure.read_json(ROOT / "assets/balance/campaign_pressure.json")
        densities = pressure.family_densities(ROOT)
        self.assertEqual(report["family_base_densities"], densities)
        self.assertEqual(len(report["levels"]), 10)
        wallets = [300, 335, 370, 410, 455, 505, 560, 620, 685, 750]
        for index, row in enumerate(report["levels"]):
            actual = pressure.level_metrics(pressure.read_json(ROOT / row["path"]), densities)
            self.assertEqual(row["after"], actual)
            self.assertEqual(row["before"]["population"], actual["population"])
            self.assertEqual(actual["starting_atp"], wallets[index])
            for before, after in zip(row["before"]["waves"], actual["waves"]):
                self.assertGreaterEqual(after["peak_density_per_second"] + 1e-8, before["peak_density_per_second"])
                for old, new in zip(before["cohorts"], after["cohorts"]):
                    self.assertEqual({k: v for k, v in old.items() if k not in {"start_time", "duration"}},
                                     {k: v for k, v in new.items() if k not in {"start_time", "duration"}})
                    self.assertLessEqual(new["duration"], old["duration"])
        self.assertEqual(report["levels"][0]["before"]["waves"][0], report["levels"][0]["after"]["waves"][0])
        economy = pressure.read_json(ROOT / "assets/config/economy.json")
        towers = pressure.read_json(ROOT / "assets/config/towers.json")
        self.assertEqual(economy["atp_per_density"], .85)
        self.assertEqual(towers["towers"]["macrophage"]["stats"]["build_cost"], 35)

    def test_retry_experiments_keep_frozen_strategies_and_disjoint_seed_samples(self):
        directory = ROOT / "assets/balance/experiments"
        train = pressure.read_json(directory / "retry_progression_training.json")
        heldout = pressure.read_json(directory / "retry_progression_heldout.json")
        self.assertFalse(set(train["seeds"]) & set(heldout["seeds"]))
        # The runner uses base_seed + attempt_index throughout a journey.
        # Distinct base seeds alone do not prevent overlapping random streams.
        allocated = set()
        for seed in heldout["seeds"]:
            stream = set(range(seed, seed + heldout["base"]["campaign"]["max_total_attempts"]))
            self.assertFalse(allocated & stream, "Held-out campaign seed streams overlap")
            allocated.update(stream)
        self.assertEqual(train["training_protocol"]["heldout_seeds"], heldout["seeds"])
        self.assertEqual(train["variants"], heldout["variants"])
        self.assertEqual(train["base"], heldout["base"])
        self.assertEqual([v["name"] for v in train["variants"]], ["control", "economy", "offense"])
        self.assertEqual(len(train["base"]["campaign"]["levels"]), 10)
        self.assertEqual(train["base"]["campaign"]["max_attempts_per_level"], 5)
        self.assertEqual(train["progression_targets"]["median_attempts_min"], 2)
        self.assertEqual(train["progression_targets"]["median_attempts_max"], 4)
        catalog = (ROOT / "src/game/meta/ImmunityTree.cpp").read_text(encoding="utf-8")
        keys = set(re.findall(r'node\("([^"]+)"', catalog))
        for variant in train["variants"]:
            priority = variant["patch"]["campaign"]["purchase_policy"]["priority"]
            self.assertEqual(len(priority), len(set(priority)))
            self.assertFalse(set(priority) - keys, "Unknown progression key in frozen purchase strategy")
            self.assertEqual(priority[0], "neutrophil.round_damage")
            self.assertIn("macrophage.arms", priority)
            self.assertIn("hub.elite_response", priority)
            self.assertIn("hub.systemic_potency", priority)
            self.assertTrue(set(variant["expected_discovery_order"]).issubset(priority))
        self.assertNotIn("overrides", train["base"])
        self.assertNotIn("progression_snapshot", train["base"])

    def test_late_income_decay_preserves_opening_levels_and_the_selected_curve(self):
        report = pressure.read_json(ROOT / "assets/balance/campaign_pressure.json")
        self.assertEqual(report["interventions"]["late_income_factor"], .75)
        levels = pressure.inspect_campaign(ROOT)["levels"]
        for index, level in enumerate(levels):
            expected = 1.0 if index < 4 else round(.75 ** (index - 3), 8)
            self.assertAlmostEqual(level["income_multiplier"], expected)
        experiment = pressure.read_json(ROOT / "assets/balance/experiments/retry_progression_training.json")
        economy = next(v for v in experiment["variants"] if v["name"] == "economy")
        priority = economy["patch"]["campaign"]["purchase_policy"]["priority"]
        for key in ("macrophage.grab_speed", "macrophage.captives", "macrophage.arms", "macrophage.search", "macrophage.health"):
            self.assertLess(priority.index(key), priority.index("hub.homeostasis"))

    def test_each_first_clear_funds_a_memory_free_discovery(self):
        meta = pressure.read_json(ROOT / "assets/config/meta.json")
        self.assertGreaterEqual(meta["base_run_reward"], meta["stat_base_cost"])
        self.assertEqual(meta["base_run_reward"], 30)
        self.assertEqual(meta["stat_cost_step"], 15)
        self.assertEqual(meta["first_clear_antibodies"], 1)
        self.assertEqual(meta["tower_unlock_antibodies"], 1)
        self.assertEqual(meta["ability_unlock_antibodies"], 1)
        self.assertEqual(meta["capstone_antibodies"], 1)
        self.assertEqual(meta["capstone_memory_cells"], 0)
        self.assertEqual(meta["capstone_threshold"], 0)


if __name__ == "__main__":
    unittest.main()
