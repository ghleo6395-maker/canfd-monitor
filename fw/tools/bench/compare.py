#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Conservative bare-metal/FreeRTOS A/B comparison of recorded evidence."""
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics

try:
    from .validate import monitor_summary, rows
except ImportError:
    from validate import monitor_summary, rows

MODES = ("bare-metal", "freertos")
MATCH_FIELDS = ("payload", "run_id", "duration_ns", "duration_s", "fps_per_channel", "extended", "fd_flags")
ENV_FIELDS = ("board_revision", "board_identity", "mcu", "clock_hz", "cache", "card", "capacities", "can_bitrates", "power", "bench_config", "busy_workload")
METRICS = {
    "can_service_gap_us": r"performance\.can\[[01]\]\.max_service_gap_us$",
    "can_service_wall_us": r"performance\.can\[[01]\]\.max_service_us$",
    "ui_service_gap_us": r"(?:(?:rtos|timing)\.max_ui_(?:service_)?gap_us|performance\.lcd\.max_service_gap_us)$",
    "control_latency_us": r"(?:rtos|timing)\.max_(?:control_(?:latency|queue)|input_latency)_us$",
    "ui_task_cpu_us": r"(?:rtos|timing)\.max_ui_(?:task|work)_us$",
    "sd_card_busy_ms": r"performance\.sd\.max_card_busy_ms$",
    "sd_write_ms": r"performance\.sd\.max_write_ms$",
    "sd_sync_ms": r"performance\.sd\.max_sync_ms$",
    "sd_task_ms": r"performance\.sd\.max_task_ms$",
}


def read_json(path):
    with open(path, encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"JSON object required: {path}")
    return value


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def summarize_numbers(values):
    values = [v for v in values if v is not None]
    return {"n": len(values), "min": min(values) if values else None,
            "median": statistics.median(values) if values else None, "max": max(values) if values else None}


def validate_environment(value, mode):
    problems = []
    def missing_marker(obj):
        if isinstance(obj, dict):
            return any(missing_marker(x) for x in obj.values())
        if isinstance(obj, list):
            return any(missing_marker(x) for x in obj)
        return isinstance(obj, str) and obj.startswith(("FILL", "TBD", "CHANGE_ME"))
    if missing_marker(value):
        problems.append(mode + " environment still contains unfilled template markers")
    positive = {"clock_hz": ("cpu", "hclk", "fdcan", "sdmmc", "spi4"),
                "capacities": ("can_ingress", "sd_record", "frame_ring"), "card": ("capacity_bytes",)}
    for section, keys in positive.items():
        data = value.get(section, {})
        if not isinstance(data, dict) or any(not isinstance(data.get(k), int) or isinstance(data.get(k), bool) or data[k] <= 0 for k in keys):
            problems.append(mode + " positive numeric environment values missing: " + section)
    cache = value.get("cache", {})
    if not isinstance(cache, dict) or not all(isinstance(cache.get(k), bool) for k in ("icache", "dcache")):
        problems.append(mode + " icache/dcache declarations must be booleans")
    card = value.get("card", {})
    if not isinstance(card, dict) or not all(card.get(k) for k in ("model", "identity_hash", "filesystem")):
        problems.append(mode + " card model/identity/filesystem missing")
    bitrates = value.get("can_bitrates", {})
    for phase in ("nominal", "data"):
        items = bitrates.get(phase, []) if isinstance(bitrates, dict) else []
        if not isinstance(items, list) or len(items) != 2 or any(not isinstance(x, int) or x <= 0 for x in items):
            problems.append(mode + " two positive bitrates required for " + phase)
    return problems


