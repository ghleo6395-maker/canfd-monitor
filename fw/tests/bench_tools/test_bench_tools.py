#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import copy
import io
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

FW = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(FW / "tools" / "bench"))
import bench_io
import protocol
import revd_bench
import validate


class Clock:
    def __init__(self):
        self.ns = 0

    def monotonic_ns(self):
        return self.ns

    def monotonic(self):
        return self.ns / 1e9

    def sleep(self, value):
        self.ns += int(value * 1e9)


class FakeSocket:
    def __init__(self, fail=False, confirm=True):
        self.sent = []
        self.pending = []
        self.fail = fail
        self.confirm = confirm
        self.closed = False

    def send(self, raw):
        if self.fail:
            raise BlockingIOError(105, "No buffer space")
        self.sent.append(raw)
        self.pending.append(raw)
        return len(raw)

    def recvmsg(self, *_):
        if not self.pending:
            raise BlockingIOError()
        flags = getattr(socket, "MSG_CONFIRM", 0x800) if self.confirm else 0
        return self.pending.pop(0), [], flags, ("can0",)

    def close(self):
        self.closed = True


class FakeSerial:
    def __init__(self, clock, status=None, malformed=False):
        self.clock = clock
        self.pending = b""
        self.answer = b""
        self.status = status or {"firmware": "test", "uptime_ms": 100, "boot_id": "test-boot"}
        self.malformed = malformed
        self.commands = []

    def write(self, data):
        # Exercise partial writes instead of assuming a tty write is atomic.
        self.pending += data[:2]
        if b"\n" in self.pending:
            command, self.pending = self.pending.split(b"\n", 1)
            self.commands.append(command)
            if self.malformed:
                self.answer += b"STATUS {bad json}\r\n"
            else:
                self.answer += ("OK OTHER\r\nSTATUS " + json.dumps(self.status) + "\r\n").encode()
        return min(2, len(data))

    def read(self, timeout):
        self.clock.sleep(min(0.01, timeout))
        result, self.answer = self.answer[:13], self.answer[13:]
        return result


class ProtocolTests(unittest.TestCase):
    def test_payload_roundtrip_both_channels_large_sequence(self):
        for ch in (0, 1):
            seq = (1 << 40) + 17
            value = protocol.payload(0xFEDCBA9876543210, ch, seq, 68_000_000)
            self.assertEqual(len(value), 64)
            self.assertEqual(protocol.decode_payload(value), {"run_id": 0xFEDCBA9876543210, "ch": ch, "seq": seq, "planned_ns": 68_000_000})

    def test_crc_rejects_single_bit_damage(self):
        data = bytearray(protocol.payload(1, 0, 0, 0))
        data[34] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC32"):
            protocol.decode_payload(data)

    def test_pattern_rejects_recomputed_crc(self):
        import zlib
        data = bytearray(protocol.payload(1, 0, 0, 0))
        data[34] ^= 1
        data[60:] = struct.pack("<I", zlib.crc32(data[:60]))
        with self.assertRaisesRegex(ValueError, "pattern"):
            protocol.decode_payload(data)

    def test_candump_extended_and_fdf_marker(self):
        text = protocol.format_candump(1, 17, 88, 68_000_000, extended=True, flags=5)
        frame = protocol.parse_candump(text)
        self.assertTrue(frame.fd and frame.extended)
        self.assertEqual(frame.can_id, protocol.EXT_IDS[1])
        self.assertEqual(protocol.decode_payload(frame.data)["seq"], 17)

    def test_classic_rtr_error_and_dotted_data(self):
        self.assertTrue(protocol.parse_candump("(1.2) can0 123#R0").rtr)
        self.assertTrue(protocol.parse_candump("(1.2) can0 20000001#0000000000000000").error)
        self.assertEqual(protocol.parse_candump("(1.2) can0 123#AB.CD").data, b"\xab\xcd")
        self.assertIsNone(protocol.parse_candump("# comment"))

    def test_malformed_candump_rejected(self):
        for text in ("bad", "(1.0000000) can0 123#AA", "(1.0) can0 123##8", "(1.0) can0 123##1AAA", "(1.0) can0 800#00", "(1.0) can0 123##1" + "00" * 9, "(1.0) can0 123#A."):
            with self.subTest(text=text), self.assertRaises(ValueError):
                protocol.parse_candump(text)

    def test_socketcan_abi_and_flags(self):
        raw = protocol.pack_socketcan(0, 1, 99, 500, True)
        self.assertEqual(len(raw), 72)
        frame = protocol.unpack_socketcan(raw, "canA")
        self.assertTrue(frame.extended and frame.fd)
        self.assertEqual(frame.flags, 1)
        self.assertEqual(protocol.decode_payload(frame.data)["run_id"], 99)
        with self.assertRaises(ValueError):
            protocol.unpack_socketcan(raw[:-1], "canA")

    def test_c_pipeline_fixture_matches_python_protocol(self):
        source = FW / "tests" / "stress" / "payload_fixture.json"
        if not source.exists():
            self.skipTest("pipeline C model fixture not supplied")
        fixture = json.loads(source.read_text())
        run_id = int(str(fixture["run_id"]), 0)
        ch = fixture.get("ch", fixture.get("channel"))
        seq = fixture.get("seq", fixture.get("sequence"))
        offset = fixture.get("planned_offset_ns", fixture.get("offset_ns"))
        self.assertEqual(protocol.payload(run_id, ch, seq, offset).hex().lower(), fixture["payload_hex"].lower())


