#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run unchanged 11 stress inputs with real FreeRTOS POSIX task contexts.

Peripheral/CPU time is deterministic model input, not a hardware benchmark.
The production storage RPC wrapper is covered by the separate rtos_host test.
"""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

FW = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("baseline_stress", FW / "tests/stress/run_stress.py")
BASELINE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(BASELINE)
KERNEL = FW / "vendor/freertos"
PORT = KERNEL / "portable/ThirdParty/GCC/Posix"


def build(binary):
    local = [FW / name for name in (
        "tests/rtos_stress/kernel_pipeline.c", "tests/rtos_host/host_support.c",
        "platform/stm32h743_revd/storage_sd.c", "platform/stm32h743_revd/diskio_sd.c",
        "platform/stm32h743_revd/can_fdcan.c", "platform/stm32h743_revd/can_ingress.c",
        "platform/stm32h743_revd/ui_font_revd_16.c", "services/frame_ring/frame_ring.c",
        "services/logfmt/candump.c", "services/config/cfg.c", "vendor/fatfs/source/ff.c")]
    local += [KERNEL / name for name in ("tasks.c", "queue.c", "list.c", "event_groups.c")]
    local += [PORT / "port.c", PORT / "utils/wait_for_event.c"]
    lvgl = sorted((FW / "vendor/lvgl/src").rglob("*.c"))
    # Upstream POSIX port uses pthreads+signals. ASan's own thread/signal runtime
    # is deliberately kept out of this scheduler test; baseline retains ASan.
    flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-pthread",
             "-DLV_CONF_INCLUDE_SIMPLE", "-include", str(FW / "platform/stm32h743_revd/fatfs_conf/ffconf.h")]
    includes = [FW / name for name in ("tests/stress/include", "tests/rtos_host/include", "platform/stm32h743_revd", "vendor/fatfs/source", "vendor/lvgl")]
    includes += [FW, KERNEL / "include", PORT]
    flags += ["-I" + str(p) for p in includes]
    binary.parent.mkdir(parents=True, exist_ok=True)
    config = "".join((FW / name).read_text() for name in ("tests/rtos_host/include/FreeRTOSConfig.h", "tests/stress/include/stm32h7xx_hal.h", "platform/stm32h743_revd/lv_conf.h", "platform/stm32h743_revd/fatfs_conf/ffconf.h"))
    cache = binary.parent / "objects" / hashlib.sha256((str(flags) + config).encode()).hexdigest()[:16]
    cache.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get("CC", "cc")

    def compile_one(source):
        obj = cache / (hashlib.sha256(str(source).encode()).hexdigest()[:24] + ".o")
        if source in lvgl and obj.exists() and obj.stat().st_mtime >= source.stat().st_mtime:
            return obj
        extra = ["-Dlogfmt_format_line=stress_format_line", "-Df_write=stress_f_write", "-Df_sync=stress_f_sync", "-Df_close=stress_f_close"] if source.name == "storage_sd.c" else []
        run = subprocess.run([compiler, *flags, *extra, "-c", str(source), "-o", str(obj)], timeout=90)
        if run.returncode:
            raise RuntimeError("compile failed: " + str(source))
        return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        objects = list(pool.map(compile_one, [*local, *lvgl]))
    run = subprocess.run([compiler, "-pthread", *map(str, objects), "-lm", "-o", str(binary)], timeout=90)
    if run.returncode:
        raise RuntimeError("FreeRTOS pipeline model link failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, default=FW / "build/rtos_stress/report.json")
    parser.add_argument("--scenario")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--wait-policy", choices=("hybrid", "tick-only"), default="hybrid")
    args = parser.parse_args()
    binary = FW / "build/rtos_stress/kernel_pipeline"
    if not args.no_build:
        build(binary)
    reports = []
    for scenario in BASELINE.SCENARIOS:
        if args.scenario and scenario["name"] != args.scenario:
            continue
        cmd = [str(binary), "--model-wait-policy", args.wait_policy]
        for key, value in scenario.items():
            cmd += ["--" + key.replace("_", "-"), str(value)]
        completed = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if completed.stderr:
            print(completed.stderr, end="")
        if not completed.stdout.strip():
            raise RuntimeError(f"scenario crashed: {scenario['name']} ({completed.returncode})")
        row = json.loads(completed.stdout.strip().splitlines()[-1])
        row["process_exit_code"] = completed.returncode
        row["ownership_pass"] = row["owner_errors"] == 0 and row["max_fatfs_call_depth"] <= 1
        row["task_progress_pass"] = row["core_runs_during_sd_wait"] > 0 and row["ui_runs_during_sd_wait"] > 0
        row["all_checks_pass"] = bool(row["pass"] and row["ownership_pass"] and row["task_progress_pass"] and completed.returncode == 0)
        reports.append(row)
        print(f"{scenario['name']}: {'ZERO LOSS' if row['zero_loss'] else 'EXPECTED FAULT' if row['pass'] else 'FAILED'}; card={row['on_card']}; core gap={row['max_core_service_gap_us']} us; UI gap={row['max_ui_service_gap_us']} us; owner errors={row['owner_errors']}", flush=True)
    if not reports:
        raise ValueError("no scenario selected")
    output = {"schema": "revd-freertos-pipeline-model-v1", "physical_hardware_tested": False,
              "kernel": {"version": "V11.3.2", "commit": "8a59a4b19bf72977819feddef87d41163ced7004", "port": "upstream GCC/POSIX"},
              "evidence_class": "MODEL", "input_scenarios": BASELINE.SCENARIOS,
              "sd_wait_policy": args.wait_policy,
              "assumptions": {"same_virtual_peripherals_and_cpu_costs_as": "tests/stress/run_stress.py",
                              "model_worker_period_us": 1000, "virtual_dispatch": "highest-priority ready worker; notification handoff",
                              "actual_kernel_context_switches": True, "stm32_irq_tick_latency_modeled": False,
                              "production_rpc_wrapper_modeled": False, "complete_ui_or_usb_control_path_modeled": False,
                              "control_probe": "synthetic pending-control marker, not a USB latency measurement",
                              "claim": "task/stack isolation and unchanged integrity/fault contracts; no physical throughput/speedup claim"},
              "all_expectations_met": all(r["all_checks_pass"] for r in reports), "scenarios": reports}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(output, indent=2, ensure_ascii=False) + "\n")
    print("Report:", args.report)
    raise SystemExit(0 if output["all_expectations_met"] else 1)


if __name__ == "__main__":
    main()
