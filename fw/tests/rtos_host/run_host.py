#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile/run pinned upstream FreeRTOS GCC/POSIX tests on Linux or macOS."""
import argparse
import os
from pathlib import Path
import subprocess

FW = Path(__file__).resolve().parents[2]
KERNEL = FW / "vendor/freertos"
PORT = KERNEL / "portable/ThirdParty/GCC/Posix"


def build(binary, sources, *, includes=(), defines=(), extra_flags=(), sanitize=False):
    binary = Path(binary)
    binary.parent.mkdir(parents=True, exist_ok=True)
    kernel = [KERNEL / name for name in ("tasks.c", "queue.c", "list.c", "event_groups.c")]
    common = [FW / "tests/rtos_host/host_support.c", PORT / "port.c", PORT / "utils/wait_for_event.c"]
    flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-pthread"]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    flags += ["-I" + str(p) for p in (FW / "tests/rtos_host/include", KERNEL / "include", PORT, FW, *includes)]
    flags += ["-D" + d for d in defines]
    result = subprocess.run([os.environ.get("CC", "cc"), *flags, *extra_flags,
                             *map(str, [*sources, *common, *kernel]), "-lm", "-o", str(binary)], timeout=120)
    if result.returncode:
        raise RuntimeError("FreeRTOS POSIX host compile failed")
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("smoke", "storage", "bridges"), default="smoke")
    parser.add_argument("--source", action="append", type=Path, default=[])
    parser.add_argument("--include", action="append", type=Path, default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    sources = [FW / {"smoke":"tests/rtos_host/test_static_tasks.c", "storage":"tests/rtos_host/test_storage_owner.c", "bridges":"tests/rtos_host/test_bridges.c"}[args.kind]]
    includes = list(args.include)
    defines = list(args.define)
    extra_flags = []
    if args.kind == "storage":
        platform = FW / "platform/stm32h743_revd"
        sources += [platform / name for name in ("storage_sd.c", "diskio_sd.c", "rtos/storage_owner.c")]
        sources += [FW / name for name in ("services/logfmt/candump.c", "services/config/cfg.c", "vendor/fatfs/source/ff.c")]
        includes += [FW / "tests/test_storage_hal", platform, platform / "fatfs_conf", FW / "vendor/fatfs/source"]
        defines += ["REVD_FREERTOS"]
        extra_flags += ["-include", str(platform / "fatfs_conf/ffconf.h")]
    if args.kind == "bridges":
        platform = FW / "platform/stm32h743_revd"
        sources += [platform / name for name in ("rtos/bridges.c", "usb_device.c", "usb_protocol.c")]
        sources += [FW / name for name in ("services/logfmt/candump.c", "services/core/app_evt.c", "services/frame_ring/frame_ring.c", "services/core/jsonw.c")]
        includes += [FW / "tests/usb/include", platform, platform / "rtos"]
        defines += ["REVD_FREERTOS", "CANFD_REVD_HOST_DIAG", "REVD_RTOS_USB_TEST"]
    sources += args.source
    binary = FW / "build/rtos_host" / (args.kind + ("-san" if args.sanitize else ""))
    if not args.no_build:
        build(binary, sources, includes=includes, defines=defines, extra_flags=extra_flags, sanitize=args.sanitize)
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
    print(run.stdout, end="")
    if run.stderr:
        print(run.stderr, end="", file=__import__("sys").stderr)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(run.stdout)
    raise SystemExit(run.returncode)


if __name__ == "__main__":
    main()