class TransportTests(unittest.TestCase):
    def test_fractional_rate_scheduler_has_no_cumulative_float_drift(self):
        planned = list(bench_io.schedule("0.5", ["7.5", "2"]))
        self.assertEqual([x[0] for x in planned if x[1] == 0], [0, 133333333, 266666666, 400000000])
        self.assertEqual([x[0] for x in planned if x[1] == 1], [0])

    def test_sender_actual_packet_and_per_channel_pacing(self):
        clock = Clock()
        sockets = [FakeSocket(), FakeSocket()]
        opened = []
        def factory(interface, can_id, extended):
            opened.append((interface, can_id, extended))
            return sockets[len(opened) - 1]
        output = io.StringIO()
        code = bench_io.send_run(output, run_id=3, duration="0.03", fps=[100, 100], extended=False,
                                 interfaces=["benchA", "benchB"], socket_factory=factory,
                                 monotonic_ns=clock.monotonic_ns, sleep=clock.sleep, echo_wait=0)
        data = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(code, 0)
        self.assertEqual(opened, [("benchA", 0x5A0, False), ("benchB", 0x5A1, False)])
        self.assertEqual(data[-1]["send_accepted"], [3, 3])
        self.assertEqual(data[-1]["local_confirm_echoes"], [3, 3])
        self.assertEqual([x["requested_ns"] for x in data if x["type"] == "tx" and x["ch"] == 0], [0, 10000000, 20000000])
        self.assertEqual(protocol.decode_payload(protocol.unpack_socketcan(sockets[1].sent[2], "benchB").data)["seq"], 2)
        self.assertTrue(all(sock.closed for sock in sockets))

    def test_sender_errors_are_not_accepted_or_retried(self):
        clock = Clock()
        sockets = [FakeSocket(fail=True), FakeSocket()]
        output = io.StringIO()
        code = bench_io.send_run(output, run_id=1, duration="0.02", fps=[100, 100], extended=False,
                                 interfaces=["a", "b"], socket_factory=lambda name, *_: sockets[int(name == "b")],
                                 monotonic_ns=clock.monotonic_ns, sleep=clock.sleep, echo_wait=0)
        end = json.loads(output.getvalue().splitlines()[-1])
        self.assertEqual(code, 1)
        self.assertEqual(end["send_accepted"], [0, 2])
        self.assertEqual(end["send_errors"], [2, 0])

    def test_sender_wall_deadline_does_not_replay_unbounded_backlog(self):
        clock = Clock()
        def slow_sleep(value):
            clock.sleep(value + 0.1)
        output = io.StringIO()
        code = bench_io.send_run(output, run_id=1, duration="0.03", fps=[100, 100], extended=False,
                                 interfaces=["a", "b"], socket_factory=lambda *_: FakeSocket(),
                                 monotonic_ns=clock.monotonic_ns, sleep=slow_sleep, echo_wait=0)
        end = json.loads(output.getvalue().splitlines()[-1])
        self.assertEqual(code, 1)
        self.assertFalse(end["complete"])
        self.assertEqual(end["requested"], [1, 1])

    def test_sender_closes_first_socket_if_second_open_fails(self):
        clock = Clock()
        sock = FakeSocket()
        def factory(name, *_):
            if name == "b":
                raise OSError("no device")
            return sock
        output = io.StringIO()
        with self.assertRaises(OSError):
            bench_io.send_run(output, run_id=1, duration=1, fps=[1, 1], extended=False,
                              interfaces=["a", "b"], socket_factory=factory, monotonic_ns=clock.monotonic_ns, sleep=clock.sleep)
        self.assertTrue(sock.closed)
        self.assertFalse(json.loads(output.getvalue().splitlines()[-1])["complete"])

    def test_monitor_partial_serial_io_and_unrelated_replies(self):
        clock = Clock()
        port = FakeSerial(clock)
        output = io.StringIO()
        result = bench_io.monitor_run(output, port, 0.88, interval=0.3, monotonic=clock.monotonic, wall=clock.monotonic)
        evidence = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(result, 0)
        self.assertGreaterEqual(len([x for x in evidence if x["type"] == "status"]), 2)
        self.assertTrue(all(command == b"STATUS" for command in port.commands))
        samples = [x for x in evidence if x["type"] == "status"]
        self.assertTrue(all(x["roundtrip_ms"] > 0 for x in samples))
        self.assertEqual([x["request"]["sequence"] for x in samples], list(range(len(samples))))

    def test_monitor_never_overlaps_unanswered_status_requests(self):
        clock = Clock()
        class NoReply(FakeSerial):
            def read(self, timeout):
                self.clock.sleep(min(0.01, timeout))
                return b""
        port = NoReply(clock)
        output = io.StringIO()
        result = bench_io.monitor_run(output, port, 2.5, interval=0.1, monotonic=clock.monotonic, wall=clock.monotonic)
        evidence = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(result, 1)
        self.assertEqual(len(port.commands), 2)
        self.assertTrue(any(x.get("error") == "STATUS response timeout" for x in evidence))

    def test_monitor_malformed_status_is_preserved_and_not_success(self):
        clock = Clock()
        port = FakeSerial(clock, malformed=True)
        output = io.StringIO()
        self.assertEqual(bench_io.monitor_run(output, port, 0.9, interval=0.3, monotonic=clock.monotonic, wall=clock.monotonic), 1)
        self.assertIn('"type":"monitor_error"', output.getvalue())

    def test_control_whitelist_blocks_unrelated_and_confirmation_commands(self):
        self.assertEqual(revd_bench.control_command("bench config 64 250 50"), "BENCH CONFIG 64 250 50")
        for command in ("FORMAT", "YES", "CONFIRM", "TX 1 SEND 0", "BENCH CONFIG 1 250 50", "BENCH CONFIG 64 249 50", "BENCH CONFIG 64 250 49"):
            with self.subTest(command=command), self.assertRaises(ValueError):
                revd_bench.control_command(command)

    def test_cli_requires_explicit_hardware_before_attempting_devices(self):
        process = subprocess.run([sys.executable, str(FW / "tools/bench/revd_bench.py"), "send", "--interfaces", "does-not-exist-a", "does-not-exist-b", "--out", "/no/such/output"], capture_output=True, text=True)
        self.assertEqual(process.returncode, 3)
        self.assertIn("requires explicit --hardware", process.stderr)


class ValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "case"
        revd_bench.fixture(self.path, run_id=77, duration=2, fps=[10, 10], segment_frames=8)

    def tearDown(self):
        self.temp.cleanup()

    def report(self, **kwargs):
        arguments = {"sender": self.path / "sender.jsonl", "logs": [self.path / "sd"], "witness": [self.path / "witness.log"],
                     "witness_external": True, "monitor": self.path / "monitor.jsonl", "csv_path": self.path / "anomalies.csv"}
        arguments.update(kwargs)
        return validate.create_report(**arguments)

    def modify_jsonl(self, filename, change):
        path = self.path / filename
        evidence = list(validate.rows(path))
        change(evidence)
        path.write_text("".join(json.dumps(row) + "\n" for row in evidence))

    def test_clean_fixture_only_claims_simulation(self):
        result = self.report()
        self.assertEqual(result["verdict"], "PASS_SIMULATION")
        self.assertEqual(result["channels"][0]["expected"], 20)
        self.assertEqual(len(result["sd_files"]), 5)

    def test_no_independent_witness_is_inconclusive(self):
        self.assertEqual(self.report(witness=None, witness_external=False)["verdict"], "INCONCLUSIVE")

    def test_local_echo_not_independent_witness(self):
        self.assertEqual(self.report(witness_external=False)["verdict"], "INCONCLUSIVE")

    def test_sd_files_cannot_be_reused_as_independent_witness(self):
        with self.assertRaisesRegex(ValueError, "same file"):
            self.report(witness=[self.path / "sd"])

    def test_one_missing_frame_and_csv_diagnostics(self):
        path = sorted((self.path / "sd").glob("*.LOG"))[0]
        lines = path.read_text().splitlines(keepends=True)
        path.write_text("".join(lines[:2] + lines[3:]))
        result = self.report()
        self.assertEqual(result["verdict"], "FAIL")
        self.assertEqual(result["counts"]["accepted_vs_sd.missing"], 1)
        self.assertIn("missing_accepted,0,1", (self.path / "anomalies.csv").read_text())

    def test_tail_loss_not_hidden_by_observed_max(self):
        path = sorted((self.path / "sd").glob("*.LOG"))[-1]
        path.write_text("".join(path.read_text().splitlines(keepends=True)[:-1]))
        result = self.report()
        self.assertEqual(result["channels"][1]["missing_tail_frames"], 1)
        self.assertEqual(result["verdict"], "FAIL")

    def test_duplicate_across_adjacent_segment_boundary(self):
        first, second = sorted((self.path / "sd").glob("*.LOG"))[:2]
        duplicate = first.read_text().splitlines(keepends=True)[-1]
        second.write_text(duplicate + second.read_text())
        result = self.report()
        self.assertEqual(result["counts"]["observed.adjacent_segment_duplicate"], 1)
        self.assertEqual(result["verdict"], "FAIL")

    def test_bad_crc_and_unterminated_tail_are_failures(self):
        path = sorted((self.path / "sd").glob("*.LOG"))[0]
        text = path.read_text()
        lines = text.splitlines(keepends=True)
        lines[0] = lines[0][:-3] + "GG\n"
        path.write_text("".join(lines).rstrip("\n"))
        result = self.report()
        self.assertGreater(result["counts"]["observed.parse_error"], 0)
        self.assertGreater(result["counts"]["observed.unterminated_line"], 0)
        self.assertEqual(result["verdict"], "FAIL")

    def test_wrong_channel_map_is_not_silently_remapped(self):
        result = self.report(channel_map={"can0": 1, "can1": 0})
        self.assertEqual(result["verdict"], "FAIL")
        self.assertEqual(result["counts"]["observed.channel_mismatch"], 40)

    def test_explicit_segment_order_is_checked(self):
        files = sorted((self.path / "sd").glob("*.LOG"))
        files[0], files[1] = files[1], files[0]
        result = self.report(logs=files)
        self.assertEqual(result["verdict"], "FAIL")
        self.assertGreater(result["counts"]["observed.sequence_order"], 0)

    def test_natural_file_sort_is_numeric(self):
        self.assertEqual(sorted([Path("10.LOG"), Path("2.LOG"), Path("1.LOG")], key=validate.natural), [Path("1.LOG"), Path("2.LOG"), Path("10.LOG")])

    def test_sender_truncated_end_is_inconclusive(self):
        self.modify_jsonl("sender.jsonl", lambda values: values.pop())
        self.assertEqual(self.report()["verdict"], "INCONCLUSIVE")

    def test_sender_cannot_claim_complete_after_omitting_both_tail_requests(self):
        def corrupt(evidence):
            evidence[:] = [row for row in evidence if not (row.get("type") == "tx" and row["seq"] == 19)]
            evidence[-1]["requested"] = [19, 19]
            evidence[-1]["send_accepted"] = [19, 19]
        self.modify_jsonl("sender.jsonl", corrupt)
        with self.assertRaisesRegex(ValueError, "configured duration/rates"):
            self.report()

    def test_sender_planned_timestamp_is_verified_against_rate(self):
        def corrupt(evidence):
            next(row for row in evidence if row.get("type") == "tx")["scheduled_offset_ns"] += 1
        self.modify_jsonl("sender.jsonl", corrupt)
        with self.assertRaisesRegex(ValueError, "schedule"):
            self.report()

    def test_counter_wrap_uses_uint32_delta(self):
        def change(evidence):
            samples = [row["status"] for row in evidence if row.get("type") == "status"]
            samples[0]["ring_dropped"] = 0xFFFFFFFE
            samples[1]["ring_dropped"] = 1
        self.modify_jsonl("monitor.jsonl", change)
        result = self.report()
        self.assertEqual(result["monitor"]["deltas"]["ring_dropped"], 3)
        self.assertEqual(result["monitor"]["uint32_wraps"]["ring_dropped"], 1)
        self.assertEqual(result["verdict"], "FAIL")

    def test_counter_reset_is_inconclusive_not_a_huge_unsigned_delta(self):
        def change(evidence):
            samples = [row["status"] for row in evidence if row.get("type") == "status"]
            samples[0]["frames"], samples[1]["frames"] = 50, 10
        self.modify_jsonl("monitor.jsonl", change)
        result = self.report()
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertTrue(any("counter reset" in issue for issue in result["monitor"]["issues"]))

    def test_boot_id_or_firmware_change_invalidates_window(self):
        def change(evidence):
            samples = [row["status"] for row in evidence if row.get("type") == "status"]
            samples[-1]["boot_id"] = "different"
            samples[-1]["firmware"] = "different"
        self.modify_jsonl("monitor.jsonl", change)
        result = self.report()
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertIn("boot_id changed during run", result["monitor"]["issues"])

    def test_monitor_must_bracket_the_sender(self):
        def change(evidence):
            samples = [row for row in evidence if row.get("type") == "status"]
            samples[0]["wall_time"] = 0.5
        self.modify_jsonl("monitor.jsonl", change)
        self.assertEqual(self.report()["verdict"], "INCONCLUSIVE")

    def test_highwater_is_gauge_not_counter_and_keeps_peak(self):
        def change(evidence):
            samples = [row["status"] for row in evidence if row.get("type") == "status"]
            samples[0]["performance"]["sd"]["peak"] = 100
            samples[1]["performance"]["sd"]["peak"] = 20
        self.modify_jsonl("monitor.jsonl", change)
        summary = self.report()["monitor"]
        self.assertEqual(summary["gauges"]["performance.sd.peak"]["max"], 100)
        self.assertNotIn("performance.sd.peak", summary["deltas"])

    def test_queue_margin_failure_is_not_reported_as_lost_frames(self):
        def change(evidence):
            samples = [row["status"] for row in evidence if row.get("type") == "status"]
            samples[1]["performance"]["sd"]["peak"] = 769
        self.modify_jsonl("monitor.jsonl", change)
        result = self.report()
        self.assertEqual(result["integrity_verdict"], "PASS_SIMULATION")
        self.assertEqual(result["headroom"]["verdict"], "FAIL")
        self.assertEqual(result["verdict"], "INCONCLUSIVE")
        self.assertEqual(result["failures"], {})

    def test_queue_margin_exact_75_percent_is_allowed(self):
        def change(evidence):
            [row["status"] for row in evidence if row.get("type") == "status"][1]["performance"]["sd"]["peak"] = 768
        self.modify_jsonl("monitor.jsonl", change)
        self.assertEqual(self.report()["headroom"]["verdict"], "PASS")

    def test_empty_boot_id_is_not_proof_against_reset(self):
        def change(evidence):
            for row in evidence:
                if row.get("type") == "status":
                    row["status"]["boot_id"] = ""
        self.modify_jsonl("monitor.jsonl", change)
        self.assertEqual(self.report()["verdict"], "INCONCLUSIVE")

    def test_max_prefix_latency_is_a_gauge_not_a_counter(self):
        def change(evidence):
            samples = [row["status"] for row in evidence if row.get("type") == "status"]
            samples[0]["performance"]["sd"]["max_card_busy_ms"] = 1000
            samples[1]["performance"]["sd"]["max_card_busy_ms"] = 400
        self.modify_jsonl("monitor.jsonl", change)
        result = self.report()
        self.assertEqual(result["monitor"]["gauges"]["performance.sd.max_card_busy_ms"]["max"], 1000)
        self.assertNotIn("performance.sd.max_card_busy_ms", result["monitor"]["deltas"])

    def test_error_sources_are_not_silently_dropped_from_telemetry(self):
        for section, field in (("sd", "card_busy_timeouts"), ("sd", "command_overflows"), ("lcd", "spi_errors"), ("lcd", "ui_snapshot_errors")):
            with self.subTest(section=section, field=field):
                def change(evidence):
                    samples = [row["status"] for row in evidence if row.get("type") == "status"]
                    for sample in samples:
                        sample["performance"][section][field] = 0
                    samples[-1]["performance"][section][field] = 1
                self.modify_jsonl("monitor.jsonl", change)
                result = self.report()
                self.assertEqual(result["verdict"], "FAIL")
                self.assertEqual(result["failures"][f"monitor.performance.{section}.{field}"], 1)

    def test_queue_not_drained_is_inconclusive(self):
        def change(evidence):
            [row["status"] for row in evidence if row.get("type") == "status"][-1]["performance"]["sd"]["q"] = 2
        self.modify_jsonl("monitor.jsonl", change)
        self.assertEqual(self.report()["verdict"], "INCONCLUSIVE")

    def test_stop_failure_is_fail_even_when_sd_files_look_complete(self):
        def change(evidence):
            [row["status"] for row in evidence if row.get("type") == "status"][-1]["rec_result"] = 1
        self.modify_jsonl("monitor.jsonl", change)
        result = self.report()
        self.assertEqual(result["verdict"], "FAIL")
        self.assertEqual(result["failures"]["monitor.final_rec_result"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
