"""Portable, strict send-plan JSON codec and durable local simulated-SD store.

This is a configuration file, not a captured bus trace. Only Python stdlib is used.
"""
from __future__ import annotations
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

FORMAT = "canfd-send-plan"
VERSION = 1
EXTENSION = ".canplan.json"
MAX_BYTES = 131072
MAX_FILES = 16
MAX_NAME_BYTES = 47
WIRE_WORDS = 259
CHECKSUMS = ("none", "xor", "sum", "crc8-j1850", "crc8-h2f")
DYNAMIC_KEYS = ("counter_byte", "counter_bits", "counter_max", "counter_next",
                "checksum", "checksum_byte", "checksum_from", "checksum_to")
ENTRY_KEYS = {"slot", "id", "extended", "fd", "brs", "rtr", "length", "data",
              "period_ms", "enabled", "repeat_count", "frame_gap_ms", "cycle_ms", "dynamic"}

class PlanFormatError(ValueError):
    pass


def _object(value, keys, where, optional=()):
    if not isinstance(value, dict):
        raise PlanFormatError(f"{where} 必须是对象")
    missing = set(keys) - set(value)
    unknown = set(value) - set(keys) - set(optional)
    if missing: raise PlanFormatError(f"{where} 缺少字段：{', '.join(sorted(missing))}")
    if unknown: raise PlanFormatError(f"{where} 含未知字段：{', '.join(sorted(unknown))}")
    return value


def _int(value, low, high, where):
    if type(value) is not int or not low <= value <= high:
        raise PlanFormatError(f"{where} 必须是 {low}～{high} 的整数")
    return value


def _bool(value, where):
    if type(value) is not bool: raise PlanFormatError(f"{where} 必须是 true 或 false")
    return value


def validate_filename(name):
    if not isinstance(name, str): raise PlanFormatError("方案文件名必须是文字")
    try: size = len(name.encode("utf-8"))
    except UnicodeError as error: raise PlanFormatError("文件名含无效 Unicode") from error
    if not 1 <= size <= MAX_NAME_BYTES or not name.lower().endswith(EXTENSION) or len(name) <= len(EXTENSION):
        raise PlanFormatError("方案文件名需以 .canplan.json 结尾，最多47个UTF-8字节")
    if name[0] in ". " or name[-1] in ". " or any(ord(c) < 32 or 127 <= ord(c) <= 159 or c in '/\\:*?"<>|' or
            0x200B <= ord(c) <= 0x200F or 0x2028 <= ord(c) <= 0x202E or
            0x2066 <= ord(c) <= 0x2069 or ord(c) == 0xFEFF for c in name):
        raise PlanFormatError("方案文件名不能包含路径、控制字符或保留符号")
    device = name.split(".", 1)[0].rstrip(" ").upper()
    if device in {"CON", "PRN", "AUX", "NUL"} or re.fullmatch(r"(?:COM|LPT)[1-9]", device):
        raise PlanFormatError("方案文件名不能使用系统保留设备名")
    return name


def validate_project(project):
    if not isinstance(project, str): raise PlanFormatError("project项目名必须是文字")
    try: size = len(project.encode("utf-8"))
    except UnicodeError as error: raise PlanFormatError("project项目名含无效Unicode") from error
    if not 1 <= size <= MAX_NAME_BYTES or project[0] == " " or project[-1] == " " or project in (".", ".."):
        raise PlanFormatError("project项目名需为1～47个UTF-8字节，首尾不留空格")
    if any(ord(c) < 32 or 127 <= ord(c) <= 159 or c in "/\\" for c in project):
        raise PlanFormatError("project项目名不能包含路径或控制字符")
    return project


