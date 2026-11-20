#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rev D dual CAN FD bench CLI. stdlib only. No hardware access by default."""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import re
import secrets
import sys
import time

try:
    from .bench_io import config, emit, monitor_run, PosixSerial, schedule, send_run
    from .protocol import format_candump, UINT64_MAX
    from .validate import create_report
    from .compare import compare_hardware, compare_models, write_csv as write_compare_csv
except ImportError:
    from bench_io import config, emit, monitor_run, PosixSerial, schedule, send_run
    from protocol import format_candump, UINT64_MAX
    from validate import create_report
    from compare import compare_hardware, compare_models, write_csv as write_compare_csv


def integer(text):
    value = int(text, 0)
    if not 0 <= value <= UINT64_MAX:
        raise argparse.ArgumentTypeError("must fit uint64")
    return value


def mappings(values):
    result = {}
    for item in values:
        if "=" not in item:
            raise ValueError("channel maps use interface=0 or interface=1")
        interface, channel = item.rsplit("=", 1)
        if not interface or channel not in ("0", "1") or interface in result:
            raise ValueError("channel maps must have unique interfaces and channels 0..1")
        result[interface] = int(channel)
    if set(result.values()) != {0, 1}:
        raise ValueError("map must describe both logical channels")
    return result


def fixture(directory, *, run_id, duration, fps, extended=False, segment_frames=8, fault="none"):
    directory = Path(directory)
    if directory.exists() and any(directory.iterdir()):
        raise ValueError("fixture output must be empty/new; previous evidence is never overwritten")
    if segment_frames < 1:
        raise ValueError("segment-frames must be >=1")
    directory.mkdir(parents=True, exist_ok=True)
    sd = directory / "sd"
    sd.mkdir()
    counts = [0, 0]
    total = 0
    sd_count = 0
    segment = 0
    out = (sd / f"{segment:08d}.LOG").open("w", encoding="ascii")

    def write_sd(line):
        nonlocal out, sd_count, segment
        if sd_count and sd_count % segment_frames == 0:
            out.close()
            segment += 1
            out = (sd / f"{segment:08d}.LOG").open("w", encoding="ascii")
        out.write(line)
        sd_count += 1
    try:
        with (directory / "sender.jsonl").open("w", encoding="utf-8") as manifest, (directory / "witness.log").open("w", encoding="ascii") as witness:
            emit(manifest, config(run_id, duration, fps, extended, ["fixture0", "fixture1"], True))
            emit(manifest, {"type": "sender_start", "monotonic_ns": 0, "wall_time": 0.0})
            for offset, ch, seq in schedule(duration, fps):
                counts[ch] += 1
                total += 1
                emit(manifest, {"type": "tx", "ch": ch, "seq": seq, "scheduled_offset_ns": offset,
                                "requested_ns": offset, "late_ns": 0, "send_accepted_ns": offset})
                line = format_candump(ch, seq, run_id, offset, extended)
                witness.write(line)
                if fault == "missing" and ch == 0 and seq == 1:
                    continue
                if fault == "crc" and ch == 0 and seq == 1:
                    line = line[:-3] + ("00" if line[-3:-1] != "00" else "01") + "\n"
                write_sd(line)
                if fault == "duplicate" and ch == 0 and seq == 1:
                    write_sd(line)
            emit(manifest, {"type": "end", "complete": True, "requested": counts,
                            "send_accepted": counts, "send_errors": [0, 0], "late_over_period": [0, 0],
                            "data_end_wall_time": float(duration),
                            "local_confirm_echoes": [0, 0], "error": None})
    finally:
        out.close()
    files = sorted(sd.glob("*.LOG"))
    if fault == "segment-order":
        if len(files) < 2:
            raise ValueError("segment-order fault needs >=2 segments")
        temporary = sd / "swap.tmp"
        files[0].rename(temporary)
        files[1].rename(files[0])
        temporary.rename(files[1])
    elif fault == "tail":
        lines = files[-1].read_text(encoding="ascii").splitlines(keepends=True)
        files[-1].write_text("".join(lines[:-1]), encoding="ascii")
    with (directory / "monitor.jsonl").open("w", encoding="utf-8") as monitor:
        emit(monitor, {"type": "monitor_start", "schema": 1})
        for i, frame_count in ((0, 0), (1, total)):
            status = {"firmware": "SYNTHETIC-fixture-only", "boot_id": "fixture-boot-1", "diag_epoch": 0,
                      "uptime_ms": 1000 + i * int(float(duration) * 1000), "frames": frame_count,
                      "rec": 2 if i == 0 else 0, "rec_result": 0,
                      "ring_dropped": 0, "event_dropped": 0, "reply_dropped": 0, "stream_dropped": 0, "rec_drops": 0,
                      "performance": {"can_capacity": 512, "sd_capacity": 1024,
                                      "can": [{"q": 0, "peak": i * 2, "irq_drops": 0, "hw_lost_min": 0, "ring_drops": 0, "irq_read_errors": 0} for _ in range(2)],
                                      "sd": {"q": 0, "peak": i * 3, "accepted": frame_count, "encoded": frame_count,
                                             "rejected": 0, "discarded": 0, "errors": 0, "timeouts": 0,
                                             "card_busy_timeouts": 0, "command_overflows": 0},
                                      "lcd": {"dma": i * 20, "timeouts": 0, "busy": 0, "spi_errors": 0, "ui_snapshot_errors": 0}}}
            emit(monitor, {"type": "status", "wall_time": i * float(duration), "elapsed_s": i * float(duration), "status": status})
        emit(monitor, {"type": "monitor_end", "complete": True, "samples": 2})
    return {"type": "fixture", "synthetic": True, "out": str(directory), "run_id": hex(run_id), "frames": counts,
            "fault": fault, "segments": len(files)}