def read_metrics(report, monitor):
    counts = report.get("counts", {})
    result = {
        "missing_frames": counts.get("witness_vs_sd.missing", counts.get("accepted_vs_sd.missing")),
        "extra_frames": counts.get("witness_vs_sd.extra", counts.get("accepted_vs_sd.extra")),
        "duplicates": counts.get("observed.duplicate", 0),
        "bad_payload_crc": counts.get("observed.bad_payload", 0),
        "segment_adjacent_duplicates": counts.get("observed.adjacent_segment_duplicate", 0),
        "segment_sequence_order_errors": counts.get("observed.sequence_order", 0),
        "exact_fence_errors": report.get("exact_fence_errors"),
    }
    gauges = monitor.get("gauges", {})
    for name, pattern in METRICS.items():
        values = [bucket["max"] for key, bucket in gauges.items() if re.search(pattern, key)]
        result[name] = max(values) if values else None
    rtt = monitor.get("status_roundtrip", {})
    result["status_roundtrip_max_ms"] = rtt.get("max_ms")
    result["status_roundtrip_p99_ms"] = rtt.get("p99_ms")
    # Compare the same host+USB+device probe in both modes. A firmware input
    # queue latency is retained separately; it is not substituted for host RTT.
    result["firmware_input_queue_max_us"] = result["control_latency_us"]
    result["control_latency_us"] = rtt["max_ms"]*1000 if rtt.get("max_ms") is not None else None
    for queue in report.get("headroom", {}).get("queues", []):
        result["queue_" + queue["queue"].lower() + "_peak_percent"] = queue.get("used_percent")
    return result


def scheduler_check(path, expected):
    schedulers = set()
    for item in rows(path):
        if item.get("type") == "status":
            status = item.get("status", {})
            scheduler = status.get("scheduler") or status.get("rtos", {}).get("scheduler")
            if scheduler is None and str(status.get("firmware", "")).startswith(("revd-stress-", "revd-dma-")):
                scheduler = "bare-metal"  # Explicit legacy Rev D tag; timing may still be absent.
            schedulers.add(scheduler)
    return schedulers == {expected}, sorted(str(x) for x in schedulers)


def compare_models(bare_path, rtos_path):
    bare, rtos = read_json(bare_path), read_json(rtos_path)
    if not isinstance(bare.get("scenarios"), list) or not isinstance(rtos.get("scenarios"), list):
        raise ValueError("model compare needs two scenario reports")
    left = {row["scenario"]: row for row in bare["scenarios"]}
    right = {row["scenario"]: row for row in rtos["scenarios"]}
    if left.keys() != right.keys():
        raise ValueError("model scenario sets differ")
    metrics = ("zero_loss", "pass", "injected", "on_card", "accepted_missing", "rejected", "queue_peak", "crc_errors", "boundary_errors", "lcd_snapshot_errors", "close_latency_us", "longest_capture_pump_gap_us", "max_core_service_gap_us", "max_ui_service_gap_us", "max_synthetic_control_probe_us", "owner_errors", "max_fatfs_call_depth")
    paired = []
    def model_margin(row):
        sd_peak, sd_capacity = row.get("queue_peak"), row.get("queue_capacity")
        peaks, capacity = row.get("ingress_peak"), row.get("ingress_capacity")
        if sd_peak is None or not sd_capacity or not peaks or not capacity:
            return {"verdict": "INCONCLUSIVE", "reason": "model capacity/high-water fields missing"}
        queue_values = [("SD", sd_peak, sd_capacity), ("CAN1", peaks[0], capacity), ("CAN2", peaks[1], capacity)]
        return {"verdict": "PASS" if all(peak*4 <= cap*3 for _, peak, cap in queue_values) else "FAIL",
                "max_used_percent": 75, "queues": [{"queue": name, "peak": peak, "capacity": cap, "used_percent": 100*peak/cap} for name, peak, cap in queue_values]}
    for name in left:
        a, b = left[name], right[name]
        for field in ("fps_per_channel", "duration_ms", "injected_busy_ms", "busy_target", "segments_requested"):
            # Segment request count may fall short precisely because a scheduler
            # regressed: show that result instead of refusing to report the fault.
            if field == "segments_requested":
                continue
            if a.get(field) != b.get(field):
                raise ValueError(f"model scenario input mismatch: {name} {field}")
        paired.append({"scenario": name, "expectation": a.get("expectation"),
                       "bare-metal": {**{field: a.get(field) for field in metrics}, "headroom": model_margin(a)},
                       "freertos": {**{field: b.get(field) for field in metrics}, "headroom": model_margin(b)}})
    return {"schema": "revd-ab-compare-v1", "verdict": "MODEL", "physical_hardware_tested": False,
            "hardware_speedup_claim": None, "integrity_expectations_met": bool(bare.get("all_expectations_met") and rtos.get("all_expectations_met")),
            "reason": "actual host kernel/task tests with modeled peripherals are not STM32 throughput or latency measurements",
            "metric_semantics": {
                "longest_capture_pump_gap_us": "same capture() entry-to-entry interval while generating input: bare cooperative drain vs RTOS core drain; includes production board_can_task+recorder delivery; not ISR arrival latency",
                "max_ui_service_gap_us": "same display_task entry-to-entry virtual interval, including recording stop/close; product UI CPU path is not modeled",
                "max_synthetic_control_probe_us": "same pending marker handled after core capture, generated every 10 ms; not a USB/API completion latency",
                "max_core_service_gap_us": "RTOS worker-only whole-lifetime entry gap; no fabricated bare counterpart",
            },
            "inputs": {"bare": {"path": str(bare_path), "sha256": sha256(bare_path)}, "rtos": {"path": str(rtos_path), "sha256": sha256(rtos_path)}},
            "scenarios": paired}


