#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Comparator policy tests. All fixtures are synthetic, never board evidence."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

FW = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(FW / "tools/bench"))
import compare
import revd_bench
import validate


class CompareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.reports = {mode: [] for mode in compare.MODES}
        self.monitors = {mode: [] for mode in compare.MODES}
        environment = json.loads((FW / "tools/bench/ab_environment.example.json").read_text())
        for mode in compare.MODES:
            values = environment["modes"][mode]
            values.update(board_identity="unit-fixture-board", power="fixture-supply")
            values["card"] = {"model": "unit-fixture-card", "identity_hash": "fixture-card", "capacity_bytes": 16777216, "filesystem": "FAT32"}
            values["busy_workload"]["conditioning"] = "unit-fixture"
            environment["builds"][mode] = {"firmware": "fixture-" + mode, "elf_sha256": ("a" if mode == "bare-metal" else "b") * 64,
                                            "toolchain": "fixture-compiler", "compiler_flags": "fixture-flags",
                                            "kernel_commit": "8a59a4b19bf72977819feddef87d41163ced7004"}
            for index in range(3):
                target = self.base / mode / str(index)
                revd_bench.fixture(target, run_id=100+index, duration=2, fps=[10, 10])
                monitor = target / "monitor.jsonl"
                items = list(validate.rows(monitor))
                for item in items:
                    if item.get("type") != "status":
                        continue
                    status = item["status"]
                    status["firmware"] = "fixture-" + mode
                    item["roundtrip_ms"] = 3.5
                    status["scheduler"] = mode
                    status["channels"] = [{"nbr": 500000, "dbr": 2000000}, {"nbr": 500000, "dbr": 2000000}]
                    status["bench"] = {"enabled": 1, "segment_kib": 64, "rotate_ms": 1000, "refresh_ms": 100}
                    for channel in status["performance"]["can"]:
                        channel.update(max_service_gap_us=100, max_service_us=50)
                    status["timing"] = {"max_ui_service_gap_us": 500, "max_control_latency_us": 200}
                monitor.write_text("".join(json.dumps(item) + "\n" for item in items))
                report = validate.create_report(sender=target / "sender.jsonl", logs=[target / "sd"], witness=[target / "witness.log"], witness_external=True, monitor=monitor)
                self.assertEqual(report["verdict"], "PASS_SIMULATION")
                report_path = target / "report.json"
                report_path.write_text(json.dumps(report))
                self.reports[mode].append(report_path)
                self.monitors[mode].append(monitor)
        for pair in environment["pairs"]:
            pair["window_s"] = {mode: 2 for mode in compare.MODES}
            pair["card_start_state_equal"] = True
        self.environment_path = self.base / "environment.json"
        self.environment_path.write_text(json.dumps(environment))

    def tearDown(self):
        self.temp.cleanup()

    def run_compare(self):
        return compare.compare_hardware(self.reports["bare-metal"], self.reports["freertos"], self.monitors["bare-metal"], self.monitors["freertos"], self.environment_path)

    def change_environment(self, update):
        value = json.loads(self.environment_path.read_text())
        update(value)
        self.environment_path.write_text(json.dumps(value))

    def declare_hypothetical_physical_inputs(self):
        # Exercises policy on hypothetical operator flags, not a hardware claim.
        for paths in self.reports.values():
            for path in paths:
                value = json.loads(path.read_text())
                value["synthetic"] = False
                value["integrity_verdict"] = "PASS"
                value["verdict"] = "PASS"
                path.write_text(json.dumps(value))
        self.change_environment(lambda value: value.update(physical_hardware_tested=True))

    def test_synthetic_three_pairs_never_claim_hardware_speedup(self):
        result = self.run_compare()
        self.assertEqual(result["verdict"], "MODEL")
        self.assertFalse(result["physical_hardware_tested"])
        self.assertIsNone(result["hardware_speedup_claim"])
        self.assertEqual(result["aggregates"]["can_service_gap_us"]["bare-metal"]["n"], 3)

    def test_operator_physical_flag_does_not_upgrade_synthetic_evidence(self):
        self.change_environment(lambda value: value.update(physical_hardware_tested=True))
        self.assertEqual(self.run_compare()["verdict"], "MODEL")

    def test_hypothetical_complete_physical_policy_pass(self):
        self.declare_hypothetical_physical_inputs()
        result = self.run_compare()
        self.assertEqual(result["issues"], [])
        self.assertEqual(result["verdict"], "PASS")

    def test_under_three_repeats_cannot_pass(self):
        self.declare_hypothetical_physical_inputs()
        for values in (self.reports, self.monitors):
            for mode in compare.MODES:
                values[mode].pop()
        result = self.run_compare()
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("at least three complete paired repeats are required", result["issues"])

    def test_different_clock_or_card_environment_cannot_pass(self):
        self.declare_hypothetical_physical_inputs()
        self.change_environment(lambda value: value["modes"]["freertos"]["clock_hz"].update(cpu=480000000))
        result = self.run_compare()
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("environment differs between modes: clock_hz", result["issues"])

    def test_pair_must_match_exact_load_payload_configuration(self):
        self.declare_hypothetical_physical_inputs()
        path = self.reports["freertos"][0]
        report = json.loads(path.read_text())
        report["sender"]["config"]["fps_per_channel"] = ["20", "10"]
        path.write_text(json.dumps(report))
        result = self.run_compare()
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertTrue(any("fps_per_channel" in issue for issue in result["issues"]))

    def test_scheduler_identity_is_checked_against_raw_monitor(self):
        self.declare_hypothetical_physical_inputs()
        path = self.monitors["freertos"][0]
        items = list(validate.rows(path))
        for item in items:
            if item.get("type") == "status":
                item["status"]["scheduler"] = "bare-metal"
        path.write_text("".join(json.dumps(item) + "\n" for item in items))
        result = self.run_compare()
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertTrue(any("scheduler identity" in issue for issue in result["issues"]))

    def test_order_must_be_counterbalanced(self):
        self.declare_hypothetical_physical_inputs()
        def update(value):
            for pair in value["pairs"]:
                pair["order"] = ["bare-metal", "freertos"]
        self.change_environment(update)
        self.assertIn("A/B order was not alternated/counterbalanced across repeats", self.run_compare()["issues"])

    def test_headroom_failure_does_not_become_frame_loss(self):
        self.declare_hypothetical_physical_inputs()
        path = self.reports["freertos"][0]
        value = json.loads(path.read_text())
        value["headroom"]["verdict"] = "FAIL"
        path.write_text(json.dumps(value))
        result = self.run_compare()
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertEqual(result["integrity_failures"], [])

    def test_frame_integrity_failure_remains_a_failure(self):
        self.declare_hypothetical_physical_inputs()
        path = self.reports["freertos"][0]
        value = json.loads(path.read_text())
        value["integrity_verdict"] = "FAIL"
        value["counts"]["witness_vs_sd.missing"] = 1
        path.write_text(json.dumps(value))
        result = self.run_compare()
        self.assertEqual(result["verdict"], "FAIL")
        self.assertEqual(result["pairs"][0]["modes"]["freertos"]["metrics"]["missing_frames"], 1)

    def test_missing_latency_stays_unknown_instead_of_zero(self):
        value = compare.read_metrics({}, {"gauges": {}})
        self.assertIsNone(value["ui_service_gap_us"])
        self.assertIsNone(value["control_latency_us"])
        self.assertIsNone(value["exact_fence_errors"])

    def test_compare_model_reports_are_always_model_even_when_faults_expected(self):
        row = {"scenario": "saturated", "fps_per_channel": 2500, "duration_ms": 5000, "injected_busy_ms": 250,
               "busy_target": "write", "expectation": "fault-must-be-detected", "pass": True, "zero_loss": False, "accepted_missing": 1}
        paths = [self.base / "model-a.json", self.base / "model-b.json"]
        for path in paths:
            path.write_text(json.dumps({"physical_hardware_tested": False, "all_expectations_met": True, "scenarios": [row]}))
        result = compare.compare_models(*paths)
        self.assertEqual(result["verdict"], "MODEL")
        self.assertFalse(result["scenarios"][0]["freertos"]["zero_loss"])
        self.assertTrue(result["integrity_expectations_met"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