def control_command(command):
    command = " ".join(command.upper().split())
    if command in ("STATUS", "REC START", "REC STOP", "REC MARK", "BENCH ON", "BENCH OFF", "BENCH ROTATE"):
        return command
    match = re.fullmatch(r"BENCH CONFIG (\d+) (\d+) (\d+)", command)
    if match:
        kib, rotate, refresh = map(int, match.groups())
        if 64 <= kib <= 65536 and (rotate == 0 or 250 <= rotate <= 0xFFFFFFFF) and (refresh == 0 or 50 <= refresh <= 0xFFFFFFFF):
            return command
    raise ValueError("control allows STATUS, REC START|STOP|MARK, BENCH ON|OFF|ROTATE or BENCH CONFIG <64..65536 KiB> <0 or >=250 ms> <0 or >=50 ms>; no confirmation bypass")


def control(port, command, out, timeout=3.0):
    command = control_command(command)
    emit(out, {"type": "control", "command": command, "wall_time": time.time(), "meaning": "request only; inspect STATUS and device confirmation"})
    pending = (command + "\n").encode("ascii")
    until = time.monotonic() + timeout
    buffer = b""
    while time.monotonic() < until:
        if pending:
            try:
                nbytes = port.write(pending)
                pending = pending[nbytes:]
            except BlockingIOError:
                pass
        buffer += port.read(min(0.05, max(0, until - time.monotonic())))
        if len(buffer) > 65536:
            raise ValueError("CDC response exceeds 64 KiB")
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").strip()
            emit(out, {"type": "control_reply", "text": line, "wall_time": time.time()})
            if not pending and (line.startswith(("OK", "ERR", "STATUS "))):
                return 1 if line.startswith("ERR") else 0
    emit(out, {"type": "control_timeout"})
    return 1


