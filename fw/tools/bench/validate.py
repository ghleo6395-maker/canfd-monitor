#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Disk-backed, streaming cross-check of sender, independent witness, SD and CDC.

Verdicts deliberately distinguish a simulated test and absence of bus evidence.
No tool can infer the physical bus from successful send() calls alone.
"""
from collections import Counter
import csv
import json
import math
from pathlib import Path
import re
import sqlite3
import statistics
import tempfile
from fractions import Fraction

try:
    from .protocol import decode_payload, parse_candump, STD_IDS, EXT_IDS, MAGIC, FD_BRS, FD_ESI
except ImportError:
    from protocol import decode_payload, parse_candump, STD_IDS, EXT_IDS, MAGIC, FD_BRS, FD_ESI


def natural(path):
    return tuple((1, int(p)) if p.isdigit() else (0, p.lower()) for p in re.split(r"(\d+)", str(path)))


def expand_paths(paths):
    result = []
    for value in paths:
        path = Path(value)
        if path.is_dir():
            result.extend(sorted((p for p in path.rglob("*") if p.is_file() and p.suffix.lower() in (".log", ".candump")), key=natural))
        elif path.is_file():
            result.append(path)
        else:
            raise ValueError(f"missing log: {path}")
    if not result:
        raise ValueError("no candump .LOG/.log/.candump files")
    if len(set(p.resolve() for p in result)) != len(result):
        raise ValueError("same log file supplied twice")
    return result


def rows(path):
    with open(path, encoding="utf-8") as source:
        for number, text in enumerate(source, 1):
            try:
                item = json.loads(text)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{number}: invalid JSONL: {exc}") from exc
            if not isinstance(item, dict):
                raise ValueError(f"{path}:{number}: JSONL object required")
            yield item


def flatten(value, prefix=""):
    if isinstance(value, dict):
        for key, item in value.items():
            yield from flatten(item, f"{prefix}.{key}" if prefix else key)
    elif isinstance(value, list):
        for i, item in enumerate(value):
            yield from flatten(item, f"{prefix}[{i}]")
    elif not isinstance(value, bool) and isinstance(value, int):
        yield prefix, value


_GAUGES = re.compile(r"(?:^|\.)(?:q|peak|high_water|capacity|can_capacity|sd_capacity|catalog_limited|catalog_omitted|busy|dirty|state|mode|nbr|dbr|fd|ask|rec|rec_result|rec_session|rec_segment|rec_reason|rec_elapsed_ms|storage|usb|host|protocol|seg|epoch|diag_epoch|stats_epoch|uptime_ms|tick_ms|now_ms|boot_id|session)$")
_ERROR_COUNTER = re.compile(r"drop|lost|reject|discard|error|timeout|fail|overflow", re.I)
_REQUIRED_METRICS = {"frames", "rec", "rec_result", "ring_dropped", "event_dropped", "reply_dropped", "rec_drops",
                     "performance.sd.q", "performance.sd.peak", "performance.sd.accepted", "performance.sd.encoded",
                     "performance.sd.rejected", "performance.sd.discarded", "performance.sd.errors", "performance.sd.timeouts",
                     "performance.sd.card_busy_timeouts", "performance.sd.command_overflows",
                     "performance.lcd.dma", "performance.lcd.timeouts", "performance.lcd.spi_errors", "performance.lcd.ui_snapshot_errors"}
for _ch in range(2):
    _REQUIRED_METRICS.update(f"performance.can[{_ch}].{name}" for name in ("q", "peak", "hw_lost_min", "irq_drops", "ring_drops", "irq_read_errors"))


def is_gauge(name):
    return bool(_GAUGES.search(name) or name.rsplit(".", 1)[-1] in ("stack_free", "stack_size", "stack_min_free_bytes", "stack_size_bytes", "segment_kib", "rotate_ms", "refresh_ms", "enabled") or name.rsplit(".", 1)[-1].startswith("max_") or
                name.endswith(("_max_us", "_max_ms", "_peak", "_high_water", "_capacity")))


def monitor_summary(path):
    if not path:
        return {"complete": False, "issues": ["no CDC monitor supplied"], "deltas": {}, "gauges": {}}
    statuses = []
    issues = []
    ended = False
    for item in rows(path):
        if item.get("type") == "status":
            if not isinstance(item.get("status"), dict):
                raise ValueError("monitor status object missing")
            statuses.append(item)
        elif item.get("type") == "monitor_error":
            issues.append("monitor parse/transport error: " + str(item.get("error")))
        elif item.get("type") == "monitor_end":
            ended = item.get("complete") is True
    if len(statuses) < 2:
        issues.append("fewer than two STATUS samples")
        return {"complete": False, "issues": issues, "deltas": {}, "gauges": {}, "samples": len(statuses)}
    if not ended:
        issues.append("monitor lacks complete end marker")
    totals = Counter()
    gauges = {}
    wraps = Counter()
    first = dict(flatten(statuses[0]["status"]))
    previous = first
    previous_status = statuses[0]["status"]
    for sample in statuses:
        current_status = sample["status"]
        current = dict(flatten(current_status))
        for name, value in current.items():
            if is_gauge(name):
                bucket = gauges.setdefault(name, {"baseline": value, "end": value, "min": value, "max": value})
                bucket.update(end=value, min=min(bucket["min"], value), max=max(bucket["max"], value))
        if sample is statuses[0]:
            continue
        if current_status.get("firmware") != previous_status.get("firmware"):
            issues.append("firmware changed during run")
        if current_status.get("boot_id") != previous_status.get("boot_id"):
            issues.append("boot_id changed during run")
        for name in ("diag_epoch", "stats_epoch", "epoch"):
            if current_status.get(name) != previous_status.get(name):
                issues.append(name + " changed (statistics reset)")
        if current.keys() != previous.keys():
            issues.append("STATUS numeric schema changed")
        for name, value in current.items():
            if name not in previous:
                continue
            old = previous[name]
            if name.endswith(("uptime_ms", "tick_ms", "now_ms")) and value < old and not (old >= 0xF0000000 and value < 0x10000000):
                issues.append(name + " moved backward (reset/reboot)")
            if is_gauge(name):
                if (name.endswith((".peak", ".high_water", "_peak", "_high_water")) and value < old):
                    issues.append(name + " high-water reset during run")
                continue
            if value >= old:
                totals[name] += value - old
            elif 0 <= value < 0x10000000 and 0xF0000000 <= old <= 0xFFFFFFFF:
                totals[name] += (value - old) & 0xFFFFFFFF
                wraps[name] += 1
            else:
                issues.append(name + " counter reset or ambiguous wrap")
        previous = current
        previous_status = current_status
    # Never silently treat an absent firmware/uptime field as evidence of no reboot.
    if not statuses[0]["status"].get("firmware"):
        issues.append("STATUS lacks firmware identity")
    if not any(k in first for k in ("uptime_ms", "tick_ms", "now_ms")):
        issues.append("STATUS lacks uptime/tick; reboot cannot be excluded")
    if statuses[0]["status"].get("boot_id") in (None, ""):
        issues.append("STATUS lacks boot_id; reset between samples cannot be excluded")
    if not _REQUIRED_METRICS <= first.keys() or not _REQUIRED_METRICS <= previous.keys():
        issues.append("required recording/CAN/SD/LCD diagnostic fields missing")
    for key in ("performance.can[0].q", "performance.can[1].q", "performance.sd.q"):
        if previous.get(key) != 0:
            issues.append(key + " did not drain to zero before final sample")
    if previous.get("rec") != 0:
        issues.append("recorder not IDLE in final sample; close/sync not verified")
    changed_errors = {k: v for k, v in totals.items() if v and _ERROR_COUNTER.search(k)}
    roundtrips = [sample["roundtrip_ms"] for sample in statuses if isinstance(sample.get("roundtrip_ms"), (int, float)) and sample["roundtrip_ms"] >= 0]
    roundtrips.sort()
    rtt = {"samples": len(roundtrips), "max_ms": max(roundtrips) if roundtrips else None,
           "median_ms": statistics.median(roundtrips) if roundtrips else None,
           "p99_ms": roundtrips[max(0, math.ceil(len(roundtrips)*0.99)-1)] if roundtrips else None,
           "meaning": "same-host STATUS request-to-full-response RTT including USB/device/host scheduling; not MCU CPU time"}
    return {"complete": ended and not issues, "samples": len(statuses), "issues": sorted(set(issues)),
            "firmware": statuses[-1]["status"].get("firmware"),
            "baseline_elapsed_s": statuses[0].get("elapsed_s"), "end_elapsed_s": statuses[-1].get("elapsed_s"),
            "baseline_wall_time": statuses[0].get("wall_time"), "end_wall_time": statuses[-1].get("wall_time"),
            "deltas": dict(totals), "uint32_wraps": dict(wraps), "error_deltas": changed_errors,
            "gauges": gauges, "final_rec_result": previous.get("rec_result"), "status_roundtrip": rtt}


def headroom_summary(telemetry):
    """Project acceptance policy: peak occupancy <=75%, i.e. >=25% spare slots."""
    gauges = telemetry.get("gauges", {})
    queues = []
    for name, peak_key, cap_key in (("CAN1", "performance.can[0].peak", "performance.can_capacity"),
                                   ("CAN2", "performance.can[1].peak", "performance.can_capacity"),
                                   ("SD", "performance.sd.peak", "performance.sd_capacity")):
        peak = gauges.get(peak_key, {}).get("max")
        capacity = gauges.get(cap_key, {}).get("min")
        known = isinstance(peak, int) and isinstance(capacity, int) and peak >= 0 and capacity > 0
        queues.append({"queue": name, "peak": peak, "capacity": capacity,
                       "used_percent": 100 * peak / capacity if known else None,
                       "spare_percent": 100 * (capacity - peak) / capacity if known else None,
                       "passes": peak * 4 <= capacity * 3 if known else None})
    verdict = "FAIL" if any(q["passes"] is False for q in queues) else "INCONCLUSIVE" if any(q["passes"] is None for q in queues) else "PASS"
    return {"verdict": verdict, "max_used_percent": 75, "minimum_spare_percent": 25, "queues": queues,
            "meaning": "queue occupancy margin policy; a margin failure alone does not demonstrate dropped frames"}


class Validator:
    def __init__(self, run_id, extended=False, channel_map=None):
        self.run_id = run_id
        self.extended = extended
        self.channel_map = channel_map or {"can0": 0, "can1": 1}
        self.temp = tempfile.TemporaryDirectory(prefix="revd-bench-")
        self.db = sqlite3.connect(str(Path(self.temp.name) / "validation.sqlite3"))
        self.db.execute("PRAGMA journal_mode=OFF")
        self.db.execute("PRAGMA synchronous=OFF")
        self.db.execute("PRAGMA cache_size=-8192")
        self.db.executescript("""
            CREATE TABLE expected(ch INTEGER, seq INTEGER, planned INTEGER, PRIMARY KEY(ch,seq));
            CREATE TABLE observed(ch INTEGER, seq INTEGER, planned INTEGER, first_seg INTEGER, last_seg INTEGER, count INTEGER, PRIMARY KEY(ch,seq));
            CREATE TABLE witness(ch INTEGER, seq INTEGER, planned INTEGER, first_seg INTEGER, last_seg INTEGER, count INTEGER, PRIMARY KEY(ch,seq));
        """)
        self.counts = Counter()
        self.examples = []

    def close(self):
        self.db.close()
        self.temp.cleanup()

    def issue(self, code, where):
        self.counts[code] += 1
        if len(self.examples) < 64:
            self.examples.append({"code": code, "where": str(where)})

    def read_sender(self, path):
        header = None
        end = None
        start = None
        accepted = [0, 0]
        requested = [0, 0]
        previous = [-1, -1]
        errors = [0, 0]
        for item in rows(path):
            kind = item.get("type")
            if kind == "run":
                if header is not None:
                    raise ValueError("sender log contains more than one run")
                header = item
                if int(item["run_id"], 0) != self.run_id or bool(item.get("extended")) != self.extended:
                    raise ValueError("sender run_id / extended config mismatch")
                if item.get("payload") != "RVDB/1" or item.get("schema") != 1:
                    raise ValueError("unsupported sender schema/payload")
            elif kind == "tx":
                if header is None or end is not None:
                    raise ValueError("sender tx outside run/end")
                ch, seq = item["ch"], item["seq"]
                if ch not in (0, 1) or not isinstance(seq, int) or seq != previous[ch] + 1:
                    raise ValueError("sender request sequence discontinuity")
                previous[ch] = seq
                requested[ch] += 1
                fps = Fraction(header["fps_per_channel"][ch])
                planned = seq * 1_000_000_000 * fps.denominator // fps.numerator
                duration_ns = header.get("duration_ns", int(Fraction(str(header["duration_s"])) * 1_000_000_000))
                if item.get("scheduled_offset_ns") != planned or planned >= duration_ns:
                    raise ValueError("sender tx schedule differs from run config")
                if item.get("send_accepted_ns") is not None:
                    if item.get("send_error"):
                        raise ValueError("sender tx has both acceptance and error")
                    self.db.execute("INSERT INTO expected VALUES(?,?,?)", (ch, seq, item["scheduled_offset_ns"]))
                    accepted[ch] += 1
                else:
                    errors[ch] += 1
            elif kind == "end":
                if end is not None:
                    raise ValueError("sender has duplicate end")
                end = item
            elif kind == "sender_start":
                if start is not None or header is None or end is not None:
                    raise ValueError("sender_start duplicated/outside run")
                start = item
        if header is None:
            raise ValueError("sender has no run header")
        complete = bool(end and end.get("complete") is True)
        if end and (end.get("requested") != requested or end.get("send_accepted") != accepted or end.get("send_errors") != errors):
            raise ValueError("sender end counts do not match tx evidence")
        duration = Fraction(header.get("duration_ns", int(Fraction(str(header["duration_s"])) * 1_000_000_000)), 1_000_000_000)
        planned_counts = [math.ceil(duration * Fraction(f)) for f in header["fps_per_channel"]]
        if complete and planned_counts != requested:
            raise ValueError("sender claims completion but requests do not cover configured duration/rates")
        self.db.commit()
        return {"complete": complete, "synthetic": bool(header.get("synthetic")), "requested": requested,
                "send_accepted": accepted, "send_errors": errors, "start": start, "end": end, "config": header}

    def read_logs(self, paths, table="observed", channel_map=None):
        mapping = channel_map or self.channel_map
        paths = expand_paths(paths)
        previous_seq = [-1, -1]
        previous_ts = [-1, -1]
        last_global_ts = -1
        files = []
        for segment, path in enumerate(paths):
            matched = 0
            lines = 0
            with path.open(encoding="ascii", errors="strict") as source:
                for lines, line in enumerate(source, 1):
                    where = f"{path}:{lines}"
                    if not line.endswith("\n"):
                        self.issue(table + ".unterminated_line", where)
                    try:
                        frame = parse_candump(line)
                    except ValueError:
                        self.issue(table + ".parse_error", where)
                        continue
                    if frame is None:
                        continue
                    known_id = frame.can_id in STD_IDS + EXT_IDS
                    if not frame.data.startswith(MAGIC) and not known_id:
                        self.counts[table + ".non_bench"] += 1
                        continue
                    try:
                        item = decode_payload(frame.data)
                    except ValueError as exc:
                        self.issue(table + ".bad_payload", where + ": " + str(exc))
                        continue
                    if item["run_id"] != self.run_id:
                        self.issue(table + ".foreign_run", where)
                        continue
                    ch, seq = item["ch"], item["seq"]
                    if seq > 0x7FFFFFFFFFFFFFFF or item["planned_ns"] > 0x7FFFFFFFFFFFFFFF:
                        self.issue(table + ".sequence_out_of_range", where)
                        continue
                    if mapping.get(frame.interface) != ch:
                        self.issue(table + ".channel_mismatch", where)
                    if (not frame.fd or frame.rtr or frame.error or frame.extended != self.extended or
                        frame.can_id != (EXT_IDS if self.extended else STD_IDS)[ch] or
                        not frame.flags & FD_BRS or frame.flags & FD_ESI):
                        self.issue(table + ".frame_type_flags_id", where)
                    if seq <= previous_seq[ch]:
                        self.issue(table + ".sequence_order", where)
                    if frame.ts_us < previous_ts[ch]:
                        self.issue(table + ".channel_time_order", where)
                    # Rev D currently drains two independent IRQ queues; cross-channel
                    # order is diagnostic, not a sequence-integrity failure.
                    if frame.ts_us < last_global_ts:
                        self.counts[table + ".cross_channel_time_reversals"] += 1
                    previous_seq[ch] = seq
                    previous_ts[ch] = frame.ts_us
                    last_global_ts = frame.ts_us
                    found = self.db.execute(f"SELECT planned,last_seg FROM {table} WHERE ch=? AND seq=?", (ch, seq)).fetchone()
                    if found:
                        self.issue(table + ".duplicate", where)
                        if found[1] == segment - 1:
                            self.issue(table + ".adjacent_segment_duplicate", where)
                        if found[0] != item["planned_ns"]:
                            self.issue(table + ".payload_conflict", where)
                        self.db.execute(f"UPDATE {table} SET count=count+1,last_seg=? WHERE ch=? AND seq=?", (segment, ch, seq))
                    else:
                        self.db.execute(f"INSERT INTO {table} VALUES(?,?,?,?,?,1)", (ch, seq, item["planned_ns"], segment, segment))
                    matched += 1
            self.db.commit()
            files.append({"path": str(path), "lines": lines, "bench_frames": matched})
        return files

    def compare(self, source, dest, prefix):
        missing = self.db.execute(f"SELECT COUNT(*) FROM {source} s LEFT JOIN {dest} d ON s.ch=d.ch AND s.seq=d.seq WHERE d.seq IS NULL").fetchone()[0]
        extra = self.db.execute(f"SELECT COUNT(*) FROM {dest} d LEFT JOIN {source} s ON s.ch=d.ch AND s.seq=d.seq WHERE s.seq IS NULL").fetchone()[0]
        conflict = self.db.execute(f"SELECT COUNT(*) FROM {source} s JOIN {dest} d ON s.ch=d.ch AND s.seq=d.seq WHERE s.planned != d.planned").fetchone()[0]
        self.counts[prefix + ".missing"] = missing
        self.counts[prefix + ".extra"] = extra
        self.counts[prefix + ".payload_conflict"] = conflict
        per_channel = []
        for ch in (0, 1):
            expected, last = self.db.execute(f"SELECT COUNT(*),MAX(seq) FROM {source} WHERE ch=?", (ch,)).fetchone()
            actual, first_seen, last_seen = self.db.execute(f"SELECT COUNT(*),MIN(seq),MAX(seq) FROM {dest} WHERE ch=?", (ch,)).fetchone()
            absent_tail = 0 if last is None else self.db.execute(f"SELECT COUNT(*) FROM {source} s LEFT JOIN {dest} d ON s.ch=d.ch AND s.seq=d.seq WHERE s.ch=? AND s.seq>? AND d.seq IS NULL", (ch, -1 if last_seen is None else last_seen)).fetchone()[0]
            per_channel.append({"channel": ch, "expected": expected, "unique_recorded": actual,
                                "first_seen_seq": first_seen, "last_expected_seq": last, "last_seen_seq": last_seen,
                                "missing_tail_frames": absent_tail})
        return per_channel

    def csv(self, destination):
        """Write anomalies, not every good frame; bounded RAM even for long runs."""
        with open(destination, "w", newline="", encoding="utf-8") as out:
            writer = csv.writer(out)
            writer.writerow(("kind", "channel", "sequence", "count", "first_segment", "last_segment"))
            for row in self.db.execute("SELECT ch,seq,count,first_seg,last_seg FROM observed WHERE count>1 ORDER BY ch,seq"):
                writer.writerow(("duplicate", *row))
            for row in self.db.execute("SELECT e.ch,e.seq FROM expected e LEFT JOIN observed o ON e.ch=o.ch AND e.seq=o.seq WHERE o.seq IS NULL ORDER BY e.ch,e.seq"):
                writer.writerow(("missing_accepted", *row, 0, "", ""))
            for row in self.db.execute("SELECT w.ch,w.seq FROM witness w LEFT JOIN observed o ON w.ch=o.ch AND w.seq=o.seq WHERE o.seq IS NULL ORDER BY w.ch,w.seq"):
                writer.writerow(("missing_witnessed", *row, 0, "", ""))


def create_report(*, sender=None, run_id=None, extended=False, logs, witness=None,
                  witness_external=False, monitor=None, channel_map=None, witness_map=None, csv_path=None):
    if sender:
        first = next((row for row in rows(sender) if row.get("type") == "run"), None)
        if not first:
            raise ValueError("sender header missing")
        recorded_run = int(first["run_id"], 0)
        if run_id is not None and run_id != recorded_run:
            raise ValueError("requested run_id disagrees with sender")
        run_id = recorded_run
        extended = bool(first.get("extended"))
    if run_id is None:
        raise ValueError("--sender or --run-id is required")
    validator = Validator(run_id, extended, channel_map)
    try:
        source = validator.read_sender(sender) if sender else {"complete": False, "synthetic": False}
        files = validator.read_logs(logs)
        witness_files = validator.read_logs(witness, "witness", witness_map) if witness else []
        if ({Path(item["path"]).resolve() for item in files} & {Path(item["path"]).resolve() for item in witness_files}):
            raise ValueError("SD log and independent witness cannot be the same file")
        channels = validator.compare("expected", "observed", "accepted_vs_sd") if sender else []
        witness_channels = validator.compare("witness", "observed", "witness_vs_sd") if witness else []
        if sender and witness:
            validator.compare("expected", "witness", "accepted_vs_witness")
        # Non-bench background and cross-channel merge order do not imply loss.
        failed = {key: value for key, value in validator.counts.items() if value and not key.endswith((".non_bench", ".cross_channel_time_reversals"))}
        telemetry = monitor_summary(monitor)
        reasons = []
        for key, value in telemetry.get("error_deltas", {}).items():
            # Stream loss is separate from recording, but it still invalidates a
            # zero-error acceptance run and is visible instead of being hidden.
            failed["monitor." + key] = value
        if telemetry.get("final_rec_result") not in (None, 0):
            failed["monitor.final_rec_result"] = telemetry["final_rec_result"]
        if failed:
            reasons.append("recording/evidence mismatch or error counters increased")
        if not sender or not source["complete"]:
            reasons.append("complete per-frame sender evidence missing")
        if source.get("send_errors") and any(source["send_errors"]):
            failed["sender.send_errors"] = sum(source["send_errors"])
        if source.get("end") and any(source["end"].get("late_over_period", [])):
            reasons.append("host missed pacing deadlines; requested load was not sustained")
        if not witness or not witness_external:
            reasons.append("no explicitly declared independent receiver witness")
        if not telemetry["complete"]:
            reasons.extend(telemetry["issues"])
        start_wall = (source.get("start") or {}).get("wall_time")
        end_wall = (source.get("end") or {}).get("data_end_wall_time")
        baseline_wall = telemetry.get("baseline_wall_time")
        monitor_end_wall = telemetry.get("end_wall_time")
        if (None in (start_wall, end_wall, baseline_wall, monitor_end_wall) or
            not baseline_wall <= start_wall <= end_wall <= monitor_end_wall):
            reasons.append("CDC baseline/end do not bracket sender wall-clock window on the same host")
        if channels and any(c["expected"] == 0 for c in channels):
            reasons.append("both channels need nonzero accepted frames")
        if witness_channels and any(c["expected"] == 0 for c in witness_channels):
            reasons.append("both channels need nonzero witnessed frames")
        integrity = "FAIL" if failed else "INCONCLUSIVE" if reasons else "PASS"
        if source.get("synthetic") and integrity == "PASS":
            integrity = "PASS_SIMULATION"
        headroom = headroom_summary(telemetry)
        verdict = integrity
        if not failed and headroom["verdict"] != "PASS":
            verdict = "INCONCLUSIVE"
            reasons.append("queue occupancy margin failed or capacity/high-water evidence missing; not proof of loss")
        if csv_path:
            validator.csv(csv_path)
        return {"schema": 1, "verdict": verdict, "integrity_verdict": integrity, "headroom": headroom,
                "run_id": hex(run_id), "synthetic": source.get("synthetic", False),
                "claim": "evidence consistency only; synthetic fixtures are not hardware validation" if source.get("synthetic") else
                         "comparison against explicitly supplied sender, receiver and CDC evidence; not a claim about unobserved bus traffic",
                "reasons": sorted(set(reasons)), "failures": failed, "counts": dict(validator.counts),
                "examples": validator.examples, "sender": source, "channels": channels,
                "witness_channels": witness_channels, "sd_files": files, "witness_files": witness_files,
                "monitor": telemetry}
    finally:
        validator.close()