def _entry(value, slot, where):
    value = _object(value, ENTRY_KEYS, where)
    if _int(value["slot"], 1, 4, where + ".slot") != slot:
        raise PlanFormatError(f"{where} 条目需按 slot 1～4 顺序完整列出")
    extended, fd, brs, rtr = (_bool(value[k], where + "." + k) for k in ("extended", "fd", "brs", "rtr"))
    if brs and not fd or rtr and fd: raise PlanFormatError(f"{where}：BRS需要FD，FD不支持RTR")
    ident = value["id"]
    if not isinstance(ident, str) or not re.fullmatch(r"(?:0[xX])?[0-9A-Fa-f]{1,8}", ident):
        raise PlanFormatError(f"{where}.id 必须是十六进制字符串，例如 123 或 0x18FF50E5")
    number = int(ident, 16)
    if number > (0x1FFFFFFF if extended else 0x7FF): raise PlanFormatError(f"{where}.id 超出当前标准/扩展格式范围")
    length = _int(value["length"], 0, 64, where + ".length")
    legal = list(range(9)) + ([12, 16, 20, 24, 32, 48, 64] if fd else [])
    if length not in legal: raise PlanFormatError(f"{where}.length 不是该协议的合法数据长度")
    raw = value["data"]
    if not isinstance(raw, str) or len(raw) > 192: raise PlanFormatError(f"{where}.data 必须是十六进制字节文本")
    try: payload = bytes.fromhex(raw)
    except ValueError as error: raise PlanFormatError(f"{where}.data 含不完整或非法十六进制字节") from error
    if len(payload) != (0 if rtr else length): raise PlanFormatError(f"{where}.data 长度不匹配；RTR没有数据载荷")
    period = _int(value["period_ms"], 0, 65535, where + ".period_ms")
    enabled = _bool(value["enabled"], where + ".enabled")
    count = _int(value["repeat_count"], 1, 10000, where + ".repeat_count")
    gap = _int(value["frame_gap_ms"], 0, 3600000, where + ".frame_gap_ms")
    cycle = _int(value["cycle_ms"], 1, 3600000, where + ".cycle_ms")
    if count > 1 and not gap or cycle <= (count - 1) * gap:
        raise PlanFormatError(f"{where}：总周期必须大于 (次数-1)×同ID间隔，重复发送间隔至少1ms")
    dynamic = _object(value["dynamic"], DYNAMIC_KEYS, where + ".dynamic")
    d = {}
    for key in DYNAMIC_KEYS:
        if key == "checksum":
            if dynamic[key] not in CHECKSUMS: raise PlanFormatError(f"{where}.dynamic.checksum 不支持该校验算法")
            d[key] = dynamic[key]
        else: d[key] = _int(dynamic[key], 0, 255, where + ".dynamic." + key)
    if d["counter_bits"] not in (0, 4, 8): raise PlanFormatError(f"{where} 计数器位数只能为0/4/8")
    if rtr and (d["counter_bits"] or d["checksum"] != "none"):
        raise PlanFormatError(f"{where} RTR不可配置载荷计数器或校验")
    if d["counter_bits"]:
        if d["counter_byte"] >= length or d["counter_max"] > (15 if d["counter_bits"] == 4 else 255) or d["counter_next"] > d["counter_max"]:
            raise PlanFormatError(f"{where} 计数器位置、范围或下一帧初值无效")
    if d["checksum"] != "none":
        if d["checksum_byte"] >= length or not d["checksum_from"] <= d["checksum_to"] < length:
            raise PlanFormatError(f"{where} 校验字节或参与范围越界")
        if d["counter_bits"] and d["counter_byte"] == d["checksum_byte"]:
            raise PlanFormatError(f"{where} 计数器与校验不能覆盖同一个字节")
    return {"slot":slot, "id":f"{number:0{8 if extended else 3}X}", "extended":extended,
            "fd":fd, "brs":brs, "rtr":rtr, "length":length,
            "data":" ".join(f"{byte:02X}" for byte in payload), "period_ms":period,
            "enabled":enabled, "repeat_count":count, "frame_gap_ms":gap,
            "cycle_ms":cycle, "dynamic":d}


