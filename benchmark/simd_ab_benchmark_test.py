"""Behavior checks for the SIMD A/B driver, using synthetic Google Benchmark reports."""

import argparse
import unittest

import simd_ab_benchmark as ab


def report(level, times, unit="ms", digest="d0", error=None):
    """A minimal simd_compare_benchmark report: one repetition per benchmark name in times."""
    benchmarks = []
    for name, value in times.items():
        entry = {"name": name + "/manual_time", "run_name": name + "/manual_time", "run_type": "iteration",
                 "real_time": value, "time_unit": unit}
        if error == name:
            entry.update({"error_occurred": True, "error_message": "token count differs"})
        benchmarks.append(entry)
    benchmarks.append({"name": "MIX/strings/manual_time_mean", "run_name": "MIX/strings/manual_time",
                       "run_type": "aggregate", "real_time": 1e9, "time_unit": "ms"})
    context = {"simd_level": level, "MIX/tokens": "10", "MIX/ranges_fnv1a64": digest, "host_name": level}
    return {"context": context, "benchmarks": benchmarks}


class ArgumentTest(unittest.TestCase):
    """Command-line values the workflow passes."""

    def test_variant_splits_level_and_path(self):
        level, path = ab.parse_variant("AVX2=build/avx2/simd_compare_benchmark")
        self.assertEqual(level, "AVX2")
        self.assertEqual(str(path), "build/avx2/simd_compare_benchmark")

    def test_variant_rejects_unknown_level_or_missing_path(self):
        for value in ("AVX512=bench", "AVX2=", "AVX2"):
            with self.assertRaises(argparse.ArgumentTypeError):
                ab.parse_variant(value)

    def test_counts_must_be_positive_integers(self):
        self.assertEqual(ab.positive_count("3"), 3)
        for value in ("0", "-1", "x"):
            with self.assertRaises(argparse.ArgumentTypeError):
                ab.positive_count(value)

    def test_levels_put_off_first(self):
        self.assertEqual(ab.variant_levels([("AVX2", "a"), ("OFF", "o"), ("SSE2", "s")]), ["OFF", "AVX2", "SSE2"])

    def test_levels_require_off_and_another_build_without_duplicates(self):
        for variants in ([("AVX2", "a")], [("OFF", "o")], [("OFF", "o"), ("AVX2", "a"), ("AVX2", "b")]):
            with self.assertRaises(ValueError):
                ab.variant_levels(variants)


class ReportTest(unittest.TestCase):
    """Reading timings and checking that every build cut the corpus identically."""

    def test_medians_convert_units_and_drop_manual_time_suffix_and_aggregates(self):
        medians = ab.iteration_medians(report("OFF", {"MIX/strings": 2000.0, "HMM/reused": 3.0}, unit="us"))
        self.assertEqual(medians, {"MIX/strings": 2.0, "HMM/reused": 0.003})

    def test_benchmark_error_aborts_the_comparison(self):
        with self.assertRaisesRegex(RuntimeError, "token count differs"):
            ab.iteration_medians(report("OFF", {"MIX/strings": 1.0}, error="MIX/strings"))

    def test_report_from_another_build_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "reports simd_level SSE2"):
            ab.check_report("AVX2", report("SSE2", {"MIX/strings": 1.0}), None)

    def test_output_difference_names_the_differing_mode(self):
        reference = ab.check_report("OFF", report("OFF", {"MIX/strings": 1.0}), None)
        with self.assertRaisesRegex(RuntimeError, "AVX2 output differs from OFF: MIX/ranges_fnv1a64"):
            ab.check_report("AVX2", report("AVX2", {"MIX/strings": 1.0}, digest="d1"), reference)

    def test_fingerprint_ignores_host_context(self):
        fingerprint = ab.check_report("OFF", report("OFF", {"MIX/strings": 1.0}), None)
        self.assertEqual(fingerprint, {"MIX/tokens": "10", "MIX/ranges_fnv1a64": "d0"})


