#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded-duration Linux SocketCAN sender and POSIX CDC status collector."""
import errno
from fractions import Fraction
import heapq
import json
import math
import os
import select
import socket
import sys
import time

try:
    from .protocol import decode_payload, pack_socketcan, unpack_socketcan, STD_IDS, EXT_IDS
except ImportError:
    from protocol import decode_payload, pack_socketcan, unpack_socketcan, STD_IDS, EXT_IDS


def emit(out, obj):
    out.write(json.dumps(obj, separators=(",", ":"), allow_nan=False) + "\n")


def rate(value):
    result = Fraction(str(value))
    if result <= 0 or result > 100_000:
        raise ValueError("fps must be > 0 and <= 100000 per channel")
    return result


def schedule(duration_s, fps):
    duration_ns = int(Fraction(str(duration_s)) * 1_000_000_000)
    if not 0 < duration_ns <= 86_400_000_000_000:
        raise ValueError("duration must be > 0 and <= 86400 seconds")
    rates = [rate(x) for x in fps]
    if len(rates) != 2:
        raise ValueError("exactly two channel rates required")
    heap = [(0, 0, 0), (0, 1, 0)]
    while heap:
        offset, ch, seq = heapq.heappop(heap)
        if offset >= duration_ns:
            continue
        yield offset, ch, seq
        seq += 1
        next_offset = seq * 1_000_000_000 * rates[ch].denominator // rates[ch].numerator
        heapq.heappush(heap, (next_offset, ch, seq))


def config(run_id, duration, fps, extended, interfaces, synthetic=False):
    return {"type": "run", "schema": 1, "payload": "RVDB/1", "run_id": hex(run_id),
            "duration_s": float(Fraction(str(duration))),
            "duration_ns": int(Fraction(str(duration)) * 1_000_000_000),
            "fps_per_channel": [str(rate(x)) for x in fps],
            "extended": bool(extended), "fd_flags": 1, "interfaces": list(interfaces),
            "synthetic": synthetic,
            "evidence": "synthetic plan" if synthetic else "SocketCAN send() acceptance; not a bus witness"}


def open_can(interface, can_id, extended):
    if not sys.platform.startswith("linux") or not hasattr(socket, "AF_CAN"):
        raise OSError("hardware send requires Linux SocketCAN")
    if not interface or len(interface.encode()) >= 16 or any(c.isspace() for c in interface):
        raise ValueError("invalid explicit interface name")
    import fcntl
    import struct
    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    try:
        mtu = fcntl.ioctl(sock.fileno(), 0x8921, struct.pack("16sI", interface.encode(), 0))
        if struct.unpack_from("=I", mtu, 16)[0] != 72:
            raise OSError(f"{interface} MTU is not CAN FD (72); configure the prepared bench separately")
        sol = getattr(socket, "SOL_CAN_RAW", 101)
        sock.setsockopt(sol, getattr(socket, "CAN_RAW_FD_FRAMES", 5), 1)
        sock.setsockopt(sol, getattr(socket, "CAN_RAW_RECV_OWN_MSGS", 4), 1)
        # Include EFF/RTR/ERR bits in the mask so unlike frame types cannot match.
        mask = 0xE0000000 | (0x1FFFFFFF if extended else 0x7FF)
        wanted = can_id | (0x80000000 if extended else 0)
        sock.setsockopt(sol, getattr(socket, "CAN_RAW_FILTER", 1), struct.pack("=II", wanted, mask))
        sock.bind((interface,))
        sock.setblocking(False)
        return sock
    except BaseException:
        sock.close()
        raise