def validate_plan(value):
    value = _object(value, {"format", "version", "channel_mask", "channels"}, "方案", ("description", "project"))
    if value["format"] != FORMAT or type(value["version"]) is not int or value["version"] != VERSION:
        raise PlanFormatError("仅支持 format=canfd-send-plan、version=1 的发送方案")
    mask = _int(value["channel_mask"], 1, 3, "channel_mask")
    channels = value["channels"]
    expected = [ch for ch in (1, 2) if mask & (1 << (ch - 1))]
    if not isinstance(channels, list) or len(channels) != len(expected): raise PlanFormatError("channels必须完整匹配channel_mask所选通道")
    normalized = []
    for source, channel in zip(channels, expected):
        where = f"CAN{channel}"
        source = _object(source, {"channel", "id_gap_ms", "entries"}, where)
        if _int(source["channel"], 1, 2, where + ".channel") != channel: raise PlanFormatError("channels需按CAN1、CAN2顺序列出且不能重复")
        entries = source["entries"]
        if not isinstance(entries, list) or len(entries) != 4: raise PlanFormatError(f"{where}必须包含完整的4个发送槽；未使用槽设enabled=false")
        normalized.append({"channel":channel, "id_gap_ms":_int(source["id_gap_ms"], 0, 3600000, where + ".id_gap_ms"),
                           "entries":[_entry(entry, i + 1, where + f".slot{i+1}") for i, entry in enumerate(entries)]})
    result = {"format":FORMAT, "version":VERSION, "channel_mask":mask, "channels":normalized}
    if "project" in value:
        result["project"] = validate_project(value["project"])
    if "description" in value:
        description = value["description"]
        if not isinstance(description, str) or len(description) > 256: raise PlanFormatError("description最多256字符")
        try: description.encode("utf-8")
        except UnicodeError as error: raise PlanFormatError("description含无效Unicode") from error
        result["description"] = description
    return result


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result: raise PlanFormatError(f"重复JSON字段：{key}")
        result[key] = value
    return result


def decode_plan(raw):
    if not isinstance(raw, (str, bytes, bytearray)): raise PlanFormatError("方案内容必须是UTF-8 JSON")
    try:
        encoded = raw.encode("utf-8") if isinstance(raw, str) else bytes(raw)
        if not encoded or len(encoded) > MAX_BYTES: raise PlanFormatError("方案文件为空或超过128KiB")
        text = encoded.decode("utf-8-sig")
        value = json.loads(text, object_pairs_hook=_pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(PlanFormatError("JSON数值必须有限")))
    except PlanFormatError:
        raise
    except (UnicodeError, ValueError, RecursionError) as error:
        # json.loads can raise plain ValueError for an overlong integer. Keep
        # malformed files within the same rejection path used by SD scans.
        raise PlanFormatError("方案不是有效的UTF-8 JSON") from error
    return validate_plan(value)