class CollectTest(unittest.TestCase):
    """Run order and pairing across rounds."""

    def test_round_order_rotates_every_label_through_the_first_position(self):
        labels = ["OFF", "AVX2", "A/A"]
        self.assertEqual([ab.round_order(labels, index)[0] for index in range(3)], ["OFF", "AVX2", "A/A"])

    def test_each_round_runs_every_level_and_off_twice(self):
        calls = []

        def run_build(level, run_index):
            calls.append(level)
            return report(level, {"MIX/strings": 1.0})

        measured, _ = ab.collect(["OFF", "SSE2", "AVX2"], run_build, 2)
        self.assertEqual(sorted(calls), ["AVX2", "AVX2", "OFF", "OFF", "OFF", "OFF", "SSE2", "SSE2"])
        self.assertEqual([sorted(medians) for medians in measured], [["A/A", "AVX2", "OFF", "SSE2"]] * 2)

    def test_ratios_pair_each_round_with_its_own_off_run(self):
        # The machine slows down by half in the second round; AVX2 stays 20% faster within each round.
        rounds = [{"OFF": {"MIX/strings": 10.0}, "AVX2": {"MIX/strings": 8.0}, "A/A": {"MIX/strings": 10.0}},
                  {"OFF": {"MIX/strings": 15.0}, "AVX2": {"MIX/strings": 12.0}, "A/A": {"MIX/strings": 15.3}}]
        row = ab.summarize(rounds, ["OFF", "AVX2"])["MIX/strings"]
        self.assertEqual(row["off_ms"], 12.5)
        self.assertAlmostEqual(row["ratios"]["AVX2"]["min"], 0.8)
        self.assertAlmostEqual(row["ratios"]["AVX2"]["max"], 0.8)
        self.assertAlmostEqual(row["ratios"]["A/A"]["max"], 1.02)

    def test_rows_follow_mode_then_output_order(self):
        names = {"HMM/strings": 1.0, "MIX/reused": 1.0, "MIX/strings": 1.0, "FULL/borrowed": 1.0}
        rounds = [{"OFF": names, "AVX2": names, "A/A": names}]
        self.assertEqual(list(ab.summarize(rounds, ["OFF", "AVX2"])),
                         ["MIX/strings", "MIX/reused", "FULL/borrowed", "HMM/strings"])


class FormatTest(unittest.TestCase):
    """Markdown the PR comment shows."""

    noise = {"median": 1.0, "min": 0.99, "max": 1.01}

    def test_change_beyond_noise_requires_every_round_outside_the_range(self):
        self.assertEqual(ab.verdict({"median": 0.9, "min": 0.88, "max": 0.95}, self.noise), "faster")
        self.assertEqual(ab.verdict({"median": 1.1, "min": 1.05, "max": 1.2}, self.noise), "slower")
        self.assertEqual(ab.verdict({"median": 0.95, "min": 0.9, "max": 0.995}, self.noise), "")

    def test_markdown_bolds_only_changes_beyond_noise(self):
        rows = {"MIX/strings": {"off_ms": 7.5, "ratios": {"AVX2": {"median": 0.92, "min": 0.9, "max": 0.94},
                                                          "SSE2": {"median": 0.995, "min": 0.98, "max": 1.0},
                                                          "A/A": self.noise}}}
        settings = {"text": "weicheng.utf8", "rounds": 5, "repetitions": 3}
        markdown = ab.format_markdown("Linux x64", settings, ["OFF", "AVX2", "SSE2"], rows)
        self.assertIn("| Benchmark | OFF ms | AVX2 | SSE2 | A/A |", markdown)
        self.assertIn("| MIX/strings | 7.500 | **-8.0% [-10.0, -6.0]** | -0.5% [-2.0, +0.0] | +0.0% [-1.0, +1.0] |",
                      markdown)
        self.assertIn("Output identical across OFF, AVX2, SSE2", markdown)

    def test_failure_section_states_the_reason(self):
        markdown = ab.format_failure("Linux x64", RuntimeError("AVX2 output differs from OFF: MIX/tokens"))
        self.assertIn("**Comparison failed:** AVX2 output differs from OFF: MIX/tokens", markdown)


if __name__ == "__main__":
    unittest.main()