def compare_hardware(bare_reports, rtos_reports, bare_monitors, rtos_monitors, environment_path):
    lengths = {len(x) for x in (bare_reports, rtos_reports, bare_monitors, rtos_monitors)}
    if len(lengths) != 1 or not bare_reports:
        raise ValueError("each mode needs equal, nonzero counts of reports and monitor files in paired order")
    environment = read_json(environment_path)
    if environment.get("schema") != "revd-ab-environment-v1":
        raise ValueError("unsupported A/B environment schema")
    issues = []
    failures = []
    synthetic = False
    n = len(bare_reports)
    if n < 3:
        issues.append("at least three complete paired repeats are required")
    modes_env = environment.get("modes", {})
    for mode in MODES:
        issues += validate_environment(modes_env.get(mode, {}), mode)
    for field in ENV_FIELDS:
        left = modes_env.get("bare-metal", {}).get(field)
        right = modes_env.get("freertos", {}).get(field)
        if left in (None, "", {}, []) or right in (None, "", {}, []):
            issues.append("environment declaration missing: " + field)
        elif left != right:
            issues.append("environment differs between modes: " + field)
    if environment.get("physical_hardware_tested") is not True:
        issues.append("operator did not declare physical hardware evidence")
    env_pairs = environment.get("pairs", [])
    if len(env_pairs) != n:
        issues.append("per-pair warmup/window/order declaration count mismatch")
    builds = environment.get("builds", {})
    for mode in MODES:
        build = builds.get(mode, {})
        if not re.fullmatch(r"[0-9a-fA-F]{64}", str(build.get("elf_sha256", ""))):
            issues.append("ELF SHA256 missing for " + mode)
        if not build.get("firmware") or not build.get("toolchain"):
            issues.append("build identity/toolchain missing for " + mode)
        if not build.get("compiler_flags"):
            issues.append("compiler flags missing for " + mode)
    if builds.get("bare-metal", {}).get("toolchain") != builds.get("freertos", {}).get("toolchain"):
        issues.append("compiler toolchain differs between modes")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", str(builds.get("freertos", {}).get("kernel_commit", ""))):
        issues.append("FreeRTOS kernel commit missing")
    if builds.get("bare-metal", {}).get("elf_sha256") == builds.get("freertos", {}).get("elf_sha256"):
        issues.append("both scheduler modes claim the same ELF binary")
    pairs = []
    configs = []
    run_ids = set()
    values = {mode: {} for mode in MODES}
    orders = []
    for index, paths in enumerate(zip(bare_reports, rtos_reports, bare_monitors, rtos_monitors)):
        a_path, b_path, am_path, bm_path = paths
        a, b = read_json(a_path), read_json(b_path)
        declaration = env_pairs[index] if index < len(env_pairs) else {}
        pair_issues = []
        order = declaration.get("order")
        if order not in (["bare-metal", "freertos"], ["freertos", "bare-metal"]):
            pair_issues.append("pair must declare order containing both modes exactly once")
        else:
            orders.append(tuple(order))
        warmup = declaration.get("warmup_s", {})
        windows = declaration.get("window_s", {})
        for name, policy in (("warmup_s", warmup), ("window_s", windows)):
            if not all(isinstance(policy.get(mode), (int, float)) and policy[mode] > 0 for mode in MODES):
                pair_issues.append(name + " must be measured positive seconds for both modes")
            elif policy["bare-metal"] != policy["freertos"]:
                pair_issues.append(name + " differs between modes")
        if declaration.get("card_start_state_equal") is not True:
            pair_issues.append("equal card starting state/free-space conditioning not declared")
        ca, cb = a.get("sender", {}).get("config", {}), b.get("sender", {}).get("config", {})
        for field in MATCH_FIELDS:
            if ca.get(field) != cb.get(field) or ca.get(field) is None:
                # Older schema1 reports do not carry duration_ns; use duration_s.
                if field == "duration_ns" and ca.get(field) is None and cb.get(field) is None:
                    continue
                pair_issues.append("load config differs or missing: " + field)
        run_id = a.get("run_id")
        if run_id != b.get("run_id") or run_id in run_ids or run_id is None:
            pair_issues.append("same run_id required within pair; a new run_id required for each repeat")
        run_ids.add(run_id)
        configs.append({key: ca.get(key) for key in MATCH_FIELDS if key != "run_id"})
        records = {}
        for mode, report, report_path, monitor_path in (("bare-metal", a, a_path, am_path), ("freertos", b, b_path, bm_path)):
            synthetic |= bool(report.get("synthetic"))
            telemetry = monitor_summary(monitor_path)
            scheduler_ok, seen = scheduler_check(monitor_path, mode)
            if not scheduler_ok:
                pair_issues.append(f"{mode} monitor scheduler identity mismatch: {seen}")
            if not telemetry.get("complete"):
                pair_issues.append(f"{mode} monitor incomplete/reset: {telemetry.get('issues')}")
            firmware = telemetry.get("firmware")
            if firmware != builds.get(mode, {}).get("firmware"):
                pair_issues.append(mode + " monitor/build firmware mismatch")
            supplied_monitor = report.get("monitor", {})
            if (supplied_monitor.get("baseline_wall_time") != telemetry.get("baseline_wall_time") or
                supplied_monitor.get("end_wall_time") != telemetry.get("end_wall_time") or
                supplied_monitor.get("firmware") != firmware):
                pair_issues.append(mode + " report does not match supplied monitor window/firmware")
            start = (report.get("sender", {}).get("start") or {}).get("wall_time")
            end = (report.get("sender", {}).get("end") or {}).get("data_end_wall_time")
            baseline, final = telemetry.get("baseline_wall_time"), telemetry.get("end_wall_time")
            if None in (start, end, baseline, final) or not baseline <= start <= end <= final:
                pair_issues.append(mode + " monitor window does not bracket the sender")
            if isinstance(windows.get(mode), (int, float)) and report.get("sender", {}).get("config", {}).get("duration_s") != windows[mode]:
                pair_issues.append(mode + " measured test window differs from sender duration")
            if report.get("integrity_verdict") not in ("PASS", "PASS_SIMULATION"):
                failures.append(f"pair {index+1} {mode} integrity {report.get('integrity_verdict')}")
            if report.get("headroom", {}).get("verdict") != "PASS":
                pair_issues.append(mode + " queue headroom did not pass")
            declared_capacity = modes_env.get(mode, {}).get("capacities", {})
            for queue in report.get("headroom", {}).get("queues", []):
                key = "can_ingress" if queue.get("queue") in ("CAN1", "CAN2") else "sd_record"
                if queue.get("capacity") != declared_capacity.get(key):
                    pair_issues.append(mode + " report queue capacity differs from environment: " + str(queue.get("queue")))
            for phase, field in (("nominal", "nbr"), ("data", "dbr")):
                declared = modes_env.get(mode, {}).get("can_bitrates", {}).get(phase, [])
                for ch in (0, 1):
                    gauge = telemetry.get("gauges", {}).get(f"channels[{ch}].{field}", {})
                    if len(declared) != 2 or gauge.get("min") != declared[ch] or gauge.get("max") != declared[ch]:
                        pair_issues.append(f"{mode} CAN{ch+1} {phase} bitrate declaration differs/missing in monitor")
            for key, number in modes_env.get(mode, {}).get("bench_config", {}).items():
                gauge = telemetry.get("gauges", {}).get("bench." + key, {})
                if gauge.get("min") != number or gauge.get("max") != number:
                    pair_issues.append(mode + " BENCH configuration declaration differs/missing in monitor: " + key)
            measurements = read_metrics(report, telemetry)
            # Do not infer a zero latency from absent instrumentation.
            for metric in ("can_service_gap_us", "ui_service_gap_us", "control_latency_us"):
                if measurements[metric] is None:
                    pair_issues.append(mode + " timing instrumentation missing: " + metric)
            for metric, number in measurements.items():
                values[mode].setdefault(metric, []).append(number)
            records[mode] = {"report": str(report_path), "report_sha256": sha256(report_path), "monitor": str(monitor_path),
                             "monitor_sha256": sha256(monitor_path), "firmware": firmware,
                             "integrity_verdict": report.get("integrity_verdict"), "headroom_verdict": report.get("headroom", {}).get("verdict"),
                             "metrics": measurements}
        issues += [f"pair {index+1}: " + issue for issue in pair_issues]
        pairs.append({"pair": index+1, "run_id": run_id, "issues": pair_issues, "modes": records})
    if any(config != configs[0] for config in configs[1:]):
        issues.append("load configuration differs across repeats")
    if len(set(orders)) < 2:
        issues.append("A/B order was not alternated/counterbalanced across repeats")
    aggregates = {}
    all_metrics = sorted(set(values["bare-metal"]) | set(values["freertos"]))
    for metric in all_metrics:
        left = summarize_numbers(values["bare-metal"].get(metric, []))
        right = summarize_numbers(values["freertos"].get(metric, []))
        a, b = left["median"], right["median"]
        aggregates[metric] = {"bare-metal": left, "freertos": right,
                              "median_delta_rtos_minus_bare": b-a if a is not None and b is not None else None,
                              "median_change_percent": 100*(b-a)/a if a not in (None, 0) and b is not None else None}
    verdict = "MODEL" if synthetic else "FAIL" if failures else "INCONCLUSIVE" if issues else "PASS"
    return {"schema": "revd-ab-compare-v1", "verdict": verdict, "physical_hardware_tested": verdict not in ("MODEL", "INCONCLUSIVE"),
            "hardware_speedup_claim": None, "repeats": n, "issues": sorted(set(issues)), "integrity_failures": failures,
            "environment": {"path": str(environment_path), "sha256": sha256(environment_path)},
            "meaning": "paired observed counters/maxima only; task isolation does not create SD bandwidth and no universal hardware speedup is inferred",
            "latency_semantics": {"control_latency_us": "same-host STATUS request-to-complete-response RTT converted to us; includes USB/device/host scheduling",
                                  "can_service_wall_us": "board_can_task wall duration including preemption; not CPU-only execution time",
                                  "ui_service_gap_us": "display_task entry gap; MCU HAL milliseconds*1000, 1 ms resolution in both targets",
                                  "firmware_input_queue_max_us": "RTOS input enqueue-to-core latency only; not substituted for common host RTT"},
            "aggregates": aggregates, "pairs": pairs}


def write_csv(report, path):
    with open(path, "w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        if "aggregates" in report:
            writer.writerow(("metric", "bare_n", "bare_median", "bare_max", "rtos_n", "rtos_median", "rtos_max", "median_delta_rtos_minus_bare", "median_change_percent"))
            for name, row in report["aggregates"].items():
                a, b = row["bare-metal"], row["freertos"]
                writer.writerow((name, a["n"], a["median"], a["max"], b["n"], b["median"], b["max"], row["median_delta_rtos_minus_bare"], row["median_change_percent"]))
        else:
            writer.writerow(("scenario", "mode", "zero_loss", "pass", "on_card", "accepted_missing", "queue_peak", "crc_errors", "boundary_errors", "capture_dispatch_gap_model_us", "UI_service_gap_model_us"))
            for scenario in report["scenarios"]:
                for mode in MODES:
                    row = scenario[mode]
                    writer.writerow((scenario["scenario"], mode, row.get("zero_loss"), row.get("pass"), row.get("on_card"), row.get("accepted_missing"), row.get("queue_peak"), row.get("crc_errors"), row.get("boundary_errors"), row.get("longest_capture_pump_gap_us"), row.get("max_ui_service_gap_us")))