def encode_plan(plan):
    normalized = validate_plan(plan)
    data = (json.dumps(normalized, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if len(data) > MAX_BYTES: raise PlanFormatError("方案超过128KiB")
    return data


def to_flat(plan):
    plan = validate_plan(plan)
    words = [0] * WIRE_WORDS
    words[0] = plan["channel_mask"]
    for channel in plan["channels"]:
        base = 1 + (channel["channel"] - 1) * 129
        words[base] = channel["id_gap_ms"]
        for i, entry in enumerate(channel["entries"]):
            at = base + 1 + i * 32
            flags = int(entry["extended"]) | (2 if entry["fd"] else 0) | (4 if entry["brs"] else 0) | (16 if entry["rtr"] else 0)
            words[at:at+4] = [int(entry["id"], 16), flags, entry["length"], entry["period_ms"]]
            payload = bytes.fromhex(entry["data"]).ljust(64, b"\0")
            words[at+4:at+20] = [int.from_bytes(payload[k:k+4], sys.byteorder) for k in range(0, 64, 4)]
            words[at+20:at+28] = [CHECKSUMS.index(entry["dynamic"][key]) if key == "checksum" else entry["dynamic"][key] for key in DYNAMIC_KEYS]
            words[at+28:at+32] = [int(entry["enabled"]), entry["repeat_count"], entry["frame_gap_ms"], entry["cycle_ms"]]
    return words


def from_flat(words):
    words = list(words)
    if len(words) != WIRE_WORDS or any(type(v) is not int or not 0 <= v <= 0xFFFFFFFF for v in words): raise PlanFormatError("C方案结构尺寸或uint32字段无效")
    mask = _int(words[0], 1, 3, "channel_mask")
    channels = []
    for ch in range(2):
        base = 1 + ch * 129
        if not mask & (1 << ch):
            if any(words[base:base+129]): raise PlanFormatError("未选通道的方案结构必须为零")
            continue
        entries = []
        for i in range(4):
            at = base + 1 + i * 32
            ident, flags, length, period = words[at:at+4]
            if flags & ~23 or length > 64 or words[at+28] not in (0, 1): raise PlanFormatError("C方案协议标志、长度或启用字段无效")
            payload = b"".join(v.to_bytes(4, sys.byteorder) for v in words[at+4:at+20])
            if any(payload[0 if flags & 16 else length:]): raise PlanFormatError("C方案有效载荷以外含非零字节")
            dynamic = dict(zip(DYNAMIC_KEYS, words[at+20:at+28]))
            checksum = dynamic["checksum"]
            if checksum >= len(CHECKSUMS): raise PlanFormatError("C方案校验算法无效")
            dynamic["checksum"] = CHECKSUMS[checksum]
            entries.append({"slot":i+1,"id":f"{ident:X}","extended":bool(flags&1),"fd":bool(flags&2),"brs":bool(flags&4),"rtr":bool(flags&16),"length":length,"data":payload[:0 if flags&16 else length].hex(" "),"period_ms":period,"dynamic":dynamic,"enabled":bool(words[at+28]),"repeat_count":words[at+29],"frame_gap_ms":words[at+30],"cycle_ms":words[at+31]})
        channels.append({"channel":ch+1,"id_gap_ms":words[base],"entries":entries})
    return validate_plan({"format":FORMAT,"version":VERSION,"channel_mask":mask,"channels":channels})


class PlanStore:
    """Actual files on the host's simulated SD. Scan never silently truncates."""
    def __init__(self, root, *, defer_errors=False):
        self.root = Path(root)
        try:
            self.root.mkdir(parents=True, exist_ok=True)
        except OSError:
            # The simulator can start and report the failing SD directory on
            # its device screen. Normal codec callers still fail immediately.
            if not defer_errors: raise

    def _read_bytes(self, name):
        name = validate_filename(name)
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(self.root / name, flags)
            try:
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_BYTES: raise PlanFormatError("方案不是普通文件或超过128KiB")
                with os.fdopen(fd, "rb", closefd=False) as source: data = source.read(MAX_BYTES + 1)
            finally: os.close(fd)
        except OSError as error: raise PlanFormatError("方案文件不可读或是符号链接") from error
        if len(data) > MAX_BYTES: raise PlanFormatError("方案超过128KiB")
        return data

    def read(self, name):
        return decode_plan(self._read_bytes(name))

    def export(self, name):
        data = self._read_bytes(name)
        decode_plan(data)
        return data

    def scan(self):
        result, errors, names = [], [], set()
        for path in sorted(self.root.iterdir(), key=lambda p:p.name.casefold()):
            if not path.name.lower().endswith(EXTENSION): continue
            try:
                validate_filename(path.name)
                folded = path.name.casefold()
                if folded in names: raise PlanFormatError("有大小写冲突的同名文件")
                plan = self.read(path.name)
                names.add(folded)
                result.append({"name":path.name,"plan":plan,"project":plan.get("project", "未分组")})
            except (PlanFormatError, OSError) as error: errors.append(f"{path.name}: {error}")
        if len(result) > MAX_FILES: raise PlanFormatError("模拟SD有超过16个有效发送方案，请整理目录；未截断目录")
        return result, errors

    def save(self, name, plan, *, overwrite=False):
        name = validate_filename(name)
        data = encode_plan(plan)  # No directory mutation before full validation.
        files, _ = self.scan()
        for other in self.root.iterdir():
            if other.name.casefold() == name.casefold():
                if not overwrite or other.name != name: raise PlanFormatError("同名方案已存在，未覆盖原文件")
                if other.is_symlink() or not other.is_file(): raise PlanFormatError("目标不是可覆盖的普通方案文件")
        if not (self.root / name).exists() and len(files) >= MAX_FILES: raise PlanFormatError("发送方案目录已满（16个）")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=self.root, prefix=".canplan-", suffix=".tmp", delete=False) as out:
                temporary = Path(out.name)
                out.write(data);out.flush();os.fsync(out.fileno())
            if overwrite: os.replace(temporary, self.root / name)
            else: os.link(temporary, self.root / name)  # Atomic no-clobber publish.
            try:
                directory = os.open(self.root, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
                try: os.fsync(directory)
                finally: os.close(directory)
            except OSError: pass  # Some hosts do not support directory fsync.
        except FileExistsError as error: raise PlanFormatError("同名方案已存在，未覆盖原文件") from error
        except OSError as error: raise PlanFormatError("方案文件保存失败，原文件保留") from error
        finally:
            if temporary is not None:
                try: temporary.unlink()
                except FileNotFoundError: pass
        return name

    def import_plan(self, name, raw, *, overwrite=False):
        return self.save(name, decode_plan(raw), overwrite=overwrite)