def send_run(out, *, run_id, duration, fps, extended, interfaces,
             socket_factory=open_can, monotonic_ns=time.monotonic_ns, sleep=time.sleep,
             echo_wait=0.25):
    """No interface discovery/configuration. send errors are logged and never retried.

    Pacing uses absolute deadlines. CPU starvation is visible as lateness and an
    incomplete schedule at the hard end time; the tool does not claim wire rate.
    """
    if len(interfaces) != 2 or interfaces[0] == interfaces[1] or not all(interfaces):
        raise ValueError("two distinct explicit interfaces required")
    # Validate all config before opening hardware.
    rates = [rate(x) for x in fps]
    next(iter(schedule(duration, fps)))
    duration_ns = int(Fraction(str(duration)) * 1_000_000_000)
    emit(out, config(run_id, duration, fps, extended, interfaces))
    sockets = []
    start = monotonic_ns()
    data_end_wall = None
    sending_elapsed = 0
    counts = [0, 0]
    requested = [0, 0]
    errors = [0, 0]
    echoes = [0, 0]
    late = [0, 0]
    max_late = [0, 0]
    completed = False
    failure = None

    def drain():
        for ch, sock in enumerate(sockets):
            for _ in range(128):
                try:
                    raw, _ancillary, flags, _address = sock.recvmsg(72, 128)
                except BlockingIOError:
                    break
                except OSError as exc:
                    emit(out, {"type": "echo_error", "ch": ch, "errno": exc.errno, "error": str(exc)})
                    break
                try:
                    frame = unpack_socketcan(raw, interfaces[ch])
                    decoded = decode_payload(frame.data)
                    if decoded["run_id"] != run_id or decoded["ch"] != ch:
                        continue
                    confirm = bool(flags & getattr(socket, "MSG_CONFIRM", 0x800))
                    echoes[ch] += int(confirm)
                    emit(out, {"type": "socket_echo", "ch": ch, "seq": decoded["seq"],
                               "monotonic_ns": monotonic_ns(), "msg_confirm": confirm,
                               "local_host": bool(flags & getattr(socket, "MSG_DONTROUTE", 4)),
                               "evidence": "local echo; driver-dependent completion, not independent witness"})
                except ValueError:
                    continue
    try:
        for ch, interface in enumerate(interfaces):
            sockets.append(socket_factory(interface, (EXT_IDS if extended else STD_IDS)[ch], extended))
        start = monotonic_ns()
        emit(out, {"type": "sender_start", "monotonic_ns": start, "wall_time": time.time()})
        for offset, ch, seq in schedule(duration, fps):
            now = monotonic_ns()
            if now < start + offset:
                sleep((start + offset - now) / 1_000_000_000)
            now = monotonic_ns()
            if now >= start + duration_ns:
                break
            requested[ch] += 1
            delay = max(0, now - start - offset)
            period = 1_000_000_000 * rates[ch].denominator // rates[ch].numerator
            late[ch] += int(delay > period)
            max_late[ch] = max(max_late[ch], delay)
            record = {"type": "tx", "ch": ch, "seq": seq, "scheduled_offset_ns": offset,
                      "requested_ns": now, "late_ns": delay, "send_accepted_ns": None}
            try:
                nbytes = sockets[ch].send(pack_socketcan(ch, seq, run_id, offset, extended))
                if nbytes != 72:
                    raise OSError(errno.EIO, f"short SocketCAN send: {nbytes}")
                record["send_accepted_ns"] = monotonic_ns()
                counts[ch] += 1
            except OSError as exc:
                errors[ch] += 1
                record.update(send_errno=exc.errno, send_error=str(exc))
            emit(out, record)
            if sum(requested) % 64 == 0:
                drain()
        else:
            completed = True
        data_end_wall = time.time()
        sending_elapsed = monotonic_ns() - start
        # Echo draining is bounded. Missing local echoes do not prove recorder loss.
        until = monotonic_ns() + int(echo_wait * 1_000_000_000)
        while monotonic_ns() < until:
            drain()
            sleep(min(0.005, max(0, until - monotonic_ns()) / 1_000_000_000))
        drain()
    except BaseException as exc:
        failure = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        elapsed = max(1, monotonic_ns() - start)
        for sock in sockets:
            sock.close()
        emit(out, {"type": "end", "complete": completed and failure is None,
                   "requested": requested, "send_accepted": counts, "send_errors": errors,
                   "local_confirm_echoes": echoes, "late_over_period": late, "max_lateness_ns": max_late,
                   "elapsed_ns_including_echo_drain": elapsed,
                   "sending_elapsed_ns": sending_elapsed, "data_end_wall_time": data_end_wall,
                   "accepted_fps_over_requested_duration": [c * 1_000_000_000 / duration_ns for c in counts],
                   "accepted_fps_including_echo_drain": [c * 1_000_000_000 / elapsed for c in counts],
                   "error": failure})
        out.flush()
    return 0 if completed and not any(errors) else 1


