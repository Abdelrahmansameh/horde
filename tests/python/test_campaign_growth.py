"""Campaign authoring contracts: family introductions and rising horde pressure.

These check authored demand, not automated-player skill or measured difficulty.
Capacity/earned power and controller adequacy are checked in real balance runs.
"""
import json
import math
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def document(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def peak_density(level, densities):
    peak = 0.0
    for wave in level["waves"]:
        events = {}
        for spawn in wave["spawns"]:
            count = spawn["count"]
            duration = spawn.get("duration", 0)
            if not count:
                continue
            if duration <= 0:
                raise ValueError("Campaign hordes require finite release durations")
            start = spawn.get("start_time", 0)
            rate = count * densities[spawn["family"]] / duration
            events[start] = events.get(start, 0) + rate
            events[start + duration] = events.get(start + duration, 0) - rate
        running = 0.0
        for time in sorted(events):
            running += events[time]
            peak = max(peak, running)
    return peak


class CampaignGrowthTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.paths = sorted((ROOT / "assets/levels").glob("campaign_[0-9][0-9]_*.json"))
        cls.levels = [document(path) for path in cls.paths]
        cls.densities = {
            family: cfg["behavior"]["base_density"]
            for family, cfg in document(ROOT / "assets/config/enemies.json")["families"].items()
        }

    def test_family_introductions_are_playable_campaign_content(self):
        self.assertEqual(len(self.levels), 10)
        families = [{s["family"] for w in level["waves"] for s in w["spawns"] if s["count"]}
                    for level in self.levels]
        self.assertEqual(families[0], {"virus"})
        self.assertIn("bacteria", families[1])
        for index, group in enumerate(families):
            with self.subTest(level=index + 1):
                self.assertEqual("spore" in group, index >= 3)

    def test_every_level_increases_population_and_peak_release_pressure(self):
        counts = [sum(s["count"] for w in level["waves"] for s in w["spawns"])
                  for level in self.levels]
        peaks = [peak_density(level, self.densities) for level in self.levels]
        for index in range(1, len(self.levels)):
            with self.subTest(level=index + 1):
                # A substantial multiplicative step, not a tiny count increase
                # hidden behind longer release durations or a lower peak.
                self.assertGreaterEqual(counts[index] / counts[index - 1], 1.35)
                self.assertLessEqual(counts[index] / counts[index - 1], 1.45)
                self.assertGreaterEqual(peaks[index] / peaks[index - 1], 1.30)
                self.assertLessEqual(peaks[index] / peaks[index - 1], 1.50)
        self.assertGreaterEqual(counts[8], 43774)
        self.assertGreaterEqual(peaks[8], 2290.0)
        self.assertGreaterEqual(counts[9], 1.35 * 43774)
        self.assertGreaterEqual(peaks[9], 1.35 * 2290.0)

    def test_peak_pressure_counts_overlapping_families_and_separate_waves(self):
        level = {"waves": [
            {"spawns": [{"family": "virus", "count": 100, "duration": 10},
                        {"family": "spore", "count": 10, "duration": 5, "start_time": 5}]},
            {"spawns": [{"family": "virus", "count": 20, "duration": 10}]},
        ]}
        self.assertEqual(peak_density(level, {"virus": 1, "spore": 3}), 16)

    def test_late_challenge_increases_per_agent_without_inflating_map_income(self):
        counts = [sum(s["count"] for w in level["waves"] for s in w["spawns"])
                  for level in self.levels]
        self.assertEqual(counts, [3000, 4200, 5880, 8232, 11525, 16135,
                                  22589, 31624, 44274, 61983])
        for index, level in enumerate(self.levels):
            with self.subTest(level=index + 1):
                scaling = level.get("enemy_scaling", {})
                durability = scaling.get("durability_multiplier", 1)
                damage = scaling.get("damage_multiplier", 1)
                steps = max(index - 3, 0)
                self.assertTrue(math.isfinite(durability) and math.isfinite(damage))
                self.assertAlmostEqual(durability, 1.35 ** steps, places=10)
                self.assertAlmostEqual(damage, 1.12 ** steps, places=10)
                # Runtime compensates damage-derived income for durability.
                # Do not also change authored income, wallet or wave bounties.
                self.assertEqual(level["economy"].get("income_multiplier", 1), 1)

    def test_late_durability_weighted_release_load_keeps_rising(self):
        loaded_peaks = [peak_density(level, self.densities) *
                        level.get("enemy_scaling", {}).get("durability_multiplier", 1)
                        for level in self.levels]
        for index in range(4, len(self.levels)):
            with self.subTest(level=index + 1):
                # 1.4x scheduled base mass and 1.35x per-agent durability:
                # this is authored damageable load, never hostile contact DPS.
                self.assertGreaterEqual(loaded_peaks[index] / loaded_peaks[index - 1], 1.85)
                self.assertLessEqual(loaded_peaks[index] / loaded_peaks[index - 1], 1.93)


if __name__ == "__main__":
    unittest.main()