def selftest(directory):
    directory = Path(directory)
    if directory.exists() and any(directory.iterdir()):
        raise ValueError("selftest output must be new/empty")
    outcomes = []
    for fault in ("none", "missing", "duplicate", "crc", "segment-order", "tail"):
        target = directory / fault
        fixture(target, run_id=0x52455644, duration=2, fps=[10, 10], fault=fault)
        report = create_report(sender=target / "sender.jsonl", logs=[target / "sd"], witness=[target / "witness.log"],
                               witness_external=True, monitor=target / "monitor.jsonl", csv_path=target / "anomalies.csv")
        (target / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        expected = "PASS_SIMULATION" if fault == "none" else "FAIL"
        checks = report["verdict"] == expected
        if fault == "missing":
            checks &= report["counts"]["accepted_vs_sd.missing"] == 1
        outcomes.append({"fault": fault, "expected": expected, "verdict": report["verdict"], "ok": bool(checks)})
    result = {"type": "selftest", "synthetic": True, "ok": all(item["ok"] for item in outcomes), "cases": outcomes}
    (directory / "selftest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="action", required=True)
    for action in ("generate", "send"):
        command = sub.add_parser(action)
        command.add_argument("--out", type=Path, required=True, help="new fixture directory / sender JSONL file (will not overwrite)")
        command.add_argument("--run-id", type=integer, default=None)
        command.add_argument("--duration", default="10")
        command.add_argument("--fps", nargs=2, default=["1000", "1000"], metavar=("CH0_FPS", "CH1_FPS"))
        command.add_argument("--extended", action="store_true")
        if action == "generate":
            command.add_argument("--segment-frames", type=int, default=1000)
            command.add_argument("--fault", choices=("none", "missing", "duplicate", "crc", "segment-order", "tail"), default="none")
        else:
            command.add_argument("--interfaces", nargs=2, required=True, metavar=("CH0_IFACE", "CH1_IFACE"))
            command.add_argument("--hardware", action="store_true", help="explicitly permit sending to the prepared bench interfaces")
    for action in ("monitor", "control"):
        command = sub.add_parser(action)
        command.add_argument("--port", required=True, help="explicit CDC tty path")
        command.add_argument("--hardware", action="store_true")
        command.add_argument("--out", type=Path, required=True, help="new JSONL file")
        if action == "monitor":
            command.add_argument("--duration", type=float, default=60.0)
            command.add_argument("--interval", type=float, default=1.0)
        else:
            command.add_argument("--command", required=True)
    report = sub.add_parser("report")
    report.add_argument("--sender", type=Path)
    report.add_argument("--run-id", type=integer)
    report.add_argument("--extended", action="store_true", help="used with --run-id when sender is absent")
    report.add_argument("--logs", nargs="+", required=True, type=Path, help="SD session directory or explicit files in desired order")
    report.add_argument("--witness", nargs="+", type=Path, help="independent receiver candump -L logs")
    report.add_argument("--witness-external", action="store_true", help="declare witness came from a separate receiving adapter/node (not local TX echo)")
    report.add_argument("--monitor", type=Path)
    report.add_argument("--channel-map", nargs="+", default=["can0=0", "can1=1"])
    report.add_argument("--witness-map", nargs="+", default=["can0=0", "can1=1"])
    report.add_argument("--json", type=Path, required=True)
    report.add_argument("--csv", type=Path, required=True)
    check = sub.add_parser("selftest")
    check.add_argument("--out", type=Path, required=True)
    compare = sub.add_parser("compare", help="paired bare-metal/FreeRTOS evidence; no hardware I/O")
    compare.add_argument("--models", nargs=2, type=Path, metavar=("BARE_MODEL", "RTOS_MODEL"))
    compare.add_argument("--bare", nargs="+", type=Path)
    compare.add_argument("--rtos", nargs="+", type=Path)
    compare.add_argument("--bare-monitor", nargs="+", type=Path)
    compare.add_argument("--rtos-monitor", nargs="+", type=Path)
    compare.add_argument("--environment", type=Path)
    compare.add_argument("--json", type=Path, required=True)
    compare.add_argument("--csv", type=Path, required=True)
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.action == "generate":
            result = fixture(args.out, run_id=args.run_id if args.run_id is not None else secrets.randbits(64),
                             duration=args.duration, fps=args.fps, extended=args.extended,
                             segment_frames=args.segment_frames, fault=args.fault)
            print(json.dumps(result))
            return 0
        if args.action == "selftest":
            result = selftest(args.out)
            print(json.dumps(result))
            return 0 if result["ok"] else 1
        if args.action == "report":
            result = create_report(sender=args.sender, run_id=args.run_id, extended=args.extended, logs=args.logs,
                                   witness=args.witness, witness_external=args.witness_external, monitor=args.monitor,
                                   channel_map=mappings(args.channel_map), witness_map=mappings(args.witness_map), csv_path=args.csv)
            args.json.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print(json.dumps({"verdict": result["verdict"], "integrity_verdict": result["integrity_verdict"], "headroom": result["headroom"]["verdict"],
                              "run_id": result["run_id"], "failures": result["failures"],
                              "reasons": result["reasons"], "json": str(args.json), "csv": str(args.csv)}, ensure_ascii=False))
            return 0 if result["verdict"] in ("PASS", "PASS_SIMULATION") else 1 if result["verdict"] == "FAIL" else 2
        if args.action == "compare":
            if args.models:
                if any((args.bare, args.rtos, args.bare_monitor, args.rtos_monitor, args.environment)):
                    raise ValueError("--models is separate from the physical A/B input form")
                result = compare_models(*args.models)
            else:
                if not all((args.bare, args.rtos, args.bare_monitor, args.rtos_monitor, args.environment)):
                    raise ValueError("physical compare needs --bare --rtos --bare-monitor --rtos-monitor --environment")
                result = compare_hardware(args.bare, args.rtos, args.bare_monitor, args.rtos_monitor, args.environment)
            args.json.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            write_compare_csv(result, args.csv)
            print(json.dumps({"verdict": result["verdict"], "physical_hardware_tested": result["physical_hardware_tested"],
                              "json": str(args.json), "csv": str(args.csv)}, ensure_ascii=False))
            return 0 if result["verdict"] in ("PASS", "MODEL") else 1 if result["verdict"] == "FAIL" else 2
        if not args.hardware:
            raise ValueError("hardware I/O requires explicit --hardware; use generate/selftest for offline work")
        if args.action == "send":
            with args.out.open("x", encoding="utf-8", buffering=1_048_576) as out:
                return send_run(out, run_id=args.run_id if args.run_id is not None else secrets.randbits(64),
                                duration=args.duration, fps=args.fps, extended=args.extended, interfaces=args.interfaces)
        if args.action == "control":
            control_command(args.command)  # Validate before opening tty.
        port = PosixSerial(args.port)
        try:
            with args.out.open("x", encoding="utf-8") as out:
                if args.action == "monitor":
                    return monitor_run(out, port, args.duration, args.interval)
                return control(port, args.command, out)
        finally:
            port.close()
    except KeyboardInterrupt:
        print("interrupted; incomplete evidence must not be accepted", file=sys.stderr)
        return 130
    except (ValueError, OSError, KeyError, TypeError, OverflowError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
