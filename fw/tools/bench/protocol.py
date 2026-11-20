#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rev D load-test payload and strict candump -L parser. No I/O on import."""
from dataclasses import dataclass
import re
import struct
import zlib

MAGIC = b"RVDB"
VERSION = 1
FD_BRS = 1
FD_ESI = 2
FD_FDF = 4
STD_IDS = (0x5A0, 0x5A1)
EXT_IDS = (0x18FF5A00, 0x18FF5A01)
FD_LENGTHS = frozenset((*range(9), 12, 16, 20, 24, 32, 48, 64))
UINT64_MAX = (1 << 64) - 1


def payload(run_id, ch, seq, planned_ns):
    if ch not in (0, 1) or not all(0 <= n <= UINT64_MAX for n in (run_id, seq, planned_ns)):
        raise ValueError("payload channel or uint64 out of range")
    prefix = struct.pack("<4sBBHQQQ", MAGIC, VERSION, ch, 0, run_id, seq, planned_ns)
    pattern = bytes((run_id + seq * 17 + ch * 31 + i * 13) & 255 for i in range(28))
    body = prefix + pattern
    return body + struct.pack("<I", zlib.crc32(body))


def decode_payload(data):
    if len(data) != 64:
        raise ValueError("bench payload length is not 64")
    magic, version, ch, reserved, run_id, seq, planned_ns = struct.unpack("<4sBBHQQQ", data[:32])
    if magic != MAGIC or version != VERSION or ch not in (0, 1) or reserved:
        raise ValueError("bench payload header")
    if struct.unpack("<I", data[60:])[0] != zlib.crc32(data[:60]):
        raise ValueError("bench payload CRC32")
    if data != payload(run_id, ch, seq, planned_ns):
        raise ValueError("bench payload deterministic pattern")
    return {"run_id": run_id, "ch": ch, "seq": seq, "planned_ns": planned_ns}


@dataclass(frozen=True)
class Frame:
    ts_us: int
    interface: str
    can_id: int
    extended: bool
    fd: bool
    flags: int
    data: bytes
    rtr: bool = False
    error: bool = False


_LINE = re.compile(r"^\((\d+)\.(\d{1,6})\)\s+([A-Za-z0-9_.:-]+)\s+([0-9A-Fa-f]{3}|[0-9A-Fa-f]{8})#(\S*)\s*$")


def parse_candump(line):
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    match = _LINE.fullmatch(line)
    if not match:
        raise ValueError("expected candump -L '(seconds.microseconds) interface ID##flagsDATA'")
    seconds, fraction, interface, id_text, body = match.groups()
    ts_us = int(seconds) * 1_000_000 + int(fraction.ljust(6, "0"))
    if ts_us > UINT64_MAX:
        raise ValueError("timestamp overflow")
    can_id = int(id_text, 16)
    error = bool(can_id & 0x20000000)
    extended = len(id_text) == 8 and not error
    if can_id > (0x3FFFFFFF if error else 0x1FFFFFFF if extended else 0x7FF):
        raise ValueError("CAN identifier out of range")
    if error:
        can_id &= 0x1FFFFFFF
    fd = body.startswith("#")
    flags = 0
    rtr = body.startswith(("R", "r"))
    if fd:
        if len(body) < 2 or body[1] not in "01234567":
            raise ValueError("CAN FD flags out of range")
        flags = int(body[1], 16)
        body = body[2:]
    if rtr:
        if body not in ("R", "r") + tuple("R" + str(n) for n in range(9)) + tuple("r" + str(n) for n in range(9)):
            raise ValueError("RTR DLC")
        data = b""
    else:
        if not re.fullmatch(r"(?:[0-9A-Fa-f]{2})(?:\.?[0-9A-Fa-f]{2})*|", body):
            raise ValueError("invalid hex data")
        data = bytes.fromhex(body.replace(".", ""))
    if len(data) not in (FD_LENGTHS if fd else range(9)) or (error and fd):
        raise ValueError("invalid CAN(FD) length/type")
    return Frame(ts_us, interface, can_id, extended, fd, flags, data, rtr, error)


def format_candump(ch, seq, run_id, planned_ns, extended=False, ts_us=None, flags=FD_BRS):
    can_id = (EXT_IDS if extended else STD_IDS)[ch]
    id_text = f"{can_id:08X}" if extended else f"{can_id:03X}"
    if ts_us is None:
        ts_us = planned_ns // 1000
    return f"({ts_us // 1_000_000}.{ts_us % 1_000_000:06d}) can{ch} {id_text}##{flags:X}{payload(run_id, ch, seq, planned_ns).hex().upper()}\n"


def pack_socketcan(ch, seq, run_id, planned_ns, extended=False):
    can_id = (EXT_IDS if extended else STD_IDS)[ch] | (0x80000000 if extended else 0)
    # Linux <linux/can.h>: 32-bit native-endian id, len, flags, two pad bytes.
    return struct.pack("=IBB2x64s", can_id, 64, FD_BRS, payload(run_id, ch, seq, planned_ns))


def unpack_socketcan(raw, interface):
    if len(raw) not in (16, 72):
        raise ValueError("invalid SocketCAN MTU")
    can_id, length, flags = struct.unpack("=IBB", raw[:6])
    fd = len(raw) == 72
    if length not in (FD_LENGTHS if fd else range(9)) or len(raw) < 8 + length:
        raise ValueError("invalid SocketCAN length")
    return Frame(0, interface, can_id & 0x1FFFFFFF, bool(can_id & 0x80000000), fd,
                 flags if fd else 0, raw[8:8 + length], bool(can_id & 0x40000000), bool(can_id & 0x20000000))