class PosixSerial:
    """Raw CDC tty; baud is a host tty setting (USB CDC device may ignore it)."""
    def __init__(self, path, baud=115200):
        import termios
        import tty
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            self.original = termios.tcgetattr(self.fd)
            tty.setraw(self.fd, termios.TCSANOW)
            attrs = termios.tcgetattr(self.fd)
            speed = getattr(termios, "B" + str(baud))
            attrs[4] = attrs[5] = speed
            attrs[2] |= termios.CLOCAL | termios.CREAD
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            # CDC host-link state uses DTR. Some pseudo ttys do not implement it.
            import fcntl
            import struct
            try:
                fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))
            except OSError as exc:
                if exc.errno not in (errno.ENOTTY, errno.EINVAL):
                    raise
        except BaseException:
            os.close(self.fd)
            raise

    def write(self, data):
        return os.write(self.fd, data)

    def read(self, timeout):
        if select.select([self.fd], [], [], max(0, timeout))[0]:
            data = os.read(self.fd, 65536)
            if not data:
                raise OSError("CDC disconnected")
            return data
        return b""

    def close(self):
        import termios
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self.original)
        except OSError:
            pass
        os.close(self.fd)

    def flush_input(self):
        import termios
        termios.tcflush(self.fd, termios.TCIFLUSH)


def monitor_run(out, port, duration, interval=1.0, *, monotonic=time.monotonic, wall=time.time):
    """Only sends STATUS. Accepts STATUS {JSON} plus unrelated CDC replies."""
    if not all(math.isfinite(x) and x > 0 for x in (duration, interval)) or duration > 86400:
        raise ValueError("monitor duration and interval must be finite positive; duration <= 86400")
    start = monotonic()
    next_request = start
    buffer = b""
    count = 0
    ok = False
    pending = b""
    outstanding = None
    transport_errors = 0
    request_count = 0
    response_timeout = max(2.0, interval * 2)
    if hasattr(port, "flush_input"):
        port.flush_input()
    emit(out, {"type": "monitor_start", "schema": 1, "wall_time": wall(), "interval_s": interval})
    try:
        while monotonic() - start < duration:
            now = monotonic()
            if outstanding is not None and now-outstanding["monotonic_s"] >= response_timeout:
                emit(out, {"type": "monitor_error", "error": "STATUS response timeout", "request": outstanding,
                           "elapsed_s": now-start})
                transport_errors += 1
                outstanding = None
                pending = b""
            if now >= next_request and not pending and outstanding is None:
                pending = b"STATUS\n"
                outstanding = {"sequence": request_count, "monotonic_s": now, "wall_time": wall()}
                request_count += 1
                next_request = now + interval
            if pending:
                try:
                    written = port.write(pending)
                    if written < 0 or written > len(pending):
                        raise OSError("invalid CDC write length")
                    pending = pending[written:]
                except BlockingIOError:
                    pass
            chunk = port.read(min(0.05, max(0, start + duration - monotonic())))
            if chunk:
                buffer += chunk
            if len(buffer) > 65536:
                emit(out, {"type": "monitor_error", "error": "CDC line exceeds 64 KiB"})
                raise ValueError("CDC line exceeds 64 KiB")
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if text.startswith("STATUS "):
                    try:
                        status = json.loads(text[7:])
                        if not isinstance(status, dict):
                            raise ValueError("status is not an object")
                    except (json.JSONDecodeError, ValueError) as exc:
                        emit(out, {"type": "monitor_error", "error": str(exc), "raw": text[:512]})
                        transport_errors += 1
                        continue
                    response_at = monotonic()
                    request = outstanding
                    if request is None or pending:
                        emit(out, {"type": "monitor_error", "error": "unmatched STATUS response", "elapsed_s": response_at-start})
                        transport_errors += 1
                    emit(out, {"type": "status", "wall_time": wall(),
                               "elapsed_s": response_at-start, "status": status,
                               "request": request,
                               "roundtrip_ms": (response_at-request["monotonic_s"])*1000 if request is not None and not pending else None,
                               "latency_semantics": "STATUS request queued on host to full response line; includes USB, device and host scheduling"})
                    outstanding = None
                    count += 1
                elif text:
                    emit(out, {"type": "cdc_other", "elapsed_s": monotonic() - start, "text": text[:512]})
        ok = count >= 2 and not buffer.startswith(b"STATUS ") and not pending and outstanding is None and not transport_errors
    finally:
        emit(out, {"type": "monitor_end", "complete": ok, "samples": count,
                   "requests": request_count, "transport_errors": transport_errors,
                   "outstanding_request": outstanding,
                   "elapsed_s": monotonic() - start, "trailing_bytes": len(buffer)})
        out.flush()
    return 0 if ok else 1
